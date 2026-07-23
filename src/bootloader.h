#ifndef BOOTLOADER_H_
#define BOOTLOADER_H_

/*
 * Software entry into the STM32H5 ROM (system-memory) bootloader, so the board
 * can be reflashed over USB-C with dfu-util alone -- no ST-Link, and without
 * touching the BOOT0 pin.
 *
 * Mechanism (ported from Betaflight src/platform/STM32/system_stm32h5xx.c):
 *   1. Host asks the running firmware to reboot (see usb_cdc.h).
 *   2. A magic word is stored in a TAMP backup register, which survives a
 *      system reset, and NVIC_SystemReset() is issued.
 *   3. Very early on the next boot, bootloader_process_reset_reason() sees the
 *      magic and jumps to the ROM bootloader instead of running the app.
 */

/*
 * Call as the FIRST statement in main(), before clocks/GPIO/USB are touched.
 * Normally returns immediately; does not return when a bootloader entry is
 * pending (it jumps to ROM) or when a post-bootloader cleanup reset is due.
 */
void bootloader_process_reset_reason(void);

/*
 * Arm the magic and reset into the ROM bootloader. Does not return.
 */
void bootloader_reboot_to_dfu(void);

#endif /* BOOTLOADER_H_ */
