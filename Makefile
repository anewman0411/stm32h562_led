# Convenience wrapper so a fresh clone can just run:
#
#     make            # configure + build   -> bin/main.{elf,bin,hex}
#     make flash      # flash over ST-Link  (first flash / recovery)
#     make flash-dfu  # flash over USB-C    (no ST-Link, no BOOT0 button)
#     make clean
#
# The real build is CMake (see CMakeLists.txt); this just drives it from the
# repo root so you don't have to cd into build/ and remember the commands.

BUILD := build

.PHONY: all build flash flash-dfu clean

all: build

build:
	@cmake -S . -B $(BUILD) >/dev/null
	@cmake --build $(BUILD)

# First flash and recovery: needs an ST-Link and the ST fork of OpenOCD.
# See "Flashing with ST-Link" in README.md.
flash: build
	openocd -f openocd.cfg \
	  -c "init" -c "halt" \
	  -c "flash write_image erase bin/main.elf" \
	  -c "verify_image bin/main.elf" \
	  -c "reset run" -c "exit"

# USB-C only. This requires firmware built from THIS tree to already be running
# on the board (it works by asking that firmware to reboot into the ROM
# bootloader). On a blank chip it cannot work yet -- run `make flash` once over
# ST-Link first, then `make flash-dfu` for every flash after that.
flash-dfu: build
	./flash_dfu.sh

clean:
	rm -rf $(BUILD)
