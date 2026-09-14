#!/usr/bin/env python3
"""nikita-flipper-bridge -- hands the phone (or qFlipper) the Flipper's real
shell, and a shell on the computer the Flipper is plugged into, over Bluetooth.

Runs on the machine that holds the Flipper on USB. macOS, Windows and Linux --
one script, no edits. It works out which OS it is running on, finds the Flipper
on its own, and if the cable is yanked or qFlipper grabs the port it waits and
reconnects instead of dying. It does not get intimidated.

    python3 bridge.py --mailbox --allow-host   # no network: through the SD card
    python3 bridge.py                          # WebSocket, same-WiFi phone
    python3 bridge.py --allow-host             # also answer "host <cmd>"
    python3 bridge.py --token secret           # require "token <secret>" first

Transport is the same either way:
  * mailbox mode -- the phone leaves "<id>.<base64(cmd)>" at
    /ext/nikita/bridge/req over BLE, this runs it and writes the answer to
    /ext/nikita/bridge/res. Nothing on the wire between them.
  * WebSocket mode -- one text frame in (a command), one text frame back.

A bare command goes to the Flipper's own CLI. "host <cmd>" runs on THIS
computer (the point of the thing -- the machine the phone cannot otherwise
see). "hostinfo" / "host os" report the validated OS of this machine, so the
caller knows exactly what it is plugged into before it acts.

Serial: uses pyserial if it is installed (best on every OS, and the only way in
on Windows without it). Otherwise a built-in backend -- termios on macOS/Linux,
the Win32 API through ctypes on Windows -- so a stock Python 3 still works.
"""

import argparse
import base64
import hashlib
import os
import platform
import socket
import struct
import subprocess
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PROMPT = b">: "
# The Flipper Zero's USB CDC is an STMicroelectronics device (VID 0x0483).
FLIPPER_VID = 0x0483

IS_WINDOWS = platform.system() == "Windows"
IS_MAC = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"


# --------------------------------------------------------------------------
# OS validation -- what machine is the Flipper actually plugged into?

def detect_host():
    """The validated identity of THIS computer -- the plugged-in system.

    Everything here comes from the running interpreter, not from guessing, so
    it is the ground truth the phone/Flipper can trust before it runs anything.
    """
    system = platform.system()
    pretty = {"Darwin": "macOS", "Windows": "Windows", "Linux": "Linux"}.get(
        system, system or "unknown")
    release = platform.release()
    version = ""
    try:
        if IS_MAC:
            version = platform.mac_ver()[0]
        elif IS_WINDOWS:
            version = platform.version()
        elif IS_LINUX:
            version = _linux_pretty_name() or release
    except Exception:
        version = release
    try:
        user = os.environ.get("USER") or os.environ.get("USERNAME") or ""
        if not user:
            import getpass
            user = getpass.getuser()
    except Exception:
        user = ""
    return {
        "os": pretty,
        "system": system,
        "version": version or release,
        "release": release,
        "arch": platform.machine(),
        "hostname": socket.gethostname(),
        "user": user,
        "python": platform.python_version(),
        "shell": _default_shell(),
    }


