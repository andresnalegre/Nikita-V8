#pragma once

// Bridge bootstrap, shipped IN the firmware and typed into the target computer
// over USB HID -- no download, nothing to install by hand. The agent exposes
// this as `bridge.install os: mac|win|linux`, reachable over the SD mailbox, so
// Nikita triggers it over BLE (which is what she could not do before: the old
// installer was a USB-only CLI command). One POSIX payload covers macOS AND
// Linux (termios; only the device glob differs, handled inline); Windows uses a
// pyserial variant. Each OS gets its own terminal-opener key sequence.
//
// The payload is the "pocket bridge": it waits for the Flipper serial to come
// back (dropped while we type as a keyboard), then serves the SD mailbox --
// read the base64 request the phone left over BLE, run it (host shell here, or
// the Flipper's own CLI), write the answer back. Same protocol as the full
// nikita-flipper-bridge.

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <string.h>

// POSIX pocket bridge (macOS + Linux). Multi-glob picks the Flipper's CDC port
// on either OS. termios is POSIX; python3 exists on both.
static const char* const NIKITA_POSIX_PAYLOAD =
    "import glob,os,time,select,termios,base64,subprocess\n"
    "def find():\n"
    " for pat in ('/dev/cu.usbmodemflip_*','/dev/cu.usbmodem*','/dev/ttyACM*','/dev/serial/by-id/*lipper*','/dev/ttyUSB*'):\n"
    "  g=sorted(glob.glob(pat))\n"
    "  if g:return g[0]\n"
    " return None\n"
    "def port():\n"
    " while 1:\n"
    "  p=find()\n"
    "  if p:\n"
    "   try:return op(p)\n"
    "   except OSError:pass\n"
    "  time.sleep(1)\n"
    "def op(p):\n"
    " fd=os.open(p,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)\n"
    " a=termios.tcgetattr(fd);a[0]=a[1]=a[3]=0;a[2]=termios.CS8|termios.CREAD|termios.CLOCAL\n"
    " a[6]=list(a[6]);a[6][termios.VMIN]=0;a[6][termios.VTIME]=0\n"
    " termios.tcsetattr(fd,termios.TCSANOW,a);return fd\n"
    "def rd(fd,cmd,t=5.0):\n"
    " os.write(fd,cmd+b'\\r\\n');e=time.time()+t;b=b''\n"
    " while time.time()<e:\n"
    "  r,_,_=select.select([fd],[],[],0.1)\n"
    "  if r:\n"
    "   try:c=os.read(fd,4096)\n"
    "   except OSError:break\n"
    "   if c:b+=c\n"
    "   if b.rstrip().endswith(b'>:'):break\n"
    " return b\n"
    "def rf(fd,path):\n"
    " b=rd(fd,b'storage read '+path)\n"
    " if b'Storage error' in b:return None\n"
    " m=b.find(b'Size: ')\n"
    " if m<0:return None\n"
    " try:n=int(b[m+6:].split()[0])\n"
    " except:return None\n"
    " nl=b.find(b'\\n',m)\n"
    " return b[nl+1:nl+1+n] if nl>=0 else None\n"
    "def aw(fd,t,to=3.0):\n"
    " e=time.time()+to;b=b''\n"
    " while time.time()<e:\n"
    "  r,_,_=select.select([fd],[],[],0.1)\n"
    "  if r:\n"
    "   c=os.read(fd,4096)\n"
    "   if c:b+=c\n"
    "   if t in b:return\n"
    "def wa(fd,d):\n"
    " while d:\n"
    "  try:n=os.write(fd,d);d=d[n:]\n"
    "  except BlockingIOError:select.select([],[fd],[],1)\n"
    "def wf(fd,path,data):\n"
    " rd(fd,b'storage remove '+path,2)\n"
    " os.write(fd,b'storage write_chunk '+path+b' '+str(len(data)).encode()+b'\\r')\n"
    " aw(fd,b'Ready')\n"
    " wa(fd,data);aw(fd,b'>:')\n"
    "REQ=b'/ext/nikita/bridge/req';RES=b'/ext/nikita/bridge/res'\n"
    "fd=port();rd(fd,b'nikita init',3)\n"
    "print('nikita bridge up')\n"
    "while 1:\n"
    " body=rf(fd,REQ)\n"
    " if not body:\n"
    "  time.sleep(1.2);continue\n"
    " rd(fd,b'storage remove '+REQ,3)\n"
    " t=body.decode('utf-8','replace').strip();i,_,c=t.partition('.')\n"
    " try:cmd=base64.b64decode(c).decode('utf-8','replace')\n"
    " except:cmd=''\n"
    " if not cmd:continue\n"
    " if cmd.startswith('host '):\n"
    "  try:o=subprocess.run(cmd[5:],shell=True,capture_output=True,text=True,timeout=20);out=(o.stdout or '')+(o.stderr or '')\n"
    "  except Exception as e:out=str(e)\n"
    " else:\n"
    "  out=rd(fd,cmd.encode(),15).decode('utf-8','replace')\n"
    " enc=base64.b64encode((out or '(no output)').encode('utf-8','replace')).decode()\n"
    " wf(fd,RES,(i+'.'+enc).encode())\n"
    " for _ in range(20):\n"
    "  time.sleep(0.5)\n"
    "  if rf(fd,RES) is None:break\n";

