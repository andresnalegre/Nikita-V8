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

def font(size):
    return ImageFont.load_default(size=size)


# The bubble on frame 1 is only 87px wide -- that is the Flipper's own layout,
# and the dolphin sits immediately to its right -- so its text is set smaller to
# fit, the way upstream's narrower face did.
BODY, SMALL, TITLE = 10, 9, 12

# Per frame: rectangles to blank, then text lines to draw at (x, y).
# Rectangles are (x0, y0, x1, y1) inclusive.
SCRIPT = [
    {   # "UNLEASHED / Firmware / Updated" -- dolphin and [next] kept
        "clear": [(56, 0, 127, 43)],
        "text":  [((60, 1),  "NIKITA-V8", TITLE),
                  ((60, 16), "Firmware", BODY),
                  ((60, 28), "Updated", BODY)],
    },
    {   # The speech bubble. The dolphin's leftmost ink is column 90, so the
        # bubble may use everything to its left -- a little taller than
        # upstream's (rows 15-47) to keep the text at a legible size. At this
        # width three lines only fit set smaller, and smaller than this turns
        # to mud once the frame is reduced to 1-bit.
        "clear": [(0, 10, 91, 54)],
        "bubble": (1, 12, 89, 51),
        "text":  [((4, 16), "Nikita is FREE", SMALL),
                  ((4, 28), "& OpenSource", SMALL),
                  ((4, 40), "Beware of scams", SMALL)],
    },
    {   # Discord logo + upstream URL. The sea decoration resumes on row 43,
        # so the cut stops at 42 and leaves the crab and shells whole.
        "clear": [(0, 0, 127, 42)],
        "text":  [((14, 6),  "One agent, three", BODY),
                  ((14, 18), "ways in: phone,", BODY),
                  ((14, 30), "desktop and CLI", BODY)],
    },
    {   # "github.com / DarkFlippers" -- cat art and [OK] kept
        "clear": [(36, 0, 127, 44)],
        "text":  [((38, 8),  "github.com/", BODY),
                  ((38, 22), "andresnalegre", BODY)],
    },
]


def draw_text(draw, xy, text, size):
    draw.text(xy, text, font=font(size), fill=BLACK)


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

        for xy, text, size in spec["text"]:
            width = draw.textlength(text, font=font(size))
            limit = spec["bubble"][2] - 1 if "bubble" in spec else W
            if xy[0] + width > limit:
                raise SystemExit(
                    f"frame {index}: {text!r} is {int(width)}px wide and starts "
                    f"at x={xy[0]}, running {int(xy[0] + width - limit)}px past "
                    f"x={limit} -- shorten it or set a smaller size"
                )
            draw_text(draw, xy, text, size)

        # The panel is 1-bit; keep the files that way so the packer sees what
        # the screen will.
        img.convert("1").save(f"{OUT}/frame_{index:02d}.png", optimize=True, bits=1)
        print(f"frame_{index:02d}: redrawn")


if __name__ == "__main__":
    main()
