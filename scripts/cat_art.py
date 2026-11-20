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


def head(L, cx=16, cy=13.5, stripe_dy=0):
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
    for y in (12+stripe_dy, 14+stripe_dy):                                        # cheek stripes
        L.pixels("d", [(int(cx) - 11, y), (int(cx) - 10, y), (int(cx) + 9, y), (int(cx) + 10, y)])


def tail(L, pts, r=1.5):
    L.stroke(pts, r, "o", stripe="d", every=2)


TAILS = {
    "a": [(22, 28), (26, 27), (28.5, 24), (28, 20), (26.5, 18.5)],
    "b": [(22, 28), (26, 27.5), (29, 25), (29.8, 21), (29.2, 18)],
    "puff": [(22, 28), (26, 27), (28.5, 24), (28, 19.5), (26, 17.5)],
}


def body_sit(arms_up=False, tail_kind="a", puff=False):
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
    # (puff: the bushy tail alone reads better than extra fur pixels)
    L.ellipse(12, 30, 2.4, 1.6, "c")
    L.ellipse(20, 30, 2.4, 1.6, "c")
    L.outline()
    if arms_up:
        L.pixels("k", [(10, 21), (11, 21), (12, 21), (13, 21), (19, 21), (20, 21), (21, 21), (22, 21)])
    L.pixels("k", [(16, 29), (16, 30), (15, 31), (17, 31)])   # gap between feet
    return L


def body_groom():
    """Head lowered toward a connected orange foreleg, with tiny cream toes."""
    L = Layer()
    tail(L, TAILS["a"])
    L.ellipse(16, 24.5, 8.3, 6.8, "o")
    L.ellipse(16, 25.5, 4.6, 4.6, "c")
    head(L, cy=15.5, stripe_dy=2)
    L.ellipse(12, 30, 2.4, 1.6, "c")
    L.ellipse(20, 30, 2.4, 1.6, "c")
    L.outline()
    L.pixels("k", [(16, 29), (16, 30), (15, 31), (17, 31)])
    paw = Layer()
    paw.stroke([(23, 26), (21, 24), (18.5, 23)], 1.6, "o")
    paw.ellipse(18, 22.5, 2.1, 1.6, "o")
    paw.outline()
    paw.pixels("c", [(17, 21), (18, 21)])
    return compose(L, paw)


def body_window(flick=False, side=-1):
    """A room-sized rear view, facing the upper-left window, head still.

    The tail leaves the base of the spine, lies on the floor toward us and
    curls up on the left (side=-1, the user's choice); the flick frame only
    lifts its tip. The user rejected a front-view tail curling up at the side.
    Pose reference: Octavi Navarro, Cat Cafe #01,
    https://x.com/OctaviNavarro/status/1093242223255322638
    This is Kiki's own geometry/palette; the outdoor back layers stay separate.
    """
    L = Layer()
    L.ellipse(16.5, 24.5, 8.1, 6.8, "o")
    L.ellipse(12, 30, 2.4, 1.6, "o")
    L.ellipse(20, 30, 2.4, 1.6, "o")
    # Head leans toward the window; the ears show their orange backs.
    cx, cy = 14.5, 12.5
    L.ellipse(cx, cy, 10.5, 8.0, "o")
    L.poly([(cx-10, cy-2), (cx-8, cy-10.5), (cx-3.5, cy-6)], "o")
    L.poly([(cx+10, cy-2), (cx+8, cy-10.5), (cx+3.5, cy-6)], "o")
    L.pixels("d", [(7, 5), (8, 6), (21, 5), (20, 6),
                   (10, 6), (10, 7), (10, 8), (18, 6), (18, 7), (18, 8),
                   (13, 7), (14, 7), (13, 8), (14, 8), (13, 9), (14, 9),
                   (5, 13), (6, 14), (23, 13), (22, 14),
                   (12, 23), (13, 24), (20, 23), (19, 24),
                   (11, 26), (12, 27), (21, 26), (20, 27)])
    # Merge the neck/head/body before outlining. No horizontal mark that
    # could look like a mouth on the back of the head.
    L.pixels("d", [(13, 18), (14, 19), (14, 20)])
    L = L.shift(0, -1)                      # room under the hips for the tail's outline
    b = 16.5
    pts = [(b, 27.0), (b + 1.6 * side, 29.3), (b + 6 * side, 29.9), (b + 10 * side, 29.3),
           (b + 11.6 * side, 27.4)]
    pts += [(b + 11.4 * side, 24.6)] if flick else [(b + 12 * side, 26.0)]
    T = Layer()
    tail(T, pts, 1.3)
    T.outline()
    L.outline()
    # The tail lies in front of the hips; no outline where it leaves the body.
    for y in range(H):
        for x in range(W):
            t = T.g[y][x]
            if t == "." or (t == "k" and math.hypot(x + 0.5 - b, y + 0.5 - 27.5) < 2.2):
                continue
            L.g[y][x] = t
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