// Windows pocket bridge (pyserial). Typed after a quick `py -m pip install`.
static const char* const NIKITA_WIN_PAYLOAD =
    "import time,base64,subprocess\n"
    "import serial\n"
    "from serial.tools import list_ports\n"
    "def port():\n"
    " while 1:\n"
    "  for pi in list_ports.comports():\n"
    "   try:return serial.Serial(pi.device,115200,timeout=0.2)\n"
    "   except: pass\n"
    "  time.sleep(1)\n"
    "def rd(s,cmd,t=5.0):\n"
    " s.write(cmd+b'\\r\\n');e=time.time()+t;b=b''\n"
    " while time.time()<e:\n"
    "  c=s.read(4096)\n"
    "  if c:b+=c\n"
    "  if b.rstrip().endswith(b'>:'):break\n"
    " return b\n"
    "def rf(s,path):\n"
    " b=rd(s,b'storage read '+path)\n"
    " if b'Storage error' in b:return None\n"
    " m=b.find(b'Size: ')\n"
    " if m<0:return None\n"
    " try:n=int(b[m+6:].split()[0])\n"
    " except:return None\n"
    " nl=b.find(b'\\n',m)\n"
    " return b[nl+1:nl+1+n] if nl>=0 else None\n"
    "def aw(s,t,to=3.0):\n"
    " e=time.time()+to;b=b''\n"
    " while time.time()<e:\n"
    "  c=s.read(4096)\n"
    "  if c:b+=c\n"
    "  if t in b:return\n"
    "def wf(s,path,data):\n"
    " rd(s,b'storage remove '+path,2)\n"
    " s.write(b'storage write_chunk '+path+b' '+str(len(data)).encode()+b'\\r')\n"
    " aw(s,b'Ready');s.write(data);aw(s,b'>:')\n"
    "REQ=b'/ext/nikita/bridge/req';RES=b'/ext/nikita/bridge/res'\n"
    "s=port();rd(s,b'nikita init',3)\n"
    "print('nikita bridge up')\n"
    "while 1:\n"
    " body=rf(s,REQ)\n"
    " if not body:\n"
    "  time.sleep(1.2);continue\n"
    " rd(s,b'storage remove '+REQ,3)\n"
    " t=body.decode('utf-8','replace').strip();i,_,c=t.partition('.')\n"
    " try:cmd=base64.b64decode(c).decode('utf-8','replace')\n"
    " except:cmd=''\n"
    " if not cmd:continue\n"
    " if cmd.startswith('host '):\n"
    "  try:o=subprocess.run(cmd[5:],shell=True,capture_output=True,text=True,timeout=20);out=(o.stdout or '')+(o.stderr or '')\n"
    "  except Exception as e:out=str(e)\n"
    " else:\n"
    "  out=rd(s,cmd.encode(),15).decode('utf-8','replace')\n"
    " enc=base64.b64encode((out or '(no output)').encode('utf-8','replace')).decode()\n"
    " wf(s,RES,(i+'.'+enc).encode())\n"
    " for _ in range(20):\n"
    "  time.sleep(0.5)\n"
    "  if rf(s,RES) is None:break\n";

