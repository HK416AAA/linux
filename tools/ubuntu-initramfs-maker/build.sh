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

# 3b. USB composite gadget: ACM console (ttyGS0) + ECM network (usb0).
#     The kernel has USB_CONFIGFS_* built in, but userspace must create the
#     configfs hierarchy and bind a UDC before ttyGS0/usb0 exist.  This is
#     the only usable console/network path when no UART is wired.
cat > /usr/local/sbin/usb-gadget-setup <<'GADGET'
#!/bin/sh
# Bind a composite USB gadget: ACM console (ttyGS0) + ECM net (usb0).
set -eu

GADGET=/sys/kernel/config/usb_gadget/charlotte
CFG="$GADGET/configs/c.1"
STR="$GADGET/strings/0x409"

modprobe libcomposite 2>/dev/null || true
modprobe usb_f_acm 2>/dev/null || true
modprobe usb_f_ecm 2>/dev/null || true

if [ ! -d "$GADGET" ]; then
	mkdir -p "$GADGET"
	echo 0x18d1 > "$GADGET/idVendor"
	echo 0x4ee7 > "$GADGET/idProduct"
	echo 0x0100 > "$GADGET/bcdDevice"
	echo 0x0200 > "$GADGET/bcdUSB"

	mkdir -p "$STR"
	echo "charlotte-mro" > "$STR/serialnumber"
	echo "MonkeyCode" > "$STR/manufacturer"
	echo "Kirin 9010 Ubuntu" > "$STR/product"

	mkdir -p "$GADGET/functions/acm.gs0"
	mkdir -p "$GADGET/functions/ecm.usb0"
	echo "02:00:ca:00:00:02" > "$GADGET/functions/ecm.usb0/dev_addr" 2>/dev/null || true
	echo "02:00:ca:00:00:01" > "$GADGET/functions/ecm.usb0/host_addr" 2>/dev/null || true

	mkdir -p "$CFG/strings/0x409"
	echo "acm+ecm" > "$CFG/strings/0x409/configuration"
	echo 500 > "$CFG/MaxPower"
	ln -sf "$GADGET/functions/acm.gs0" "$CFG/acm.gs0"
	ln -sf "$GADGET/functions/ecm.usb0" "$CFG/ecm.usb0"
fi