# Lying poses are a big-head "loaf": the seated head, unchanged, moved by
# (-4, +5) so the seated mood faces fit it exactly (dash_cat.c compose()).
# The user rejected the earlier small-head, long-body lying poses.
# Shape references: https://toffeecraft.itch.io/cat-sleeping-animation-free ,
# https://luizmelo.itch.io/pet-cat-pack and https://brato.itch.io/bratos-pixel-cats
LOAF = (12, 18.5)


def body_loaf(breath=0.0, sleep=False):
    """Rest (awake, tail tip up behind the hip) and sleep (tail over the paws)."""
    L = Layer()
    if not sleep:
        tail(L, [(27, 27), (29.6, 26), (30, 22.5), (28.6, 20.2)], 1.3)
    L.ellipse(20.2, 24.9 - breath / 2, 10.2, 6.0 + breath / 2, "o")       # loaf
    L.ellipse(12.5, 26.8, 7.6, 3.6, "o")                                   # chest under the chin
    y = round(19.6 - breath / 2)
    for x in (23, 26, 29):                                                 # back stripes
        L.pixels("d", [(x, y), (x, y + 1)])
    L.pixels("d", [(28, 24), (29, 25), (24, 23)])
    head(L, *LOAF)
    L.ellipse(8.4, 29.3, 2.4, 1.5, "c")                                    # tucked forepaws
    L.ellipse(15.6, 29.3, 2.4, 1.5, "c")
    if sleep:
        tail(L, [(30, 25.5), (29, 29), (24, 30.2), (17, 30.4)], 1.3)
    L.outline()
    L.pixels("k", [(12, 29), (12, 30)])                                    # gap between the paws
    return L


# A: weight moves onto the hind paws as the forepaws slide forward, then back.
# Pose references: https://unsplash.com/photos/V9DiohYX3Xg and
# https://commons.wikimedia.org/wiki/File:Stretching_street_cat_(41753892522).jpg
# Standing forelegs: https://commons.wikimedia.org/wiki/File:Cat,_Standard_Orange_Tabby.jpg
BREAK_STRETCH = {
    # head x/y, rump y, far and near forepaw x, tail-tip height
    "crouch": (10, 20, 21, 11, 14.4, 20),
    "reach": (9.5, 22, 18.5, 6.6, 10.4, 13),
    "hold": (10.3, 23, 17, 3.8, 7.8, 9),
    "recover": (9.5, 21, 19.5, 9, 12.8, 13),
    "stand": (10.5, 17, 21, 12.1, 15.6, 17),
}