// ---- HID typing -----------------------------------------------------------

static void nkb_tap(uint16_t key) {
    for(int i = 0; i < 50 && !furi_hal_hid_kb_press(key); i++) furi_delay_ms(4);
    furi_delay_ms(6);
    for(int i = 0; i < 50 && !furi_hal_hid_kb_release_all(); i++) furi_delay_ms(4);
    furi_delay_ms(6);
}

static void nkb_type(const char* text) {
    for(const char* p = text; *p; p++) {
        if(*p == '\n') {
            nkb_tap(HID_KEYBOARD_RETURN);
            furi_delay_ms(12);
            continue;
        }
        uint16_t key = HID_ASCII_TO_KEY(*p);
        if(key != HID_KEYBOARD_NONE) nkb_tap(key);
    }
}

// Returns 0 on success, or a reason code. os = "mac"|"win"|"linux".
// Switches USB to HID, opens a terminal for that OS, types the bootstrap, and
// restores the previous USB config so the serial link comes back.
static int nikita_agent_install_bridge(const char* os) {
    if(furi_hal_usb_is_locked()) return 1; // a screen stream owns USB

    FuriHalUsbInterface* prev = furi_hal_usb_get_config();
    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        furi_hal_usb_set_config(prev, NULL);
        return 2;
    }
    furi_delay_ms(2200); // host enumerates the keyboard

    if(!strcmp(os, "win")) {
        // Win+R -> powershell -> Enter
        nkb_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_R);
        furi_delay_ms(900);
        nkb_type("powershell\n");
        furi_delay_ms(3500);
        nkb_type("py -m pip install --quiet pyserial\n");
        furi_delay_ms(6000);
        // write the script via a here-string, then run it detached
        nkb_type("$c=@'\n");
        nkb_type(NIKITA_WIN_PAYLOAD);
        nkb_type("'@; Set-Content -Path $env:TEMP\\nikita_bridge.py -Value $c\n");
        nkb_type(
            "Start-Process -WindowStyle Hidden py -ArgumentList "
            "\"$env:TEMP\\nikita_bridge.py\"\n");
    } else if(!strcmp(os, "linux")) {
        // Ctrl+Alt+T is the common GNOME terminal shortcut
        nkb_tap(KEY_MOD_LEFT_CTRL | KEY_MOD_LEFT_ALT | HID_KEYBOARD_T);
        furi_delay_ms(3500);
        nkb_tap(HID_KEYBOARD_RETURN);
        furi_delay_ms(400);
        nkb_type("cat > /tmp/nikita_bridge.py <<'NIKITA_EOF'\n");
        nkb_type(NIKITA_POSIX_PAYLOAD);
        nkb_type("NIKITA_EOF\n");
        nkb_type("nohup python3 /tmp/nikita_bridge.py >/tmp/nikita_bridge.log 2>&1 &\n");
    } else {
        // macOS: Spotlight -> Terminal
        nkb_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_SPACEBAR);
        furi_delay_ms(700);
        nkb_type("Terminal");
        furi_delay_ms(600);
        nkb_tap(HID_KEYBOARD_RETURN);
        furi_delay_ms(4500);
        nkb_tap(HID_KEYBOARD_RETURN);
        furi_delay_ms(250);
        nkb_tap(HID_KEYBOARD_RETURN);
        furi_delay_ms(700);
        nkb_type("cat > /tmp/nikita_bridge.py <<'NIKITA_EOF'\n");
        nkb_type(NIKITA_POSIX_PAYLOAD);
        nkb_type("NIKITA_EOF\n");
        nkb_type("nohup python3 /tmp/nikita_bridge.py >/tmp/nikita_bridge.log 2>&1 &\n");
    }

    furi_delay_ms(400);
    furi_hal_usb_set_config(prev, NULL); // hand the serial link back
    return 0;
}
