#!/usr/bin/env python3
"""Wrap a kernel payload and/or a cpio.gz ramdisk into an Android boot image.

The vendor ramdisk partition on this device stores a boot image with an
empty kernel: ANDROID! header, page size 2048, kernel_size 0, ramdisk at
0x11000000.  The bootloader pairs it with the kernel from the kernel
partition, so the ramdisk can be replaced without touching the kernel.

Pass --kernel to embed a flat "Image" style payload (for example the UEFI
boot shim plus firmware) instead of an empty kernel.

The id field is the SHA1 that mkbootimg v0 writes:
sha1(kernel + len(kernel) + ramdisk + len(ramdisk) + second + len(second)).
"""

import argparse
import gzip
import hashlib
import struct
import sys

PAGE_SIZE = 2048
KERNEL_ADDR = 0x10008000
RAMDISK_ADDR = 0x11000000
TAGS_ADDR = 0x10000100
OS_VERSION = 0x1800015A
MAGIC = b"ANDROID!"

HEADER = struct.Struct("<8s10I16s512s32s")


def build_id(kernel, ramdisk, second):
    digest = hashlib.sha1()
    digest.update(kernel)
    digest.update(struct.pack("<I", len(kernel)))
    digest.update(ramdisk)
    digest.update(struct.pack("<I", len(ramdisk)))
    digest.update(second)
    digest.update(struct.pack("<I", len(second)))
    return digest.digest()


def pad(data, page):
    remainder = len(data) % page
    return data if remainder == 0 else data + b"\0" * (page - remainder)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", help="flat kernel or firmware payload")
    parser.add_argument("--ramdisk", help="cpio.gz ramdisk")
    parser.add_argument("--out", required=True, help="boot image output path")
    parser.add_argument("--page-size", type=int, default=PAGE_SIZE)
    parser.add_argument("--kernel-addr", type=lambda v: int(v, 0), default=KERNEL_ADDR)
    parser.add_argument("--ramdisk-addr", type=lambda v: int(v, 0), default=RAMDISK_ADDR)
    parser.add_argument("--tags-addr", type=lambda v: int(v, 0), default=TAGS_ADDR)
    parser.add_argument("--cmdline", default="buildvariant=user")
    parser.add_argument("--os-version", type=lambda v: int(v, 0), default=OS_VERSION)
    args = parser.parse_args()

    kernel = open(args.kernel, "rb").read() if args.kernel else b""
    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""

    if ramdisk:
        gzip.decompress(ramdisk)

    if len(kernel) > 0xFFFFFFFF:
        raise SystemExit("kernel does not fit the 32-bit size field")
    if len(ramdisk) > 0xFFFFFFFF:
        raise SystemExit("ramdisk does not fit the 32-bit size field")

    cmdline = args.cmdline.encode()
    if len(cmdline) > 512:
        raise SystemExit("cmdline longer than 512 bytes")

    digest = build_id(kernel, ramdisk, b"")
    header = HEADER.pack(
        MAGIC,
        len(kernel),
        args.kernel_addr,
        len(ramdisk),
        args.ramdisk_addr,
        0,
        0,
        args.tags_addr,
        args.page_size,
        0,
        args.os_version,
        b"",
        cmdline,
        digest,
    )
    image = pad(header, args.page_size) + pad(kernel, args.page_size) + pad(ramdisk, args.page_size)
    with open(args.out, "wb") as handle:
        handle.write(image)

    fields = HEADER.unpack(image[: HEADER.size])
    if fields[0] != MAGIC or fields[1] != len(kernel) or fields[3] != len(ramdisk):
        raise SystemExit("header does not read back")

    kernel_start = fields[8]
    if image[kernel_start : kernel_start + len(kernel)] != kernel:
        raise SystemExit("kernel payload does not read back")

    ramdisk_start = kernel_start + len(pad(kernel, fields[8]))
    if image[ramdisk_start : ramdisk_start + len(ramdisk)] != ramdisk:
        raise SystemExit("ramdisk payload does not read back")
    if fields[13][:20] != digest:
        raise SystemExit("id field does not read back")

    print("kernel  : {} bytes ({})".format(len(kernel), args.kernel))
    print("ramdisk : {} bytes ({})".format(len(ramdisk), args.ramdisk))
    print("cmdline : {}".format(args.cmdline))
    print("page    : {}".format(args.page_size))
    print("id      : {}".format(digest.hex()))
    print("image   : {} bytes ({})".format(len(image), args.out))
    print("sha256  : {}".format(hashlib.sha256(image).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
