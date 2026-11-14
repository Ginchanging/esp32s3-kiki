#!/usr/bin/env python3
"""Authoring script for the pixel cat (original art).

Builds 32x32 layers from simple shapes and writes them as editable ASCII
pixel files to assets/cat/*.txt (the source of truth for gen_sprites.py).
  python3 scripts/cat_art.py            # regenerate assets/cat/*.txt
  python3 scripts/cat_art.py --sheet out.png   # also render a review sheet (needs Pillow)

Palette (one char per pixel, '.' = transparent):
  k outline  o orange  d stripe  c cream  p pink  w white  B blue  g green
  r red      y yellow  n brown   s silver m dark   l paper  v violet
"""
import argparse
import math
from pathlib import Path

W = H = 32
PALETTE = {
    ".": None, "k": "3B2A2E", "o": "FFB454", "d": "E5873A", "c": "FFF1D6", "p": "FF9EAA",
    "w": "FFFFFF", "B": "6EC6FF", "g": "7BD389", "r": "FF6B6B", "y": "FFD166", "n": "9C6B4E",
    "s": "A8A3B8", "m": "4A4658", "l": "EDE6F5", "v": "B48CFF", "h": "FFD39A",
}
MUZZLE = "none"   # "cream" (first draft), "none", "soft", "chin"
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "cat"


