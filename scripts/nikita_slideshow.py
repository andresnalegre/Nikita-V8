#!/usr/bin/env python3
"""Redraw the first-start slideshow so the Flipper's welcome is Nikita's.

Run it after editing SCRIPT below; it rewrites assets/slideshow/first_start/,
which the build packs into the .slideshow shown on a device's first boot:

    python3 scripts/nikita_slideshow.py

Needs Pillow. Reads assets/slideshow/first_start/.original/, never its own
output, so it is safe to run repeatedly.

The dolphin art is kept -- it is the Flipper's own mascot, and the frames were
drawn around it -- and only the speech bubbles are replaced, so the result still
looks native rather than pasted on.

The cut between art and bubble is made in the blank gap that already separates
them, found per frame rather than guessed. Cutting anywhere else severs the
line joining the bubble to the dolphin's hand and leaves the far half of it
floating; cutting in the gap severs nothing.
"""
from PIL import Image, ImageDraw, ImageFont
import os

# The pristine frames this is derived from. Reading the generated frames back
# in would composite a bubble onto a bubble, so the source is kept separate.
HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(HERE, "..", "assets", "slideshow", "first_start")
SRC = os.path.join(FRAMES, ".original")
OUT = os.environ.get("OUT", FRAMES)

W, H = 128, 64
BLACK, WHITE = 0, 1

font = ImageFont.load_default(size=10)

# Which side of the frame the speech bubble sits on, and what it now says.
SCRIPT = [
    ("right", ["Hi, I'm", "Nikita >"]),
    ("left",  ["I live in", "your", "Flipper >"]),
    ("right", ["Reach me", "from phone,", "Mac or CLI >"]),
    ("right", ["Type", "`nikita`", "in the", "shell >"]),
    ("left",  ["I remember", "on the SD", "card, not", "the cloud >"]),
    ("left",  ["Nothing", "leaves your", "device."]),
]

# frame_04's dolphin holds the bubble with no blank column anywhere between
# them, so there is no gap to find and the cut column is stated outright. It is
# placed past the whole connector rather than through it.
EXPLICIT_CUT = {4: (0, 79)}


def blank_columns(img):
    px = img.load()
    return [
        x for x in range(W)
        if all(px[x, y] for y in range(H))
    ]


def bubble_region(img, index, side):
    """The x-range to clear: the bubble's side, up to the gap beside it."""
    if index in EXPLICIT_CUT:
        return EXPLICIT_CUT[index]

    blanks = set(blank_columns(img))
    runs, start = [], None
    for x in range(W):
        if x in blanks:
            if start is None:
                start = x
        elif start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, W - 1))

    interior = [r for r in runs if r[0] > 10 and r[1] < W - 10]
    if not interior:
        raise SystemExit(f"frame {index}: no gap between art and bubble")
    gap = max(interior, key=lambda r: r[1] - r[0])

    # Clear the bubble's side up to and including the gap, so the cut lands on
    # blank pixels and the art keeps every stroke it had.
    return (0, gap[1]) if side == "left" else (gap[0], W - 1)


def draw_bubble(draw, box, lines):
    x0, y0, x1, y1 = box
    # The originals carry a one-pixel drop shadow on the right and bottom.
    draw.rectangle([x0 + 1, y0 + 1, x1 + 1, y1 + 1], fill=BLACK)
    draw.rectangle([x0, y0, x1, y1], fill=WHITE, outline=BLACK)
    y = y0 + 3
    for line in lines:
        draw.text((x0 + 4, y), line, font=font, fill=BLACK)
        y += 10


def main():
    os.makedirs(OUT, exist_ok=True)
    for index, (side, lines) in enumerate(SCRIPT):
        img = Image.open(f"{SRC}/frame_{index:02d}.png").convert("1")
        sx0, sx1 = bubble_region(img, index, side)

        draw = ImageDraw.Draw(img)
        draw.rectangle([sx0, 0, sx1, H - 1], fill=WHITE)

        text_w = max(draw.textlength(line, font=font) for line in lines)
        bubble_w = int(text_w) + 9
        bubble_h = len(lines) * 10 + 5

        free = sx1 - sx0 + 1
        if bubble_w + 2 > free:
            raise SystemExit(
                f"frame {index}: bubble needs {bubble_w + 2}px, "
                f"only {free}px free -- shorten the text"
            )

        bx0 = sx0 + 1 if side == "left" else sx1 - bubble_w - 1
        by0 = (H - bubble_h) // 2

        draw_bubble(draw, (bx0, by0, bx0 + bubble_w, by0 + bubble_h), lines)
        img.save(f"{OUT}/frame_{index:02d}.png", optimize=True, bits=1)
        print(
            f"frame_{index:02d}: {side}, cleared x{sx0}-{sx1}, "
            f"bubble {bubble_w}x{bubble_h} at x={bx0}"
        )


if __name__ == "__main__":
    main()
