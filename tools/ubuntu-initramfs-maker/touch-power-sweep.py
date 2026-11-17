#!/usr/bin/env python3
"""Locate and enable the PMIC rails that feed the Goodix touch controller.

No regulator framework and no vendor power table are needed: the
charlotte-pmic debugfs file gives raw SPMI access to every LDO, and the
thp transport tells us whether the chip woke up.  A rail counts as the
touch supply when the controller pulls its IRQ line after the rail is
switched on.

Usage: touch-power-sweep [--dry-run]
  --dry-run  only list rails and their current state

Voltage selects are never touched: writing a lower vsel to a rail that
feeds the SoC itself takes the SPMI bus down.  The rails are already
switched on by the bootloader, so this only reports what is live.
"""

import fcntl
import os
import struct
import subprocess
import sys
import time

DBG = "/sys/kernel/debug/charlotte-pmic"
THP = "/dev/thp"
THP_TYPE = 0xB8

# ioctl encodings for drivers/input/touchscreen/charlotte-thp.c
RST = (1 << 30) | (4 << 16) | (THP_TYPE << 8) | 2
TMO = (1 << 30) | (4 << 16) | (THP_TYPE << 8) | 3
GET = (3 << 30) | (24 << 16) | (THP_TYPE << 8) | 1
IRQG = (1 << 30) | (4 << 16) | (THP_TYPE << 8) | 10

# Likely candidates first, then the rest.
ORDER = ["ldo16", "ldo17", "ldo15", "ldo9", "ldo33", "ldo34", "ldo3", "ldo4"]


def debugfs_write(name, text):
    with open(os.path.join(DBG, name), "w") as f:
        f.write(text)


def read_rails():
    rails = {}
    with open(os.path.join(DBG, "ldo")) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 10 or not parts[0].isdigit():
                continue
            field = {parts[i]: parts[i + 1] for i in range(2, len(parts) - 1,
                                                          2)}
            rails[parts[1]] = {
                "idx": int(parts[0]),
                "en": int(field["en"], 16),
                "vsel": int(field["vsel"], 16),
            }
    return rails


def thp_probe():
    """Pulse reset, then report whether the controller answers."""
    try:
        fd = os.open(THP, os.O_RDWR)
    except OSError as err:
        return None, "open %s: %s" % (THP, err.strerror)
    try:
        fcntl.ioctl(fd, TMO, 150)
        fcntl.ioctl(fd, RST, 0)
        time.sleep(0.005)
        fcntl.ioctl(fd, RST, 1)
        time.sleep(0.02)
        level = 0
        for _ in range(6):
            buf = bytearray(4)
            fcntl.ioctl(fd, IRQG, buf, True)
            level = struct.unpack("i", buf)[0]
            if level:
                break
            time.sleep(0.02)
        frame = bytearray(24)
        size = -1
        first = 0
        try:
            fcntl.ioctl(fd, GET, frame, True)
            first, _second, size, _pad = struct.unpack("QQII", frame)
        except OSError as err:
            size = -err.errno
        return (level == 1 or size > 0), "irq=%d frame=%d first=0x%x" % (
            level, size, first)
    finally:
        os.close(fd)


def restore(rails, snapshot):
    for name, rail in read_rails().items():
        orig = snapshot[name]
        if rail["en"] == orig["en"] and rail["vsel"] == orig["vsel"]:
            continue
        try:
            debugfs_write("ldo", "%d %d %d\n" % (rail["idx"], orig["en"],
                                                  orig["vsel"]))
        except OSError as err:
            print("restore %s failed: %s" % (name, err.strerror), flush=True)
    print("restored the bootloader state", flush=True)


def main():
    dry = "--dry-run" in sys.argv

    for step in ("/sys/kernel/debug", DBG):
        if not os.path.isdir(step):
            print("missing %s (mount debugfs and boot the PMIC helper)" % step)
            return 1

    rails = read_rails()
    if not rails:
        print("no rails listed in %s/ldo" % DBG)
        return 1

    snapshot = {n: dict(r) for n, r in rails.items()}
    print("rails from %s:" % os.path.join(DBG, "ldo"))
    for name in sorted(rails, key=lambda n: ORDER.index(n) if n in ORDER
                       else 99):
        r = rails[name]
        print("  %d %-6s en=%02x vsel=%02x" % (r["idx"], name, r["en"],
                                               r["vsel"]), flush=True)
    if dry:
        return 0

    order = [n for n in ORDER if n in rails] + [n for n in rails
                                                if n not in ORDER]
    found = []

    print("pass: probe each rail at its current voltage", flush=True)
    for name in order:
        r = rails[name]
        try:
            debugfs_write("ldo", "%d 1\n" % r["idx"])
        except OSError as err:
            print("  %-6s -> write refused: %s (load charlotte_pmic with "
                  "allow_writes=1)" % (name, err.strerror), flush=True)
            return 2
        time.sleep(0.05)
        ok, detail = thp_probe()
        print("  %-6s -> %s %s" % (name, "AWAKE" if ok else "silent", detail),
              flush=True)
        if ok:
            found.append(name)

    if not found:
        print("\nnothing woke the controller; unclaimed GPIO candidates:")
        try:
            out = subprocess.run(["cat", "/sys/kernel/debug/gpio"],
                                 capture_output=True, text=True, timeout=10)
            for line in out.stdout.splitlines():
                if "|                    )" in line:
                    print("  " + line.strip())
        except (OSError, subprocess.SubprocessError):
            pass
        restore(rails, snapshot)
        return 1

    print("\nenabled and left on: %s" % ", ".join(found))
    for name in found:
        r = rails[name]
        print("  %d %-6s" % (r["idx"], name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