def _linux_pretty_name():
    try:
        with open("/etc/os-release", "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return ""


def _default_shell():
    if IS_WINDOWS:
        return os.environ.get("COMSPEC", "cmd.exe")
    return os.environ.get("SHELL", "/bin/sh")


def host_banner():
    h = detect_host()
    return (f"{h['os']} {h['version']} ({h['arch']}) -- "
            f"{h['user']}@{h['hostname']}, python {h['python']}")


def host_report():
    h = detect_host()
    return "\n".join(f"{k}: {v}" for k, v in h.items())


# --------------------------------------------------------------------------
# Serial backends -- one small interface, three implementations.
#
#   read(maxbytes, timeout) -> bytes   waits up to timeout, b"" if nothing
#   write(data)                        writes every byte
#   close()
#
# Flipper (below) speaks only to this interface, so the OS differences live
# here and nowhere else.

class SerialError(Exception):
    pass


class _PySerial:
    """pyserial backend -- the good path on all three OSes."""

    def __init__(self, port):
        import serial  # noqa
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200          # CDC ignores it, but it must be set
        self.ser.timeout = 0                 # non-blocking reads
        self.ser.write_timeout = 5
        self.ser.rtscts = False
        self.ser.dsrdtr = False
        self.ser.xonxoff = False
        self.ser.open()
        try:
            self.ser.dtr = True              # some stacks gate output on DTR
        except Exception:
            pass
        self.ser.reset_input_buffer()

    def read(self, maxbytes=4096, timeout=0.1):
        end = time.time() + timeout
        while True:
            n = self.ser.in_waiting
            if n:
                return self.ser.read(min(n, maxbytes))
            if time.time() >= end:
                return b""
            time.sleep(0.004)

    def write(self, data):
        try:
            self.ser.write(data)
            self.ser.flush()
        except Exception as exc:
            raise SerialError(str(exc))

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


class _PosixSerial:
    """Built-in macOS/Linux backend -- raw termios, no packages."""

    def __init__(self, port):
        import termios
        self.termios = termios
        import select as _select
        self.select = _select
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(
            self.fd, termios.TCSANOW,
            [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])

    def read(self, maxbytes=4096, timeout=0.1):
        r, _, _ = self.select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, maxbytes)
        except OSError as exc:
            raise SerialError(str(exc))

    def write(self, data):
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.fd, view)
                view = view[n:]
            except BlockingIOError:
                self.select.select([], [self.fd], [], 1.0)
            except OSError as exc:
                raise SerialError(str(exc))

    def close(self):
        try:
            os.close(self.fd)
        except Exception:
            pass


class _WinSerial:
    """Built-in Windows backend -- the Win32 serial API through ctypes.

    A last resort for a stock Python with no pyserial. pyserial is still the
    recommended path on Windows (pip install pyserial); this exists so the
    bridge is not dead in the water without it.
    """

    def __init__(self, port):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.k = ctypes.windll.kernel32
        name = port if port.startswith("\\\\.\\") else ("\\\\.\\" + port)
        GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
        OPEN_EXISTING = 3
        self.handle = self.k.CreateFileW(
            name, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if self.handle == wintypes.HANDLE(-1).value or self.handle == -1:
            raise SerialError(f"CreateFile failed for {port}")

        # DCB: 115200 8N1, no flow control.
        class DCB(ctypes.Structure):
            _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD),
                        ("fFlags", wintypes.DWORD), ("wReserved", wintypes.WORD),
                        ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                        ("ByteSize", ctypes.c_byte), ("Parity", ctypes.c_byte),
                        ("StopBits", ctypes.c_byte), ("XonChar", ctypes.c_char),
                        ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                        ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                        ("wReserved1", wintypes.WORD)]
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not self.k.GetCommState(self.handle, ctypes.byref(dcb)):
            dcb.fFlags = 0
        dcb.BaudRate = 115200
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        dcb.fFlags = 0x0001  # fBinary; no parity, no flow control
        if not self.k.SetCommState(self.handle, ctypes.byref(dcb)):
            raise SerialError("SetCommState failed")

        # Timeouts: return immediately with whatever is there (polling model).
        class TO(ctypes.Structure):
            _fields_ = [("ReadIntervalTimeout", wintypes.DWORD),
                        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                        ("ReadTotalTimeoutConstant", wintypes.DWORD),
                        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                        ("WriteTotalTimeoutConstant", wintypes.DWORD)]
        to = TO(0xFFFFFFFF, 0, 0, 0, 5000)
        self.k.SetCommTimeouts(self.handle, ctypes.byref(to))
        self.k.SetCommMask(self.handle, 0)
        # Assert DTR/RTS.
        self.k.EscapeCommFunction(self.handle, 5)  # SETDTR
        self.k.EscapeCommFunction(self.handle, 3)  # SETRTS

    def read(self, maxbytes=4096, timeout=0.1):
        ctypes = self.ctypes
        end = time.time() + timeout
        buf = ctypes.create_string_buffer(maxbytes)
        read = ctypes.wintypes.DWORD(0)
        while True:
            ok = self.k.ReadFile(self.handle, buf, maxbytes,
                                 ctypes.byref(read), None)
            if ok and read.value:
                return buf.raw[:read.value]
            if time.time() >= end:
                return b""
            time.sleep(0.004)

    def write(self, data):
        ctypes = self.ctypes
        written = ctypes.wintypes.DWORD(0)
        view = memoryview(data)
        total = 0
        while total < len(data):
            chunk = bytes(view[total:])
            ok = self.k.WriteFile(self.handle, chunk, len(chunk),
                                  ctypes.byref(written), None)
            if not ok:
                raise SerialError("WriteFile failed")
            total += written.value or 0
            if not written.value:
                break

    def close(self):
        try:
            self.k.CloseHandle(self.handle)
        except Exception:
            pass