def body_break_stretch(pose):
    cx, cy, rump, far_paw, near_paw, tip = BREAK_STRETCH[pose]
    L = Layer()
    tail(L, [(25, rump + 2), (29, rump - 1), (28.5, tip + 2), (25.5, tip)])

    # Plant both rear paws through the bow. The far legs sit a pixel higher.
    L.stroke([(21, rump + 1), (21, 25), (20, 29)], 1.5, "d")
    L.ellipse(20, 29, 1.9, 1.1, "c")
    L.stroke([(cx + 3, cy + 3.5), (far_paw, 28.3)], 1.2, "d")
    L.ellipse(far_paw, 28.6, 1.45, 1.05, "c")
    L.ellipse(23.2, rump + 0.8, 5.4, 4.4, "o")
    L.stroke([(25, rump + 1), (25.5, 25), (25, 29)], 1.8, "o")
    L.ellipse(25, 30, 2.2, 1.25, "c")
    L.poly([(cx + 3, cy + 1), (21, rump - 2), (25, rump + 2), (24, rump + 5),
            (cx + 6, cy + 6), (cx + 1, cy + 5)], "o")

    # Keep each paw at the end of its leg. As the shoulders rise, the legs
    # straighten under the chest and only the cream toes touch the floor.
    L.stroke([(cx + 5.8, cy + 3.7), (near_paw, 29.4)], 1.5, "o")
    L.ellipse(near_paw - 0.3, 30, 1.75, 1.2, "c")
    for x, y in ((20, rump - 2.5), (23, rump - 3.1), (26, rump - 1.8)):
        L.stroke([(x, y), (x + 1, y + 2.2)], 0.73, "d")

    # Low three-quarter head, with the nose and cheeks in Kiki's orange coat.
    L.poly([(cx - 6.5, cy - 2), (cx - 5.4, cy - 10), (cx - 1.5, cy - 5)], "o")
    L.poly([(cx + 1.5, cy - 5), (cx + 5.8, cy - 10), (cx + 7, cy - 1)], "o")
    L.ellipse(cx, cy, 7.3, 5.8, "o")
    L.poly([(cx - 5.5, cy - 4.1), (cx - 5.1, cy - 8), (cx - 3.1, cy - 5.3)], "p")
    L.poly([(cx + 3.1, cy - 5.4), (cx + 5.3, cy - 8), (cx + 5.7, cy - 3.6)], "p")
    x, y = round(cx), round(cy)
    L.pixels("d", [(x - 2, y - 5), (x, y - 5), (x - 1, y - 4), (x - 1, y - 3),
                   (x + 5, y), (x + 6, y), (x + 6, y + 2), (x - 7, y + 1)])
    if pose in ("reach", "hold"):
        L.pixels("k", [(x - 5, y), (x - 4, y + 1), (x - 3, y),
                       (x + 1, y), (x + 2, y + 1), (x + 3, y)])
    else:
        L.pixels("k", [(x - 4, y), (x - 4, y + 1), (x + 2, y), (x + 2, y + 1)])
    L.set(x - 1, y + 2, "p")
    L.set(x - 1, y + 3, "k")
    L.outline()
    return L


def head_back():
    """A fixed rear-view head; the outdoor animation never turns it."""
    L = Layer()
    L.ellipse(16, 10.7, 7.5, 5.8, "o")
    L.poly([(8.5, 10), (9, 3), (14, 6)], "o")
    L.poly([(18, 6), (23, 3), (23.5, 10)], "o")
    L.pixels("y", [(9, 5), (10, 6), (11, 6), (22, 5), (21, 6), (20, 6)])
    L.pixels("d", [(15, 6), (16, 6), (15, 7), (16, 7),
                   (12, 7), (12, 8), (19, 7), (19, 8),
                   (10, 13), (11, 14), (20, 14), (21, 13)])
    return L


def body_back(tail_phase=0):
    """Fixed seated back and head, with three gentle tail positions behind the hip."""
    paths = [
        [(20, 27), (23.5, 29), (28, 29), (30, 27.5)],
        [(20, 27), (23.5, 29), (27, 28.3), (29, 26)],
        [(20, 27), (23.5, 29), (26.8, 27.2), (27, 24.8)],
    ]
    stripes = [
        [(24, 28), (24, 29), (27, 28), (27, 29)],
        [(24, 28), (24, 29), (27, 27), (28, 27)],
        [(24, 27), (25, 28), (26, 25), (27, 25)],
    ]
    T = Layer()
    T.stroke(paths[tail_phase], 1.25, "o")
    for x, y in stripes[tail_phase]:
        if T.get(x, y) != ".":
            T.set(x, y, "d")
    T.outline()
    L = Layer()
    L.poly([(13, 12), (19, 12), (20, 16), (22, 20), (23, 24),
            (22, 28), (20, 30), (12, 30), (9, 28), (8, 24), (10, 20), (12, 16)], "o")
    L.ellipse(16, 23, 7.2, 7.0, "o")
    L.poly([(9, 23), (10, 26), (12, 28), (19, 29), (21, 28),
            (20, 30), (12, 30), (10, 29), (9, 27)], "d")
    L.pixels("y", [(11, 19), (10, 21), (10, 22), (11, 23), (12, 23)])
    L.pixels("d", [(12, 18), (13, 19), (14, 19), (19, 19), (20, 18),
                   (10, 22), (11, 22), (12, 23), (21, 22), (20, 23),
                   (10, 25), (11, 25), (12, 26), (21, 25), (20, 26)])
    L = compose(L, head_back())
    L.outline()
    return compose(T, L)


