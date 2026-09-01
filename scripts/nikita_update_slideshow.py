#!/usr/bin/env python3
"""Draw the post-update slideshow -- the four frames a Nikita device shows
right after it finishes flashing.

These are assets/slideshow/update_default/, packed into splash.bin by
scripts/update.py. They are a different set from first_start/ (shown on a
factory-fresh device), which is why rebranding that one left these still
carrying the upstream project's wording and links.

Everything here is drawn from primitives -- nothing is inherited from
upstream's frames. Run it after editing:

    python3 scripts/nikita_update_slideshow.py

Needs Pillow. It writes the four PNGs outright, so it has no source images to
get out of step with and is safe to re-run.

The panel is 128x64 and one bit deep: no greys, no anti-aliasing that survives.
Keep strokes on whole pixels and text at size 9 or above, or it turns to mud.
"""
from PIL import Image, ImageDraw, ImageFont
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get(
    "OUT", os.path.join(HERE, "..", "assets", "slideshow", "update_default")
)

W, H = 128, 64
BLACK, WHITE = 0, 1


def font(size):
    return ImageFont.load_default(size=size)


def text_w(draw, s, size):
    return draw.textlength(s, font=font(size))


def centered(draw, y, s, size, fill=BLACK):
    draw.text(((W - text_w(draw, s, size)) / 2, y), s, font=font(size), fill=fill)


def button(draw, x1, y1, label):
    """A press-me chip, bottom-aligned at its right edge (x1, y1)."""
    size = 10
    pad_x, pad_y = 5, 3
    tw = text_w(draw, label, size)
    w = int(tw) + pad_x * 2 + 7          # +7 for the arrow
    h = 15
    x0, y0 = x1 - w, y1 - h
    draw.rounded_rectangle([x0, y0, x1, y1], radius=3, fill=WHITE, outline=BLACK)
    draw.text((x0 + pad_x, y0 + pad_y), label, font=font(size), fill=BLACK)
    # Solid triangle, drawn by hand so it stays crisp at one bit.
    ax = x0 + pad_x + int(tw) + 2
    ay = y0 + h // 2
    for i in range(5):
        draw.line([ax + i, ay - 4 + i, ax + i, ay + 4 - i], fill=BLACK)


def phone(draw, cx, top):
    draw.rounded_rectangle([cx - 7, top, cx + 7, top + 20], radius=2, outline=BLACK)
    draw.rectangle([cx - 5, top + 3, cx + 5, top + 15], outline=BLACK)
    draw.line([cx - 2, top + 18, cx + 2, top + 18], fill=BLACK)


def laptop(draw, cx, top):
    draw.rectangle([cx - 9, top + 2, cx + 9, top + 14], outline=BLACK)
    draw.line([cx - 13, top + 18, cx + 13, top + 18], fill=BLACK)
    draw.line([cx - 9, top + 14, cx - 13, top + 18], fill=BLACK)
    draw.line([cx + 9, top + 14, cx + 13, top + 18], fill=BLACK)


def terminal(draw, cx, top):
    draw.rectangle([cx - 10, top + 1, cx + 10, top + 17], outline=BLACK)
    draw.rectangle([cx - 10, top + 1, cx + 10, top + 5], fill=BLACK)
    draw.text((cx - 7, top + 6), ">", font=font(9), fill=BLACK)
    draw.rectangle([cx - 1, top + 12, cx + 5, top + 13], fill=BLACK)


def speech_mark(draw, x, y):
    """A small chat bubble with three dots -- the agent, as a mark."""
    draw.rounded_rectangle([x, y, x + 26, y + 19], radius=4, outline=BLACK)
    draw.polygon([(x + 6, y + 19), (x + 12, y + 19), (x + 7, y + 25)], outline=BLACK)
    draw.line([(x + 6, y + 19), (x + 12, y + 19)], fill=WHITE)
    for i in range(3):
        cx = x + 7 + i * 6
        draw.rectangle([cx, y + 9, cx + 2, y + 11], fill=BLACK)


def frame_00(draw):
    # A knocked-out wordmark: the update announcing itself.
    draw.rectangle([0, 3, W - 1, 25], fill=BLACK)
    label, size = "NIKITA", 16
    draw.text(((W - text_w(draw, label, size)) / 2, 5), label,
              font=font(size), fill=WHITE)
    centered(draw, 30, "V8  firmware updated", 10)
    button(draw, W - 3, H - 2, "next")


def frame_01(draw):
    # The agent, as it is actually reached: a shell prompt.
    draw.rectangle([3, 4, W - 4, 43], outline=BLACK)
    draw.rectangle([4, 5, W - 5, 16], fill=BLACK)
    draw.text((7, 5), "nikita", font=font(9), fill=WHITE)
    draw.text((7, 19), "> nikita", font=font(10), fill=BLACK)
    draw.rectangle([57, 21, 62, 29], fill=BLACK)     # cursor block
    draw.text((7, 30), "your agent is aboard", font=font(9), fill=BLACK)
    button(draw, W - 3, H - 2, "next")


def frame_02(draw):
    centered(draw, 0, "Three ways in", 10)
    for cx, icon, label in ((20, phone, "phone"),
                            (58, laptop, "desktop"),
                            (96, terminal, "CLI")):
        icon(draw, cx, 13)
        # Labels end at y=46 and the chip starts at 48; any lower and "CLI"
        # runs under it.
        draw.text((cx - text_w(draw, label, 9) / 2, 34), label,
                  font=font(9), fill=BLACK)
    button(draw, W - 3, H - 2, "next")


def frame_03(draw):
    speech_mark(draw, 6, 12)
    draw.text((42, 14), "github.com/", font=font(10), fill=BLACK)
    draw.text((42, 27), "andresnalegre", font=font(10), fill=BLACK)
    button(draw, W - 3, H - 2, "ok")


FRAMES = [frame_00, frame_01, frame_02, frame_03]


def main():
    os.makedirs(OUT, exist_ok=True)
    for index, render in enumerate(FRAMES):
        img = Image.new("1", (W, H), WHITE)
        draw = ImageDraw.Draw(img)
        render(draw)
        img.save(f"{OUT}/frame_{index:02d}.png", optimize=True, bits=1)
        print(f"frame_{index:02d}: drawn")


if __name__ == "__main__":
    main()
