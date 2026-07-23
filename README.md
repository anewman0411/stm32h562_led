# STM32H562RGT6 — LED blink

Flashed over ST-Link V2 using OpenOCD via `dapdirect_swd` (see [openocd.cfg](openocd.cfg)).

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
sudo apt install -y autoconf automake libtool texinfo \
                    libusb-1.0-0-dev libhidapi-dev libftdi1-dev

git clone --depth 1 https://github.com/STMicroelectronics/OpenOCD.git
cd OpenOCD
./bootstrap                     # pulls jimtcl + libjaylink submodules
./configure --prefix=$HOME/.local --enable-stlink --disable-werror
make -j$(nproc)
make install
```

Installing to `$HOME/.local` leaves the distro's `/usr/bin/openocd` in place as a fallback. Make sure `~/.local/bin` comes **before** `/usr/bin` on your `PATH`:

```bash
echo $PATH | tr ':' '\n' | grep -nE '^(/usr/bin|'"$HOME"'/.local/bin)$'
```

If it doesn't, add `export PATH="$HOME/.local/bin:$PATH"` to your `~/.bashrc`.

## 2. Build the project

```bash
mkdir -p build
cd build
cmake ..
make
```

Artifacts land in [bin/](bin/) — `main.elf`, `main.bin`, `main.hex`.

## 3. Flash it

From the `build/` directory:

```bash
openocd -f ../openocd.cfg \
  -c "init" -c "halt" \
  -c "flash write_image erase ../bin/main.elf" \
  -c "verify_image ../bin/main.elf" \
  -c "reset run" -c "exit"
```

Or from the project root, drop the `../`:

```bash
openocd -f openocd.cfg \
  -c "init" -c "halt" \
  -c "flash write_image erase bin/main.elf" \
  -c "verify_image bin/main.elf" \
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
wrote 1408 bytes from file ../bin/main.elf in 0.203373s (6.761 KiB/s)
verified 1400 bytes in 0.083132s (16.446 KiB/s)
```

### Harmless output you can ignore

Every run prints some noise from ST's own `stm32h5x.cfg`. **These are not failures:**

- `Error executing event examine-end on target ...` from `stm32h5x_mmw` / `stm32h5x_mrw` — followed by `Examination succeed` anyway.
- `DEPRECATED! use 'read_memory' not 'mem2array'`
- `Warn : target was in unknown state when halt was requested`
- `Warn : Adding extra erase range ...` and `Padding image section 0 ...` — normal flash-alignment behaviour.

Judge success by the `wrote ... bytes` and `verified ... bytes` lines, not by the absence of warnings.

## Hardware notes

- Probe: ST-Link V2 (`0483:3748`). Verify with `lsusb | grep 0483`.
- No `sudo` or udev rules were needed on this machine — the USB node came up world-accessible. If you get a permissions error, install ST's udev rules or add yourself to `plugdev`.
- Memory layout is in [link/STM32H562RGTX_FLASH.ld](link/STM32H562RGTX_FLASH.ld): 1024K flash @ `0x08000000`, 640K RAM @ `0x20000000`. A RAM-only link script is also provided.