def _have_pyserial():
    try:
        import serial  # noqa
        return True
    except Exception:
        return False


def open_serial(port):
    if _have_pyserial():
        return _PySerial(port)
    if IS_WINDOWS:
        return _WinSerial(port)
    return _PosixSerial(port)


# --------------------------------------------------------------------------
# Finding the Flipper, per OS.

def discover_port():
    # pyserial's enumeration is the most reliable, on every OS: match the
    # Flipper by its USB vendor id, or by name.
    if _have_pyserial():
        try:
            from serial.tools import list_ports
            ports = list(list_ports.comports())
            for p in ports:
                if (p.vid == FLIPPER_VID) or ("flipper" in (p.description or "").lower()):
                    return p.device
            # A lone CDC device is very likely it.
            cdc = [p for p in ports if p.vid is not None]
            if len(cdc) == 1:
                return cdc[0].device
        except Exception:
            pass

    import glob
    if IS_MAC:
        for pat in ("/dev/cu.usbmodemflip_*", "/dev/cu.usbmodem*"):
            found = sorted(glob.glob(pat))
            if found:
                return found[0]
    elif IS_LINUX:
        for pat in ("/dev/serial/by-id/*Flipper*", "/dev/serial/by-id/*flipper*",
                    "/dev/ttyACM*", "/dev/ttyUSB*"):
            found = sorted(glob.glob(pat))
            if found:
                return found[0]
    elif IS_WINDOWS:
        for com in _windows_com_ports():
            return com  # first COM port; pyserial path above is preferred

    raise SerialError(
        "No Flipper found on USB. Plug it in, and quit qFlipper (or press "
        "RELEASE PORT in it) so the port is free. On Windows, "
        "`pip install pyserial` makes detection reliable.")


def _windows_com_ports():
    ports = []
    try:
        import winreg
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        i = 0
        while True:
            try:
                _, val, _ = winreg.EnumValue(key, i)
                ports.append(val)
                i += 1
            except OSError:
                break
    except Exception:
        pass
    return ports


# --------------------------------------------------------------------------
# The Flipper's serial CLI -- with reconnect.