class Layer:
    def __init__(self):
        self.g = [["."] * W for _ in range(H)]

    def get(self, x, y):
        return self.g[y][x] if 0 <= x < W and 0 <= y < H else "."

    def set(self, x, y, ch):
        if 0 <= x < W and 0 <= y < H:
            self.g[y][x] = ch

    def pixels(self, ch, pts):
        for x, y in pts:
            self.set(x, y, ch)

    def ellipse(self, cx, cy, rx, ry, ch, only_on=None):
        for y in range(H):
            for x in range(W):
                if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1.0:
                    if only_on is None or self.g[y][x] in only_on:
                        self.g[y][x] = ch

    def poly(self, pts, ch):
        for y in range(H):
            for x in range(W):
                px, py, inside = x + 0.5, y + 0.5, False
                j = len(pts) - 1
                for i in range(len(pts)):
                    xi, yi = pts[i]
                    xj, yj = pts[j]
                    if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
                        inside = not inside
                    j = i
                if inside:
                    self.g[y][x] = ch

    def rect(self, x0, y0, x1, y1, ch):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, ch)

    def stroke(self, pts, r, ch, stripe=None, every=3):
        """thick polyline: circles of radius r along the path"""
        n = 0
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            steps = max(1, int(math.hypot(x1 - x0, y1 - y0) * 3))
            for s in range(steps + 1):
                t = s / steps
                n += 1
                c = stripe if stripe and (n // (every * 3)) % 2 else ch
                self.ellipse(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r, r, c)

    def outline(self, ch="k"):
        src = [row[:] for row in self.g]
        for y in range(H):
            for x in range(W):
                if src[y][x] != ".":
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < W and 0 <= yy < H and src[yy][xx] not in (".", ch):
                        self.g[y][x] = ch
                        break

    def shift(self, dx, dy):
        n = Layer()
        for y in range(H):
            for x in range(W):
                if self.g[y][x] != ".":
                    n.set(x + dx, y + dy, self.g[y][x])
        return n

    def text(self):
        return "\n".join("".join(r) for r in self.g)


# ------------------------------------------------------------------ body poses
HEAD = (16, 13.5)  # face anchor used by the face layers


def head(L, cx=16, cy=13.5):
    L.poly([(cx - 10.5, cy - 3), (cx - 8, cy - 11), (cx - 3.5, cy - 7)], "o")   # left ear
    L.poly([(cx + 10.5, cy - 3), (cx + 8, cy - 11), (cx + 3.5, cy - 7)], "o")   # right ear
    L.ellipse(cx, cy, 11, 8.3, "o")
    L.poly([(cx - 8.8, cy - 4.5), (cx - 7.8, cy - 9), (cx - 5.3, cy - 6.5)], "p")
    L.poly([(cx + 8.8, cy - 4.5), (cx + 7.8, cy - 9), (cx + 5.3, cy - 6.5)], "p")
    if MUZZLE == "cream":
        L.ellipse(cx, cy + 3.2, 4.6, 2.9, "c")
    elif MUZZLE == "soft":                          # small, low-contrast muzzle
        L.ellipse(cx, cy + 3.4, 3.3, 2.1, "h")
    elif MUZZLE == "chin":                          # cream only below the mouth
        L.ellipse(cx, cy + 6.6, 3.6, 1.7, "c")
    for x, y in ((0, -7), (-1, -6), (1, -6), (-2, -7), (2, -7), (0, -5)):        # tabby "M"
        L.set(int(cx) + x - (1 if x <= 0 else 0) + (1 if x > 0 else 0) - 1, int(cy) + y, "d")
    for y in (12, 14):                                                           # cheek stripes
        L.pixels("d", [(int(cx) - 11, y), (int(cx) - 10, y), (int(cx) + 9, y), (int(cx) + 10, y)])


def tail(L, pts):
    L.stroke(pts, 1.5, "o", stripe="d", every=2)


TAILS = {
    "a": [(22, 28), (26, 27), (28.5, 24), (28, 20), (26.5, 18.5)],
    "b": [(22, 28), (26, 27.5), (29, 25), (29.8, 21), (29.2, 18)],
    "puff": [(22, 28), (26, 27), (28.5, 24), (28, 19.5), (26, 17.5)],
}


def body_sit(arms_up=False, tail_kind="a", wash=False, puff=False):
    L = Layer()
    if puff:
        L.stroke(TAILS["puff"], 2.4, "o", stripe="d", every=2)
    else:
        tail(L, TAILS[tail_kind])
    L.ellipse(16, 24.5, 8.3, 6.8, "o")
    L.ellipse(16, 25.5, 4.6, 4.6, "c")
    head(L)
    if arms_up:   # paws raised in front of the chest (holding something)
        L.ellipse(11.5, 22.5, 2.3, 1.8, "c")
        L.ellipse(20.5, 22.5, 2.3, 1.8, "c")
    if wash:      # one paw up at the cheek
        L.ellipse(22, 17.5, 2.2, 2.0, "c")
    # (puff: the bushy tail alone reads better than extra fur pixels)
    L.ellipse(12, 30, 2.4, 1.6, "c")
    L.ellipse(20, 30, 2.4, 1.6, "c")
    L.outline()
    if arms_up:
        L.pixels("k", [(10, 21), (11, 21), (12, 21), (13, 21), (19, 21), (20, 21), (21, 21), (22, 21)])
    L.pixels("k", [(16, 29), (16, 30), (15, 31), (17, 31)])   # gap between feet
    return L


def body_jump():
    L = Layer()
    tail(L, [(21, 25), (25, 24), (28, 21), (29, 17)])
    L.ellipse(16, 22.5, 7.6, 7.3, "o")
    L.ellipse(16, 23.5, 4.2, 4.5, "c")
    head(L, cy=12.5)
    L.ellipse(7.5, 17, 2.0, 2.4, "c")    # arms up
    L.ellipse(24.5, 17, 2.0, 2.4, "c")
    L.ellipse(11.5, 29.5, 2.2, 1.8, "c")  # legs stretched down
    L.ellipse(20.5, 29.5, 2.2, 1.8, "c")
    L.outline()
    return L


def body_sleep(breath=0.0):
    L = Layer()
    L.ellipse(18.5, 24.5 - breath / 2, 12.2, 6.3 + breath, "o")    # curled body
    for x in (19, 22, 25, 28):                                     # back stripes
        L.pixels("d", [(x, 19), (x, 20)])
    L.ellipse(10, 22.5, 7.6, 6.2, "o")                             # head resting on the left
    L.poly([(3.2, 20), (4.2, 12.5), (8.6, 17)], "o")              # ears
    L.poly([(11.4, 17), (15.8, 12.5), (16.8, 20)], "o")
    L.poly([(4.3, 18.3), (4.8, 14.6), (7.2, 17.2)], "p")
    L.poly([(12.8, 17.2), (15.2, 14.6), (15.7, 18.3)], "p")
    L.ellipse(10, 25.4, 3.6, 2.0, "c")                             # muzzle
    L.stroke([(29, 26.5), (26, 29.6), (18, 30.4), (9, 30.2)], 1.3, "o", stripe="d", every=2)  # tail in front
    L.outline()
    return L


# ------------------------------------------------------------------ faces (anchor = HEAD)
def face(kind, cx=16, cy=13.5):
    F = Layer()
    ex_l, ex_r, ey = int(cx) - 6, int(cx) + 4, int(cy) - 1
    def eyes_open():
        for ex in (ex_l, ex_r):
            F.rect(ex, ey, ex + 1, ey + 2, "k")
            F.set(ex, ey, "w")
    def mouth_w():
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 3), (int(cx) - 1, int(cy) + 4), (int(cx), int(cy) + 4), (int(cx) + 1, int(cy) + 3)])
    def blush():
        F.pixels("p", [(ex_l - 2, ey + 3), (ex_l - 1, ey + 3), (ex_r + 2, ey + 3), (ex_r + 3, ey + 3)])
    if kind == "normal":
        eyes_open(); mouth_w(); blush()
    elif kind == "happy":     # ^ ^
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 2), (ex, ey + 1), (ex + 1, ey + 1), (ex + 2, ey + 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 3), (int(cx) + 1, int(cy) + 3)])
        F.rect(int(cx) - 1, int(cy) + 3, int(cx), int(cy) + 4, "r")
        blush()
    elif kind == "worried":
        eyes_open()
        F.pixels("k", [(ex_l + 1, ey - 2), (ex_l + 2, ey - 3), (ex_r, ey - 3), (ex_r - 1 + 1, ey - 3)])
        F.pixels("k", [(ex_l, ey - 2), (ex_r + 1, ey - 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 4), (int(cx) - 1, int(cy) + 3), (int(cx), int(cy) + 3), (int(cx) + 1, int(cy) + 4)])
    elif kind == "shocked":   # tiny pupils + raised brows
        for ex in (ex_l, ex_r):
            F.rect(ex, ey + 1, ex + 1, ey + 2, "k")
            F.pixels("k", [(ex - 1, ey - 2), (ex, ey - 3), (ex + 1, ey - 3), (ex + 2, ey - 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.rect(int(cx) - 1, int(cy) + 3, int(cx), int(cy) + 5, "k")
    elif kind == "sleepy":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 1), (ex, ey + 2), (ex + 1, ey + 2), (ex + 2, ey + 1)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 1, int(cy) + 4), (int(cx), int(cy) + 4)])
        blush()
    elif kind == "confused":
        F.rect(ex_l, ey, ex_l + 1, ey + 2, "k"); F.set(ex_l, ey, "w")
        F.pixels("k", [(ex_r - 1, ey + 1), (ex_r, ey + 1), (ex_r + 1, ey + 1), (ex_r + 2, ey + 1)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 4), (int(cx) - 1, int(cy) + 4), (int(cx), int(cy) + 3), (int(cx) + 1, int(cy) + 3)])
    elif kind == "chew":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 2), (ex, ey + 1), (ex + 1, ey + 1), (ex + 2, ey + 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 3), (int(cx) - 1, int(cy) + 4), (int(cx), int(cy) + 4), (int(cx) + 1, int(cy) + 3)])
        blush()
    elif kind == "blink":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex, ey + 2), (ex + 1, ey + 2)])
        mouth_w(); blush()
    elif kind == "yawn":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 1), (ex, ey + 2), (ex + 1, ey + 2), (ex + 2, ey + 1)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.rect(int(cx) - 2, int(cy) + 3, int(cx) + 1, int(cy) + 5, "k")
        F.rect(int(cx) - 1, int(cy) + 4, int(cx), int(cy) + 5, "r")
    elif kind == "chew_b":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 2), (ex, ey + 1), (ex + 1, ey + 1), (ex + 2, ey + 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.rect(int(cx) - 2, int(cy) + 3, int(cx) + 1, int(cy) + 4, "k")
        F.rect(int(cx) - 1, int(cy) + 4, int(cx), int(cy) + 4, "r")
        blush()
    elif kind == "meow":
        eyes_open()
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.rect(int(cx) - 1, int(cy) + 3, int(cx), int(cy) + 4, "k")
        F.set(int(cx) - 1, int(cy) + 4, "r"); F.set(int(cx), int(cy) + 4, "r")
        blush()
    elif kind == "focus":
        eyes_open()
        F.pixels("k", [(ex_l - 1, ey - 2), (ex_l, ey - 2), (ex_l + 1, ey - 1), (ex_r, ey - 1), (ex_r + 1, ey - 2), (ex_r + 2, ey - 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 1, int(cy) + 4), (int(cx), int(cy) + 4)])
    elif kind == "lick":
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex - 1, ey + 2), (ex, ey + 1), (ex + 1, ey + 1), (ex + 2, ey + 2)])
        F.pixels("p", [(int(cx) - 1, int(cy) + 2), (int(cx), int(cy) + 2)])
        F.pixels("k", [(int(cx) - 2, int(cy) + 3), (int(cx) + 1, int(cy) + 3)])
        F.rect(int(cx) - 1, int(cy) + 3, int(cx), int(cy) + 5, "r")
        blush()
    elif kind == "sleep_smile":
        F.pixels("k", [(5, 23), (6, 22), (7, 22), (8, 23), (11, 23), (12, 22), (13, 22), (14, 23)])
        F.pixels("p", [(9, 24), (10, 24), (4, 24), (15, 24)])
    elif kind == "sleep_closed":   # for the curled pose (head at 10, 22.5)
        F.pixels("k", [(5, 22), (6, 23), (7, 23), (8, 22), (11, 22), (12, 23), (13, 23), (14, 22)])
        F.pixels("p", [(9, 24), (10, 24), (4, 24), (15, 24)])
    return F