def body_side(typing=False):
    """Kiki at the desk in 3/4 view, turned right toward the laptop. Chibi sprites keep the
    big round head and both eyes in this view; the face just slides toward the screen.
    The face is part of this layer (the front-facing mood faces would not fit)."""
    cx, cy = 12.5, 13
    T = Layer()                                                           # tail, outlined on its own
    T.stroke([(6, 29.6), (2.6, 28.6), (1.5, 25), (2.4, 21.4)], 1.3, "o", stripe="d", every=2)
    T.outline()
    L = Layer()
    L.ellipse(12, 24.8, 7.6, 6.5, "o")                                    # body, turned right
    L.ellipse(14.4, 25.6, 3.7, 4.2, "c")                                  # belly
    L.ellipse(8.8, 30.2, 2.3, 1.5, "c"); L.ellipse(15.6, 30.2, 2.3, 1.5, "c")   # hind feet
    lift = -1.0 if typing else 0.0
    for (sx, sy), (px, py) in (((17.2, 24.2), (23.0, 21.2)), ((16.2, 22.8), (20.6, 20.8 + lift))):
        L.stroke([(sx, sy), (px - 1.2, py + 0.2)], 1.35, "o")            # front legs to the keys
        L.ellipse(px, py, 1.9, 1.25, "c")
    head(L, cx, cy)
    L.outline()
    ey, el, er, m = int(cy) - 1, 10, 17, 15                              # eyes and mouth slid right
    for ex in (el, er):
        L.rect(ex, ey, ex + 1, ey + 2, "k"); L.set(ex, ey, "w")
    L.pixels("p", [(m - 1, ey + 3), (m, ey + 3)])                         # nose
    L.pixels("k", [(m - 1, ey + 5), (m, ey + 5)])                         # mouth
    L.pixels("p", [(el - 3, ey + 4), (el - 2, ey + 4), (er + 3, ey + 4)])  # blush
    return compose(T, L)


