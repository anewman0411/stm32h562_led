#ifndef USB_CDC_H_
#define USB_CDC_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimal USB CDC-ACM (virtual COM port) device for the STM32H562's USB_DRD_FS
 * peripheral, written directly against the registers -- no HAL, no ST USB
 * middleware.
 *
 * Its only job is to give the host a way to talk to the running firmware so it
 * can ask for a reboot into the ROM bootloader. It enumerates as 0483:5740
 * (ST's stock VCP id), so Linux binds cdc_acm and it shows up as /dev/ttyACM*.
 *
 * Two reboot triggers are accepted, both handled in the USB interrupt:
 *   - the 1200-baud touch: host opens the port at 1200 baud then drops DTR
 *     (the Arduino/Betaflight convention, and what tools/dfu-flash.sh uses)
 *   - the byte 'R' written to the port, for poking it by hand from a terminal
 */

/* Bring up VDDUSB, the 48 MHz clock, PA11/PA12 and the peripheral. */
void usb_cdc_init(void);

/*
 * True once a reboot-to-bootloader has been requested over USB. Poll this from
 * the main loop; the actual reset is deliberately left to non-interrupt context
 * so the USB status stage can complete first and the host does not see the
 * device vanish mid-transfer.
 */
bool usb_cdc_bootloader_requested(void);

#endif /* USB_CDC_H_ */