class Flipper:
    """The Flipper's text shell, one command at a time under a lock.

    Reconnecting by design: if a read or write fails (cable pulled, qFlipper
    grabbed the port, the Flipper rebooted) it drops the handle and re-opens on
    the next call, waiting the device back if it has to. The caller sees a slow
    command, never a crash.
    """

    def __init__(self, port=None):
        self.want_port = port
        self.port = None
        self.io = None
        self.lock = threading.Lock()
        self._connect(initial=True)

    def _connect(self, initial=False):
        deadline = time.time() + (30 if initial else 3600)
        warned = False
        while True:
            try:
                self.port = self.want_port or discover_port()
                self.io = open_serial(self.port)
                self._drain()
                self._raw_cmd("", timeout=2.0)   # prompt handshake
                print(f"[bridge] Flipper on {self.port}")
                return
            except (SerialError, OSError) as exc:
                if self.io:
                    self.io.close()
                    self.io = None
                if initial and time.time() > deadline:
                    raise SystemExit(f"[bridge] {exc}")
                if not warned:
                    print(f"[bridge] waiting for the Flipper... ({exc})")
                    warned = True
                time.sleep(1.5)

    def _reopen(self):
        if self.io:
            self.io.close()
            self.io = None
        print("[bridge] serial dropped -- reconnecting")
        self._connect(initial=False)

    # -- low level ---------------------------------------------------------

    def _drain(self):
        while True:
            if not self.io.read(4096, 0.05):
                return

    def _read_until(self, ending, timeout):
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            chunk = self.io.read(4096, 0.1)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(ending):
                    break
        return buf

    def _raw_cmd(self, command, timeout):
        self.io.write(command.encode() + b"\r\n")
        return self._read_until(PROMPT.strip(), timeout)

    # -- public, all reconnect-guarded -------------------------------------

    def run(self, command, timeout=15.0):
        with self.lock:
            for attempt in (1, 2):
                try:
                    raw = self._raw_cmd(command, timeout)
                    return self._clean(raw, command)
                except (SerialError, OSError):
                    if attempt == 2:
                        return "(serial error)"
                    self._reopen()

    def read_file(self, path):
        with self.lock:
            for attempt in (1, 2):
                try:
                    self.io.write(("storage read " + path + "\r\n").encode())
                    buf = self._read_until(b">:", 5.0)
                    break
                except (SerialError, OSError):
                    if attempt == 2:
                        return None
                    self._reopen()
        if b"Storage error" in buf or b"does not exist" in buf:
            return None
        marker = buf.find(b"Size: ")
        if marker < 0:
            return None
        try:
            size = int(buf[marker + 6:].split()[0])
        except (ValueError, IndexError):
            return None
        if size <= 0:
            return b""
        nl = buf.find(b"\n", marker)
        if nl < 0:
            return None
        return buf[nl + 1:nl + 1 + size]

    def write_file(self, path, data):
        with self.lock:
            for attempt in (1, 2):
                try:
                    self._raw_cmd_nowait("storage remove " + path)
                    self._settle(0.2)
                    self.io.write(
                        ("storage write_chunk " + path + " " +
                         str(len(data)) + "\r").encode())
                    self._await(b"Ready", timeout=3.0)
                    self.io.write(data)
                    self._settle(1.0)
                    return
                except (SerialError, OSError):
                    if attempt == 2:
                        raise
                    self._reopen()

    def _raw_cmd_nowait(self, command):
        self.io.write(command.encode() + b"\r\n")

    def _await(self, token, timeout=3.0):
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            chunk = self.io.read(4096, 0.1)
            if chunk:
                buf += chunk
                if token in buf:
                    return buf
        return buf

    def _settle(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if not self.io.read(4096, 0.05):
                continue

    @staticmethod
    def _clean(raw, command):
        text = raw.decode("utf-8", "replace")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        lines = text.split("\n")
        if lines and command and lines[0].strip() == command.strip():
            lines = lines[1:]
        while lines and lines[-1].strip() in ("", ">:"):
            lines.pop()
        return "\n".join(lines).strip()


# --------------------------------------------------------------------------
# WebSocket (RFC 6455) -- one text frame each way.

def ws_handshake(conn):
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        request += chunk
    key = None
    for line in request.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip()
    if not key:
        conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        return False
    accept = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
    conn.sendall(
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
    return True


def ws_recv(conn):
    header = recv_exact(conn, 2)
    if not header:
        return None, None
    opcode = header[0] & 0x0F
    masked = header[1] & 0x80
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(conn, 8))[0]
    mask = recv_exact(conn, 4) if masked else b""
    payload = recv_exact(conn, length) if length else b""
    if masked and payload:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def recv_exact(conn, count):
    data = b""
    while len(data) < count:
        chunk = conn.recv(count - len(data))
        if not chunk:
            return b"" if not data else data
        data += chunk
    return data


def ws_send(conn, payload, opcode=0x1):
    data = payload.encode() if isinstance(payload, str) else payload
    header = bytes([0x80 | opcode])
    length = len(data)
    if length < 126:
        header += bytes([length])
    elif length < (1 << 16):
        header += bytes([126]) + struct.pack(">H", length)
    else:
        header += bytes([127]) + struct.pack(">Q", length)
    conn.sendall(header + data)


def handle(conn, addr, flipper, args):
    authorised = args.token is None
    try:
        if not ws_handshake(conn):
            return
        print(f"[bridge] {addr[0]} connected")
        while True:
            opcode, payload = ws_recv(conn)
            if opcode is None or opcode == 0x8:
                break
            if opcode == 0x9:
                ws_send(conn, payload, opcode=0xA)
                continue
            if opcode != 0x1:
                continue
            command = payload.decode("utf-8", "replace").strip()
            if not command:
                ws_send(conn, "")
                continue
            if args.token is not None and not authorised:
                if command == f"token {args.token}":
                    authorised = True
                    ws_send(conn, "authorised")
                else:
                    ws_send(conn, "not authorised -- send: token <secret>")
                continue
            print(f"[bridge] > {command}")
            ws_send(conn, respond(command, flipper, args))
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    finally:
        print(f"[bridge] {addr[0]} disconnected")
        conn.close()


# --------------------------------------------------------------------------
# Binary transfer + a live Python REPL, driven through the mailbox.

import base64 as _b64
import urllib.request as _urlreq
import code as _code
import io as _io
import contextlib as _ctx


def _is_flip(path):
    return path.startswith("/ext") or path.startswith("/int")


def _host_path(path, cwd):
    path = os.path.expanduser(path)
    if not os.path.isabs(path):
        base = os.path.expanduser(cwd or "~")
        path = os.path.normpath(os.path.join(base, path))
    return path


def _flip_is_dir(flipper, path):
    return "Directory" in flipper.run("storage stat " + path, timeout=6)


def _flip_mkparents(flipper, path):
    parts = path.strip("/").split("/")
    acc = ""
    for seg in parts[:-1]:
        acc += "/" + seg
        if acc in ("/ext", "/int"):
            continue
        flipper.run("storage mkdir " + acc, timeout=4)


def _read_bytes(flipper, path, is_flip):
    if is_flip:
        data = flipper.read_file(path)
        if data is None:
            raise IOError("not found on Flipper: " + path)
        return data
    with open(path, "rb") as fh:
        return fh.read()


def _write_bytes(flipper, path, data, is_flip):
    if is_flip:
        _flip_mkparents(flipper, path)
        flipper.write_file(path, data)
    else:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)


