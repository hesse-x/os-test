#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# gen_cursors.py — generate the compositor's MIT-licensed macOS-style cursor
# set as 32x32 RGBA PNGs.
#
# These are ORIGINAL artwork drawn from scratch in this script. They are NOT
# derived from any third-party cursor pack (no apple_cursor / ful1e5 assets are
# used). The shapes are generic cursor glyphs (arrow, I-beam, hand, resize
# arrows) drawn as simple polygons; only the visual style (white fill + thin
# black outline) evokes a macOS look, which is an uncopyrightable style.
#
# Pipeline: draw every cursor as a union of filled primitives on a 256x256
# working canvas (8x supersample), build a clean unified outline by dilating the
# silhouette mask, composite white-fill + black-outline over transparent, then
# downscale to 32x32 with LANCZOS for anti-aliasing. A per-cursor hotspot is
# emitted into cursors.h (in 32px output coordinates).
#
# Reproduce:  python3 user/cursor/gen_cursors.py
# Requires:   Pillow  (python3 -m pip install --user Pillow)

import json
import os
from PIL import Image, ImageDraw, ImageFilter, ImageChops

OUT_DIR = os.path.dirname(os.path.abspath(__file__))
WORK = 256          # working canvas size
OUT = 32            # final cursor size
SCALE = WORK // OUT  # 8
OUTLINE_R = 8       # outline radius in working px (~1px at 32)

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)


def render(parts, hotspot_w):
    """parts: list of draw callables on an ImageDraw (L mode), each filling 255.
    hotspot_w: (hx, hy) in working coords (the click point of the FINAL cursor,
    i.e. accounting for the outline ring sitting outside the silhouette).
    Returns (RGBA Image at OUT px, hotspot at OUT px)."""
    mask = Image.new("L", (WORK, WORK), 0)
    d = ImageDraw.Draw(mask)
    for part in parts:
        part(d)
    # Dilate silhouette to form the outline ring just outside the shape.
    dilated = mask.filter(ImageFilter.MaxFilter(2 * OUTLINE_R + 1))
    ring = ImageChops.subtract(dilated, mask)

    img = Image.new("RGBA", (WORK, WORK), (0, 0, 0, 0))
    white = Image.new("RGBA", (WORK, WORK), WHITE)
    black = Image.new("RGBA", (WORK, WORK), BLACK)
    img.paste(white, (0, 0), mask)    # interior fill
    img.paste(black, (0, 0), ring)    # outline ring on top

    img = img.resize((OUT, OUT), Image.LANCZOS)
    hx = round(hotspot_w[0] / SCALE)
    hy = round(hotspot_w[1] / SCALE)
    return img, (hx, hy)


def poly(pts):
    def draw(d):
        d.polygon(pts, fill=255)
    return draw


def rrect(box, radius):
    def draw(d):
        d.rounded_rectangle(box, radius=radius, fill=255)
    return draw


# ---------------------------------------------------------------------------
# Cursor definitions. Coordinates in 256-space. Hotspot = the click point of
# the final outlined cursor (silhouette + outline ring outside it).
# ---------------------------------------------------------------------------

def left_ptr():
    # Classic arrow pointing up-left: arrowhead triangle + shaft parallelogram,
    # one concave polygon. Tip at top-left.
    T, L, R = (16, 16), (16, 120), (120, 16)        # arrowhead
    P, Q = (52, 84), (84, 52)                        # shaft near edge on hypotenuse
    Pp, Qp = (52 + 110, 84 + 110), (84 + 110, 52 + 110)  # shaft far edge
    pts = [T, R, Q, Qp, Pp, P, L]
    # Tip of the outlined cursor = tip - outline radius.
    return render([poly(pts)], (16 - OUTLINE_R, 16 - OUTLINE_R))


def xterm():
    # I-beam: vertical bar + top/bottom serifs, one polygon.
    pts = [(88, 40), (168, 40), (168, 60), (144, 60),
           (144, 196), (168, 196), (168, 216), (88, 216),
           (88, 196), (112, 196), (112, 60), (88, 60)]
    return render([poly(pts)], (128, 128))