# ------------------------------------------------------------------ props
def prop(kind):
    P = Layer()
    if kind == "fish":       # dried fish held in front
        P.ellipse(16, 22.5, 5.5, 1.7, "s")
        P.poly([(21, 22.5), (24.5, 20), (24.5, 25)], "s")
        P.set(12, 22, "k")
        P.outline()
    elif kind == "book":
        P.rect(9, 20, 22, 27, "g")
        P.rect(15, 20, 16, 27, "n")
        P.rect(10, 21, 14, 26, "l")
        P.rect(17, 21, 21, 26, "l")
        for y in (22, 24):
            P.pixels("s", [(11, y), (12, y), (13, y), (18, y), (19, y), (20, y)])
        P.outline()
    elif kind == "laptop":
        P.rect(8, 19, 23, 27, "s")
        P.rect(9, 20, 22, 26, "m")
        for x, y, c in ((10, 21, "g"), (11, 21, "g"), (13, 21, "y"), (14, 21, "y"), (10, 23, "B"),
                        (11, 23, "B"), (12, 23, "B"), (14, 23, "g"), (15, 23, "g"), (11, 25, "v"), (12, 25, "v")):
            P.set(x, y, c)
        P.rect(6, 28, 25, 29, "s")
        P.outline()
    elif kind == "sign":     # paper sign held up above the head
        P.rect(6, 1, 25, 8, "l")
        P.rect(15, 9, 16, 12, "n")
        for x in range(8, 24, 2):
            P.set(x, 4, "s")
        for x in range(9, 22, 3):
            P.set(x, 6, "s")
        P.outline()
    elif kind == "sweat":
        P.pixels("B", [(26, 6), (25, 7), (26, 7), (25, 8), (26, 8)])
        P.set(26, 8, "w")
    elif kind == "bang":
        P.rect(27, 1, 28, 5, "r"); P.rect(27, 7, 28, 8, "r")
    elif kind == "question":
        P.pixels("y", [(26, 1), (27, 1), (28, 1), (25, 2), (29, 2), (29, 3), (28, 4), (27, 5), (27, 6), (27, 8)])
    elif kind == "note":   # eighth note
        P.rect(28, 1, 28, 6, "v")
        P.pixels("v", [(29, 1), (30, 2), (30, 3), (29, 4)])
        P.rect(26, 6, 27, 7, "v")
        P.set(28, 7, "v")
    elif kind == "zzz":
        for ox, oy, s in ((19, 12, 3), (24, 5, 4)):
            P.rect(ox, oy, ox + s, oy, "B")
            P.rect(ox, oy + s, ox + s, oy + s, "B")
            for i in range(1, s):
                P.set(ox + s - i, oy + i, "B")
    elif kind == "book_b":   # page turning
        P.rect(9, 20, 22, 27, "g")
        P.rect(15, 20, 16, 27, "n")
        P.rect(10, 21, 14, 26, "l")
        P.rect(17, 19, 21, 24, "l")
        P.pixels("s", [(11, 22), (12, 22), (13, 22), (11, 24), (12, 24), (13, 24), (18, 20), (19, 20), (20, 21)])
        P.outline()
    elif kind == "laptop_b":
        P.rect(8, 19, 23, 27, "s")
        P.rect(9, 20, 22, 26, "m")
        for x, y, c in ((10, 21, "g"), (11, 21, "g"), (12, 21, "g"), (14, 21, "B"), (10, 23, "y"),
                        (11, 23, "y"), (13, 23, "g"), (14, 23, "g"), (15, 23, "g"), (10, 25, "v"), (11, 25, "v"), (12, 25, "w")):
            P.set(x, y, c)
        P.rect(6, 28, 25, 29, "s")
        P.outline()
    elif kind == "bowl":     # milk / rice bowl on the floor in front
        P.rect(8, 26, 23, 26, "c")
        P.rect(8, 27, 23, 29, "B")
        P.rect(10, 30, 21, 30, "B")
        P.pixels("w", [(10, 28), (11, 28)])
        P.outline()
    elif kind == "rope_over":
        P.stroke([(11, 22), (5, 13), (8, 4), (16, 1.2), (24, 4), (27, 13), (21, 22)], 0.55, "n")
    elif kind == "rope_under":
        P.stroke([(7.5, 15), (5.5, 23), (9, 30), (16, 31.4), (23, 30), (26.5, 23), (24.5, 15)], 0.55, "n")
    elif kind == "yarn":
        P.ellipse(27, 28, 3.6, 3.4, "v")
        P.pixels("l", [(25, 27), (26, 26), (28, 28), (27, 30), (29, 26)])
        P.stroke([(23.5, 29), (20, 30.5), (16, 30.8)], 0.5, "v")
        P.outline()
    elif kind == "headphones":
        P.stroke([(5.5, 13), (6, 6), (16, 2.2), (26, 6), (26.5, 13)], 0.6, "m")
        P.rect(3, 11, 6, 16, "r"); P.rect(26, 11, 29, 16, "r")
        P.outline()
    elif kind == "heart":
        P.pixels("r", [(26, 3), (27, 3), (29, 3), (30, 3), (25, 4), (26, 4), (27, 4), (28, 4), (29, 4), (30, 4),
                       (31, 4), (26, 5), (27, 5), (28, 5), (29, 5), (30, 5), (27, 6), (28, 6), (29, 6), (28, 7)])
    elif kind == "sun":
        P.ellipse(27, 4, 2.6, 2.6, "y")
        for x, y in ((27, 0), (31, 4), (23, 4), (27, 8), (24, 1), (30, 1), (24, 7), (30, 7)):
            P.set(x, y, "y")
    elif kind == "zzz_b":
        for ox, oy, s in ((20, 10, 3), (25, 3, 4)):
            P.rect(ox, oy, ox + s, oy, "B")
            P.rect(ox, oy + s, ox + s, oy + s, "B")
            for i in range(1, s):
                P.set(ox + s - i, oy + i, "B")
    elif kind == "confetti_b":
        for x, y, c in ((4, 6, "y"), (9, 3, "B"), (14, 1, "r"), (21, 5, "y"), (26, 2, "g"), (29, 6, "v"),
                        (3, 12, "r"), (28, 12, "y"), (6, 17, "B"), (26, 17, "g")):
            P.set(x, y, c); P.set(x + 1, y + 1, c)
    elif kind == "confetti":
        for x, y, c in ((3, 3, "r"), (7, 1, "y"), (12, 4, "B"), (19, 2, "g"), (24, 5, "v"), (28, 2, "y"),
                        (2, 9, "g"), (29, 9, "r"), (5, 14, "y"), (27, 14, "B")):
            P.set(x, y, c); P.set(x + 1, y + 1, c)
    return P


