"""Regenerate all Aristotle branding assets from `modules/ai/aristotle logo.png`.

Traces the logo bitmap into SVG paths (editor logo icons, root repo art, web
editor logo) and rasterizes the icon/splash PNGs and Windows ICOs.

Usage: python misc/scripts/generate_aristotle_assets.py
Requires: Pillow, numpy, opencv-python-headless
"""
import os

import cv2
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGO = os.path.join(ROOT, "modules", "ai", "aristotle logo.png")

INK = "#35221F"
CREAM = "#FDF3E6"
CREAM_RGB = (0xFD, 0xF3, 0xE6)
MONO = "#e0e0e0"

# Monogram (circled A) geometry in the source bitmap.
MONO_CX, MONO_CY, DISC_HALF = 769, 381, 223

gray = cv2.imdecode(np.fromfile(LOGO, dtype=np.uint8), cv2.IMREAD_GRAYSCALE)
mask = (gray < 150).astype(np.uint8)

ys, xs = np.nonzero(mask[600:, :])
WM_X0, WM_X1 = xs.min(), xs.max()
WM_Y0, WM_Y1 = ys.min() + 600, ys.max() + 600
WM_W, WM_H = WM_X1 - WM_X0, WM_Y1 - WM_Y0
WM_ASPECT = WM_W / WM_H


def trace(region_mask, eps=1.2):
    contours, _ = cv2.findContours(region_mask, cv2.RETR_TREE, cv2.CHAIN_APPROX_NONE)
    out = []
    for c in contours:
        if cv2.contourArea(c) < 40:
            continue
        out.append(cv2.approxPolyDP(c, eps, True).reshape(-1, 2).astype(np.float64))
    return out


m_x0, m_y0 = MONO_CX - DISC_HALF, MONO_CY - DISC_HALF
mono_contours = trace(mask[m_y0:m_y0 + 2 * DISC_HALF, m_x0:m_x0 + 2 * DISC_HALF])
word_contours = trace(mask[WM_Y0:WM_Y1 + 1, WM_X0:WM_X1 + 1])


def path_data(contours, transform, prec=2):
    parts = []
    for c in contours:
        pts = [transform(x, y) for x, y in c]
        parts.append("M" + " ".join(f"{x:.{prec}f} {y:.{prec}f}" for x, y in pts) + "Z")
    return "".join(parts)


def mono_path(dst_cx, dst_cy, disc_r, prec=2):
    s = disc_r / DISC_HALF
    return path_data(mono_contours, lambda x, y: ((x - DISC_HALF) * s + dst_cx, (y - DISC_HALF) * s + dst_cy), prec)


def word_path(dst_cx, dst_cy, height, prec=2):
    s = height / WM_H
    return path_data(word_contours, lambda x, y: ((x - WM_W / 2) * s + dst_cx, (y - WM_H / 2) * s + dst_cy), prec)


def write(path, content):
    full = os.path.join(ROOT, path)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("wrote", path)


P = f'fill="{INK}" fill-rule="evenodd"'

write("editor/icons/Godot.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">'
      f'<circle cx="8" cy="8" r="8" fill="{CREAM}"/>'
      f'<path {P} d="{mono_path(8, 8, 8)}"/></svg>\n')

write("editor/icons/GodotMonochrome.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">'
      f'<path fill="{MONO}" fill-rule="evenodd" d="{mono_path(8, 8, 8.4)}"/></svg>\n')

SHEET = ('<path fill="#fff" fill-opacity=".6" d="M14 5a4 4 0 0 0-4 4v46a4 4 0 0 0 4 4h36a4 4 0 0 0 '
         '4-4V22a1 1 0 0 0-.285-.707l-16-16A1 1 0 0 0 37 5zm0 2h22v12a4 4 0 0 0 4 4h12v32a2 2 0 0 '
         '1-2 2H14a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2z"/>')
write("editor/icons/GodotFile.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">{SHEET}'
      f'<circle cx="32" cy="38" r="14" fill="{CREAM}"/>'
      f'<path {P} d="{mono_path(32, 38, 14)}"/></svg>\n')

write("editor/icons/DefaultProjectIcon.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128">'
      f'<rect width="124" height="124" x="2" y="2" fill="#363d52" stroke="#212532" stroke-width="4" rx="14"/>'
      f'<circle cx="64" cy="64" r="44" fill="{CREAM}"/>'
      f'<path {P} d="{mono_path(64, 64, 44)}"/></svg>\n')