def _flip_walk(flipper, root):
    out = flipper.run("storage list " + root, timeout=8)
    files = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("[F]"):
            rest = line[3:].strip()
            name = rest.rsplit(" ", 1)[0].strip() if rest and rest[-1].isdigit() else rest
            files.append(root.rstrip("/") + "/" + name)
        elif line.startswith("[D]"):
            name = line[3:].strip()
            files += _flip_walk(flipper, root.rstrip("/") + "/" + name)
    return files


def do_copy(payload, flipper):
    parts = payload.split()
    if len(parts) < 3:
        return "cp: usage error"
    src = _b64.b64decode(parts[0]).decode("utf-8", "replace")
    dst = _b64.b64decode(parts[1]).decode("utf-8", "replace")
    cwd = _b64.b64decode(parts[2]).decode("utf-8", "replace")
    recursive = "-r" in parts[3:] or "-rf" in parts[3:]
    sflip, dflip = _is_flip(src), _is_flip(dst)
    rsrc = src if sflip else _host_path(src, cwd)
    rdst = dst if dflip else _host_path(dst, cwd)
    src_is_dir = _flip_is_dir(flipper, rsrc) if sflip else os.path.isdir(rsrc)
    dst_is_dir = _flip_is_dir(flipper, rdst) if dflip else os.path.isdir(rdst)

    def arrow():
        a = "Flipper" if sflip else "PC"
        b = "Flipper" if dflip else "PC"
        return "%s:%s -> %s:%s" % (a, rsrc, b, rdst)

    try:
        if src_is_dir:
            if not recursive:
                return "cp: %s is a directory (use -r)" % src
            if sflip:
                srcfiles = _flip_walk(flipper, rsrc)
            else:
                srcfiles = [os.path.join(dp, f)
                            for dp, _, fs in os.walk(rsrc) for f in fs]
            n = 0
            for sf in srcfiles:
                rel = sf[len(rsrc):].lstrip("/").lstrip("\\")
                df = (rdst.rstrip("/") + "/" + rel.replace("\\", "/"))
                data = _read_bytes(flipper, sf, sflip)
                _write_bytes(flipper, df, data, dflip)
                n += 1
            return "copied %d files (%s)" % (n, arrow())
        final = rdst
        if dst_is_dir:
            final = rdst.rstrip("/") + "/" + os.path.basename(rsrc)
        data = _read_bytes(flipper, rsrc, sflip)
        _write_bytes(flipper, final, data, dflip)
        want = hashlib.md5(data).hexdigest()
        if dflip:
            chk = flipper.run("storage md5 " + final, timeout=8).strip().split()
            got = chk[-1] if chk else ""
        else:
            got = hashlib.md5(open(final, "rb").read()).hexdigest()
        ok = got == want
        return "copied %d bytes %s\nmd5 %s %s" % (
            len(data), arrow().replace(rdst, final),
            want, "(verified)" if ok else "(MISMATCH got %s)" % got)
    except Exception as exc:
        return "cp: %s" % exc