# DWC3 may probe after systemd reaches multi-user; wait for a UDC.
i=0
while [ "$i" -lt 30 ] && ! ls /sys/class/udc/* >/dev/null 2>&1; do
	i=$((i + 1))
	sleep 1
done

UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1 || true)
if [ -z "$UDC" ]; then
	echo "usb-gadget: no UDC present, dwc3 has not probed" >&2
	exit 1
fi
echo "usb-gadget: binding UDC $UDC"
echo "$UDC" > "$GADGET/UDC"
GADGET

chmod 0755 /usr/local/sbin/usb-gadget-setup

cat > /etc/systemd/system/usb-gadget.service <<'UNIT'
[Unit]
Description=USB composite gadget (ACM console + ECM network)
After=sys-kernel-config.mount
Wants=sys-kernel-config.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/usb-gadget-setup
TimeoutStartSec=45

[Install]
WantedBy=multi-user.target
UNIT

# 3c. Persistent logging for the UART/kernel console.
#     ttyHW0 is the debug UART (uart6 @0xffb82000, console=ttyHW0).  The
#     kernel mirrors every printk into the ramoops console zone and keeps a
#     64 KiB snapshot of the previous boot in /sys/kernel/charlotte_previous_ramlog.
#     Archive those plus the current dmesg into /var/log/uart on the ext4
#     root, and make journald store kernel logs on disk as well.
cat > /etc/systemd/journald.conf.d/10-persistent.conf <<'JOURNAL'
[Journal]
Storage=persistent
SystemMaxUse=256M
RuntimeMaxUse=64M
ForwardToKMsg=yes
MaxLevelKMsg=notice
JOURNAL

mkdir -p /usr/local/sbin

cat > /usr/local/sbin/charlotte-stage <<'STAGE'
#!/bin/sh
msg="$*"
[ -n "$msg" ] || exit 0
line="$(date -u +%Y-%m-%dT%H:%M:%SZ) $msg"
if [ -w /sys/kernel/charlotte_stage ]; then
	printf '%s\n' "$line" > /sys/kernel/charlotte_stage 2>/dev/null || true
fi
if [ -w /dev/pmsg0 ]; then
	printf '%s\n' "$line" > /dev/pmsg0 2>/dev/null || true
fi
exit 0
STAGE

cat > /usr/local/sbin/uart-log-archive <<'ARCHIVE'
#!/bin/sh
set -eu
LOG=/var/log/uart
KEEP=30
mkdir -p "$LOG"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
boot=$(tr -d '-' < /proc/sys/kernel/random/boot_id)

if ! grep -q ' /sys/fs/pstore ' /proc/mounts 2>/dev/null; then
	mount -t pstore pstore /sys/fs/pstore 2>/dev/null || true
fi

if [ -s /sys/kernel/charlotte_previous_ramlog ]; then
	cp /sys/kernel/charlotte_previous_ramlog \
		"$LOG/previous-$stamp-$boot.log" || true
fi
for f in /sys/fs/pstore/*; do
	[ -f "$f" ] || continue
	cp "$f" "$LOG/$(basename "$f")-$stamp.log" || true
done
dmesg > "$LOG/dmesg-$stamp-$boot.log" 2>/dev/null || true
{
	printf 'cmdline: '
	cat /proc/cmdline 2>/dev/null || true
	printf '\nbootargs: '
	tr -d '\0' < /proc/device-tree/chosen/bootargs 2>/dev/null || true
	printf '\nmodel: '
	tr -d '\0' < /proc/device-tree/model 2>/dev/null || true
	printf '\n'
} > "$LOG/cmdline-$stamp-$boot.log" 2>/dev/null || true
/usr/local/sbin/charlotte-stage "archived pstore for $boot"

prev=$(ls -1t "$LOG"/previous-*.log 2>/dev/null | head -n 1 || true)
[ -n "$prev" ] && ln -sfn "$(basename "$prev")" "$LOG/previous-boot.log"
pstore=$(ls -1t "$LOG"/console-ramoops-*.log 2>/dev/null | head -n 1 || true)
[ -n "$pstore" ] && ln -sfn "$(basename "$pstore")" "$LOG/console-last-boot.log"

find "$LOG" -maxdepth 1 -type f -name '*.log' -mtime +14 -delete 2>/dev/null || true
for pat in previous- console-ramoops- dmesg-; do
	ls -1t "$LOG"/"$pat"*.log 2>/dev/null |
		tail -n +$((KEEP + 1)) | xargs -r rm -f
done
exit 0
ARCHIVE

cat > /usr/local/sbin/uart-dmesg-snapshot <<'SNAPSHOT'
#!/bin/sh
set -eu
LOG=/var/log/uart
mkdir -p "$LOG"
tmp="$LOG/.dmesg-latest.$$"
dmesg > "$tmp" 2>/dev/null || true
mv "$tmp" "$LOG/dmesg-latest.log"
SNAPSHOT

cat > /usr/local/sbin/uart-boot-snapshot <<'BOOT'
#!/bin/sh
set -u
LOG=/var/log/uart
stamp=$(date -u +%Y%m%dT%H%M%SZ)
boot=$(tr -d '-' < /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
mkdir -p "$LOG"
file="$LOG/boot-$stamp-$boot.log"
{
	printf 'boot %s %s\n' "$stamp" "$boot"
	printf 'model: '
	tr -d '\0' < /proc/device-tree/model 2>/dev/null || true
	printf '\nbootargs: '
	tr -d '\0' < /proc/device-tree/chosen/bootargs 2>/dev/null || true
	printf '\ncmdline: '
	cat /proc/cmdline 2>/dev/null || true
	printf '\n'
	dmesg 2>/dev/null || true
} > "$file" 2>/dev/null || true
ln -sfn "$(basename "$file")" "$LOG/boot-last.log" 2>/dev/null || true
/usr/local/sbin/charlotte-stage "userspace boot snapshot"
exit 0
BOOT

cat > /usr/local/sbin/uart-shutdown-snapshot <<'SHUTDOWN'
#!/bin/sh
LOG=/var/log/uart
stamp=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$LOG" 2>/dev/null || true
dmesg > "$LOG/shutdown-$stamp.log" 2>/dev/null || true
/usr/local/sbin/charlotte-stage "userspace shutdown"
exit 0
SHUTDOWN

chmod 0755 /usr/local/sbin/charlotte-stage \
	/usr/local/sbin/uart-log-archive \
	/usr/local/sbin/uart-dmesg-snapshot \
	/usr/local/sbin/uart-boot-snapshot \
	/usr/local/sbin/uart-shutdown-snapshot

cat > /etc/systemd/system/uart-log-archive.service <<'UNIT'
[Unit]
Description=Archive previous-boot UART/pstore console log
After=local-fs.target
Before=systemd-pstore.service serial-getty@ttyHW0.service serial-getty@ttyGS0.service
Wants=local-fs.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/uart-log-archive

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/uart-dmesg-snapshot.service <<'UNIT'
[Unit]
Description=Snapshot kernel/UART console log to /var/log/uart

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/uart-dmesg-snapshot
UNIT

cat > /etc/systemd/system/uart-dmesg-snapshot.timer <<'UNIT'
[Unit]
Description=Periodically snapshot kernel/UART console log

[Timer]
OnBootSec=2min
OnUnitActiveSec=5min

[Install]
WantedBy=timers.target
UNIT

cat > /etc/systemd/system/uart-boot-snapshot.service <<'UNIT'
[Unit]
Description=Snapshot early boot UART/pstore state to /var/log/uart
After=local-fs.target
Before=serial-getty@ttyHW0.service serial-getty@ttyGS0.service
Wants=local-fs.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/uart-boot-snapshot

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/uart-shutdown-snapshot.service <<'UNIT'
[Unit]
Description=Snapshot final UART/pstore state before poweroff
DefaultDependencies=no
Before=shutdown.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/uart-shutdown-snapshot

[Install]
WantedBy=shutdown.target
UNIT

cat > /etc/tmpfiles.d/uart-log.conf <<'TMPFILES'
d /var/log/uart 0755 root root -
TMPFILES

cat > /etc/profile.d/uart-session-log.sh <<'SESSION'
[ -n "${UART_SESSION_LOGGED:-}" ] && return 0
_uart_tty=$(tty 2>/dev/null) || return 0
case "$_uart_tty" in
/dev/ttyHW*|/dev/ttyGS*) ;;
*) return 0 ;;
esac
mkdir -p /var/log/uart
UART_SESSION_LOGGED=1
export UART_SESSION_LOGGED
exec script -q -f -O "/var/log/uart/session-$(date -u +%Y%m%dT%H%M%SZ)-${_uart_tty##*/}.log"
SESSION
chmod 0644 /etc/profile.d/uart-session-log.sh

