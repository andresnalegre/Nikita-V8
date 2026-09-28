# klgen — Flipper .kl layout generator (from kbdlayout.info)

Generates Flipper BadUSB keyboard-layout files (.kl) from the machine-readable
XML at https://kbdlayout.info . This is how Nikita covers keyboards beyond the
built-in set, without hand-authoring: kbdlayout.info has ~200 Windows layouts.

## .kl format
256 bytes = 128 little-endian uint16, indexed by ASCII (0..127). Each entry is
`(HID_modifier << 8) | HID_usage`. Modifier byte: 0x02 = Shift, 0x40 = AltGr
(Right-Alt), 0x42 = Shift+AltGr. Verified byte-exact against the shipped
en-US.kl (0 diffs on all 95 printables).

## Use
    curl -sL -o LAYOUT.xml "https://kbdlayout.info/<KBID>/download/xml"
    python3 klgen.py <KBID> out.kl LAYOUT.xml
Then drop out.kl into /ext/badusb/assets/layouts/ (or the firmware resources).

## Known limitation
Dead keys (^ ` ~ and diacritics) on layouts that make them dead are left
unmapped — they need a combining keypress a one-shot HID can't do cleanly. The
whole alphanumeric + common-punctuation block is byte-accurate.