def desk(code="a"):
    """Low desk on the right; the laptop's screen faces Kiki and, at an angle, us."""
    P = Layer()
    P.rect(18, 23, 31, 24, "n")                                          # table top
    P.rect(20, 25, 21, 31, "n"); P.rect(29, 25, 30, 31, "n")             # legs
    P.rect(19, 21, 30, 22, "s")                                          # laptop base
    for x in range(21, 29, 2):
        P.set(x, 21, "m")                                                # keys
    for y in range(13, 21):                                              # lid, leaning back
        x0 = 25 + (20 - y) // 4
        P.rect(x0, y, 29, y, "m")                                        # screen
        P.set(30, y, "s")                                                # lid edge
    rows = {"a": ((14, "g", 3), (16, "y", 2), (18, "B", 3)),
            "b": ((14, "g", 2), (16, "B", 3), (18, "w", 1))}[code]
    for y, c, n in rows:
        x0 = 26 + (20 - y) // 4
        for i in range(n):
            P.set(x0 + i, y, c)
    P.outline()
    return P


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
    elif kind in ("sleep_smile", "rest", "rest_blink"):   # loaf faces: nose a row higher
        ic = int(cx)
        if kind == "sleep_smile":   # ^ ^ in the sun
            for ex in (ex_l, ex_r):
                F.pixels("k", [(ex - 1, ey + 2), (ex, ey + 1), (ex + 1, ey + 1), (ex + 2, ey + 2)])
            F.pixels("k", [(ic - 2, ey + 3), (ic - 1, ey + 4), (ic, ey + 4), (ic + 1, ey + 3)])
        else:                       # relaxed half-lids, or shut on the blink
            for ex in (ex_l, ex_r):
                F.pixels("k", [(ex, ey), (ex + 1, ey)])
                if kind == "rest":
                    F.pixels("k", [(ex, ey + 1), (ex + 1, ey + 1)])
                    F.set(ex, ey, "w")
            F.pixels("k", [(ic - 1, ey + 4), (ic, ey + 4)])
        F.pixels("p", [(ic - 1, ey + 2), (ic, ey + 2)])
        F.pixels("p", [(ex_l - 3, ey + 3), (ex_l - 2, ey + 3), (ex_r + 3, ey + 3), (ex_r + 4, ey + 3)])
    elif kind in ("groom", "groom_lick"):
        for ex in (ex_l, ex_r):
            F.pixels("k", [(ex-1, 15), (ex, 16), (ex+1, 16), (ex+2, 15)])
        F.pixels("p", [(15, 18), (16, 18)])
        F.pixels("k", [(15, 19), (16, 19)])
        if kind == "groom_lick":
            F.pixels("r", [(16, 20), (17, 20)])
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
    elif kind == "sign":     # a board held up in front of the chest, paws on its top edge
        P.rect(6, 19, 25, 28, "l")
        for x in range(9, 18):
            P.set(x, 22, "s")
        for x in range(9, 15):
            P.set(x, 25, "s")
        P.pixels("r", [(19, 22), (20, 22), (22, 22), (23, 22), (19, 23), (20, 23), (21, 23), (22, 23), (23, 23),
                       (20, 24), (21, 24), (22, 24), (21, 25)])           # heart
        P.outline()
        Q = Layer()
        for cx in (8.5, 23.5):
            Q.ellipse(cx, 19.2, 2.3, 1.8, "c")
        Q.outline()
        Q.pixels("k", [(8, 20), (23, 20)])                                # toes
        P = compose(P, Q)
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
    elif kind == "zzz":      # clear of the loaf head's right ear
        for ox, oy, s in ((22, 9, 3), (26, 2, 4)):
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
    elif kind == "heart":
        P.pixels("r", [(26, 3), (27, 3), (29, 3), (30, 3), (25, 4), (26, 4), (27, 4), (28, 4), (29, 4), (30, 4),
                       (31, 4), (26, 5), (27, 5), (28, 5), (29, 5), (30, 5), (27, 6), (28, 6), (29, 6), (28, 7)])
    elif kind == "sun":
        P.ellipse(27, 4, 2.6, 2.6, "y")
        for x, y in ((27, 0), (31, 4), (23, 4), (27, 8), (24, 1), (30, 1), (24, 7), (30, 7)):
            P.set(x, y, "y")
    elif kind == "zzz_b":
        for ox, oy, s in ((23, 7, 3), (27, 1, 4)):
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


def side_phones():
    """Headphones on the 3/4 head: near cup on the left cheek, the far one just peeking out."""
    P = Layer()
    P.stroke([(3.6, 11), (4.6, 5.4), (12.5, 2.0), (20.5, 4.6), (22.4, 10)], 0.6, "m")
    P.rect(1, 10, 4, 15, "r")
    P.rect(22, 10, 23, 14, "r")
    P.outline()
    return P


def compose(*layers):
    out = Layer()
    for L in layers:
        for y in range(H):
            for x in range(W):
                if L.g[y][x] != ".":
                    out.g[y][x] = L.g[y][x]
    return out


# Pet reaction: Kiki stays seated, tilts her head into the hand and smiles; a
# heart floats up while she purrs. The user rejected lying-down belly-rub
# versions (both the noisy native side-flop grids and a chibi flop).
# Small tilts move each facial feature with the head but keep the eye/mouth
# pixel patterns upright, which avoids rotation jaggies on the 32-px face.
def rot(px, py, cx, cy, deg):
    a = math.radians(deg)
    dx, dy = px - cx, py - cy
    return cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a)


def ellipse_rot(L, cx, cy, rx, ry, ch, deg):
    a = math.radians(-deg)
    for y in range(H):
        for x in range(W):
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            u = dx * math.cos(a) - dy * math.sin(a)
            v = dx * math.sin(a) + dy * math.cos(a)
            if (u / rx) ** 2 + (v / ry) ** 2 <= 1.0:
                L.g[y][x] = ch