def do_wget(payload, flipper):
    parts = payload.split()
    if len(parts) < 2:
        return "wget: usage error"
    url = _b64.b64decode(parts[0]).decode("utf-8", "replace")
    dst = _b64.b64decode(parts[1]).decode("utf-8", "replace")
    cwd = _b64.b64decode(parts[2]).decode("utf-8", "replace") if len(parts) > 2 else ""
    try:
        req = _urlreq.Request(url, headers={"User-Agent": "nikita-flipper-bridge"})
        data = _urlreq.urlopen(req, timeout=30).read()
    except Exception as exc:
        return "wget: %s" % exc
    if len(data) > 8 * 1024 * 1024:
        return "wget: %d bytes is over the 8 MB cap" % len(data)
    dflip = _is_flip(dst)
    rdst = dst if dflip else _host_path(dst, cwd)
    try:
        _write_bytes(flipper, rdst, data, dflip)
    except Exception as exc:
        return "wget: save failed: %s" % exc
    where = "Flipper" if dflip else "PC"
    return "downloaded %d bytes -> %s:%s (md5 %s)" % (
        len(data), where, rdst, hashlib.md5(data).hexdigest())


_REPL = {"con": None}


def do_repl(payload):
    line = _b64.b64decode(payload).decode("utf-8", "replace")
    if _REPL["con"] is None:
        _REPL["con"] = _code.InteractiveConsole()
    buf = _io.StringIO()
    try:
        with _ctx.redirect_stdout(buf), _ctx.redirect_stderr(buf):
            more = _REPL["con"].push(line)
    except SystemExit:
        _REPL["con"] = None
        return "\x02" + (buf.getvalue() or "")
    return ("\x01" if more else "\x00") + buf.getvalue()


_HOST_OFF = ("host commands are off. Restart the bridge with "
             "--allow-host to enable them.")


