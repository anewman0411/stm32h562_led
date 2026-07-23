#include "stm32h5xx.h"
#include "global_constants.h"
#include "setup.h"
#include "bootloader.h"
#include "usb_cdc.h"

/* Simple busy-wait delay (not cycle-accurate, just for blinking) */
void delay(volatile uint32_t count)
{
    while (count--) {
        __NOP();
    }
}

int main(void)
{
    /*
     * Must run before anything else touches the hardware: if the previous boot
     * asked for DFU, this never returns -- it jumps straight to the ROM
     * bootloader so the board can be reflashed over USB-C.
     */
    bootloader_process_reset_reason();

    /* Initialize system clock and GPIO */
    setup();

    /* Virtual COM port, so the host can request the reboot above */
    usb_cdc_init();

    /* Main loop: blink LED */
    while (1)
    {
        /* Toggle LED pin */
        LED_PORT->ODR ^= (0x1UL << LED_PIN);

        /*
         * Rough delay ~500 ms:
         *   HSI  @ 64 MHz  -> try 1_000_000
         *   PLL1 @ 250 MHz -> try 4_000_000
         * Adjust to taste.
         *
         * Split into slices so a reboot request raised by the USB interrupt is
         * acted on promptly instead of up to a full blink period later. The
         * reset is issued from here rather than from the interrupt so the
         * control transfer that asked for it can complete cleanly first.
         */
        for (int i = 0; i < 8; i++) {
            if (usb_cdc_bootloader_requested()) {
                bootloader_reboot_to_dfu();  /* does not return */
            }
            delay(1000000);
        }
    }

    return 0;
}