disc_r, ring_cx = 24, 32
wm_x0d, wm_x1d = ring_cx + disc_r + 8, 179
wm_h = (wm_x1d - wm_x0d) / WM_ASPECT
write("editor/icons/Logo.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="187" height="69">'
      f'<rect width="186" height="68" x=".5" y=".5" fill="{CREAM}" rx="12"/>'
      f'<path {P} d="{mono_path(ring_cx, 34.5, disc_r)}"/>'
      f'<path {P} d="{word_path((wm_x0d + wm_x1d) / 2, 35.5, wm_h)}"/></svg>\n')

write("editor/icons/TitleBarLogo.svg",
      f'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="24">'
      f'<rect width="99" height="23" x=".5" y=".5" fill="{CREAM}" rx="6"/>'
      f'<path {P} d="{word_path(50, 12.5, 13)}"/></svg>\n')

icon_svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">'
            f'<circle cx="512" cy="512" r="512" fill="{CREAM}"/>'
            f'<path {P} d="{mono_path(512, 512, 512, prec=1)}"/></svg>\n')
write("icon.svg", icon_svg)
write("icon_outlined.svg", icon_svg)

l_disc, l_cx = 145, 205
l_x0, l_x1 = l_cx + l_disc + 40, 960
l_h = (l_x1 - l_x0) / WM_ASPECT
lockup = (f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="414">'
          f'<rect width="1022" height="412" x="1" y="1" fill="{CREAM}" rx="64"/>'
          f'<path {P} d="{mono_path(l_cx, 207, l_disc, prec=1)}"/>'
          f'<path {P} d="{word_path((l_x0 + l_x1) / 2, 212, l_h, prec=1)}"/></svg>\n')
write("logo.svg", lockup)
write("logo_outlined.svg", lockup)
write("misc/dist/html/logo.svg", lockup)

# ---------- raster assets ----------
rgb = Image.open(LOGO).convert("RGB")

# Circular monogram disc with transparent corners.
crop = rgb.crop((m_x0, m_y0, m_x0 + 2 * DISC_HALF, m_y0 + 2 * DISC_HALF))
S = crop.size[0]
mask_big = Image.new("L", (S * 4, S * 4), 0)
ImageDraw.Draw(mask_big).ellipse((0, 0, S * 4 - 1, S * 4 - 1), fill=255)
disc = crop.convert("RGBA")
disc.putalpha(mask_big.resize((S, S), Image.LANCZOS))
disc_1024 = disc.resize((1024, 1024), Image.LANCZOS)


def write_png(path, image):
    image.save(os.path.join(ROOT, path))
    print("wrote", path)


write_png("icon.png", disc_1024.resize((256, 256), Image.LANCZOS))
write_png("icon_outlined.png", disc_1024.resize((256, 256), Image.LANCZOS))
write_png("main/app_icon.png", disc_1024.resize((128, 128), Image.LANCZOS))

ico_sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
for ico in ("platform/windows/godot.ico", "platform/windows/godot_console.ico"):
    disc_1024.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, ico), sizes=ico_sizes)
    print("wrote", ico)

# Full lockup (stacked monogram + wordmark) crop with cream padding.
pad = 60
lockup_crop = rgb.crop((min(m_x0, WM_X0) - pad, m_y0 - pad,
                        max(MONO_CX + DISC_HALF, WM_X1) + pad, WM_Y1 + pad))
write_png("logo.png", lockup_crop.resize((512, round(lockup_crop.height * 512 / lockup_crop.width)), Image.LANCZOS))
write_png("logo_outlined.png",
          lockup_crop.resize((476, round(lockup_crop.height * 476 / lockup_crop.width)), Image.LANCZOS))

# 800x600 splash: orphaned fallback in main/ plus the iOS packaging placeholders.
splash = Image.new("RGBA", (800, 600), CREAM_RGB + (255,))
sc = lockup_crop.resize((round(lockup_crop.width * 420 / lockup_crop.height), 420), Image.LANCZOS)
splash.paste(sc, ((800 - sc.width) // 2, (600 - sc.height) // 2))
write_png("main/splash.png", splash)
write_png("misc/dist/ios_xcode/godot_ios/Images.xcassets/SplashImage.imageset/splash@2x.png", splash)
write_png("misc/dist/ios_xcode/godot_ios/Images.xcassets/SplashImage.imageset/splash@3x.png", splash)