def compose(*layers):
    out = Layer()
    for L in layers:
        for y in range(H):
            for x in range(W):
                if L.g[y][x] != ".":
                    out.g[y][x] = L.g[y][x]
    return out


SPRITES = {
    "body_sit": body_sit(),
    "body_sit_b": body_sit(tail_kind="b"),
    "body_hold": body_sit(arms_up=True),
    "body_wash": body_sit(wash=True),
    "body_puff": body_sit(puff=True),
    "body_jump": body_jump(),
    "body_jump_hi": body_jump().shift(0, -2),
    "body_sleep": body_sleep(),
    "body_sleep_b": body_sleep(breath=0.6),
    **{f"face_{k}": face(k) for k in ("normal", "blink", "happy", "worried", "shocked", "sleepy", "yawn",
                                      "confused", "chew", "chew_b", "meow", "focus", "lick")},
    "face_jump": face("happy", cy=12.5),
    "face_jump_hi": face("happy", cy=12.5).shift(0, -2),
    "face_sleep_closed": face("sleep_closed"),
    "face_sleep_smile": face("sleep_smile"),
    **{f"prop_{k}": prop(k) for k in ("fish", "book", "book_b", "laptop", "laptop_b", "sign", "bowl",
                                      "rope_over", "rope_under", "yarn", "headphones", "heart", "sun",
                                      "sweat", "bang", "question", "note", "zzz", "zzz_b", "confetti", "confetti_b")},
}

