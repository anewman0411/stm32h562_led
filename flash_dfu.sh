#!/usr/bin/env bash
#
# Buttonless flash of the STM32H562 over USB DFU, single cable.
#
# The running firmware exposes a USB CDC virtual COM port (/dev/ttyACM*). This
# script asks it to reboot into the STM32H5 ROM DFU bootloader, then flashes
# bin/main.bin with dfu-util over the SAME USB cable. No BOOT0/RESET press.
#
# Trigger methods (both attempted):
#   - data token      : writes "DFU!" to the CDC port
#   - 1200-baud touch : opens the CDC port at 1200 baud and drops DTR
#
# Requirements: dfu-util.  Wiring: board USB (PA11 = D-, PA12 = D+) -> host.
#
# ---------------------------------------------------------------------------
# THIS SCRIPT IS FOR STM32H5 (H562/H563/H573) ONLY -- NOT F4.
#
# It is deliberately not interchangeable with the F405 version:
#
#   * The H5 ROM bootloader entry point is 0x0BF97000, not the F4's 0x1FFF0000.
#     That address lives in the firmware (src/bootloader.c), not here, but it
#     is why an F4 build will never reach DFU on this part.
#   * The H5 exposes 1 MB of flash in a DUAL-BANK layout, so the DFU alt
#     setting and sector map differ from the F405's 1 MB single bank.
#   * The device is verified below to actually be an H5 before anything is
#     written, so pointing this at an F4 board fails loudly instead of
#     flashing an H5 image onto it.
# ---------------------------------------------------------------------------
#
# Usage:
#   ./flash_dfu.sh [serial_port] [firmware.bin]
# Examples:
#   ./flash_dfu.sh                          # auto-detect /dev/ttyACM*, bin/main.bin
#   ./flash_dfu.sh /dev/ttyACM0
#   ./flash_dfu.sh /dev/ttyACM0 bin/main.bin

set -euo pipefail

cd "$(dirname "$0")"

FW="${2:-bin/main.bin}"
DFU_ID="0483:df11"        # STM32 system bootloader DFU VID:PID
CDC_ID="0483:5740"        # this firmware's CDC virtual COM port
FLASH_ADDR="0x08000000"   # app start (alt 0 = Internal Flash)

# H5 signature in the DFU descriptor string. The ROM advertises its part
# family here, which is what lets us refuse an F4 board.
H5_MATCH="@Internal Flash"

command -v dfu-util >/dev/null 2>&1 || {
    echo "error: dfu-util not found. Install it:  sudo apt install dfu-util"; exit 1; }

[ -f "$FW" ] || { echo "error: firmware '$FW' not found (build it first: cd build && make)"; exit 1; }

# Pick the CDC port: explicit arg, else prefer one that really is our 0483:5740,
# else fall back to the first /dev/ttyACM*.
PORT="${1:-}"
if [ -z "$PORT" ]; then
    for candidate in /dev/ttyACM*; do
        [ -e "$candidate" ] || continue
        sysdev="/sys/class/tty/$(basename "$candidate")/device/.."
        vid=$(cat "$sysdev/idVendor" 2>/dev/null || true)
        pid=$(cat "$sysdev/idProduct" 2>/dev/null || true)
        if [ "$vid:$pid" = "$CDC_ID" ]; then PORT="$candidate"; break; fi
    done
    if [ -z "$PORT" ]; then
        PORT="$(ls /dev/ttyACM* 2>/dev/null | head -n1 || true)"
    fi
fi

if dfu-util -l 2>/dev/null | grep -q "$DFU_ID"; then
    echo ">> Board is already in DFU mode — skipping reboot request"
elif [ -n "$PORT" ] && [ -e "$PORT" ]; then
    echo ">> Found CDC port $PORT — requesting reboot into DFU..."
    # Method 1: data token.
    stty -F "$PORT" 115200 raw -echo 2>/dev/null || true
    printf 'DFU!' > "$PORT" 2>/dev/null || true
    sleep 0.3
    # Method 2: 1200-baud touch (hupcl drops DTR on close, which is the trigger).
    stty -F "$PORT" 1200 hupcl 2>/dev/null || true
    (exec 3<>"$PORT") 2>/dev/null || true
    sleep 1.5   # allow reset + re-enumeration as the ROM bootloader
