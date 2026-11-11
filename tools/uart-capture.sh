#!/bin/sh
set -eu

usage() {
	cat <<'EOF'
Usage: tools/uart-capture.sh [options]

Capture the board debug UART on the host and persist it to a stable log file.

Options:
  -d, --dev DEV     serial device (default: first /dev/ttyUSB* or /dev/ttyACM*)
  -b, --baud RATE   baud rate (default: 115200)
  -o, --out FILE    log file (default: <repo>/logs/uart/uart-<timestamp>.log)
  -q, --quiet       write only to the log file, do not echo to the terminal
  -h, --help        show this help

Wiring: board UART TX/RX/GND to a 1.8 V USB-TTL adapter, 115200 8N1.
EOF
}

repo_root=$(cd "$(dirname "$0")/.." && pwd)
dev=
baud=115200
out=
quiet=0

while [ $# -gt 0 ]; do
	case "$1" in
	-d | --dev) dev=$2; shift 2 ;;
	-b | --baud) baud=$2; shift 2 ;;
	-o | --out) out=$2; shift 2 ;;
	-q | --quiet) quiet=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

if [ -z "$dev" ]; then
	for candidate in /dev/ttyUSB* /dev/ttyACM*; do
		[ -e "$candidate" ] || continue
		dev=$candidate
		break
	done
fi
if [ -z "$dev" ] || [ ! -e "$dev" ]; then
	echo "uart-capture: no serial device found; pass --dev" >&2
	exit 1
fi

if [ -z "$out" ]; then
	out="$repo_root/logs/uart/uart-$(date +%Y%m%d-%H%M%S).log"
fi
mkdir -p "$(dirname "$out")"

stty -F "$dev" "$baud" cs8 -cstopb -parenb -crtscts raw -echo
trap 'stty -F "$dev" sane 2>/dev/null || true' EXIT INT TERM

echo "uart-capture: $dev @ $baud -> $out"
if [ "$quiet" -eq 1 ]; then
	cat "$dev" >>"$out"
else
	cat "$dev" | tee -a "$out"
fi
