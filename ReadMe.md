# Nikita-V8 Firmware

The Flipper Zero half of the **Nikita** ecosystem, versioned **Nikita-V8**: a fork of
[Unleashed](https://github.com/DarkFlippers/unleashed-firmware), which is itself a fork of
[the official firmware](https://github.com/flipperdevices/flipperzero-firmware), carrying
everything those two provide plus the pieces the Nikita agents need on the device
itself.

> [!WARNING]
> Experimental purposes only. Not affiliated with Flipper Devices, and not
> affiliated with the Unleashed team either — this is a personal fork.

## The ecosystem

Nikita is one assistant reachable from three places, all pointing at the same
Flipper:

| Piece | Link to the device | What it drives |
|---|---|---|
| [nikita-IOS](https://github.com/andresnalegre/nikita-IOS) | Bluetooth (RPC) | SD-card files, framebuffer, D-pad, app open/close |
| **nikita-qflipper** | USB (serial CLI) | the full text CLI, plus the host computer |
| **nikita-flipper-bridge** | USB → WebSocket | hands the phone the real CLI, through a computer |
| **this firmware** | — | the device side all three land on |

Bluetooth cannot carry the text CLI; USB can. That split is why the phone gets
screen-and-buttons tools and the desktop gets `run_cli`.

## What this firmware adds

### The `nikita` command

A CLI command built for the agents rather than for a person, so a question that
used to cost five round-trips costs one.

```
nikita info                 # one parseable snapshot of the device
nikita init                 # create /ext/nikita on the SD card
nikita memory               # list remembered facts
nikita memory add <text>    # remember one fact
nikita memory forget <n>    # drop fact number <n>
nikita memory clear         # drop all of them
```

`nikita info` prints `key : value` lines — firmware origin, version, branch,
commit, build date, device name, hardware target, API version, battery, charge
state, Bluetooth state, SD total/free, and whether `/ext/nikita` exists.

**Memory lives on the card, not on the client.** `/ext/nikita/memory.txt` is one
fact per line, capped at 64 lines of 256 characters. A fact remembered from the
phone is the same fact the desktop reads back, because it never left the device.
`nikita memory forget` rewrites through a temporary file and swaps, so losing
power mid-write doesn't take the whole memory with it.

The command is `CliCommandFlagParallelSafe`, so it answers while an app is open.

### Storage layout

`nikita init` (or the first `nikita memory add`) creates:

```
/ext/nikita/            memory.txt
/ext/nikita/artifacts/  what the agent writes for you
/ext/nikita/scripts/    what the agent writes to be run
```

### Identity

`FIRMWARE_ORIGIN` is `Nikita-V8`, so `device_info`, the JS SDK's vendor string
(`"nikita"`) and the About screen all report this firmware as itself rather than
as Unleashed.

## Install

Same as upstream — [Installation Guide](/documentation/HowToInstall.md),
[How to build](/documentation/HowToBuild.md#how-to-build-by-yourself).

```bash
./fbt COMPACT=1 DEBUG=0 updater_package
```

The build reads the version from git, so the working copy has to be a git
repository.

## Everything else

Every Unleashed feature is here untouched — the Sub-GHz protocol work, the
extra frequencies, BadKB, the NFC parsers, the plugin set, the whole feature
list. That list, the upstream credits, and the upstream team's donation links
are kept in [ReadMe.Unleashed.md](/ReadMe.Unleashed.md). If this firmware is
useful to you, the people to support are the ones who wrote the parts it is
built on.

## Project structure

- `applications`    - Applications and services used in firmware
- `assets`          - Assets used by applications and services
- `furi`            - Furi Core: OS-level primitives and helpers
- `debug`           - Debug tool: GDB-plugins, SVD-file and etc
- `documentation`   - Documentation generation system configs and input files
- `firmware`        - Firmware source code
- `lib`             - Our and 3rd party libraries, drivers and etc...
- `site_scons`      - Build helpers
- `scripts`         - Supplementary scripts and python libraries home

## Links

- Unleashed (upstream): [github.com/DarkFlippers/unleashed-firmware](https://github.com/DarkFlippers/unleashed-firmware)
- Official docs: [docs.flipper.net](https://docs.flipper.net)
- Developer docs: [developer.flipper.net](https://developer.flipper.net/flipperzero/doxygen)

## License

GPL-3.0, unchanged from upstream. See [LICENSE](/LICENSE).
