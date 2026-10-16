"""Generates the TaskNest icon (resources/app.ico + icon PNGs).

Motif: a violet "nest" bowl cradling a lime check mark on a graphite tile.
Run:  python resources/make_icon.py   (needs Pillow). Output is committed;
this script is the source artwork.
"""
from PIL import Image, ImageDraw
import math, os

S = 2048            # supersampled canvas
HERE = os.path.dirname(os.path.abspath(__file__))

def lerp(a, b, t): return tuple(int(a[i] + (b[i]-a[i])*t) for i in range(3))

def thick_line(d, pts, w, fill):
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        d.line([(x0, y0), (x1, y1)], fill=fill, width=w)
    for x, y in pts:
        d.ellipse([x-w/2, y-w/2, x+w/2, y+w/2], fill=fill)

def render():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # tile: vertical graphite gradient, rounded square, thin light rim
    tile = Image.new("RGBA", (S, S))
    td = ImageDraw.Draw(tile)
    top, bot = (38, 44, 58), (14, 16, 20)
    for y in range(S):
        td.line([(0, y), (S, y)], fill=lerp(top, bot, y / S) + (255,))
    mask = Image.new("L", (S, S), 0)
    m = int(S * 0.04)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, S-m, S-m], radius=int(S*0.23), fill=255)
    img.paste(tile, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([m, m, S-m, S-m], radius=int(S*0.23), outline=(255, 255, 255, 38), width=10)

    cx, cy = S/2, S*0.44
    # nest bowl: thick violet lower arc (a circle's bottom) with round caps
    r, w = S*0.30, int(S*0.105)
    violet = (139, 108, 255, 255)
    d.arc([cx-r, cy-r, cx+r, cy+r], start=10, end=170, fill=violet, width=w)
    for ang in (10, 170):
        a = math.radians(ang)
        rc = r - w/2; ex, ey = cx + rc*math.cos(a), cy + rc*math.sin(a)
        d.ellipse([ex-w/2, ey-w/2, ex+w/2, ey+w/2], fill=violet)
    # lime check mark rising out of the nest
    lime = (190, 255, 60, 255)
    pts = [(cx-S*0.16, cy-S*0.08), (cx-S*0.04, cy+S*0.05), (cx+S*0.20, cy-S*0.24)]
    thick_line(d, pts, int(S*0.12), lime)
    return img

if __name__ == "__main__":
    big = render()
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    imgs = [big.resize((n, n), Image.LANCZOS) for n in sizes]
    big.resize((512, 512), Image.LANCZOS).save(os.path.join(HERE, "icon-512.png"))
    imgs[-1].save(os.path.join(HERE, "icon-256.png"))
    imgs[-1].save(os.path.join(HERE, "app.ico"), sizes=[(n, n) for n in sizes])
    print("ok")
