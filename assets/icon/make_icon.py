#!/usr/bin/env python3
"""Draws icon.png in the repo root: the 48x48 HOME Menu icon, a one-colour glyph of the 3DS seen from behind.

The top screen's lid with the two outer cameras as small holes, the hinge, and the shoulder buttons. The Makefile
picks icon.png up for the SMDH; smdhtool makes the 24x24 one from it. Shapes are built with shapely, drawn at 16x
and filtered down so the edges stay clean at 48 px.

Run: python assets/icon/make_icon.py [preview.png]   (needs Pillow and shapely)
The optional preview shows the icon at 8x, 48 px and 24 px.
"""
import os
import sys

from PIL import Image, ImageDraw
from shapely.geometry import MultiPolygon, Point, box
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "icon.png")

N = 48            # icon size
S = 16            # supersampling
W = N * S

BG = (12, 12, 14)
INK = (238, 235, 228)

LID = (4, 9, 44, 33)          # top screen's lid
LID_R_TOP, LID_R_BOTTOM = 4.5, 1.5
GAP = 1.6                     # between lid, hinge and shoulder buttons
ROW_H = 4.6                   # hinge / shoulder button height
HINGE = (10.5, 37.5)
CAM_Y, CAM_DX, CAM_R = 15.5, 5.8, 1.6


def rrect(x0, y0, x1, y1, r):
    return box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=48)


def disc(cx, cy, r):
    return Point(cx, cy).buffer(r, quad_segs=64)


def lid():
    """Round top corners, tighter bottom ones."""
    x0, y0, x1, y1 = LID
    mid = (y0 + y1) / 2
    top = rrect(x0, y0, x1, y1, LID_R_TOP).intersection(box(x0, y0, x1, mid))
    bottom = rrect(x0, y0, x1, y1, LID_R_BOTTOM).intersection(box(x0, mid, x1, y1))
    return unary_union([top, bottom])


def shoulder(x0, x1, y0, y1, outer):
    """Rounded on the outer end (outer = -1 left, 1 right), flat on the end facing the hinge."""
    h = y1 - y0
    pill = rrect(x0, y0, x1, y1, h / 2)
    flat = box(x1 - h / 2, y0, x1, y1) if outer < 0 else box(x0, y0, x0 + h / 2, y1)
    return unary_union([pill, flat])


def glyph():
    y0, y1 = LID[3] + GAP, LID[3] + GAP + ROW_H
    g = unary_union([lid(), box(HINGE[0], y0, HINGE[1], y1),
                     shoulder(LID[0], HINGE[0] - GAP, y0, y1, -1), shoulder(HINGE[1] + GAP, LID[2], y0, y1, 1)])
    for x in (24 - CAM_DX, 24 + CAM_DX):
        g = g.difference(disc(x, CAM_Y, CAM_R))
    return g


def raster(g):
    m = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(m)
    for p in (g.geoms if isinstance(g, MultiPolygon) else [g]):
        d.polygon([(x * S, y * S) for x, y in p.exterior.coords], fill=255)
        for h in p.interiors:
            d.polygon([(x * S, y * S) for x, y in h.coords], fill=0)
    return m


def main():
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    tile = Image.new("L", (W, W), 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, W, W], radius=8 * S, fill=255)
    img.paste(Image.new("RGBA", (W, W), BG + (255,)), (0, 0), tile)
    img.paste(Image.new("RGBA", (W, W), INK + (255,)), (0, 0), raster(glyph()))

    icon = img.resize((N, N), Image.LANCZOS)
    icon.save(OUT)
    print("wrote", os.path.normpath(OUT))
    if len(sys.argv) > 1:
        big = Image.new("RGBA", (N * 8 + 24 + N * 4 + 24 + 24 * 4 + 24, N * 8), (210, 210, 206, 255))
        big.alpha_composite(img.resize((N * 8, N * 8), Image.LANCZOS), (0, 0))
        big.alpha_composite(icon.resize((N * 4, N * 4), Image.NEAREST), (N * 8 + 24, 0))
        small = icon.resize((24, 24), Image.LANCZOS)
        big.alpha_composite(small.resize((24 * 4, 24 * 4), Image.NEAREST), (N * 12 + 48, 0))
        big.alpha_composite(icon, (N * 8 + 24, N * 4 + 24))
        big.alpha_composite(small, (N * 12 + 48, N * 4 + 24))
        big.save(sys.argv[1])
        print("wrote", sys.argv[1])


if __name__ == "__main__":
    main()
