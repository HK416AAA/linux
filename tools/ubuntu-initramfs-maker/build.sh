#!/usr/bin/env bash
# Build the Ubuntu arm64 initramfs used by the Charlotte (Kirin 9010) kernel.
#
# The rootfs is bootstrapped with debootstrap under qemu-user-static, so this
# script needs root and runs on an x86_64 Linux host.  It is intended for CI,
# but works the same way on a local workstation.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KERNEL_SRC=${KERNEL_SRC:-$(cd "$HERE/../.." && pwd)}
ARCH=${ARCH:-arm64}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
UBUNTU_SUITE=${UBUNTU_SUITE:-noble}
UBUNTU_MIRROR=${UBUNTU_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
IMAGE_NAME=${IMAGE_NAME:-charlotte-ubuntu-24.04}
OUT=${OUT:-$HERE/out}
WORK=${WORK:-$(mktemp -d "/tmp/${IMAGE_NAME}.XXXXXX")}
ROOTFS=$WORK/rootfs
KERNEL_RELEASE=${KERNEL_RELEASE:-$(make -s -C "$KERNEL_SRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)}

echo "kernel release : $KERNEL_RELEASE"
echo "kernel source  : $KERNEL_SRC"
echo "work directory : $WORK"
mkdir -p "$OUT"

# 1. Bootstrap a minimal Ubuntu arm64 rootfs.
debootstrap --arch=arm64 --foreign --variant=minbase \
	--components=main,universe "$UBUNTU_SUITE" "$ROOTFS" "$UBUNTU_MIRROR"
cp /usr/bin/qemu-aarch64-static "$ROOTFS/usr/bin/"
chroot "$ROOTFS" /debootstrap/debootstrap --second-stage

# 2. Make apt work inside the chroot.
cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
cat > "$ROOTFS/etc/apt/sources.list" <<SOURCES
deb $UBUNTU_MIRROR $UBUNTU_SUITE main universe
deb $UBUNTU_MIRROR $UBUNTU_SUITE-updates main universe
deb $UBUNTU_MIRROR $UBUNTU_SUITE-security main universe
SOURCES

# 3. Install and configure the userspace.  Keep this list to what is useful
#    on a bring-up tablet: shell, SSH, USB networking, and hardware probing.
chroot "$ROOTFS" /usr/bin/env DEBIAN_FRONTEND=noninteractive TZ=UTC /bin/bash -s <<'CHROOT'
set -e
export DEBIAN_FRONTEND=noninteractive TZ=UTC

echo charlotte-mro > /etc/hostname
printf '127.0.0.1\tlocalhost\n127.0.1.1\tcharlotte-mro\n' > /etc/hosts
ln -snf /usr/share/zoneinfo/UTC /etc/localtime
echo UTC > /etc/timezone
echo 'LANG=C.UTF-8' > /etc/default/locale

apt-get update
apt-get install -y --no-install-recommends \
	systemd-sysv systemd-resolved udev dbus kmod \
	openssh-server sudo nano less htop bash-completion \
	iproute2 iputils-ping net-tools ethtool iw rfkill \
	usbutils pciutils i2c-tools gpiod spi-tools \
	ca-certificates curl wget e2fsprogs dosfstools fdisk parted \
	f2fs-tools python3 strace lsof

echo 'root:charlotte' | chpasswd

mkdir -p /etc/systemd/network /etc/ssh/sshd_config.d /run/sshd
mkdir -p /etc/systemd/system/getty@tty1.service.d

cat > /etc/systemd/network/10-usb0.link <<'LINK'
[Match]
OriginalName=usb0

[Link]
Name=usb0
LINK

cat > /etc/systemd/network/10-usb0.network <<'NET'
[Match]
Name=usb0 enx*

[Network]
Address=10.0.0.2/24
DHCPServer=yes
MulticastDNS=yes
LLMNR=yes

[DHCPServer]
PoolOffset=10
PoolSize=10
EmitDNS=yes
NET

cat > /etc/ssh/sshd_config.d/10-charlotte.conf <<'SSHD'
PermitRootLogin yes
PasswordAuthentication yes
SSHD

cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<'GETTY'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I $TERM
GETTY

systemctl enable systemd-networkd.service
systemctl enable systemd-resolved.service
systemctl enable ssh.service
systemctl enable getty@tty1.service
systemctl enable serial-getty@ttyHW0.service
systemctl enable serial-getty@ttyGS0.service
systemctl disable apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true

systemd-machine-id-setup
ssh-keygen -A

# Let systemd-resolved own resolv.conf at runtime.
ln -snf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf

/usr/sbin/sshd -t
for unit in ssh.service getty@tty1.service \
	serial-getty@ttyHW0.service serial-getty@ttyGS0.service \
	systemd-networkd.service systemd-resolved.service; do
	systemctl is-enabled --quiet "$unit"
done
CHROOT

# 4. Install the matching kernel modules and refresh depmod.
make -C "$KERNEL_SRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	INSTALL_MOD_PATH="$ROOTFS" modules_install
depmod -b "$ROOTFS" "$KERNEL_RELEASE"

# 5. Publish the initramfs and a standalone module tree.
tar -C "$ROOTFS/lib/modules" -czf "$OUT/modules-$KERNEL_RELEASE.tar.gz" "$KERNEL_RELEASE"
(cd "$ROOTFS" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9) > "$OUT/$IMAGE_NAME.cpio.gz"

echo "modules : $OUT/modules-$KERNEL_RELEASE.tar.gz"
echo "initramfs: $OUT/$IMAGE_NAME.cpio.gz"
ls -l "$OUT"
sha256sum "$OUT/modules-$KERNEL_RELEASE.tar.gz" "$OUT/$IMAGE_NAME.cpio.gz"