def head_tilt(L, cx, cy, deg):
    """head() rotated about its centre; its stripe pixels are moved, not redrawn."""
    R = lambda pts: [rot(px, py, cx, cy, deg) for px, py in pts]  # noqa: E731
    L.poly(R([(cx - 10.5, cy - 3), (cx - 8, cy - 11), (cx - 3.5, cy - 7)]), "o")
    L.poly(R([(cx + 10.5, cy - 3), (cx + 8, cy - 11), (cx + 3.5, cy - 7)]), "o")
    ellipse_rot(L, cx, cy, 11, 8.3, "o", deg)
    L.poly(R([(cx - 8.8, cy - 4.5), (cx - 7.8, cy - 9), (cx - 5.3, cy - 6.5)]), "p")
    L.poly(R([(cx + 8.8, cy - 4.5), (cx + 7.8, cy - 9), (cx + 5.3, cy - 6.5)]), "p")
    up = Layer()
    head(up)                                   # upright reference at HEAD
    for y in range(H):
        for x in range(W):
            if up.g[y][x] == "d":
                px, py = rot(x + 0.5 - HEAD[0] + cx, y + 0.5 - HEAD[1] + cy, cx, cy, deg)
                L.set(math.floor(px), math.floor(py), "d")


# (x, y, colour) cells and the point each pattern is centred on
G_HAPPY = ([(0, 1, "k"), (1, 0, "k"), (2, 0, "k"), (3, 1, "k")], (2, 1))      # ^
G_SHUT = ([(0, 0, "k"), (1, 1, "k"), (2, 1, "k"), (3, 0, "k")], (2, 1))       # content, shut
G_MOUTH = ([(1, 0, "p"), (2, 0, "p"), (0, 1, "k"), (1, 2, "k"), (2, 2, "k"), (3, 1, "k")], (2, 0.5))
G_BLUSH = ([(0, 0, "p"), (1, 0, "p")], (1, 0.5))


def glyph(L, g, at):
    cells, (gx, gy) = g
    ox, oy = round(at[0] - gx), round(at[1] - gy)
    for x, y, ch in cells:
        L.set(ox + x, oy + y, ch)


def face_tilt(L, cx, cy, deg, eyes):
    P = lambda dx, dy: rot(cx + dx, cy + dy, cx, cy, deg)  # noqa: E731
    for s in (-1, 1):
        glyph(L, eyes, P(5 * s, 0))
        glyph(L, G_BLUSH, P(8 * s, 2.5))
    glyph(L, G_MOUTH, P(0, 2.5))


PET = {"tilt": (14.5, 14.5, -10, "a"), "nuzzle": (14.0, 15.0, -16, "b"), "purr": (14.5, 14.5, -10, "a")}


def body_pet(kind):
    """Seated, body leaning a little; head tilted (nuzzle: further and lower)."""
    cx, cy, deg, tail_kind = PET[kind]
    L = Layer()
    tail(L, TAILS[tail_kind])
    ellipse_rot(L, 16.6, 25.2, 8.6, 6.4, "o", -8)
    ellipse_rot(L, 16.0, 26.0, 4.6, 4.4, "c", -8)
    head_tilt(L, cx, cy, deg)
    L.ellipse(12, 30, 2.4, 1.5, "c")
    L.ellipse(20, 30, 2.4, 1.5, "c")
    L.outline()
    L.pixels("k", [(16, 29), (16, 30), (15, 31), (17, 31)])
    face_tilt(L, cx, cy, deg, G_SHUT if kind == "purr" else G_HAPPY)
    return L