# What the state machine draws, as (label, layers...) -- also the review sheet.
LOOKS = [
    ("idle", "body_sit", "face_normal"), ("blink", "body_sit", "face_blink"),
    ("tail", "body_sit_b", "face_normal"), ("happy", "body_sit_b", "face_happy", "prop_note"),
    ("worried", "body_sit", "face_worried", "prop_sweat"), ("puffed", "body_puff", "face_shocked", "prop_bang"),
    ("confused", "body_sit", "face_confused", "prop_question"), ("yawn", "body_sit", "face_yawn"),
    ("rope 1", "body_jump_hi", "face_jump_hi", "prop_rope_under"), ("rope 2", "prop_rope_over", "body_hold", "face_happy"),
    ("milk", "body_sit", "face_lick", "prop_bowl"), ("fish", "body_hold", "face_chew_b", "prop_fish"),
    ("read", "body_sit", "face_normal", "prop_book"), ("read 2", "body_sit", "face_normal", "prop_book_b"),
    ("code", "body_sit", "face_focus", "prop_laptop"), ("code 2", "body_sit", "face_focus", "prop_laptop_b"),
    ("yarn", "body_sit_b", "face_happy", "prop_yarn"), ("wash", "body_wash", "face_lick"),
    ("nap/sleep", "body_sleep", "face_sleep_closed", "prop_zzz"), ("sleep 2", "body_sleep_b", "face_sleep_closed", "prop_zzz_b"),
    ("sunbathe", "body_sleep", "face_sleep_smile", "prop_sun"), ("focus", "body_sit", "face_focus", "prop_headphones", "prop_laptop"),
    ("CI pass", "body_jump", "face_jump", "prop_confetti"), ("CI pass 2", "body_jump_hi", "face_jump_hi", "prop_confetti_b"),
    ("Muse", "body_hold", "face_meow", "prop_sign"), ("meow", "body_sit", "face_meow", "prop_heart"),
    ("milk 2", "body_sit", "face_chew", "prop_bowl"), ("fish 2", "body_hold", "face_chew", "prop_fish"),
    ("dinner", "body_sit", "face_chew_b", "prop_bowl"), ("wash 2", "body_wash", "face_happy"),
    ("sun 2", "body_sleep_b", "face_sleep_smile", "prop_sun"),
    ("focus 2", "body_sit", "face_focus", "prop_headphones", "prop_laptop_b"),
    ("alert", "body_sit", "face_shocked", "prop_bang"), ("puffed 2", "body_puff", "face_shocked"),
]


