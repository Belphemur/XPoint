#!/usr/bin/env python3
"""Download, preview, manifest, and upload TTF fonts for direct device download.

This pipeline serves raw TTF files to PSRAM-enabled devices (ESP32-S3 / X4 Pro).
Non-PSRAM devices (ESP32-C3) continue to use the .cpfont flow via build-sd-fonts.py
— this script and its manifest are gated behind ``CONFIG_SPIRAM`` at the firmware
level, so C3 builds never reference it.

Phases (run individually by flags or all by default):
  --download    Fetch TTF/OTF files from configured GitHub raw URLs
  --previews    Render Pillow PNGs showing sample text at 14/16/18pt
  --manifest    Build fonts.json with CRC32 (esp_rom_crc32_le compatible) + metadata
  --upload      Push fonts, previews, and manifest to a Cloudflare R2 bucket
  --make-public Enable r2.dev public access and print the public base URL

Usage:
  python3 distribute-fonts.py           # all phases
  python3 distribute-fonts.py --download --manifest
  python3 distribute-fonts.py --upload --make-public

Environment:
  WRANGLER_BIN  Override path to wrangler CLI (default: ~/.local/bin/wrangler)
  R2_BUCKET     Override R2 bucket name (default: reader-fonts)
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zlib
from pathlib import Path

# ── Paths ────────────────────────────────────────────────────────────────────

SCRIPT_DIR = Path(__file__).resolve().parent
CACHE_DIR = Path(os.environ.get("HERMES_CACHE", Path.home() / ".hermes" / "cache")) / "scratch" / "font_ttf"
PREVIEW_DIR = Path(os.environ.get("HERMES_CACHE", Path.home() / ".hermes" / "cache")) / "scratch" / "font_previews"
MANIFEST_PATH = SCRIPT_DIR.parent.parent.parent / "font-distribution" / "fonts.json"
WORKSPACE = SCRIPT_DIR.parent.parent.parent.parent  # repo root

# ── R2 settings ──────────────────────────────────────────────────────────────

R2_BUCKET = os.environ.get("R2_BUCKET", "reader-fonts")
WRANGLER_BIN = os.environ.get("WRANGLER_BIN", os.path.expanduser("~/.local/bin/wrangler"))

# ── Font source catalog ─────────────────────────────────────────────────────

# Each family lists style → (source_label, url).
# Sources:
#   "nicoverbruggen/ebook-fonts" = e-ink-optimized NV_* variants (hinted for e-ink)
#   upstream repos               = original Google Fonts / Adobe / community releases

FONT_CATALOG = {
    # ── e-ink optimized (nicoverbruggen/ebook-fonts — core/) ──────────────────
    "NV Libron": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Source Serif Pro)",
        "description": "e-ink-optimized serif derived from Source Serif Pro. Excellent readability on e-ink displays.",
        "preview": "preview_Libron_e-ink_opt.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Libron-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Libron-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Libron-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Libron-BoldItalic.ttf",
        },
    },
    "NV Sourcerer": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized)",
        "description": "Serif with high-contrast strokes, specially hinted for crisp e-ink rendering.",
        "preview": "preview_Sourcerer_e-ink_opt.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Sourcerer-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Sourcerer-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Sourcerer-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Sourcerer-BoldItalic.ttf",
        },
    },
    "NV Bitter": {
        "category": "slab-serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized)",
        "description": "Slab serif designed for screens, specially hinted for e-ink. Sturdy and readable.",
        "preview": "preview_NV_Bitter_e-ink_opt.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Bitter-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Bitter-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Bitter-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Bitter-BoldItalic.ttf",
        },
    },
    "NV Jost": {
        "category": "sans-serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Sora/Jost)",
        "description": "Clean geometric sans-serif, e-ink-hinted. Modern and unobtrusive.",
        "preview": "preview_NV_Jost_sans.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Jost-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Jost-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Jost-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Jost-BoldItalic.ttf",
        },
    },
    "NV Legible Next": {
        "category": "sans-serif (accessibility)",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Atkinson Hyperlegible Next)",
        "description": "Accessibility font for low vision, e-ink-hinted. Maximum legibility with generous spacing.",
        "preview": "preview_NV_Legible_Next_sans.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Legible_Next-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Legible_Next-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Legible_Next-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Legible_Next-BoldItalic.ttf",
        },
    },
    "NV Disleksio": {
        "category": "sans-serif (dyslexia)",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized)",
        "description": "Dyslexia-friendly sans-serif adapted for e-ink. Weighted bottoms for letter orientation.",
        "preview": "preview_NV_Disleksio_OpenDyslexic_opt.png",
        "subdir": "extra",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Disleksio-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Disleksio-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Disleksio-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Disleksio-BoldItalic.ttf",
        },
    },
    "NV Tabula": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, IBM Plex Serif)",
        "description": "e-ink-optimized serif, clean and traditional.",
        "preview": "preview_NV_Tabula_IBM_Plex_Serif_opt.png",
        "subdir": "extra",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Tabula-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Tabula-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Tabula-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/extra/NV_Tabula-BoldItalic.ttf",
        },
    },
    "Cartisse": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized)",
        "description": "Calligraphic serif with distinctive stroke modulation, e-ink-hinted.",
        "preview": "preview_Cartisse_e-ink_opt.png",
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Cartisse-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Cartisse-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Cartisse-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/Cartisse-BoldItalic.ttf",
        },
    },
    # ── e-ink optimized (extra/) — add as needed ─────────────────────────────
    "NV Charis": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Charis SIL)",
        "description": "Scholarly serif with wide Unicode coverage, e-ink-hinted.",
        "preview": None,
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Charis-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Charis-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Charis-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Charis-BoldItalic.ttf",
        },
    },
    "NV Garamond": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Garamond revival)",
        "description": "French Old Style serif, elegant and traditional, e-ink-hinted.",
        "preview": None,
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Garamond-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Garamond-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Garamond-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Garamond-BoldItalic.ttf",
        },
    },
    "NV Palatium": {
        "category": "serif",
        "source": "nicoverbruggen/ebook-fonts (e-ink optimized, Palatino revival)",
        "description": "Warm serif with large x-height, e-ink-hinted.",
        "preview": None,
        "subdir": "core",
        "files": {
            "regular":     "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Palatium-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Palatium-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Palatium-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/nicoverbruggen/ebook-fonts/main/fonts/core/NV_Palatium-BoldItalic.ttf",
        },
    },
    # ── Original source fonts ─────────────────────────────────────────────────
    "Literata": {
        "category": "serif",
        "source": "googlefonts/literata",
        "description": "Google's purpose-designed e-reading font. Transitional serif optimized for long reading sessions on digital screens.",
        "preview": "preview_Literata.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-BoldItalic.ttf",
        },
    },
    "Source Serif 4": {
        "category": "serif",
        "source": "adobe-fonts/source-serif",
        "description": "Adobe's transitional serif with extensive language support. Well-proportioned and highly readable.",
        "preview": "preview_Source_Serif_4.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/adobe-fonts/source-serif/release/TTF/SourceSerif4-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/adobe-fonts/source-serif/release/TTF/SourceSerif4-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/adobe-fonts/source-serif/release/TTF/SourceSerif4-It.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/adobe-fonts/source-serif/release/TTF/SourceSerif4-BoldIt.ttf",
        },
    },
    "Merriweather": {
        "category": "serif",
        "source": "SorkinType/Merriweather",
        "description": "Warm serif designed for screen reading. Generous x-height and soft contrast.",
        "preview": "preview_Merriweather.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/SorkinType/Merriweather/master/fonts/ttf/Merriweather-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/SorkinType/Merriweather/master/fonts/ttf/Merriweather-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/SorkinType/Merriweather/master/fonts/ttf/Merriweather-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/SorkinType/Merriweather/master/fonts/ttf/Merriweather-BoldItalic.ttf",
        },
    },
    "Lora": {
        "category": "serif",
        "source": "cyrealtype/Lora-Cyrillic",
        "description": "Calligraphic serif for literary reading. Elegant curves with excellent readability.",
        "preview": "preview_Lora.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/cyrealtype/Lora-Cyrillic/main/fonts/ttf/Lora-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/cyrealtype/Lora-Cyrillic/main/fonts/ttf/Lora-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/cyrealtype/Lora-Cyrillic/main/fonts/ttf/Lora-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/cyrealtype/Lora-Cyrillic/main/fonts/ttf/Lora-BoldItalic.ttf",
        },
    },
    "Crimson Pro": {
        "category": "serif",
        "source": "google/fonts (ofl/crimsonpro)",
        "description": "Classic old-style serif, traditional and scholarly.",
        "preview": "preview_Crimson_Pro.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/google/fonts/main/ofl/crimsonpro/CrimsonPro-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/google/fonts/main/ofl/crimsonpro/CrimsonPro-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/google/fonts/main/ofl/crimsonpro/CrimsonPro-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/google/fonts/main/ofl/crimsonpro/CrimsonPro-BoldItalic.ttf",
        },
    },
    "Cardo": {
        "category": "serif",
        "source": "davidshaner/cardo",
        "description": "Scholarly serif with wide Unicode coverage. Designed for medievalists and classicists.",
        "preview": "preview_Cardo.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/davidshaner/cardo/master/Cardo-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/davidshaner/cardo/master/Cardo-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/davidshaner/cardo/master/Cardo-Italic.ttf",
        },
    },
    "PT Serif": {
        "category": "serif",
        "source": "google/fonts (ofl/ptserif)",
        "description": "Classic serif by ParaType. Traditional and well-balanced for long reading.",
        "preview": "preview_PT_Serif.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/google/fonts/main/ofl/ptserif/PT_Serif-Web-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/google/fonts/main/ofl/ptserif/PT_Serif-Web-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/google/fonts/main/ofl/ptserif/PT_Serif-Web-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/google/fonts/main/ofl/ptserif/PT_Serif-Web-BoldItalic.ttf",
        },
    },
    "Fira Sans": {
        "category": "sans-serif",
        "source": "mozilla/fira (googlefonts/fira)",
        "description": "Firefox's humanist sans-serif. Clean and modern.",
        "preview": "preview_Fira_Sans_sans.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/google/fonts/main/ofl/firasans/FiraSans-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/google/fonts/main/ofl/firasans/FiraSans-Bold.ttf",
        },
    },
    "Atkinson Hyperlegible": {
        "category": "sans-serif (accessibility)",
        "source": "googlefonts/atkinson-hyperlegible",
        "description": "Accessibility font for low vision. Distinctive letter shapes to prevent confusion.",
        "preview": "preview_Atkinson_Hyperlegible.png",
        "subdir": "upstream",
        "files": {
            "regular":     "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible/main/fonts/ttf/AtkinsonHyperlegible-Regular.ttf",
            "bold":        "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible/main/fonts/ttf/AtkinsonHyperlegible-Bold.ttf",
            "italic":      "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible/main/fonts/ttf/AtkinsonHyperlegible-Italic.ttf",
            "bolditalic":  "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible/main/fonts/ttf/AtkinsonHyperlegible-BoldItalic.ttf",
        },
    },
}

# ── CRC32 (ESP32-compatible) ─────────────────────────────────────────────────

def compute_crc32(filepath: Path) -> int:
    """Compute CRC32 matching esp_rom_crc32_le(0xFFFFFFFF, data) ^ 0xFFFFFFFF.

    Equivalent to: zlib.crc32(data, 0) — which is standard CRC-32 (same as
    Python's zlib.crc32 with init=0, matching the C implementation).
    See generate-font-manifest.py for the reference implementation.
    """
    crc = 0
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


# ── Phase 1: Download ────────────────────────────────────────────────────────

def _download_one(url: str, dest: Path) -> tuple[str, bool, int, str | None]:
    """Download a single file. Returns (url, success, size, error)."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "font-distributor/1.0"})
        urllib.request.urlretrieve(url, dest)
        return (url, True, dest.stat().st_size, None)
    except Exception as e:
        return (url, False, 0, str(e))


def download_fonts(catalog: dict) -> None:
    """Download all TTF files configured in the catalog."""
    print(f"\n=== Downloading {sum(len(f['files']) for f in catalog.values())} font files ===")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)

    tasks = []
    for family, meta in catalog.items():
        family_dir = CACHE_DIR / family.replace(" ", "_")
        family_dir.mkdir(parents=True, exist_ok=True)
        for style, url in meta["files"].items():
            fname = Path(url).name
            dest = family_dir / fname
            if dest.exists() and dest.stat().st_size > 0:
                print(f"  SKIP {family}/{fname} (already cached, {dest.stat().st_size:,} bytes)")
                continue
            tasks.append((family, style, url, dest))

    print(f"  {len(tasks)} files to download (concurrent, 8 workers)...")

    def worker(t):
        family, style, url, dest = t
        result = _download_one(url, dest)
        url, ok, size, err = result
        if ok:
            print(f"  OK   {family}/{style} ({size:,} bytes)")
        else:
            print(f"  FAIL {family}/{style}: {err and err[:80]}")
        return (family, style, dest, ok, size)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for _ in ex.map(worker, tasks):
            pass

    total = sum(f.stat().st_size for d in CACHE_DIR.rglob("*.ttf") for f in [d] if f.exists())
    file_count = len(list(CACHE_DIR.rglob("*.ttf")))
    print(f"\n  Total cached: {file_count} files, {total / 1048576:.1f} MB")