systemctl mask systemd-pstore.service 2>/dev/null || true
systemctl enable uart-log-archive.service
systemctl enable uart-boot-snapshot.service
systemctl enable uart-shutdown-snapshot.service
systemctl enable uart-dmesg-snapshot.timer

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
systemctl enable usb-gadget.service
systemctl disable apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true

systemd-machine-id-setup
ssh-keygen -A

# Let systemd-resolved own resolv.conf at runtime.
ln -snf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf

/usr/sbin/sshd -t
for unit in ssh.service getty@tty1.service \
	serial-getty@ttyHW0.service serial-getty@ttyGS0.service \
	usb-gadget.service uart-log-archive.service uart-boot-snapshot.service \
	uart-shutdown-snapshot.service uart-dmesg-snapshot.timer \
	systemd-networkd.service systemd-resolved.service; do
	systemctl is-enabled --quiet "$unit"
done
CHROOT

# 4. Install the matching kernel modules and refresh depmod.
make -C "$KERNEL_SRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	INSTALL_MOD_PATH="$ROOTFS" modules_install
depmod -b "$ROOTFS" "$KERNEL_RELEASE"

# 4b. Install the MRO-W00/W09 stock WiFi calibration files.  The kernel
#     requests /lib/firmware/hi11xx/cfg_mro_w00_va.ini from the board DTS.
install -D -m 0644 "$HERE/firmware/cfg_mro_w00_va.ini" \
	"$ROOTFS/lib/firmware/hi11xx/cfg_mro_w00_va.ini"
install -D -m 0644 "$HERE/firmware/cfg_mro_w00_vb.ini" \
	"$ROOTFS/lib/firmware/hi11xx/cfg_mro_w00_vb.ini"

# 5. Publish the initramfs and a standalone module tree.
tar -C "$ROOTFS/lib/modules" -czf "$OUT/modules-$KERNEL_RELEASE.tar.gz" "$KERNEL_RELEASE"
(cd "$ROOTFS" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9) > "$OUT/$IMAGE_NAME.cpio.gz"

echo "modules : $OUT/modules-$KERNEL_RELEASE.tar.gz"
echo "initramfs: $OUT/$IMAGE_NAME.cpio.gz"
ls -l "$OUT"
sha256sum "$OUT/modules-$KERNEL_RELEASE.tar.gz" "$OUT/$IMAGE_NAME.cpio.gz"
