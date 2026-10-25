#!/usr/bin/env python3
"""Build the Rayverse launcher icon from vector source.

Renders the SVG master with headless Edge, then writes:
  - adaptive icon layers (foreground / background / monochrome)
  - legacy PNGs for API 21-25 in every density
  - round legacy PNGs
  - Play Store 512px export and previews

Run from this directory:  python build_icon.py
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageDraw

ICON_DIR = Path(__file__).resolve().parent
RES_DIR = ICON_DIR.parent / "app" / "src" / "main" / "res"
DEBUG_RES_DIR = ICON_DIR.parent / "app" / "src" / "debug" / "res"
PREVIEW_DIR = ICON_DIR / "preview"

EDGE_CANDIDATES = [
    Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
    Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"),
]

DENSITIES = {
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}

# --------------------------------------------------------------------------
# Palette / shared geometry
# --------------------------------------------------------------------------

DEFS = """
  <defs>
    <radialGradient id="bg" cx="46%" cy="34%" r="84%">
      <stop offset="0" stop-color="#3C2668"/>
      <stop offset="0.55" stop-color="#241546"/>
      <stop offset="1" stop-color="#130D26"/>
    </radialGradient>
    <radialGradient id="violetGlow" cx="50%" cy="50%" r="50%">
      <stop offset="0" stop-color="#7A55C9" stop-opacity="0.42"/>
      <stop offset="0.7" stop-color="#7A55C9" stop-opacity="0.10"/>
      <stop offset="1" stop-color="#7A55C9" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="goldGlow" cx="50%" cy="50%" r="50%">
      <stop offset="0" stop-color="#FFD766" stop-opacity="0.6"/>
      <stop offset="1" stop-color="#FFD766" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="gold" x1="0.12" y1="0" x2="0.88" y2="1">
      <stop offset="0" stop-color="#FFF0BE"/>
      <stop offset="0.45" stop-color="#FFC94A"/>
      <stop offset="1" stop-color="#E09B2D"/>
    </linearGradient>
    <linearGradient id="star" x1="0.5" y1="0" x2="0.5" y2="1">
      <stop offset="0" stop-color="#FFFDF2"/>
      <stop offset="0.5" stop-color="#FFE9A8"/>
      <stop offset="1" stop-color="#F2A62B"/>
    </linearGradient>
    <linearGradient id="glove" x1="0" y1="0" x2="0.85" y2="1">
      <stop offset="0" stop-color="#FFFDF7"/>
      <stop offset="0.55" stop-color="#F7EFE1"/>
      <stop offset="1" stop-color="#D9C8B4"/>
    </linearGradient>
    <linearGradient id="cuff" x1="0" y1="0" x2="0.2" y2="1">
      <stop offset="0" stop-color="#FFE9A8"/>
      <stop offset="0.5" stop-color="#FFC94A"/>
      <stop offset="1" stop-color="#D98E1F"/>
    </linearGradient>
    <filter id="softShadow" x="-30%" y="-30%" width="160%" height="160%">
      <feDropShadow dx="0" dy="12" stdDeviation="16" flood-color="#0A0518" flood-opacity="0.38"/>
    </filter>
  </defs>
"""

BACKGROUND = """
  <rect width="1024" height="1024" fill="url(#bg)"/>
  <circle cx="512" cy="500" r="430" fill="url(#violetGlow)"/>
  <g fill="#FFE9A8">
    <circle cx="196" cy="214" r="4" opacity="0.34"/>
    <circle cx="286" cy="136" r="3" opacity="0.24"/>
    <circle cx="772" cy="180" r="4" opacity="0.30"/>
    <circle cx="854" cy="306" r="3" opacity="0.22"/>
    <circle cx="162" cy="430" r="3" opacity="0.20"/>
    <circle cx="840" cy="622" r="4" opacity="0.24"/>
    <circle cx="246" cy="766" r="3" opacity="0.18"/>
    <circle cx="766" cy="812" r="4" opacity="0.26"/>
    <circle cx="652" cy="104" r="3" opacity="0.20"/>
    <circle cx="394" cy="236" r="3" opacity="0.18"/>
  </g>
  <path d="M748 250 c4 18 10 24 28 28 c-18 4 -24 10 -28 28 c-4 -18 -10 -24 -28 -28 c18 -4 24 -10 28 -28 Z" fill="#FFE9A8" opacity="0.55"/>
  <path d="M274 292 c3 12 7 16 19 19 c-12 3 -16 7 -19 19 c-3 -12 -7 -16 -19 -19 c12 -3 16 -7 19 -19 Z" fill="#FFE9A8" opacity="0.35"/>
