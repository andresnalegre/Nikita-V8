# Nikita bridge

> **This lives with Nikita, not in a separate folder.** The one canonical copy is
> `Nikita-V8/assets/resources/nikita/bridge.py`, which ships to the Flipper's SD at
> **`/ext/nikita/bridge.py`** on every flash. The old standalone `nikita-flipper-bridge`
> repo was deleted; do not recreate it. To bring the bridge up on a computer, use
> the Flipper's own `bridge.install` / `nikita install flipper-bridge` (they type
> THIS file in) — never download a bridge from anywhere else.

## Multi-client hub — CONFIRMED WORKING (2026-09-28)

The WebSocket mode is already a **multi-client hub**: the accept loop spawns a
thread per connection and serialises them on the single serial port with a lock.
Verified on hardware: TWO WebSocket clients connected at once to one Flipper, each
running host (`host os`) and Flipper CLI (`storage list ...`) commands
concurrently, both correct, no cross-talk.

So the "2 computers + 1 phone + 1 Flipper" topology works TODAY without any
firmware change: the computer the Flipper is cabled to runs `bridge.py`; every
other machine connects to it over WiFi. This sidesteps the BLE limit entirely
(the light radio stack allows only ONE BLE link at a time — see
`targets/f7/ble_glue/gap.c`).

## Roadmap: ESP32 as the standalone WiFi hub

Goal: remove the host-computer dependency — the Flipper + its GPIO ESP32 become a
self-contained, networked, multi-client Nikita hub, with Marauder intact.
- **Phase 1 (Flipper):** expose the Flipper CLI/RPC over the GPIO UART (a 3rd
  transport beside USB-CDC and BLE-serial).
- **Phase 2 (ESP32):** one firmware that keeps ALL Marauder AND runs a
  TCP/WebSocket server bridging WiFi↔UART↔Flipper (the same line protocol this
  `bridge.py` speaks), multiplexed with the Marauder channel on the shared UART.
- **Phase 3 (clients):** a "connect via WiFi" transport in the Nikita apps
  pointing at the ESP32's IP.

Until Phase 2/3 land, the computer-hosted hub above is the working path.

---

_Original bridge README follows._

# nikita-flipper-bridge

Hands the phone (or qFlipper) the Flipper's real CLI, and a shell on the
computer the Flipper is plugged into — over Bluetooth.

**One script, every OS.** macOS, Windows and Linux, no edits. It works out
which system it is running on, finds the Flipper on its own (by USB vendor id,
falling back to per-OS device names), and if the cable is pulled or qFlipper
grabs the port it waits and reconnects instead of dying.

Bluetooth cannot carry the Flipper's text shell; USB can. This runs on the
computer holding the Flipper on USB and exposes that shell over a WebSocket,
so the iPhone app's **MACHINE** channel reaches `subghz`, `nfc`, `gpio`, `ir`,
`led`, `vibro`, `js` and everything else the firmware offers — none of which
the BLE channel can do.

## Validating the plugged-in system

Before it runs anything, the caller can ask the bridge exactly what machine it
is talking to — the ground truth, straight from the running interpreter:

```bash
python3 bridge.py --hostinfo     # print this computer's OS and exit
```

and over the live link: `hostinfo` (full report) or `host os` (one line, e.g.
`macOS 26.5.2 (arm64)`). The bridge adapts to that OS on its own — serial
backend, device discovery and the shell it runs `host` commands in all follow
the detected system.

## Serial on every OS

Uses **pyserial** if it is installed (the good path everywhere, and the way in
on Windows without extra work: `pip install pyserial`). Otherwise a built-in
backend — `termios` on macOS/Linux, the Win32 API through `ctypes` on Windows —
so a stock Python 3 still connects.

```
iPhone  ──WebSocket──►  this bridge  ──USB serial──►  Flipper
```

## Running it

The way that needs no network at all -- the phone reaches the bridge
**through the Flipper's SD card over Bluetooth**, no WiFi, no address to type:

```bash
python3 bridge.py --mailbox --allow-host
```

