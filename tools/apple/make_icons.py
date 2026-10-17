#!/usr/bin/env python3
"""Generate the app's asset catalog icons (app/Resources/Assets.xcassets).

The icon is an original abstract picture drawn with Pillow - a winter night
sky, a pale moon and a stylised white wing of feathers - and uses nothing
from the game. Writes the iOS icon (1024 px) and the visionOS layered icon,
with their Contents.json files.

usage: make_icons.py [ASSETS_DIR]
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "app", "Resources", "Assets.xcassets")

TOP = (14, 28, 74)
BOTTOM = (92, 138, 206)


def sky(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def moon_layer(w, h):
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    r = min(w, h) * 0.13
    cx, cy = w * 0.70, h * 0.27
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - r * 2, cy - r * 2, cx + r * 2, cy + r * 2), fill=(220, 232, 255, 60))
    layer = Image.alpha_composite(layer, glow.filter(ImageFilter.GaussianBlur(r * 0.6)))
    d = ImageDraw.Draw(layer)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(246, 248, 255, 255))
    return layer


def wing_layer(w, h):
    """Seven curved feathers fanning out from the lower left."""
    layer = Image.new("RGBA", (w * 2, h * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    s = 2.0
    base = (w * 0.30 * s, h * 0.80 * s)
    for i in range(7):
        ang = math.radians(200 + i * 13)
        length = min(w, h) * (0.62 - i * 0.045) * s
        width = min(w, h) * (0.085 - i * 0.006) * s
        pts = []
        steps = 24
        for k in range(steps + 1):
            t = k / steps
            bend = math.sin(t * math.pi) * width * 0.9
            px = base[0] - math.cos(ang) * length * t
            py = base[1] + math.sin(ang) * length * t
            nx, ny = math.sin(ang), math.cos(ang)
            pts.append((px + nx * bend, py + ny * bend))
        for k in range(steps, -1, -1):
            t = k / steps
            bend = math.sin(t * math.pi) * width * 0.15
            px = base[0] - math.cos(ang) * length * t
            py = base[1] + math.sin(ang) * length * t
            nx, ny = math.sin(ang), math.cos(ang)
            pts.append((px - nx * bend, py - ny * bend))
        alpha = 255 - i * 14
        d.polygon(pts, fill=(255, 255, 255, alpha))
    layer = layer.resize((w, h), Image.LANCZOS)
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shadow.paste((10, 20, 60, 90), mask=layer.split()[3].filter(ImageFilter.GaussianBlur(min(w, h) * 0.02)))
    return Image.alpha_composite(shadow, layer)


def stars_layer(w, h):
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    pts = [(0.12, 0.14), (0.24, 0.30), (0.42, 0.10), (0.88, 0.48), (0.55, 0.36), (0.93, 0.12), (0.34, 0.46)]
    for i, (x, y) in enumerate(pts):
        r = min(w, h) * (0.008 + (i % 3) * 0.004)
        d.ellipse((x * w - r, y * h - r, x * w + r, y * h + r), fill=(255, 255, 255, 220))
    return layer


def flat_icon(w, h):
    img = sky(w, h).convert("RGBA")
    for layer in (stars_layer(w, h), moon_layer(w, h), wing_layer(w, h)):
        img = Image.alpha_composite(img, layer)
    return img.convert("RGB")


def save(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


def contents(path, data):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "Contents.json"), "w") as f:
        json.dump(data, f, indent=2)


INFO = {"author": "xcode", "version": 1}


def ios_icon():
    d = os.path.join(ASSETS, "AppIcon.appiconset")
    save(flat_icon(1024, 1024), os.path.join(d, "icon-1024.png"))
    contents(d, {"images": [{"filename": "icon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}],
                 "info": INFO})


def vision_icon():
    stack = os.path.join(ASSETS, "AppIconVision.solidimagestack")
    makers = {
        "Back": lambda W, H: Image.alpha_composite(sky(W, H).convert("RGBA"), stars_layer(W, H)).convert("RGB"),
        "Middle": moon_layer,
        "Front": wing_layer,
    }
    order = ["Front", "Middle", "Back"]
    contents(stack, {"info": INFO, "layers": [{"filename": f"{n}.solidimagestacklayer"} for n in order]})
    for n in order:
        layer_dir = os.path.join(stack, f"{n}.solidimagestacklayer")
        contents(layer_dir, {"info": INFO})
        img_dir = os.path.join(layer_dir, "Content.imageset")
        for old in os.listdir(img_dir) if os.path.isdir(img_dir) else []:
            if old.endswith(".png"):
                os.remove(os.path.join(img_dir, old))
        save(makers[n](1024, 1024), os.path.join(img_dir, f"{n.lower()}.png"))
        contents(img_dir, {"images": [{"filename": f"{n.lower()}.png", "idiom": "vision", "scale": "2x"}], "info": INFO})


def launch_color():
    d = os.path.join(ASSETS, "LaunchBackground.colorset")
    contents(d, {"colors": [{"color": {"color-space": "srgb", "components": {
        "red": "0.000", "green": "0.000", "blue": "0.000", "alpha": "1.000"}}, "idiom": "universal"}], "info": INFO})


if __name__ == "__main__":
    contents(ASSETS, {"info": INFO})
    ios_icon()
    vision_icon()
    launch_color()
    print("icons written to", ASSETS)