else
    echo ">> No CDC port found; assuming board is already in DFU mode"
    echo "   (or enter DFU manually: hold BOOT0, tap RESET)."
fi

echo ">> Waiting for STM32 DFU device ($DFU_ID)..."
found=0
for _ in $(seq 1 20); do
    if dfu-util -l 2>/dev/null | grep -q "$DFU_ID"; then found=1; break; fi
    sleep 0.5
done

if [ "$found" -ne 1 ]; then
    echo "error: no DFU device ($DFU_ID) appeared." >&2
    echo "       - Is firmware with USB CDC support actually running?" >&2
    echo "       - Check 'sudo dmesg -w' while plugging in." >&2
    echo "       - Recovery is always ST-Link (see README section 3)." >&2
    exit 1
fi

# --- Refuse to flash anything that is not an H5 ----------------------------
# The F405 and H562 both present 0483:df11, so VID:PID alone cannot tell them
# apart. The alt-0 descriptor string carries the flash layout, and the H5's
# 1 MB dual-bank map differs from the F405's. Bail out rather than write an
# H5 image into an F4.
DFU_LIST="$(dfu-util -l 2>/dev/null || true)"
if ! grep -q "$H5_MATCH" <<<"$DFU_LIST"; then
    echo "error: DFU device found but its flash descriptor was not readable." >&2
    echo "$DFU_LIST" >&2
    exit 1
fi

# STM32F4 ROM reports 12 sectors of 16/64/128 KB; STM32H5 reports a uniform
# 8 KB sector map across 1 MB. Detect the F4 pattern explicitly and refuse.
if grep -qE '16\*016Kg|64\*128Kg|_16Kg' <<<"$DFU_LIST"; then
    echo "error: this looks like an STM32F4 DFU device, not an STM32H5." >&2
    echo "       This script is strictly for STM32H5 (H562/H563/H573)." >&2
    echo "       Use the F405 project's own flash_dfu.sh for that board." >&2
    echo "$DFU_LIST" >&2
    exit 1
fi

echo ">> Confirmed STM32H5-style DFU target"
echo ">> Flashing $FW -> $FLASH_ADDR"

# :leave  => flash then jump to the app, so the board runs immediately.
#
# dfu-util almost always exits non-zero here with:
#     dfu-util: Error during download get_status
# That is NOT a flash failure. ":leave" makes the board reset and jump to the
# app the instant the download completes, so it is already gone when dfu-util
# sends its final GET_STATUS poll. The write itself has succeeded by then.
# ("Invalid DFU suffix signature" is likewise expected -- raw .bin files carry
# no DFU suffix.)
#
# So judge success by what actually happened, not by the exit code.
set +e
DFU_OUT="$(dfu-util -a 0 -d "$DFU_ID" -s "${FLASH_ADDR}:leave" -D "$FW" 2>&1)"
DFU_RC=$?
set -e

echo "$DFU_OUT"

if ! grep -q "File downloaded successfully" <<<"$DFU_OUT"; then
    echo >&2
    echo "error: download did not complete (dfu-util exit $DFU_RC)." >&2
    exit 1
fi

# The download reported success. Confirm the board actually came back on its
# own, which is only possible if the new firmware is genuinely running.
echo ">> Download OK — waiting for the board to re-enumerate..."
for _ in $(seq 1 20); do
    if lsusb 2>/dev/null | grep -qi "$CDC_ID"; then
        echo ">> Board is back as $CDC_ID — new firmware is running."
        echo ">> Done."
        exit 0
    fi
    sleep 0.5
done

echo >&2
echo "warning: flash completed, but the board did not re-appear as $CDC_ID." >&2
echo "         The write succeeded; the new firmware may not be enumerating." >&2
echo "         Check with: sudo dmesg -w" >&2
exit 1
