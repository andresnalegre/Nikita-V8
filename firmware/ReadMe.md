# The update feed

`directory.json` is what [nikita-qflipper](https://github.com/andresnalegre/nikita-qflipper)
and [Nikita-iOS](https://github.com/andresnalegre/Nikita-iOS) fetch to find out
which Nikita firmware exists. Both clients point their **main** update path at
it, so Nikita is what they offer by default; every other firmware (Official,
Momentum, Unleashed, RogueMaster, ARF, Xero) stays one click away in their
firmware-store panel.

    https://raw.githubusercontent.com/andresnalegre/Nikita-V8/main/firmware/directory.json

It is the same format Flipper Devices' own update server serves, which is why
neither client needed a new protocol to read it.

## Do not edit it by hand

`.github/workflows/release.yml` regenerates it on every `nkt-*` tag, after the
GitHub release exists, and commits it back here. Editing it by hand means the
next release overwrites the edit — or, worse, that it names a download which
does not exist.

The channel comes from the tag:

| Tag | Channel |
|---|---|
| `nkt-001` | `release` |
| `nkt-001-rc` | `release-candidate` |
| `nkt-001-dev` | `development` |

## Empty channels

A channel with `"versions": []` means nothing has been published to it yet. The
clients read that as "no Nikita release on this channel", which is a true and
quiet answer — unlike a missing file, which they can only report as a network
failure.