def respond(command, flipper, args):
    # OS validation -- always allowed, reads nothing but the interpreter.
    if command == "hostinfo":
        return host_report()
    if command in ("host os", "os"):
        h = detect_host()
        return f"{h['os']} {h['version']} ({h['arch']})"

    if command.startswith("xcp "):
        return do_copy(command[4:], flipper) if args.allow_host else _HOST_OFF
    if command.startswith("xwget "):
        return do_wget(command[6:], flipper) if args.allow_host else _HOST_OFF
    if command.startswith("xpy "):
        return do_repl(command[4:]) if args.allow_host else _HOST_OFF

    if command.startswith("host "):
        if not args.allow_host:
            return _HOST_OFF
        return run_host(command[len("host "):].strip())

    if command == "bridge":
        return ("nikita-flipper-bridge\n"
                f"host: {host_banner()}\n"
                f"port: {flipper.port}\n"
                f"serial: {'pyserial' if _have_pyserial() else 'built-in'}\n"
                f"host commands: {'on' if args.allow_host else 'off'}")

    return flipper.run(command) or "(no output)"


def run_host(command, timeout=20):
    try:
        done = subprocess.run(
            command, shell=True, capture_output=True, text=True,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"host: timed out after {timeout}s"
    except Exception as exc:
        return f"host: {exc}"
    output = (done.stdout or "") + (done.stderr or "")
    return output.strip() or "(no output)"


BRIDGE_REQ = "/ext/nikita/bridge/req"
BRIDGE_RES = "/ext/nikita/bridge/res"


def mailbox_loop(flipper, args):
    print("[bridge] mailbox mode: polling the card, no network")
    print(f"[bridge] host: {host_banner()}")
    if args.allow_host:
        print("[bridge] host commands ON")
    flipper.run("nikita init", timeout=4)
    while True:
        body = flipper.read_file(BRIDGE_REQ)
        if not body:
            time.sleep(1.2)
            continue
        flipper.run("storage remove " + BRIDGE_REQ, timeout=4)
        text = body.decode("utf-8", "replace").strip()
        req_id, _, b64 = text.partition(".")
        try:
            command = base64.b64decode(b64).decode("utf-8", "replace")
        except Exception:
            command = ""
        if not command:
            continue
        print(f"[bridge] > [{req_id}] {command}")
        try:
            answer = respond(command, flipper, args)
        except Exception as exc:
            answer = f"bridge error: {exc}"
        enc = base64.b64encode(answer.encode("utf-8", "replace")).decode("ascii")
        flipper.write_file(BRIDGE_RES, (req_id + "." + enc).encode("ascii"))
        print(f"[bridge] < [{req_id}] {len(answer)} bytes")
        for _ in range(20):
            time.sleep(0.5)
            if flipper.read_file(BRIDGE_RES) is None:
                break


def local_addresses(port):
    lines = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip:
            lines.append(f"connect ws://{ip}:{port}")
    except OSError:
        pass
    if not lines:
        lines.append(f"listening on :{port}")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device (default: autodetect)")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--listen", type=int, default=8765, help="bind port")
    parser.add_argument(
        "--allow-host", action="store_true",
        help="answer 'host <cmd>' by running it on THIS computer")
    parser.add_argument(
        "--token", help="require 'token <secret>' before any command")
    parser.add_argument(
        "--mailbox", action="store_true",
        help="serve through the SD card, no WebSocket, no WiFi")
    parser.add_argument(
        "--hostinfo", action="store_true",
        help="print this computer's validated OS and exit")
    args = parser.parse_args()

    if args.hostinfo:
        print(host_report())
        return

    print(f"[bridge] this computer: {host_banner()}")
    flipper = Flipper(args.port)

    if args.mailbox:
        try:
            mailbox_loop(flipper, args)
        except KeyboardInterrupt:
            print("\n[bridge] stopped")
        return

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.listen))
    server.listen(4)
    for line in local_addresses(args.listen):
        print(f"[bridge] {line}")
    if args.allow_host and args.token is None:
        print("[bridge] WARNING: --allow-host with no --token: anyone on this "
              "network can run commands on this computer.")
    try:
        while True:
            conn, addr = server.accept()
            threading.Thread(
                target=handle, args=(conn, addr, flipper, args),
                daemon=True).start()
    except KeyboardInterrupt:
        print("\n[bridge] stopped")


if __name__ == "__main__":
    main()
