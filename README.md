# STM32H562RGT6 — LED blink

Bare-metal STM32H562 project: CMSIS only, no HAL, no ST middleware. Includes a from-scratch USB CDC virtual COM port so the board can reflash itself over USB-C.

## Which flashing method do I need?

| Situation | Method |
|---|---|
| Normal development, USB firmware already running | **[USB-C only](#flashing-using-only-usb-c-no-st-link-no-boot0-button)** — one cable, no buttons |
| First-ever flash on a blank chip | [ST-Link](#flashing-with-st-link-first-flash-and-recovery) |
| Board wedged, bricked, or not enumerating | [ST-Link](#flashing-with-st-link-first-flash-and-recovery) |

The USB-C path needs firmware built from this tree already running on the chip, because it works by asking that firmware to reboot into the bootloader. So the **first** flash must go over ST-Link; every one after that can be USB-C.

## Build the project

```bash
mkdir -p build
cd build
cmake ..
make
```

Artifacts land in [bin/](bin/) — `main.elf`, `main.bin`, `main.hex`.

---

# Flashing using only USB-C (no ST-Link, no BOOT0 button)

Once firmware built from this tree is running, the board reflashes itself over the USB-C connector alone:

```bash
cd build && make      # produces bin/main.bin
cd .. && ./flash_dfu.sh
```

That is the whole loop. No ST-Link, no BOOT0 pin, no RESET button, one cable.

A successful run ends with:

```
>> Found CDC port /dev/ttyACM0 — requesting reboot into DFU...
>> Confirmed STM32H5-style DFU target
>> Flashing bin/main.bin -> 0x08000000
   Download [=========================] 100%   5088 bytes
   File downloaded successfully
>> Download OK — waiting for the board to re-enumerate...
>> Board is back as 0483:5740 — new firmware is running.
>> Done.
```

Requires `dfu-util` (`sudo apt install dfu-util`). [tools/dfu-flash.py](tools/dfu-flash.py) is a Python equivalent with more verbose diagnostics.

## How it works

| Step | What happens |
|---|---|
| 1 | Running firmware exposes a USB CDC virtual COM port, **`0483:5740`** → `/dev/ttyACM*` |
| 2 | Script sends the token `DFU!`, then does a **1200-baud touch** (opens the port at 1200 baud and drops DTR) |
| 3 | Firmware writes a magic word to a **TAMP backup register** and resets. Backup registers survive a reset, which is what carries the request across the reboot |
| 4 | Next boot, `bootloader_process_reset_reason()` sees the magic *before anything else initialises* and jumps to the ROM bootloader |
| 5 | ROM bootloader enumerates as DFU, **`0483:df11`**, on the same USB-C port |
| 6 | `dfu-util -a 0 -s 0x08000000:leave -D bin/main.bin` writes the image and restarts into it |

The BOOT0 pin is never involved — entering the bootloader is entirely a software decision made by the running firmware.

## Triggering it by hand

Either trigger works from a terminal:

```bash
printf 'DFU!' > /dev/ttyACM0     # data token
echo -n R > /dev/ttyACM0         # single-key escape hatch
```

## It refuses to flash a non-H5 board

The F405 and H562 **both** enumerate as `0483:df11`, so VID:PID cannot tell them apart. `flash_dfu.sh` reads the DFU flash-layout descriptor and aborts if it sees the F4 sector map, rather than writing an H5 image onto an F4 board:

```
error: this looks like an STM32F4 DFU device, not an STM32H5.
```

The firmware side is guarded too — [src/bootloader.c](src/bootloader.c) has `#error` for any part that isn't an H5, so a wrong-target build fails to compile instead of producing a broken binary.

## Source layout

- [src/bootloader.c](src/bootloader.c) — magic word, reset-reason state machine, jump to ROM
- [src/usb_cdc.c](src/usb_cdc.c) — bare-metal USB CDC-ACM on `USB_DRD_FS` (no HAL, no ST middleware)
- [flash_dfu.sh](flash_dfu.sh) — host side (same interface as the F405 boards' script)
- [tools/dfu-flash.py](tools/dfu-flash.py) — Python alternative

## Two dfu-util messages that are NOT failures

Both appear on every successful flash:

```
dfu-util: Invalid DFU suffix signature
dfu-util: Error during download get_status
```

- **`Invalid DFU suffix signature`** — raw `.bin` files carry no DFU suffix; only `.dfu` files do. Purely informational.
- **`Error during download get_status`** — caused by `:leave`. The board resets and jumps into the app the instant the download finishes, so it is already gone when dfu-util sends its final status poll. dfu-util reports an error and **exits non-zero even though the flash succeeded**.

`flash_dfu.sh` handles this: it judges success by `File downloaded successfully` **and** by confirming the board re-enumerates as `0483:5740`, rather than trusting dfu-util's exit code. If you run `dfu-util` by hand, ignore the `get_status` error and check that the board comes back on its own.

## Four details that will cost you a day if you get them wrong

1. **`USBSEL` for HSI48 is `0b11` — both bits set — not `0b00`.** ST's `RCC_USBCLKSOURCE_HSI48` is the *full* `RCC_CCIPR4_USBSEL` mask. Clearing the field instead selects no USB kernel clock, and the failure is brutal to diagnose: the D+ pull-up runs off the APB clock, so the host **does** detect the device and tries to enumerate, but the USB core has no clock and answers nothing. `dmesg` shows

   ```
   usb 1-1: new full-speed USB device number 38 using xhci_hcd
   usb 1-1: Device not responding to setup address.
   usb 1-1: device not accepting address 38, error -71
   ```

   which reads exactly like a bad cable, a bad solder joint, or missing CC resistors. It isn't — it's one wrong bit pattern in a clock mux. **If you see `error -71` *after* the device is detected, suspect the kernel clock, not the wiring.**

2. **The ROM bootloader entry point is `0x0BF97000`, not `0x0BF80000`.** The latter is the system-flash *region base*; reading the initial SP and reset vector from it gives garbage, so the software jump silently fails while the BOOT0 pin still works (the ROM handles that path itself). This is the most common STM32H5 DFU-jump bug.

3. **VDDUSB is isolated at reset on the H5.** `PWR->USBSCR` `USB33DEN` and `USB33SV` must both be set *before* the USB peripheral is clocked. Skip it and the host sees the pull-up but enumeration dies with *"Device Descriptor Request Failed"* — which again looks like a wiring fault rather than a power-domain one.

4. **HSI48 needs CRS.** Raw HSI48 is trimmed to roughly ±1%; full-speed USB requires ±0.25%. Without CRS disciplining it against host SOF packets, enumeration is flaky and intermittent rather than cleanly broken — the worst failure mode to debug.

All four are handled in the code. Points 2 and 3 come from Betaflight's `src/platform/STM32/`, which documents having hit both.

## If it doesn't work

| Symptom | Likely cause |
|---|---|
| Device detected, then `error -71` | USB kernel clock — see gotcha 1 above, **not** a cable fault |
| No `/dev/ttyACM*` at all | Power-only USB-C cable (very common), or firmware without USB support running |
| `permission denied` on the port | `sudo usermod -aG dialout $USER`, then log out and back in |
| Board reboots but no `0483:df11` | Jump failed — confirm `src/bootloader.c` is in the build |
| `dfu-util` permission error | Install the udev rule the script prints on failure |

Diagnose with `sudo dmesg -w` while plugging the cable in. Whether the host prints *anything* separates a firmware bug from an electrical one.

**You cannot lock yourself out.** A bad USB flash is always recoverable with ST-Link, because the ROM bootloader lives in mask ROM and cannot be erased.

---

# Flashing with ST-Link (first flash and recovery)

## 1. You need OpenOCD 0.12.0+ from ST's fork

**This is the part that bites you.** STM32H5 support does not exist in any general OpenOCD build:

| Source | Version | STM32H5? |
|---|---|---|
| Ubuntu 22.04 `apt install openocd` | 0.11.0 | No |
| xPack openocd 0.12.0-6 / -7 | 0.12.0 | **No** — ships `stm32h7x.cfg` only |
| [STMicroelectronics/OpenOCD](https://github.com/STMicroelectronics/OpenOCD) | 0.12.0+dev | Yes |

H5 support postdates the upstream 0.12.0 release, so "0.12.0" alone is not sufficient — it must be **ST's fork**, which is the only source of `target/stm32h5x.cfg` and the `stm32h5x` flash driver. ST publishes no prebuilt binaries, so you build it.

### Version check

```bash
openocd --version
```

You need to see the ST fork URL — the commit hash and date will differ from this:

```
Open On-Chip Debugger 0.12.0+dev-g49ef1d0 (2026-07-23-16:32) [https://github.com/STMicroelectronics/OpenOCD]
Licensed under GNU GPL v2
```

If you see `0.11.0`, or `0.12.0` without the ST URL, flashing will fail with an unknown-target error. Build it:

### Building ST's OpenOCD

```bash
sudo apt install -y autoconf automake libtool texinfo pkg-config \
                    libusb-1.0-0-dev libhidapi-dev libftdi1-dev

git clone --depth 1 https://github.com/STMicroelectronics/OpenOCD.git
cd OpenOCD
./bootstrap                     # pulls jimtcl + libjaylink submodules
./configure --prefix=$HOME/.local --enable-stlink --disable-werror
make -j$(nproc)
make install
```

`pkg-config` is required for `./bootstrap` — without it, `aclocal` fails immediately with `Macro PKG_PROG_PKG_CONFIG is not available`.

### Put it on your PATH — don't skip this

`make install` only copies the binary to `~/.local/bin/openocd`. It does **not** put `~/.local/bin` on your `PATH`, and the distro's `/usr/bin/openocd` (wrong version, no STM32H5 support) will keep shadowing it until you do that yourself. This step is easy to think you've done when you haven't — verify it explicitly:

```bash
which openocd
```

If this prints `/usr/bin/openocd` instead of `~/.local/bin/openocd`, your PATH isn't set up yet:

1. Add this line to `~/.bashrc`:
   ```bash
   export PATH="$HOME/.local/bin:$PATH"
   ```
2. Load it into your **current, interactive** shell — either open a new terminal, or run `source ~/.bashrc`. (Editing `~/.bashrc` alone changes nothing until it's re-sourced or a new shell starts; also note `~/.bashrc` only runs for *interactive* shells, so `source`-ing it from a script or non-interactive context won't take effect either.)
3. Confirm again with `which openocd`, then check the version above.

## 2. Flash it

From the project root:

```bash
openocd -f openocd.cfg \
  -c "init" -c "halt" \
  -c "flash write_image erase bin/main.elf" \
  -c "verify_image bin/main.elf" \
  -c "reset run" -c "exit"
```

Or from `build/`, adding `../` to both paths:

```bash
openocd -f ../openocd.cfg \
  -c "init" -c "halt" \
  -c "flash write_image erase ../bin/main.elf" \
  -c "verify_image ../bin/main.elf" \
  -c "reset run" -c "exit"
```

**NOTE:** whether the program starts running immediately on flash is controlled by [openocd.cfg](openocd.cfg) — read the comments in that file to change the reset behaviour. By default it runs the program on flashing.

### A successful flash looks like this

```
Info : device idcode = 0x10076484 (STM32H56/H57xx - Rev X : 0x1007)
Info : TZEN = 0xC3 : TrustZone disabled by option bytes
Info : Product State = 0xED : 'Open'
Info : flash size = 1024kbytes
Info : flash mode : dual-bank
wrote 1408 bytes from file bin/main.elf in 0.203373s (6.761 KiB/s)
verified 1400 bytes in 0.083132s (16.446 KiB/s)
```

### If the flash command produces no useful output, or errors on `stm32h5x.cfg`

This almost always means `openocd` is still resolving to the wrong binary (`/usr/bin/openocd`, the distro build) instead of your `~/.local/bin/openocd` fork build — usually because the PATH step above was skipped, or was only applied in a `~/.bashrc` edit that was never sourced into your actual terminal. Symptoms include:

- The command exits with `Error: Can't find target/stm32h5x.cfg` (the distro build doesn't ship this file).
- The command appears to print nothing useful, or exits immediately.

Check which binary is actually running and what version it is:

```bash
which openocd
openocd --version
```

If `which` doesn't point at `~/.local/bin/openocd`, or the version output doesn't show the ST fork URL, go back and fix your `PATH` — that is the fix, not a hardware or build problem.

### Harmless output you can ignore

Every run prints some noise from ST's own `stm32h5x.cfg`. **These are not failures:**

- `Error executing event examine-end on target ...` from `stm32h5x_mmw` / `stm32h5x_mrw` — followed by `Examination succeed` anyway.
- `DEPRECATED! use 'read_memory' not 'mem2array'`
- `Warn : target was in unknown state when halt was requested`
- `Warn : Adding extra erase range ...` and `Padding image section 0 ...` — normal flash-alignment behaviour.

Judge success by the `wrote ... bytes` and `verified ... bytes` lines, not by the absence of warnings.

---

## Hardware notes

- MCU: STM32H562RGT6, Cortex-M33, 1 MB flash (dual-bank) @ `0x08000000`, 640 KB RAM @ `0x20000000`. See [link/STM32H562RGTX_FLASH.ld](link/STM32H562RGTX_FLASH.ld); a RAM-only link script is also provided.
- USB device port: **PA11 = `USB_DM`, PA12 = `USB_DP`**, alternate function 10.
- A USB-C *device* needs **5.1 kΩ from CC1→GND and CC2→GND** (two separate resistors). Without them a USB-C host never supplies VBUS and never looks at the port — a completely silent failure.
- VDDUSB is a separate supply pin and must be powered; the internal isolation switch is not a substitute.
- Probe: ST-Link V2 (`0483:3748`). Verify with `lsusb | grep 0483`.
- No `sudo` or udev rules were needed on this machine — the USB nodes came up world-accessible. If you get a permissions error, install ST's udev rules or add yourself to `plugdev`.
