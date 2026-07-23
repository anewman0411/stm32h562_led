#include "stm32h5xx.h"
#include "bootloader.h"

/*
 * STM32H5 system bootloader vector-table address (AN2606).
 *
 * This is the bootloader ENTRY POINT, not the system-flash region base
 * (0x0BF80000). Reading the initial SP / reset vector from the region base
 * lands on garbage, so a software jump there never enters the bootloader even
 * though the hardware BOOT pin still works -- the ROM handles the pin path
 * itself. Value and rationale taken from Betaflight, which hit exactly this.
 */
#if defined(STM32H562xx) || defined(STM32H563xx) || defined(STM32H573xx)
#define SYSMEMBOOT_VECTOR_TABLE ((uint32_t *)0x0BF97000UL)
#elif defined(STM32H503xx)
#define SYSMEMBOOT_VECTOR_TABLE ((uint32_t *)0x0BF87000UL)
#else
#error "STM32H5: system bootloader address unknown for this part (see AN2606)"
#endif

/*
 * Persistent state lives in a TAMP backup register. Backup registers are reset
 * only by a backup-domain reset (VBAT loss), so they survive both
 * NVIC_SystemReset() and the bootloader's own activity -- which is the whole
 * point.
 */
#define BOOTLOADER_BKP_REG          (TAMP->BKP0R)

#define RESET_NONE                  0x00000000UL
#define RESET_BOOTLOADER_REQUEST    0xB007B007UL  /* app -> "enter DFU"       */
#define RESET_BOOTLOADER_POST       0xB007F00DUL  /* set just before jumping  */

static void backup_domain_access_enable(void)
{
    /*
     * TAMP sits behind the RTC APB clock gate, and the backup domain is
     * write-protected out of reset. PWR itself has no RCC enable bit on H5
     * (it is always clocked), unlike F4/F7.
     */
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;
    (void)RCC->APB3ENR;         /* ensure the clock is live before first use */

    PWR->DBPCR |= PWR_DBPCR_DBP;
    while ((PWR->DBPCR & PWR_DBPCR_DBP) == 0) {
        ; /* wait for backup-domain write protection to actually lift */
    }
}

static void jump_to_rom_bootloader(void)
{
    __disable_irq();

    /*
     * Hand the ROM a machine that looks freshly reset. Anything left running --
     * USB in particular -- can make the bootloader misbehave or fail to
     * enumerate, since it re-initialises the same peripheral from scratch.
     */
    USB_DRD_FS->CNTR = USB_CNTR_PDWN | USB_CNTR_USBRST;
    RCC->APB2ENR &= ~RCC_APB2ENR_USBEN;

    /* Release PA11/PA12 back to their reset (analog) state. */
    GPIOA->MODER |= (0x3UL << (11 * 2)) | (0x3UL << (12 * 2));

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;   /* disable */
        NVIC->ICPR[i] = 0xFFFFFFFFUL;   /* clear pending */
    }

    __DSB();
    __ISB();

    /*
     * STM32H5 is Cortex-M33 and has no SYSCFG memory remap, so point VTOR
     * straight at the system-flash vector table.
     */
    SCB->VTOR = (uint32_t)SYSMEMBOOT_VECTOR_TABLE;
    __DSB();

    const uint32_t boot_stack = SYSMEMBOOT_VECTOR_TABLE[0];
    void (*const boot_entry)(void) = (void (*)(void))SYSMEMBOOT_VECTOR_TABLE[1];

    __set_MSP(boot_stack);
    __ISB();

    boot_entry();

    while (1) {
        ; /* unreachable */
    }
}

void bootloader_process_reset_reason(void)
{
    backup_domain_access_enable();

    const uint32_t request = BOOTLOADER_BKP_REG;

    switch (request) {
    case RESET_BOOTLOADER_REQUEST:
        /*
         * Mark that we are about to hand control to the ROM. If the host then
         * uses dfu-util's ":leave", the ROM jumps straight into the new
         * firmware WITHOUT a reset, so the next thing to run this function is
         * the freshly flashed app -- which lands on the POST case below.
         */
        BOOTLOADER_BKP_REG = RESET_BOOTLOADER_POST;
        jump_to_rom_bootloader();
        break;

    case RESET_BOOTLOADER_POST:
        /*
         * We got here via the bootloader's ":leave" jump rather than a real
         * reset, so the machine still carries the ROM's peripheral state.
         * Clear the flag and take one clean reset to get a known-good start.
         */
        BOOTLOADER_BKP_REG = RESET_NONE;
        __DSB();
        NVIC_SystemReset();
        break;

    default:
        /* Normal boot. Leave the register alone. */
        break;
    }
}

void bootloader_reboot_to_dfu(void)
{
    backup_domain_access_enable();

    BOOTLOADER_BKP_REG = RESET_BOOTLOADER_REQUEST;
    __DSB();

    __disable_irq();
    NVIC_SystemReset();

    while (1) {
        ; /* unreachable */
    }
}
