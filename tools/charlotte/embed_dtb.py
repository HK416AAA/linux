#!/usr/bin/env python3
"""Append an arm64 device tree blob to a kernel Image at __pa_symbol(_end).

charlotte's arch/arm64/kernel/setup.c looks for a fallback FDT at
__pa_symbol(_end) when the bootloader hands over an unusable tree.  _end is
image_size bytes after _text, while the Image file ends at the start of BSS.
The file therefore has to be zero padded up to the image_size field of the
arm64 Image header, and the DTB appended afterwards.

The boot image repacker must use the resulting file as the complete kernel
payload (kernel_size must cover the padded Image plus the DTB).
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ARM64_IMAGE_MAGIC = 0x644D5241  # "ARM\x64"
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
IMAGE_HEADER_SIZE = 0x40
IMAGE_IMAGE_SIZE_OFFSET = 0x10
IMAGE_TEXT_OFFSET_OFFSET = 0x08
IMAGE_MAGIC_OFFSET = 0x38
FDT_TOTALSIZE_OFFSET = 0x04


def fail(message):
    print("error: " + message, file=sys.stderr)
    raise SystemExit(1)


def load_image(path):
    data = Path(path).read_bytes()
    if len(data) < IMAGE_HEADER_SIZE:
        fail("%s: shorter than the arm64 Image header" % path)
    magic = struct.unpack_from("<I", data, IMAGE_MAGIC_OFFSET)[0]
    if magic != ARM64_IMAGE_MAGIC:
        fail("%s: bad arm64 Image magic 0x%08x (need 0x%08x)"
             % (path, magic, ARM64_IMAGE_MAGIC))
    text_offset = struct.unpack_from("<Q", data, IMAGE_TEXT_OFFSET_OFFSET)[0]
    image_size = struct.unpack_from("<Q", data, IMAGE_IMAGE_SIZE_OFFSET)[0]
    if image_size == 0:
        fail("%s: image_size is zero" % path)
    if image_size < len(data):
        fail("%s: file is already %d bytes, beyond image_size %d; "
             "input appears to contain appended data"
             % (path, len(data), image_size))
    if image_size % 8:
        fail("%s: image_size %d is not 8-byte aligned; the DTB start would "
             "not match __pa_symbol(_end)" % (path, image_size))
    return data, text_offset, image_size


def load_dtb(path):
    data = Path(path).read_bytes()
    if len(data) < 8 or data[:4] != FDT_MAGIC:
        fail("%s: not a flattened device tree" % path)
    totalsize = struct.unpack_from(">I", data, FDT_TOTALSIZE_OFFSET)[0]
    if totalsize != len(data):
        fail("%s: header totalsize %d does not match file size %d"
             % (path, totalsize, len(data)))
    return data


def main():
    parser = argparse.ArgumentParser(
        description="Pad an arm64 Image to image_size and append a DTB at "
                    "__pa_symbol(_end).")
    parser.add_argument("--image", required=True, help="kernel Image")
    parser.add_argument("--dtb", required=True, help="device tree blob")
    parser.add_argument("--out", required=True, help="output Image with DTB")
    args = parser.parse_args()

    image, text_offset, image_size = load_image(args.image)
    dtb = load_dtb(args.dtb)

    pad = image_size - len(image)
    blob = bytearray()
    blob += image
    blob += b"\x00" * pad
    dtb_offset = len(blob)
    blob += dtb

    if bytes(blob[dtb_offset:dtb_offset + 4]) != FDT_MAGIC:
        fail("internal error: DTB magic missing at offset %d" % dtb_offset)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)

    digest = hashlib.sha256(blob).hexdigest()
    print("image:          %s" % args.image)
    print("dtb:            %s" % args.dtb)
    print("text_offset:    0x%x" % text_offset)
    print("image_file_size:%d" % len(image))
    print("image_size:     %d" % image_size)
    print("bss_pad:        %d" % pad)
    print("dtb_offset:     %d (0x%x, __pa_symbol(_end))"
          % (dtb_offset, dtb_offset))
    print("dtb_size:       %d" % len(dtb))
    print("output:         %s" % out)
    print("output_size:    %d" % len(blob))
    print("sha256:         %s" % digest)


if __name__ == "__main__":
    main()