def hand2():
    # Pointing hand (index finger up). Stylized, readable at 32px.
    palm = rrect([96, 150, 182, 224], radius=30)
    finger = rrect([108, 66, 148, 172], radius=20)
    tip = poly([(108, 80), (148, 80), (128, 48)])     # pointed fingertip
    thumb = rrect([162, 168, 188, 210], radius=12)
    # hotspot = fingertip tip, minus outline radius upward.
    return render([palm, finger, tip, thumb], (128, 48 - OUTLINE_R))


def move():
    vbar = rrect([116, 44, 140, 212], radius=12)
    hbar = rrect([44, 116, 212, 140], radius=12)
    up = poly([(110, 44), (146, 44), (128, 12)])
    down = poly([(110, 212), (146, 212), (128, 244)])
    left = poly([(44, 110), (44, 146), (12, 128)])
    right = poly([(212, 110), (212, 146), (244, 128)])
    return render([vbar, hbar, up, down, left, right], (128, 128))


def sb_h():
    bar = rrect([80, 116, 176, 140], radius=12)
    left = poly([(80, 100), (80, 156), (12, 128)])
    right = poly([(176, 100), (176, 156), (244, 128)])
    return render([bar, left, right], (128, 128))


def sb_v():
    bar = rrect([116, 80, 140, 176], radius=12)
    up = poly([(100, 80), (156, 80), (128, 12)])
    down = poly([(100, 176), (156, 176), (128, 244)])
    return render([bar, up, down], (128, 128))


CURSORS = [
    ("left_ptr", "left_ptr", left_ptr),
    ("xterm", "xterm", xterm),
    ("hand2", "hand2", hand2),
    ("move", "move", move),
    ("sb_h_double_arrow", "sb_h_double_arrow", sb_h),
    ("sb_v_double_arrow", "sb_v_double_arrow", sb_v),
]


def main():
    table = []
    manifest = []
    for name, filebase, fn in CURSORS:
        img, (hx, hy) = fn()
        path = f"{filebase}.png"
        img.save(os.path.join(OUT_DIR, path))
        # cursors.h feeds runtime fopen(), which needs an image-absolute path;
        # cursors.json is a human reference and keeps the image-relative form.
        table.append((name, f"/usr/share/cursors/{path}", hx, hy))
        manifest.append({"name": name, "file": path,
                         "hotspot_x": hx, "hotspot_y": hy})
        print(f"  {name:22s} -> {path}  hotspot=({hx},{hy})")

    # cursors.h — compiled into the compositor; maps cursor name to on-disk
    # path (image-absolute, for fopen) + hotspot (32px coords).
    with open(os.path.join(OUT_DIR, "cursors.h"), "w") as f:
        f.write("/* SPDX-License-Identifier: MIT\n")
        f.write(" * Auto-generated by user/cursor/gen_cursors.py — do not edit.\n")
        f.write(" * Original MIT-licensed cursor artwork. Hotspots in 32px coords. */\n")
        f.write("#ifndef OS_CURSOR_CURSORS_H\n#define OS_CURSOR_CURSORS_H\n\n")
        f.write("struct os_cursor_def {\n")
        f.write("  const char *name;   /* wlroots/xcursor cursor name */\n")
        f.write("  const char *path;   /* image-absolute path for runtime fopen */\n")
        f.write("  int hotspot_x, hotspot_y; /* 32px coords */\n")
        f.write("};\n\n")
        f.write("static const struct os_cursor_def os_cursors[] = {\n")
        for name, path, hx, hy in table:
            f.write(f'  {{ "{name}", "{path}", {hx}, {hy} }},\n')
        f.write("};\n")
        f.write("static const int os_cursors_n = "
                f"sizeof(os_cursors)/sizeof(os_cursors[0]);\n\n")
        f.write("#endif\n")

    with open(os.path.join(OUT_DIR, "cursors.json"), "w") as f:
        json.dump({"size": OUT, "cursors": manifest}, f, indent=2)
    print(f"\nWrote {len(table)} cursors, cursors.h, cursors.json into {OUT_DIR}")


if __name__ == "__main__":
    main()
