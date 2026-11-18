#!/bin/bash
#
# Wrap the raw UEFI payload (boot shim + firmware FD) into an Android boot
# image that can be flashed to the boot partition.
#
set -e

cd "$(dirname "$0")"

PAYLOAD="Mu-mro.bin"
IMAGE="Mu-mro-boot.img"

if [ ! -f "${PAYLOAD}" ]; then
  echo "missing ${PAYLOAD}, run build.sh first"
  exit 1
fi

# Keep an Image named copy of the flat payload for direct use
cp "${PAYLOAD}" Image

python3 ../tools/charlotte/mkbootimg.py --kernel Image --out "${IMAGE}"