"""

EMBLEM = """
  <circle cx="512" cy="512" r="292" fill="none" stroke="url(#gold)" stroke-width="14" opacity="0.92"/>
  <circle cx="512" cy="512" r="270" fill="none" stroke="#FFE9A8" stroke-width="2" opacity="0.22"/>
  <circle cx="512" cy="398" r="132" fill="url(#goldGlow)"/>
  <g filter="url(#softShadow)">
    <line x1="462" y1="688" x2="398" y2="636" stroke="url(#glove)" stroke-width="46" stroke-linecap="round"/>
    <path d="M512 282 C521 348 536 380 604 394 C536 408 521 440 512 506 C503 440 488 408 420 394 C488 380 503 348 512 282 Z" fill="url(#star)"/>
    <path d="M512 320 C518 360 527 380 566 388 C527 396 518 416 512 456 C506 416 497 396 458 388 C497 380 506 360 512 320 Z" fill="#FFFDF6" opacity="0.45"/>
    <rect x="448" y="498" width="42" height="146" rx="21" fill="url(#glove)" transform="rotate(-11 469 571)"/>
    <rect x="491" y="466" width="43" height="180" rx="21.5" fill="url(#glove)"/>
    <rect x="535" y="503" width="42" height="141" rx="21" fill="url(#glove)" transform="rotate(11 556 573)"/>
    <rect x="446" y="596" width="132" height="166" rx="52" fill="url(#glove)"/>
    <rect x="432" y="738" width="160" height="62" rx="31" fill="url(#cuff)"/>
    <rect x="432" y="738" width="160" height="62" rx="31" fill="none" stroke="#FFF0BE" stroke-width="2" opacity="0.5"/>
    <path d="M512 752 l7 18 18 7 -18 7 -7 18 -7 -18 -18 -7 18 -7 Z" fill="#2A1850" opacity="0.85"/>
  </g>
  <path d="M706 330 c4 18 10 24 28 28 c-18 4 -24 10 -28 28 c-4 -18 -10 -24 -28 -28 c18 -4 24 -10 28 -28 Z" fill="url(#gold)" opacity="0.9"/>
  <path d="M338 368 c3 12 7 16 19 19 c-12 3 -16 7 -19 19 c-3 -12 -7 -16 -19 -19 c12 -3 16 -7 19 -19 Z" fill="url(#gold)" opacity="0.65"/>
  <circle cx="684" cy="596" r="6" fill="#FFC94A" opacity="0.6"/>
  <circle cx="356" cy="616" r="4" fill="#FFC94A" opacity="0.45"/>
"""

MONOCHROME = """
  <circle cx="512" cy="512" r="292" fill="none" stroke="#FFFFFF" stroke-width="14"/>
  <g fill="#FFFFFF">
    <line x1="462" y1="688" x2="398" y2="636" stroke="#FFFFFF" stroke-width="46" stroke-linecap="round"/>
    <path d="M512 282 C521 348 536 380 604 394 C536 408 521 440 512 506 C503 440 488 408 420 394 C488 380 503 348 512 282 Z"/>
    <rect x="448" y="498" width="42" height="146" rx="21" transform="rotate(-11 469 571)"/>
    <rect x="491" y="466" width="43" height="180" rx="21.5"/>
    <rect x="535" y="503" width="42" height="141" rx="21" transform="rotate(11 556 573)"/>
    <rect x="446" y="596" width="132" height="166" rx="52"/>
    <rect x="432" y="738" width="160" height="62" rx="31"/>
  </g>
