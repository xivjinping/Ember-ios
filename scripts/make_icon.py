#!/usr/bin/env python3
"""Draws Ember's app icon: a glowing ember on charcoal. Original artwork,
generated from code so it can be regenerated or restyled.

    python3 scripts/make_icon.py   ->  Resources/Assets.xcassets/AppIcon.appiconset/AppIcon.png
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
SS = 2  # supersampling factor for smooth edges
N = SIZE * SS


def teardrop(cx, cy, width, height, lean=0.0, points=400):
    """A flame-like teardrop: round at the bottom, pointed at the top."""
    outline = []
    for i in range(points):
        t = 2 * math.pi * i / points
        toward_tip = (1 + math.cos(t)) / 2          # 1 at the tip, 0 at the base
        x = math.sin(t) * math.sin(t / 2) ** 1.6 * width
        x += lean * width * toward_tip ** 2          # lean the tip so it reads as a flame, not a drop
        y = -math.cos(t) * height                    # tip at -height: the top, in image coordinates
        outline.append((cx + x, cy + y))
    return outline


def vertical_gradient(top, bottom):
    img = Image.new("RGB", (N, N))
    px = img.load()
    for y in range(N):
        k = y / (N - 1)
        row = tuple(round(top[c] + (bottom[c] - top[c]) * k) for c in range(3))
        for x in range(N):
            px[x, y] = row
    return img


def main():
    # Background: charcoal, slightly lighter towards the top.
    icon = vertical_gradient((46, 40, 38), (16, 14, 14))

    # Warm glow behind the ember.
    glow = Image.new("L", (N, N), 0)
    ImageDraw.Draw(glow).ellipse([N * 0.18, N * 0.28, N * 0.82, N * 0.92], fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(N * 0.10))
    icon.paste(Image.new("RGB", (N, N), (255, 96, 24)), mask=glow.point(lambda v: int(v * 0.55)))

    # Three nested flames, outer to inner, sharing nearly the same base.
    cx, bottom = N * 0.5, N * 0.87
    layers = [
        ((226, 62, 28), 0.300, 0.330, 0.30, 0.000),   # deep red-orange
        ((255, 128, 30), 0.215, 0.245, 0.24, 0.022),  # orange
        ((255, 214, 92), 0.125, 0.150, 0.16, 0.044),  # yellow core
    ]
    draw = ImageDraw.Draw(icon)
    for colour, w, h, lean, lift in layers:
        cy = bottom - (h + lift) * N
        draw.polygon(teardrop(cx, cy, N * w, N * h, lean), fill=colour)

    # Sparks rising from the tip.
    for sx, sy, r in [(0.700, 0.150, 0.016), (0.775, 0.245, 0.011), (0.640, 0.085, 0.009), (0.330, 0.215, 0.010)]:
        draw.ellipse([N * (sx - r), N * (sy - r), N * (sx + r), N * (sy + r)], fill=(255, 190, 80))

    out = os.path.join(os.path.dirname(__file__), "..", "Resources", "Assets.xcassets", "AppIcon.appiconset", "AppIcon.png")
    # App icons must be opaque: save as RGB, no alpha channel.
    icon.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.normpath(out), "PNG")
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
