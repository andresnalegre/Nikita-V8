#pragma once

// The bridge, embedded so `nikita install flipper-bridge` can type it into a
// zeroed machine and run it -- no download, no fourth folder, nothing for the
// user to install. It is deliberately tiny: every line here is typed one
// keystroke at a time over USB HID, so this is the mailbox loop and nothing
// more. The full-featured bridge (WebSocket, host allow-list, tokens) still
// lives in nikita-flipper-bridge for a set-up machine; this is the pocket
// version that bootstraps itself.
//
// It waits for the serial port to come back (the Flipper drops it while typing
// as a keyboard, then restores it), then serves the SD-card mailbox: read a
// base64 request the phone left over BLE, run it -- "host ..." on this machine,
// anything else on the Flipper's own shell -- and write the answer back.
//
// macOS today: the port glob and `python3` are Apple-shaped. Linux and Windows
// come later, each with its own opener in the installer.

static const char* const NIKITA_BRIDGE_PAYLOAD =
    "import glob,os,time,select,termios,base64,subprocess\n"
    "def port():\n"
    " while 1:\n"
    "  g=sorted(glob.glob('/dev/cu.usbmodemflip_*') or glob.glob('/dev/cu.usbmodem*'))\n"
    "  if g:\n"
    "   try:return op(g[0])\n"
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
