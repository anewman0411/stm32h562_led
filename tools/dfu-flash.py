#!/usr/bin/env python3
"""
Flash the STM32H562 over USB-C alone -- no ST-Link, no BOOT0 button.

    tools/dfu-flash.py [firmware.bin]

How it works:

  1. The running firmware exposes a USB CDC virtual COM port (0483:5740).
     Opening it at 1200 baud and then dropping DTR is the agreed "reboot to
     bootloader" signal (the Arduino/Betaflight convention).
  2. The firmware stores a magic word in a TAMP backup register and resets.
     Backup registers survive a reset, so the very next boot sees the magic
     and jumps to the STM32H5 ROM bootloader instead of running the app.
  3. The ROM bootloader enumerates as DFU (0483:df11) on the same USB-C port,
     and dfu-util writes the new image and leaves.

If the board is already sitting in DFU mode, step 1 and 2 are skipped, so this
still works when the app is missing, wedged, or was never flashed with USB
support -- provided something can get it into DFU (BOOT0 or a prior run).
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

VCP_VID, VCP_PID = "0483", "5740"   # app: CDC virtual COM port
DFU_VID, DFU_PID = "0483", "df11"   # ROM bootloader: DFU
FLASH_ORIGIN = "0x08000000"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def log(msg):
    print(f"  {msg}", flush=True)


def usb_device_present(vid, pid):
    """True if a USB device with this vid:pid is on the bus."""
    for path in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        base = os.path.dirname(path)
        try:
            with open(path) as f:
                if f.read().strip().lower() != vid:
                    continue
            with open(os.path.join(base, "idProduct")) as f:
                if f.read().strip().lower() == pid:
                    return True
        except OSError:
            continue
    return False


def find_vcp_port():
    """Locate the /dev/ttyACM* backed by our CDC device, by VID:PID."""
    for tty in sorted(glob.glob("/dev/ttyACM*")):
        name = os.path.basename(tty)
        # /sys/class/tty/ttyACM0/device is the interface; its parent is the device
        base = f"/sys/class/tty/{name}/device/.."
        try:
            with open(os.path.join(base, "idVendor")) as f:
                vid = f.read().strip().lower()
            with open(os.path.join(base, "idProduct")) as f:
                pid = f.read().strip().lower()
        except OSError:
            continue
        if (vid, pid) == (VCP_VID, VCP_PID):
            return tty
    return None


def touch_1200_baud(port):
    """Open at 1200 baud and drop DTR -- the reboot-to-bootloader signal."""
    try:
        import serial  # pyserial
    except ImportError:
        # stty fallback: `hupcl` lowers DTR when the port is closed.
        log(f"pyserial not found, using stty on {port}")
        subprocess.run(["stty", "-F", port, "1200", "hupcl"], check=True)
        with open(port, "rb"):
            pass
        return

    log(f"1200-baud touch on {port}")
    s = serial.Serial(port, 1200)
    try:
        s.dtr = False       # this is the actual trigger
        time.sleep(0.05)
    finally:
        s.close()


def wait_for(vid, pid, what, timeout):
    log(f"waiting for {what} ({vid}:{pid}) ...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if usb_device_present(vid, pid):
            log(f"{what} appeared")
            return True
        time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser(description="Flash STM32H562 over USB-C via DFU")
    ap.add_argument("firmware", nargs="?",
                    default=os.path.join(REPO_ROOT, "bin", "main.bin"),
                    help="raw .bin to flash (default: bin/main.bin)")
    ap.add_argument("--port", help="override the CDC port (e.g. /dev/ttyACM0)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for the DFU device (default: 10)")
    args = ap.parse_args()

    if not os.path.isfile(args.firmware):
        sys.exit(f"error: firmware not found: {args.firmware}\n"
                 f"       build it first:  cd build && cmake .. && make")

    if shutil.which("dfu-util") is None:
        sys.exit("error: dfu-util not found. Install it:  sudo apt install dfu-util")

    print(f"Flashing {args.firmware}")

    # --- Get the board into DFU mode -------------------------------------
    if usb_device_present(DFU_VID, DFU_PID):
        log("board is already in DFU mode, skipping reboot")
    else:
        port = args.port or find_vcp_port()
        if port is None:
            sys.exit(
                "error: no STM32H562 CDC port (0483:5740) and no DFU device found.\n"
                "       - Is the USB-C cable connected (and a data cable, not power-only)?\n"
                "       - Is firmware with USB CDC support actually running?\n"
                "       If the board has never had USB firmware, flash it once over\n"
                "       ST-Link (see README) or hold BOOT0 to reach DFU manually."
            )

        try:
            touch_1200_baud(port)
        except PermissionError:
            sys.exit(f"error: permission denied on {port}.\n"
                     f"       Add yourself to the dialout group:\n"
                     f"         sudo usermod -aG dialout $USER   (then log out and back in)")

        if not wait_for(DFU_VID, DFU_PID, "DFU bootloader", args.timeout):
            sys.exit(
                "error: board never entered DFU mode.\n"
                "       The 1200-baud touch was sent but no 0483:df11 device appeared.\n"
                "       Check that the running firmware includes src/bootloader.c."
            )

    # --- Write it --------------------------------------------------------
    cmd = [
        "dfu-util",
        "-a", "0",
        "-s", f"{FLASH_ORIGIN}:leave",
        "-D", args.firmware,
    ]
    log(" ".join(cmd))
    result = subprocess.run(cmd)

    if result.returncode != 0:
        print("\ndfu-util failed. If this is a permissions error, install udev rules:\n"
              "  echo 'SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0483\", ATTR{idProduct}==\"df11\", MODE=\"0666\"' \\\n"
              "    | sudo tee /etc/udev/rules.d/50-stm32-dfu.rules\n"
              "  sudo udevadm control --reload-rules && sudo udevadm trigger",
              file=sys.stderr)
        sys.exit(result.returncode)

    print("\nDone. The board should be running the new firmware.")


if __name__ == "__main__":
    main()
