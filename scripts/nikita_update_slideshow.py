#!/usr/bin/env python3
"""Redraw the post-update slideshow so what a Nikita device says after
flashing is Nikita's.

This is the one shown right after an update completes -- assets/slideshow/
update_default/, packed into splash.bin by scripts/update.py. It is a
different set from first_start/ (the one shown on a factory-fresh device),
which is why rebranding that one left these four frames still saying
"UNLEASHED" and pointing at the upstream project's Discord and GitHub.

Run it after editing SCRIPT below:

    python3 scripts/nikita_update_slideshow.py

Needs Pillow. Reads update_default/.original/, never its own output, so it
is safe to run repeatedly.

The Flipper's own dolphin and the sea decorations are kept -- they belong to
the device, not to any fork -- and only the wording and the links change.
"""
from PIL import Image, ImageDraw, ImageFont
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(HERE, "..", "assets", "slideshow", "update_default")
SRC = os.path.join(FRAMES, ".original")
OUT = os.environ.get("OUT", FRAMES)

W, H = 128, 64
BLACK, WHITE = (0, 0, 0), (255, 255, 255)

font = ImageFont.load_default(size=10)

# Per frame: rectangles to blank, then text lines to draw at (x, y).
# Rectangles are (x0, y0, x1, y1) inclusive.
SCRIPT = [
    {   # "UNLEASHED / Firmware / Updated" -- dolphin and [next] kept
        "clear": [(56, 0, 127, 43)],
        "text":  [((60, 2),  "NIKITA-V8", True),
                  ((60, 15), "Firmware", False),
                  ((60, 27), "Updated", False)],
    },
    {   # the speech bubble -- dolphin and [next] kept
        "clear": [(0, 6, 110, 50)],
        "bubble": (1, 8, 108, 46),
        "text":  [((5, 11), "Nikita software -", False),
                  ((5, 23), "FREE & OpenSource", False),
                  ((5, 35), "Beware of scammers", False)],
    },
    {   # Discord logo + upstream URL -- crab, shells and [next] kept
        "clear": [(0, 0, 127, 44)],
        "text":  [((14, 6),  "One agent, three", False),
                  ((14, 19), "ways in: phone,", False),
                  ((14, 30), "desktop and CLI", False)],
    },
    {   # "github.com / DarkFlippers" -- cat art and [OK] kept
        "clear": [(36, 0, 127, 44)],
        "text":  [((38, 8),  "github.com/", False),
                  ((38, 22), "andresnalegre", False)],
    },
]


def draw_text(draw, xy, text, bold):
    draw.text(xy, text, font=font, fill=BLACK)
    if bold:
        # The font has no bold face; one pixel of horizontal smear is what
        # upstream's title weight looks like at this size.
        draw.text((xy[0] + 1, xy[1]), text, font=font, fill=BLACK)


def main():
    os.makedirs(OUT, exist_ok=True)
    for index, spec in enumerate(SCRIPT):
        img = Image.open(f"{SRC}/frame_{index:02d}.png").convert("RGB")
        draw = ImageDraw.Draw(img)

        for box in spec["clear"]:
            draw.rectangle(list(box), fill=WHITE)

        if "bubble" in spec:
            x0, y0, x1, y1 = spec["bubble"]
            draw.rectangle([x0 + 1, y0 + 1, x1 + 1, y1 + 1], fill=BLACK)
            draw.rectangle([x0, y0, x1, y1], fill=WHITE, outline=BLACK)

        for xy, text, bold in spec["text"]:
            width = draw.textlength(text, font=font)
            if xy[0] + width > W:
                raise SystemExit(
                    f"frame {index}: {text!r} is {int(width)}px wide and starts "
                    f"at x={xy[0]}, running {int(xy[0] + width - W)}px off the "
                    "128px screen -- shorten it"
                )
            draw_text(draw, xy, text, bold)

        # The panel is 1-bit; keep the files that way so the packer sees what
        # the screen will.
        img.convert("1").save(f"{OUT}/frame_{index:02d}.png", optimize=True, bits=1)
        print(f"frame_{index:02d}: redrawn")


if __name__ == "__main__":
    main()