# ── Phase 2: Previews ───────────────────────────────────────────────────────

PREVIEW_TEXT = (
    "The quick brown fox jumps over the lazy dog.\n"
    "Sphinx of black quartz, judge my vow.\n"
    "Pack my box with five dozen liquor jugs."
)
PREVIEW_SAMPLES = "\"Abracadabra!\" he exclaimed, levitating the vase, "
PREVIEW_PT = [14, 16, 18]


def _find_font_file(family: str, style: str, catalog: dict) -> Path | None:
    """Locate a cached TTF file for a given family and style."""
    family_dir = CACHE_DIR / family.replace(" ", "_")
    url = catalog.get(family, {}).get("files", {}).get(style, "")
    if not url:
        # Try without style (fallback)
        return None
    fname = Path(url).name
    return family_dir / fname


def _has_pillow() -> bool:
    try:
        import PIL
        return True
    except ImportError:
        return False


def generate_previews(catalog: dict) -> None:
    """Render PNG previews for each font family."""
    if not _has_pillow():
        print("WARNING: Pillow not installed. Skipping previews.")
        print("  Install: pip install Pillow")
        return

    from PIL import Image, ImageDraw, ImageFont

    if not SCRIPT_DIR.exists():
        return

    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)

    print(f"\n=== Generating font previews ===")

    for family, meta in catalog.items():
        preview_name = meta.get("preview")
        if not preview_name:
            # Generate a default preview name
            preview_name = f"preview_{family.replace(' ', '_')}.png"

        # Find a regular font file to preview
        regular = _find_font_file(family, "regular", catalog)
        if not regular or not regular.exists():
            print(f"  SKIP {family} (no cached TTF)")
            continue

        try:
            # Create a vertical strip: each row is a different size
            rows = []
            for pt in PREVIEW_PT:
                font = ImageFont.truetype(str(regular), size=pt * 4 // 3)  # rough px conversion
                # Measure text
                lines = PREVIEW_TEXT.split("\n")
                max_w = max(font.getlength(line) for line in lines)
                # Height per line ≈ pt * 1.2 in pixels at 96 DPI
                line_h = int(pt * 1.2 * 96 / 72)
                img_h = line_h * len(lines)
                img = Image.new("1", (int(max_w) + 40, img_h), color=1)
                draw = ImageDraw.Draw(img)

                y = 0
                for line in lines:
                    draw.text((20, y), line, fill=0, font=font)
                    y += line_h

                # Add a sample size label
                label = f"{family} — Regular {pt}pt"
                label_font = ImageFont.truetype(str(regular), size=max(12, pt * 4 // 3))
                label_img = Image.new("1", (int(max_w) + 40, line_h + 10), color=1)
                ld = ImageDraw.Draw(label_img)
                ld.text((20, 0), label, fill=0, font=label_font)

                rows.append(img)
                rows.append(label_img)

            # Stack vertically
            total_h = sum(r.height for r in rows)
            max_w = max(r.width for r in rows)
            sheet = Image.new("1", (max_w + 20, total_h + 20), color=1)
            y = 10
            for r in rows:
                sheet.paste(r, (10, y))
                y += r.height

            out_path = PREVIEW_DIR / preview_name
            sheet.save(out_path, "PNG")
            print(f"  OK   {preview_name} ({out_path.stat().st_size:,} bytes)")

        except Exception as e:
            print(f"  FAIL {family}: {e}")


# ── Phase 3: Manifest ───────────────────────────────────────────────────────

def generate_manifest(catalog: dict) -> None:
    """Build the JSON manifest from cached font files."""
    print(f"\n=== Building font manifest ===")

    r2_base = f"https://{os.environ.get('R2_DEV_URL', '')}"
    r2_dev = os.environ.get("R2_DEV_URL", "")
    if r2_dev:
        base_url = r2_dev.rstrip("/") + "/ttf/"
        previews_url = r2_dev.rstrip("/") + "/previews/"
    else:
        # Local only — just set paths relative to ttf/
        base_url = "ttf/"
        previews_url = "previews/"

    families = []
    total_files = 0
    total_size = 0

    # The device derives the on-card folder from the path's last directory
    # component, so two catalog names that slugify to the same folder would
    # collide on the card (both families writing into one directory). Fail
    # before uploading rather than publish a manifest the device cannot map.
    slug_owner = {}
    for family in sorted(catalog.keys()):
        slug = family.replace(" ", "_")
        if slug in slug_owner:
            raise SystemExit(
                f"ERROR: family '{family}' slugifies to '{slug}', already used by "
                f"'{slug_owner[slug]}'. Rename one of them in the catalog."
            )
        slug_owner[slug] = family

    for family in sorted(catalog.keys()):
        meta = catalog[family]
        family_dir = CACHE_DIR / family.replace(" ", "_")

        file_entries = []
        for style in ["regular", "bold", "italic", "bolditalic"]:
            url = meta["files"].get(style)
            if not url:
                continue
            fname = Path(url).name
            fpath = family_dir / fname
            if not fpath.exists():
                print(f"  WARN {family}/{style}: file not cached at {fpath}")
                continue

            size = fpath.stat().st_size
            crc = compute_crc32(fpath)
            r2_path = f"{family.replace(' ', '_')}/{fname}"

            file_entries.append({
                "name": fname,
                "style": style,
                "path": r2_path,
                "size": size,
                "crc32": crc,
            })
            total_files += 1
            total_size += size

        if not file_entries:
            continue

        entry = {
            "name": family,
            "type": meta["category"],
            "description": meta["description"],
            "source": meta["source"],
            "license": "OFL",
            "styles": [e["style"] for e in file_entries],
            "files": file_entries,
        }
        if meta.get("preview"):
            entry["preview"] = f"{previews_url}{meta['preview']}"

        families.append(entry)

    manifest = {
        "version": 1,
        "kind": "ttf",
        "description": (
            "TTF font download manifest for PSRAM-enabled devices (ESP32-S3). "
            "C3/non-PSRAM devices use the .cpfont system via build-sd-fonts.py."
        ),
        "generated": "2026-10-02",
        "license": "SIL Open Font License (OFL) — all fonts unless noted",
        "baseUrl": base_url,
        "previewsUrl": previews_url,
        "families": families,
    }

    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(MANIFEST_PATH, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"  Written: {MANIFEST_PATH}")
    print(f"  Families: {len(families)}")
    print(f"  Files: {total_files} ({total_size / 1048576:.1f} MB)")


# ── Phase 4: Upload ─────────────────────────────────────────────────────────

def _wrangler(*args: str) -> subprocess.CompletedProcess:
    """Run a wrangler command."""
    cmd = [WRANGLER_BIN] + list(args) + ["--remote"]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def upload_to_r2() -> None:
    """Upload all fonts, previews, and manifest to the R2 bucket."""
    print(f"\n=== Uploading to R2 bucket '{R2_BUCKET}' ===")

    # Verify wrangler is available
    result = subprocess.run([WRANGLER_BIN, "whoami"], capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        print(f"ERROR: wrangler not available or not authenticated")
        print(f"  Run: {WRANGLER_BIN} login")
        return

    # Upload TTF files
    ttf_count = 0
    for family_dir in sorted(CACHE_DIR.iterdir()):
        if not family_dir.is_dir():
            continue
        for ttf_file in sorted(family_dir.glob("*.ttf")) + sorted(family_dir.glob("*.otf")):
            r2_key = f"ttf/{family_dir.name}/{ttf_file.name}"
            result = _wrangler("r2", "object", "put", f"{R2_BUCKET}/{r2_key}",
                               "--file", str(ttf_file), "--content-type", "font/ttf")
            if result.returncode == 0:
                ttf_count += 1
            else:
                print(f"  FAIL {r2_key}: {result.stderr.strip()[:100]}")
    print(f"  Uploaded {ttf_count} TTF files")

    # Upload previews
    preview_count = 0
    if PREVIEW_DIR.exists():
        for png in sorted(PREVIEW_DIR.glob("preview_*.png")):
            r2_key = f"previews/{png.name}"
            result = _wrangler("r2", "object", "put", f"{R2_BUCKET}/{r2_key}",
                               "--file", str(png), "--content-type", "image/png")
            if result.returncode == 0:
                preview_count += 1
            else:
                print(f"  FAIL {r2_key}: {result.stderr.strip()[:100]}")
    print(f"  Uploaded {preview_count} preview images")

    # Upload HTML gallery
    html_path = MANIFEST_PATH.parent / "font-gallery.html"
    if html_path.exists():
        result = _wrangler("r2", "object", "put", f"{R2_BUCKET}/index.html",
                           "--file", str(html_path), "--content-type", "text/html")
        if result.returncode == 0:
            print(f"  Uploaded gallery (index.html)")
        else:
            print(f"  FAIL gallery: {result.stderr.strip()[:100]}")

    # Upload manifest
    if MANIFEST_PATH.exists():
        result = _wrangler("r2", "object", "put", f"{R2_BUCKET}/manifest/fonts.json",
                           "--file", str(MANIFEST_PATH), "--content-type", "application/json")
        if result.returncode == 0:
            print(f"  Uploaded manifest (fonts.json)")
        else:
            print(f"  FAIL manifest: {result.stderr.strip()[:100]}")


def make_bucket_public() -> None:
    """Enable r2.dev public access and print the public URL."""
    print(f"\n=== Enabling public R2 access ===")
    result = subprocess.run(
        [WRANGLER_BIN, "r2", "bucket", "dev-url", "enable", R2_BUCKET],
        capture_output=True, text=True, timeout=30
    )
    print(result.stdout.strip())
    if result.returncode != 0:
        print(f"  stderr: {result.stderr.strip()[:200]}")

    # Print the public URL
    print(f"\n  Public URL: https://<r2-dev-url>.r2.dev/")
    print(f"  Manifest: https://<r2-dev-url>.r2.dev/manifest/fonts.json")
    print(f"  Fonts:    https://<r2-dev-url>.r2.dev/ttf/<Family>/<File>.ttf")
    print(f"  Previews: https://<r2-dev-url>.r2.dev/previews/preview_<Family>.png")


# ── Phase: HTML Gallery ──────────────────────────────────────────────────────

def generate_html_gallery(catalog: dict) -> Path:
    """Generate a static HTML gallery page with all font previews."""
    print(f"\n=== Generating HTML preview gallery ===")

    r2_dev = os.environ.get("R2_DEV_URL", "")
    if r2_dev:
        # Strip to base for building relative paths
        base = r2_dev.rstrip("/")
        preview_prefix = f"{base}/previews/"
        manifest_url = f"{base}/manifest/fonts.json"
    else:
        preview_prefix = "../previews/"
        manifest_url = "#local-manifest"

    html_parts = [
        '<!DOCTYPE html>',
        '<html lang="en">',
        '<head>',
        '  <meta charset="utf-8">',
        '  <meta name="viewport" content="width=device-width, initial-scale=1">',
        '  <title>CrossPoint Reader — Font Gallery</title>',
        '  <style>',
        '    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;',
        '           margin: 0; padding: 24px; background: #f8f9fa; color: #1a1a1a; }',
        '    h1 { font-size: 1.5rem; margin: 0 0 4px; }',
        '    .subtitle { color: #6c757d; font-size: 0.9rem; margin-bottom: 24px; }',
        '    .filter-bar { margin-bottom: 20px; display: flex; gap: 12px; flex-wrap: wrap; }',
        '    .filter-btn { padding: 6px 16px; border: 1px solid #dee2e6; border-radius: 20px;',
        '                   background: white; cursor: pointer; font-size: 0.85rem; }',
        '    .filter-btn.active { background: #0d6efd; color: white; border-color: #0d6efd; }',
        '    .font-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(480px, 1fr));',
        '                   gap: 20px; }',
        '    .font-card { background: white; border-radius: 8px; padding: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.1);',
        '                  border: 1px solid #e9ecef; }',
        '    .font-card img { max-width: 100%; height: auto; border: 1px solid #dee2e6; border-radius: 6px; }',
        '    .font-name { font-weight: 700; font-size: 1.1rem; margin: 8px 0 4px; }',
        '    .font-meta { font-size: 0.85rem; color: #495057; margin-bottom: 4px; }',
        '    .font-desc { font-size: 0.85rem; color: #6c757d; line-height: 1.4; }',
        '    .badge { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 0.75rem;',
        '              font-weight: 600; margin-right: 4px;',
        '              background: #e9ecef; color: #495057; }',
        '    .badge.eink { background: #e3f2fd; color: #1565c0; }',
        '    .badge.accessibility { background: #e8f5e9; color: #2e7d32; }',
        '    .badge.dyslexia { background: #fff3e0; color: #e65100; }',
        '    .badge.serif { background: #f3e5f5; color: #6a1b9a; }',
        '    .badge.sans { background: #e0f2f1; color: #00695c; }',
        '    .badge.slab { background: #fff8e1; color: #8d6e63; }',
        '  </style>',
        '</head>',
        '<body>',
        f'  <h1>CrossPoint Reader Font Gallery</h1>',
        f'  <div class="subtitle">Font previews for PSRAM-enabled devices (ESP32-S3). ',
        f'Download the <a href="{manifest_url}">manifest</a> or browse the <a href="{preview_prefix}">previews</a>.</div>',
        '  <div class="filter-bar">',
        '    <div class="filter-btn active" data-filter="all">All</div>',
        '    <div class="filter-btn" data-filter="eink">E-ink Optimized</div>',
        '    <div class="filter-btn" data-filter="accessibility">Accessibility</div>',
        '    <div class="filter-btn" data-filter="serif">Serif</div>',
        '    <div class="filter-btn" data-filter="sans">Sans-serif</div>',
        '    <div class="filter-btn" data-filter="slab">Slab-serif</div>',
        '  </div>',
        '  <div class="font-grid">',
    ]

    for family, meta in sorted(catalog.items()):
        preview = meta.get("preview")
        if not preview:
            continue

        is_eink = "ebook-fonts" in meta.get("source", "")
        is_accessibility = "accessibility" in meta.get("category", "")
        is_dyslexia = "dyslexia" in meta.get("category", "")
        cat = meta.get("category", "serif").lower()

        badges = []
        if is_eink:
            badges.append('<span class="badge eink">E-ink optimized</span>')
        if is_accessibility:
            badges.append('<span class="badge accessibility">Accessibility</span>')
        if is_dyslexia:
            badges.append('<span class="badge dyslexia">Dyslexia</span>')
        if "serif" in cat and "sans" not in cat and "slab" not in cat:
            badges.append('<span class="badge serif">Serif</span>')
        elif "slab" in cat:
            badges.append('<span class="badge slab">Slab-serif</span>')
        elif "sans" in cat:
            badges.append('<span class="badge sans">Sans-serif</span>')

        # Data attributes for filtering
        filter_tags = []
        if is_eink:
            filter_tags.append("eink")
        if is_accessibility or is_dyslexia:
            filter_tags.append("accessibility")
        if "serif" in cat:
            filter_tags.append("serif")
        if "sans" in cat and "slab" not in cat:
            filter_tags.append("sans")
        if "slab" in cat:
            filter_tags.append("slab")

        html_parts.append(
            f'    <div class="font-card" data-tags="{" ".join(filter_tags)}">'
        )
        html_parts.append(
            f'      <img src="{preview_prefix}{preview}" alt="{family} preview" loading="lazy">'
        )
        html_parts.append(f'      <div class="font-name">{family}</div>')
        html_parts.append(f'      <div class="font-meta">{" ".join(badges)}</div>')
        html_parts.append(f'      <div class="font-desc">{meta["description"]}</div>')
        html_parts.append(f'      <div class="font-meta">Source: {meta["source"]}</div>')
        html_parts.append('    </div>')

    html_parts.extend([
        '  </div>',
        '  <script>',
        '    document.querySelectorAll(".filter-btn").forEach(btn => {',
        '      btn.addEventListener("click", () => {',
        '        document.querySelector(".filter-btn.active").classList.remove("active");',
        '        btn.classList.add("active");',
        '        const filter = btn.dataset.filter;',
        '        document.querySelectorAll(".font-card").forEach(card => {',
        '          const tags = card.dataset.tags;',
        '          if (filter === "all" || tags.includes(filter)) {',
        '            card.style.display = "";',
        '          } else {',
        '            card.style.display = "none";',
        '          }',
        '        });',
        '      });',
        '    });',
        '  </script>',
        '</body>',
        '</html>',
    ])

    html_path = MANIFEST_PATH.parent / "font-gallery.html"
    with open(html_path, "w") as f:
        f.write("\n".join(html_parts))

    print(f"  Written: {html_path} ({html_path.stat().st_size:,} bytes)")
    return html_path


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Distribute TTF fonts for CrossPoint reader (PSRAM/S3 devices)"
    )
    parser.add_argument("--download", action="store_true", help="Download font files")
    parser.add_argument("--previews", action="store_true", help="Generate preview PNGs")
    parser.add_argument("--manifest", action="store_true", help="Build fonts.json manifest")
    parser.add_argument("--html", action="store_true", help="Generate HTML preview gallery")
    parser.add_argument("--upload", action="store_true", help="Upload to R2")
    parser.add_argument("--make-public", action="store_true", help="Enable r2.dev public URL")
    parser.add_argument("--all", action="store_true", help="Run all phases")
    args = parser.parse_args()

    if not any([args.download, args.previews, args.manifest, args.html,
                args.upload, args.make_public]):
        args.all = True

    if args.all or args.download:
        download_fonts(FONT_CATALOG)
    if args.all or args.previews:
        generate_previews(FONT_CATALOG)
    if args.all or args.manifest:
        generate_manifest(FONT_CATALOG)
    if args.all or args.html:
        generate_html_gallery(FONT_CATALOG)
    if args.all or args.upload:
        upload_to_r2()
    if args.all or args.make_public:
        make_bucket_public()


if __name__ == "__main__":
    main()