SPRITES = {
    "body_sit": body_sit(),
    "body_sit_b": body_sit(tail_kind="b"),
    "body_hold": body_sit(arms_up=True),
    "body_wash": body_groom(),
    "body_window": body_window(),
    "body_window_b": body_window(flick=True),
    "body_rest": body_loaf(),
    "body_rest_b": body_loaf(breath=0.6),
    **{f"body_break_stretch_{p}": body_break_stretch(p) for p in BREAK_STRETCH},
    **{f"body_pet_{p}": body_pet(p) for p in PET},
    "body_puff": body_sit(puff=True),
    "body_jump": body_jump(),
    "body_jump_hi": body_jump().shift(0, -2),
    "body_sleep": body_loaf(sleep=True),
    "body_sleep_b": body_loaf(breath=0.6, sleep=True),
    "body_back": body_back(),
    "body_back_tail_mid": body_back(1),
    "body_back_tail_up": body_back(2),
    "side_body": body_side(),
    "side_body_b": body_side(typing=True),
    "side_desk": desk("a"),
    "side_desk_b": desk("b"),
    "side_phones": side_phones(),
    **{f"face_{k}": face(k) for k in ("normal", "blink", "happy", "worried", "shocked", "sleepy", "yawn",
                                      "confused", "chew", "chew_b", "meow", "focus", "lick")},
    "face_jump": face("happy", cy=12.5),
    "face_jump_hi": face("happy", cy=12.5).shift(0, -2),
    "face_sleep_closed": face("sleepy", *LOAF),
    "face_sleep_smile": face("sleep_smile", *LOAF),
    "face_rest": face("rest", *LOAF),
    "face_rest_blink": face("rest_blink", *LOAF),
    **{f"face_{k}": face(k) for k in ("groom", "groom_lick")},
    **{f"prop_{k}": prop(k) for k in ("fish", "book", "book_b", "sign", "bowl",
                                      "rope_over", "rope_under", "yarn", "heart", "sun",
                                      "sweat", "bang", "question", "note", "zzz", "zzz_b", "confetti", "confetti_b")},
    "prop_yarn_b": prop("yarn").shift(-2, 0),
    "prop_heart_up": prop("heart").shift(0, -2),
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
    ("code", "side_desk", "side_body"), ("code 2", "side_desk_b", "side_body_b"),
    ("yarn", "body_sit_b", "face_happy", "prop_yarn"), ("wash", "body_wash", "face_groom_lick"),
    ("nap/sleep", "body_sleep", "face_sleep_closed", "prop_zzz"), ("sleep 2", "body_sleep_b", "face_sleep_closed", "prop_zzz_b"),
    ("sunbathe", "body_sleep", "face_sleep_smile", "prop_sun"), ("focus", "side_desk", "side_body", "side_phones"),
    ("CI pass", "body_jump", "face_jump", "prop_confetti"), ("CI pass 2", "body_jump_hi", "face_jump_hi", "prop_confetti_b"),
    ("Muse", "body_sit", "face_meow", "prop_sign"), ("meow", "body_sit", "face_meow", "prop_heart"),
    ("milk 2", "body_sit", "face_chew", "prop_bowl"), ("fish 2", "body_hold", "face_chew", "prop_fish"),
    ("dinner", "body_sit", "face_chew_b", "prop_bowl"), ("wash 2", "body_wash", "face_groom"),
    ("sun 2", "body_sleep_b", "face_sleep_smile", "prop_sun"),
    ("focus 2", "side_desk_b", "side_body_b", "side_phones"),
    ("alert", "body_sit", "face_shocked", "prop_bang"), ("puffed 2", "body_puff", "face_shocked"),
    ("sky back", "body_back"), ("sky tail mid", "body_back_tail_mid"),
    ("sky tail up", "body_back_tail_up"),
    ("window", "body_window"),
    ("window blink", "body_window"),  # retained debug name; the rear view has no visible eyes
    ("window tail", "body_window_b"),
    ("rest", "body_rest", "face_rest"), ("rest 2", "body_rest_b", "face_rest"),
    ("rest blink", "body_rest", "face_rest_blink"),
    ("drowsy", "body_sit", "face_sleepy"),
    ("yarn 2", "body_sit", "face_happy", "prop_yarn_b"),
    ("pet tilt", "body_pet_tilt"), ("pet nuzzle", "body_pet_nuzzle", "prop_heart"),
    ("pet tilt 2", "body_pet_tilt", "prop_heart_up"), ("pet purr", "body_pet_purr", "prop_heart_up"),
    ("pet purr 2", "body_pet_purr", "prop_heart"),
    *[(f"break stretch {p}", f"body_break_stretch_{p}") for p in BREAK_STRETCH],
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
