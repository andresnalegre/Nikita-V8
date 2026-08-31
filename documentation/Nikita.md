# Nikita on the device

Nikita is one assistant reachable from three clients. This page is about the
part that lives in the firmware.

```
iPhone  (nikita-IOS) ──BLE, RPC──────────────────────┐
                                                     ├──> Flipper Zero
desktop (nikita-qflipper) ──USB, text CLI────────────┤    (this firmware)
                                                     │
iPhone ──WebSocket──> bridge.py ──USB, text CLI──────┘
```

Bluetooth carries RPC but not the text CLI, so the phone gets file/screen/button
tools while the cable gets the whole shell. Both end up here.

## `nikita`

A CLI command shaped for a program rather than a person: one call, parseable
output, and it runs while an app is open (`CliCommandFlagParallelSafe`).

```
nikita [info]               device snapshot
nikita init                 create /ext/nikita
nikita memory               list remembered facts
nikita memory add <text>    remember one fact
nikita memory forget <n>    drop fact number <n>
nikita memory clear         drop all of them
```

### `nikita info`

```
firmware_origin  : Nikita-V8
firmware_version : local
firmware_branch  : dev
firmware_commit  : a1b2c3d4
firmware_build   : 31-08-2026
device_name      : Nikita
hardware_target  : f7
api_version      : 86.0
battery_pct      : 74
charging         : no
bluetooth        : on
sd_total_kb      : 31234560
sd_free_kb       : 29110272
nikita_dir       : /ext/nikita
```

Every line is `key`, padded, `: `, value. An agent can split on the first colon
and stop guessing. `sd_total_kb`/`sd_free_kb` read `none` when no card is
mounted; `nikita_dir` reads `missing` until `nikita init` has run.

### Memory

`/ext/nikita/memory.txt`, one fact per line, at most 64 lines of 256
characters. It lives on the card rather than in a client so that a fact
remembered on the phone is the same fact the desktop reads back — before this,
nikita-IOS kept memory in the phone and nikita-qflipper kept it on the
computer, and neither could see the other's.

`forget` writes the surviving lines to `memory.txt.tmp`, removes the original
and renames, so a power loss mid-write costs at most the one edit.

Blank lines are skipped, and the numbering `list` prints is the numbering
`forget` takes.

## Storage layout

```
/ext/nikita/
  memory.txt      durable user facts
  artifacts/      what the agent writes for you
  scripts/        what the agent writes to be run
```

`nikita init` creates all three; `nikita memory add` creates them on demand.

## Where the code is

- `applications/services/cli/commands/nikita.c` — the command
- `applications/services/cli/application.fam` — the `cli_nikita` plugin entry

It builds as an external CLI plugin (`cli_nikita.fal`), deployed to
`/ext/apps_data/cli/plugins/` with the rest of the firmware resources — so the
SD card has to be present and up to date for `nikita` to appear in `help`.

## PARASITE — live host navigation

The Remote Control in nikita-IOS already sends the phone's D-pad to whatever app
is open on the Flipper, over BLE RPC. What was missing was an app on the device
that turns those button events into **live USB keystrokes to the host** — so the
phone drives the host's file explorer and you read the result on the host's own
monitor. The USB link never has to send output back: the monitor is the return
channel.

```
iPhone Remote (D-pad) ──BLE RPC──> Flipper ──USB HID──> host file explorer
                                                        you watch the host screen
```

This is not BadUSB. BadUSB plays a fixed script blind. PARASITE is live and
visual — the Flipper rides the host's own USB keyboard interface and each press
moves the selection while you watch.

### Where it lives

`USB Keyboard & Mouse` app → **PARASITE**
(`applications/system/hid_app`, view `hid_parasite`). It ships in both the USB
build (`hid_usb`) and the BLE build (`hid_ble`); for driving a host over the
cable, use the USB one.

| Button | Sends | In a file explorer |
|---|---|---|
| Up / Down / Left / Right | arrow keys | move the selection |
| OK | Enter | open / go into the folder |
| Back (short) | Backspace | up one folder / back |
| Back (hold) | — | detach PARASITE |

The physical D-pad and the iOS Remote drive it identically, because the Remote
injects the same `InputEvent`s the view reads. **No change to nikita-IOS is
needed** — point its Remote Control at this app and navigate.

### Using it from the phone

1. Plug the Flipper into the host over USB.
2. On the Flipper (or via the phone), open `USB Keyboard & Mouse` → `PARASITE`.
3. Bring the host's file explorer to the foreground and click once into its file
   list so it has keyboard focus.
4. Drive with the iPhone Remote while watching the host monitor.