The phone leaves a command in a mailbox file on the card (over BLE), the bridge
picks it up over USB, runs it here, and leaves the answer back. In the app:
**Tools -> CLI**, then `ls /some/path` for this computer, `fls /ext` for the
Flipper. No `connect` needed.

There is also a WebSocket mode, for a phone and computer on the same WiFi:

```bash
python3 bridge.py
```

No packages to install: the WebSocket framing and the serial setup are both
done in the script, so a stock Python 3 is enough.

It prints the address to type into the app:

```
[bridge] Flipper on /dev/cu.usbmodemflip_Ut4me1
[bridge] connect ws://192.168.0.10:8765
```

In the app: **Tools → CLI**, then `channel machine`, then that `connect` line.

**qFlipper holds the same serial port.** Press RELEASE PORT in it, or quit it,
before starting the bridge.

## Seeing the computer

The phone can also ask about the machine the Flipper is plugged into — the
thing it has no other way to see:

```
host ls ~/Desktop
host cat /etc/hosts
host df -h
```

This is **off by default**, because it is a shell on a network socket:

```bash
python3 bridge.py --allow-host
python3 bridge.py --allow-host --token hunter2   # ask for a secret first
```

With `--token`, the first thing a client must send is `token hunter2`;
anything else is refused until it does. Without one, anyone who can reach port
8765 can run commands as you. Use a token on any network you do not control,
and prefer leaving `--allow-host` off entirely when you only want the Flipper.

## Commands

| Typed in the app | Goes to |
|---|---|
| anything | the Flipper's shell, verbatim |
| `host <cmd>` | this computer (needs `--allow-host`) |
| `hostinfo` / `host os` | the validated OS of THIS computer (always allowed) |
| `bridge` | the bridge itself: OS, port, serial backend, host on/off |
| `token <secret>` | authorises the session when `--token` is set |

## Options

| Flag | Meaning |
|---|---|
| `--port` | serial device / COM port (default: autodetect, any OS) |
| `--host` | bind address (default `0.0.0.0`) |
| `--listen` | bind port (default `8765`) |
| `--allow-host` | answer `host <cmd>` |
| `--token` | require `token <secret>` before anything else |
| `--mailbox` | serve through the SD card over BLE, no network |
| `--hostinfo` | print this computer's validated OS and exit |

## Running it in the background (a service you own)

To have the bridge always up with **no terminal window**, install it as a
login service on your own machine. You install it knowingly, you can see it,
and you remove it in one line -- it is yours, not hidden from you. `--allow-host`
is on, so a paired Flipper/phone can run shell commands on this machine; only
install it where you want that.

```bash
sh service/install-mac.sh                                   # macOS  (LaunchAgent)
sh service/install-linux.sh                                 # Linux  (systemd --user)
powershell -ExecutionPolicy Bypass -File service\install-windows.ps1   # Windows (Scheduled Task, pythonw)
```

Each copies `bridge.py` to `~/.nikita/bridge.py`, registers the service, and
starts it. Remove it with the one-liner the installer prints.

## Bootstrapping a bare machine (`nikita install flipper-bridge`)

On a computer that has nothing set up yet, the Flipper can type this bridge in
itself — it becomes a USB keyboard, opens Terminal, writes a pocket version of
the mailbox bridge to `/tmp/nikita_bridge.py` and starts it. Trigger it from the
Flipper's own CLI:

1. Plug the Flipper in over USB.
2. Open the CLI with `screen /dev/cu.usbmodemflip*` — **not** qFlipper if you
   can help it. `screen` releases the serial port on its own the instant the
   Flipper flips to keyboard mode; qFlipper reconnects and grabs it back, and
   then the typed bridge cannot open the port it needs. With qFlipper you must
   press **RELEASE PORT** right after issuing the command.
3. Type `nikita install flipper-bridge` and leave the keyboard/mouse alone for
   ~30s while it types.

The pocket bridge waits for the serial port to come back (the Flipper drops it
while acting as a keyboard) and retries the open, so a brief busy moment is
fine. For a set-up machine, prefer running `bridge.py` here directly.