def write_assets():
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.txt"):
        old.unlink()
    for name, L in SPRITES.items():
        (OUT / f"{name}.txt").write_text(L.text() + "\n")
    (OUT / "palette.cfg").write_text("".join(f"{k} {v or '-'}\n" for k, v in PALETTE.items()))
    (OUT / "looks.cfg").write_text("".join(f"{lab}: {' '.join(names)}\n" for lab, *names in LOOKS))


def render_sheet(path):
    from PIL import Image, ImageDraw
    S = SPRITES
    looks = [(lab, compose(*(S[n] for n in names))) for lab, *names in LOOKS]
    _unused = [
        ("normal", compose(S["body_sit"], S["face_normal"])),
        ("happy", compose(S["body_sit"], S["face_happy"], S["prop_note"])),
        ("worried", compose(S["body_sit"], S["face_worried"], S["prop_sweat"])),
        ("shocked", compose(S["body_sit"], S["face_shocked"], S["prop_bang"])),
        ("sleepy", compose(S["body_sit"], S["face_sleepy"])),
        ("confused", compose(S["body_sit"], S["face_confused"], S["prop_question"])),
        ("eating", compose(S["body_hold"], S["face_chew"], S["prop_fish"])),
        ("reading", compose(S["body_sit"], S["face_normal"], S["prop_book"])),
        ("coding", compose(S["body_sit"], S["face_happy"], S["prop_laptop"])),
        ("CI pass: jump", compose(S["body_jump"], S["face_jump"], S["prop_confetti"])),
        ("Muse: sign", compose(S["body_hold"], S["face_happy"], S["prop_sign"])),
        ("sleeping", compose(S["body_sleep"], S["face_sleep_closed"], S["prop_zzz"])),
    ]
    scale, pad, cols = 5, 14, 6
    cw, ch = W * scale + pad, H * scale + pad + 18
    rows = (len(looks) + cols - 1) // cols
    img = Image.new("RGB", (cols * cw + pad, rows * ch + pad), (23, 21, 31))
    d = ImageDraw.Draw(img)
    for i, (label, L) in enumerate(looks):
        ox, oy = pad + (i % cols) * cw, pad + (i // cols) * ch
        d.rectangle([ox - 2, oy - 2, ox + W * scale + 1, oy + H * scale + 1], fill=(36, 33, 47))
        for y in range(H):
            for x in range(W):
                c = PALETTE[L.g[y][x]]
                if c:
                    d.rectangle([ox + x * scale, oy + y * scale, ox + x * scale + scale - 1, oy + y * scale + scale - 1],
                                fill=tuple(int(c[j:j + 2], 16) for j in (0, 2, 4)))
        d.text((ox, oy + H * scale + 4), label, fill=(242, 233, 220))
    img.save(path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet")
    a = ap.parse_args()
    write_assets()
    if a.sheet:
        render_sheet(a.sheet)
        print("sheet:", a.sheet)
