#!/usr/bin/env python3
"""Regenerates the Android launcher icon from android/branding/Nyanroid.png.

Adaptive icon (Android 8+): the logo as the foreground layer, inside the 72dp safe zone of the
108dp canvas, on a light grey background. Plus a plain square icon per density.
"""
import pathlib
from PIL import Image

root = pathlib.Path(__file__).resolve().parents[2]
res = root / "android/app/src/main/res"
logo = Image.open(root / "android/branding/Nyanroid.png").convert("RGBA")
logo = logo.crop(logo.getbbox())
BG = (200, 204, 208, 255)  # light grey
DENSITIES = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}


def fit(img, box):
    s = box / max(img.size)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)


for name, k in DENSITIES.items():
    d = res / f"mipmap-{name}"
    d.mkdir(parents=True, exist_ok=True)
    # Adaptive foreground: 108dp canvas, logo within the 66dp circle-safe area.
    fg_size = round(108 * k)
    fg = Image.new("RGBA", (fg_size, fg_size), (0, 0, 0, 0))
    l = fit(logo, round(64 * k))
    fg.alpha_composite(l, ((fg_size - l.width) // 2, (fg_size - l.height) // 2))
    fg.save(d / "ic_launcher_foreground.png")
    # Legacy square icon: 48dp.
    sz = round(48 * k)
    sq = Image.new("RGBA", (sz, sz), BG)
    l = fit(logo, round(42 * k))
    sq.alpha_composite(l, ((sz - l.width) // 2, (sz - l.height) // 2))
    sq.save(d / "ic_launcher.png")

ad = res / "mipmap-anydpi-v26"
ad.mkdir(parents=True, exist_ok=True)
(ad / "ic_launcher.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <background android:drawable="@color/ic_launcher_background" />\n'
    '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
    '    <monochrome android:drawable="@mipmap/ic_launcher_foreground" />\n'
    '</adaptive-icon>\n')
(res / "values/ic_launcher_colors.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
    '    <color name="ic_launcher_background">#C8CCD0</color>\n</resources>\n')
print("icons written")
