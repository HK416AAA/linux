#!/usr/bin/env python3
"""Virtual mouse bridge for the charlotte Ubuntu image.

Listens on TCP 7788 and turns short text commands from the phone into
/dev/uinput events, so the phone can drive the X cursor over the USB
Ethernet link that the initramfs gadget already provides.

Commands (one per line):
  M <dx> <dy>   relative move, floats allowed
  C <btn>       click (press + release), btn 1 left / 2 right / 3 middle
  T <btn>       double click with a 60 ms gap
  D <btn>       button press
  U <btn>       button release
  W <steps>     vertical wheel, positive scrolls up
  H <steps>     horizontal wheel, positive scrolls right
  S <text>      type UTF-8 text (Chinese comes from the phone IME)
  K <combo>     press and release a key combo, e.g. Return, ctrl+l
  P             ping, answers OK

Text and keys are injected through xdotool, which needs the X session on
$MOUSE_BRIDGE_DISPLAY (default :0) to be up.
"""

import fcntl
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import threading

EV_SYN = 0x00
EV_KEY = 0x01
EV_REL = 0x02

REL_X = 0x00
REL_Y = 0x01
REL_HWHEEL = 0x06
REL_WHEEL = 0x08

BTN_LEFT = 0x110
BTN_RIGHT = 0x111
BTN_MIDDLE = 0x112

BTN_BY_ID = {1: BTN_LEFT, 2: BTN_RIGHT, 3: BTN_MIDDLE}

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_RELBIT = 0x40045566
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502

INPUT_EVENT = struct.Struct("@llHHi")
USER_DEV = struct.Struct("80sHHHHI" + "i" * 256)

LISTEN_PORT = int(os.environ.get("MOUSE_BRIDGE_PORT", "7788"))
DISPLAY = os.environ.get("MOUSE_BRIDGE_DISPLAY", ":0")
KEY_COMBO = re.compile(r"^[A-Za-z0-9_+\-]+$")


class UInput:
    def __init__(self, path="/dev/uinput"):
        self.fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        self.lock = threading.Lock()

        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
        for code in (REL_X, REL_Y, REL_WHEEL, REL_HWHEEL):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        for code in BTN_BY_ID.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)

        name = b"charlotte-remote-mouse"
        buf = USER_DEV.pack(
            name,
            0x03,  # BUS_USB
            0x1209,
            0x0001,
            1,
            0,
            *([0] * 256),
        )
        os.write(self.fd, buf)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def _emit(self, etype, code, value):
        os.write(self.fd, INPUT_EVENT.pack(0, 0, etype, code, value))

    def sync(self):
        self._emit(EV_SYN, 0, 0)

    def move(self, dx, dy):
        with self.lock:
            if dx:
                self._emit(EV_REL, REL_X, dx)
            if dy:
                self._emit(EV_REL, REL_Y, dy)
            self.sync()

    def button(self, btn, down):
        code = BTN_BY_ID.get(btn)
        if code is None:
            return
        with self.lock:
            self._emit(EV_KEY, code, 1 if down else 0)
            self.sync()

    def wheel(self, steps, horizontal=False):
        code = REL_HWHEEL if horizontal else REL_WHEEL
        with self.lock:
            self._emit(EV_REL, code, steps)
            self.sync()


class XInput:
    """Types text and key combos into the running X session."""

    def __init__(self):
        self.xdotool = shutil.which("xdotool")
        self.env = dict(os.environ, DISPLAY=DISPLAY)
        self.lock = threading.Lock()

    def _run(self, args):
        if not self.xdotool:
            return "xdotool not installed"
        try:
            done = subprocess.run([self.xdotool] + args, env=self.env,
                                  capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError) as exc:
            return str(exc)
        if done.returncode:
            return (done.stderr or done.stdout or "xdotool failed").strip()
        return None

    def text(self, text):
        with self.lock:
            return self._run(["type", "--clearmodifiers", "--delay", "12",
                              "--", text])

    def key(self, combo):
        if not KEY_COMBO.match(combo):
            return "bad key combo"
        with self.lock:
            return self._run(["key", "--clearmodifiers", "--delay", "12",
                              combo])


class Client(threading.Thread):
    def __init__(self, conn, addr, dev, xinput, rest_x, rest_y):
        super().__init__(daemon=True)
        self.conn = conn
        self.addr = addr
        self.dev = dev
        self.xinput = xinput
        self.rest = [rest_x, rest_y]

    def log(self, msg):
        print("[mouse] %s %s" % (self.addr[0], msg), flush=True)

    def run(self):
        log = self.log
        log("connected")
        buf = b""
        try:
            while True:
                chunk = self.conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.handle(line.decode("utf-8", "replace").strip("\r"))
        except OSError as exc:
            log("socket error: %s" % exc)
        finally:
            try:
                self.conn.close()
            except OSError:
                pass
            log("disconnected")

    def handle(self, line):
        if not line:
            return
        parts = line.split()
        cmd = parts[0].upper()

        if cmd == "M" and len(parts) >= 3:
            self.rest[0] += float(parts[1])
            self.rest[1] += float(parts[2])
            dx = int(self.rest[0])
            dy = int(self.rest[1])
            if dx or dy:
                self.rest[0] -= dx
                self.rest[1] -= dy
                self.dev.move(dx, dy)
        elif cmd == "C" and len(parts) >= 2:
            btn = int(float(parts[1]))
            self.dev.button(btn, True)
            self.dev.button(btn, False)
        elif cmd == "T" and len(parts) >= 2:
            btn = int(float(parts[1]))
            self.dev.button(btn, True)
            self.dev.button(btn, False)
            time.sleep(0.06)
            self.dev.button(btn, True)
            self.dev.button(btn, False)
        elif cmd == "D" and len(parts) >= 2:
            self.dev.button(int(float(parts[1])), True)
        elif cmd == "U" and len(parts) >= 2:
            self.dev.button(int(float(parts[1])), False)
        elif cmd == "W" and len(parts) >= 2:
            self.dev.wheel(int(float(parts[1])))
        elif cmd == "H" and len(parts) >= 2:
            self.dev.wheel(int(float(parts[1])), horizontal=True)
        elif cmd == "S":
            text = line[2:] if line[:2] in ("S ", "s ") else ""
            if text:
                err = self.xinput.text(text)
                if err:
                    self.log("type failed: %s" % err)
        elif cmd == "K" and len(parts) >= 2:
            err = self.xinput.key(parts[1])
            if err:
                self.log("key failed: %s" % err)
        elif cmd == "P":
            try:
                self.conn.sendall(b"OK\n")
            except OSError:
                pass


def main():
    try:
        dev = UInput()
    except OSError as exc:
        print("[mouse] cannot open /dev/uinput: %s" % exc, file=sys.stderr,
              flush=True)
        return 1

    xinput = XInput()
    if not xinput.xdotool:
        print("[mouse] xdotool missing, text and key injection disabled",
              file=sys.stderr, flush=True)

    print("[mouse] virtual mouse ready on port %d" % LISTEN_PORT, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", LISTEN_PORT))
    srv.listen(4)

    while True:
        try:
            conn, addr = srv.accept()
        except OSError as exc:
            if exc.errno == errno.EINTR:
                continue
            raise
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Client(conn, addr, dev, xinput, 0.0, 0.0).start()


if __name__ == "__main__":
    sys.exit(main())
