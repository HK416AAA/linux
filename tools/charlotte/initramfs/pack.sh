#!/bin/sh
# Pack tools/charlotte/initramfs into a cpio.gz that can be flashed into the
# vendor ramdisk partition.  The kernel no longer carries this image, so this
# is how the rescue shell is delivered.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out/charlotte-rescue.cpio.gz}
stage=$(mktemp -d /tmp/charlotte-rescue.XXXXXX)

cp -a "$HERE/rootfs/." "$stage/"
mkdir -p "$stage/dev"
mknod -m 600 "$stage/dev/console" c 5 1
mknod -m 666 "$stage/dev/null" c 1 3
mknod -m 666 "$stage/dev/zero" c 1 5

mkdir -p "$(dirname "$OUT")"
(cd "$stage" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9) > "$OUT"
ls -l "$OUT"
sha256sum "$OUT"