"""


def svg(content: str) -> str:
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        f'viewBox="0 0 1024 1024">{DEFS}{content}</svg>'
    )


def emblem(scale: float) -> str:
    return (
        f'<g transform="translate(512 512) scale({scale}) translate(-512 -512)">'
        f"{EMBLEM}</g>"
    )


def find_edge() -> Path:
    for path in EDGE_CANDIDATES:
        if path.exists():
            return path
    raise RuntimeError("Microsoft Edge not found; set EDGE in the script")


def render(svg_text: str, out_png: Path, transparent: bool = False) -> Image.Image:
    edge = find_edge()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    tmp_svg = out_png.with_suffix(".svg")
    tmp_svg.write_text(svg_text, encoding="utf-8")
    if out_png.exists():
        out_png.unlink()

    profile = Path(tempfile.mkdtemp(prefix="rayverse-icon-edge-"))
    try:
        cmd = [
            str(edge),
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--no-first-run",
            "--disable-extensions",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--window-size=1024,1024",
            f"--user-data-dir={profile}",
        ]
        if transparent:
            cmd.append("--default-background-color=00000000")
        cmd += [f"--screenshot={out_png}", tmp_svg.as_uri()]
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

        last = -1
        for _ in range(150):
            if out_png.exists():
                size = out_png.stat().st_size
                if size > 0 and size == last:
                    break
                last = size
            time.sleep(0.1)
        else:
            raise RuntimeError(f"Edge did not produce {out_png}")
        return Image.open(out_png)
    finally:
        tmp_svg.unlink(missing_ok=True)
        shutil.rmtree(profile, ignore_errors=True)


def rounded_mask(size: int, radius: int, circular: bool = False) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    if circular:
        draw.ellipse((0, 0, size - 1, size - 1), fill=255)
    else:
        draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


ADAPTIVE_XML = """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />{monochrome}
</adaptive-icon>
"""


def write_adaptive_xml() -> None:
    for qualifier, mono in (
        ("v26", ""),
        ("v33", '\n    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />'),
    ):
        folder = RES_DIR / f"mipmap-anydpi-{qualifier}"
        folder.mkdir(parents=True, exist_ok=True)
        for name in ("ic_launcher", "ic_launcher_round"):
            (folder / f"{name}.xml").write_text(
                ADAPTIVE_XML.format(monochrome=mono), encoding="utf-8"
            )


def debug_border(image: Image.Image, circular: bool) -> Image.Image:
    """Mint outline that keeps the debug build visually distinct from release."""
    out = image.copy()
    draw = ImageDraw.Draw(out)
    box = (16, 16, 1023 - 16, 1023 - 16)
    color = (36, 217, 126, 255)
    if circular:
        draw.ellipse(box, outline=color, width=34)
    else:
        draw.rounded_rectangle(box, radius=180, outline=color, width=34)
    return out


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, "PNG", optimize=True)


def main() -> None:
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)

    print("Rendering master layers ...")
    bg = render(svg(BACKGROUND), ICON_DIR / "_render_bg.png")
    fg = render(svg(EMBLEM), ICON_DIR / "_render_fg.png", transparent=True)
    legacy = render(svg(BACKGROUND + emblem(1.22)), ICON_DIR / "_render_legacy.png")
    mono = render(svg(MONOCHROME), ICON_DIR / "_render_mono.png", transparent=True)

    # Adaptive layers: 108dp at xxxhdpi == 432px.
    adaptive = 432
    bg_432 = bg.convert("RGB").resize((adaptive, adaptive), Image.LANCZOS)
    fg_432 = fg.convert("RGBA").resize((adaptive, adaptive), Image.LANCZOS)
    mono_432 = mono.convert("RGBA").resize((adaptive, adaptive), Image.LANCZOS)

    # Themed-icon preview: the system supplies the background and tints the alpha.
    mono_alpha = mono_432.getchannel("A").resize((256, 256), Image.LANCZOS)
    mono_light = Image.new("RGBA", (256, 256), (247, 244, 238, 255))
    mono_fill = Image.new("RGBA", (256, 256), (42, 24, 80, 255))
    mono_fill.putalpha(mono_alpha)
    mono_light.alpha_composite(mono_fill)
    save_png(mono_light.convert("RGB"), PREVIEW_DIR / "monochrome-on-light.png")

    xxxhdpi = RES_DIR / "mipmap-xxxhdpi"
    save_png(bg_432, xxxhdpi / "ic_launcher_background.png")
    save_png(fg_432, xxxhdpi / "ic_launcher_foreground.png")
    save_png(mono_432, xxxhdpi / "ic_launcher_monochrome.png")

    # Legacy icons (API 21-25): rounded square, transparent corners.
    legacy_rgba = legacy.convert("RGBA")
    square = legacy_rgba.copy()
    square.putalpha(rounded_mask(1024, radius=196))
    circle = legacy_rgba.copy()
    circle.putalpha(rounded_mask(1024, radius=0, circular=True))

    debug_square = debug_border(square, circular=False)
    debug_circle = debug_border(circle, circular=True)

    for name, px in DENSITIES.items():
        size = (px, px)
        save_png(square.resize(size, Image.LANCZOS), RES_DIR / f"mipmap-{name}" / "ic_launcher.png")
        save_png(circle.resize(size, Image.LANCZOS), RES_DIR / f"mipmap-{name}" / "ic_launcher_round.png")
        save_png(debug_square.resize(size, Image.LANCZOS), DEBUG_RES_DIR / f"mipmap-{name}" / "ic_launcher.png")
        save_png(debug_circle.resize(size, Image.LANCZOS), DEBUG_RES_DIR / f"mipmap-{name}" / "ic_launcher_round.png")

    # Store export: full-bleed square, no baked corners.
    store = render(svg(BACKGROUND + emblem(1.12)), PREVIEW_DIR / "_store.png").convert("RGB")
    save_png(store.resize((512, 512), Image.LANCZOS), PREVIEW_DIR / "play-store-512.png")

    # Full-size master and launcher simulations.
    master = render(svg(BACKGROUND + emblem(1.22)), PREVIEW_DIR / "_master.png").convert("RGB")
    save_png(master, PREVIEW_DIR / "ic_launcher_1024.png")

    composed = bg_432.convert("RGBA")
    composed.alpha_composite(fg_432)
    # Visible adaptive area is the central 72/108 of the layer.
    crop = int(adaptive * 72 / 108)
    off = (adaptive - crop) // 2
    visible = composed.crop((off, off, off + crop, off + crop)).resize((512, 512), Image.LANCZOS)
    for label, mask in (
        ("adaptive-circle", rounded_mask(512, 0, circular=True)),
        ("adaptive-squircle", rounded_mask(512, 96)),
    ):
        preview = visible.copy()
        preview.putalpha(mask)
        save_png(preview, PREVIEW_DIR / f"{label}.png")

    # Verify the adaptive foreground stays inside the 66dp safe circle.
    alpha = fg_432.getchannel("A")
    bbox = alpha.getbbox()
    safe_radius = adaptive * (66 / 108) / 2
    print(f"Foreground opaque bbox: {bbox} (safe circle radius {safe_radius:.1f}px at 432px)")

    write_adaptive_xml()

    for leftover in ICON_DIR.glob("_render_*"):
        leftover.unlink(missing_ok=True)
    for leftover in PREVIEW_DIR.glob("*.svg"):
        leftover.unlink(missing_ok=True)
    (PREVIEW_DIR / "_store.png").unlink(missing_ok=True)
    (PREVIEW_DIR / "_master.png").unlink(missing_ok=True)

    print("Wrote adaptive + legacy icons.")
    for path in sorted(PREVIEW_DIR.glob("*.png")):
        print(f"  preview: {path.name}")


if __name__ == "__main__":
    main()
