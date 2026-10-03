#!/usr/bin/env python3
"""Generate the stall-badge LVGL font subsets from the strings the firmware uses.

Collects every character inside C string literals of the badge sources (comments
are ignored), adds printable ASCII, and converts Noto Sans SC into LVGL C fonts
with a pinned lv_font_conv. Re-run after editing shop content or UI text:

    python3 tools/gen_badge_fonts.py            # regenerate assets/fonts/badge_*.c
    python3 tools/gen_badge_fonts.py --check    # only verify coverage, no node needed

Requires Node.js (npx) for generation. The source fonts are downloaded once into
build/font-cache/ from a pinned noto-cjk commit and verified by SHA-256.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
CACHE_DIR = ROOT / "build" / "font-cache"
INVENTORY = FONT_DIR / "badge_glyphs.txt"

LV_FONT_CONV = "lv_font_conv@1.5.3"
NOTO_COMMIT = "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
NOTO_URL = ("https://raw.githubusercontent.com/notofonts/noto-cjk/"
            f"{NOTO_COMMIT}/Sans/SubsetOTF/SC/NotoSansSC-{{weight}}.otf")
NOTO_SHA256 = {
    "Regular": "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9",
    "Bold": "c6cb5a93abaa9edc8ee7463b7ebb7f42d618d40e6ed2f7a5371c97b0b64767c0",
}

# Files whose string literals end up on screen. The local content file is optional.
TEXT_SOURCES = (
    "main/badge_ui.c",
    "main/main.c",
    "main/badge_content.c",
    "main/badge_content_local.c",
)

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
YUAN = "¥"
NUMBER_GLYPHS = "0123456789,." + YUAN

# (output name, weight, size, glyph set: "text" or "number")
FONTS = (
    ("badge_font_16", "Regular", 16, "text"),
    ("badge_font_20", "Bold", 20, "text"),
    ("badge_font_32", "Bold", 32, "text"),
    ("badge_num_48", "Bold", 48, "number"),
)

_TOKEN = re.compile(
    r'//[^\n]*'                      # line comment
    r'|/\*.*?\*/'                    # block comment
    r'|"(?:\\.|[^"\\\n])*"'          # string literal
    r"|'(?:\\.|[^'\\\n])*'",         # char literal
    re.S,
)


# Log format strings go to the serial console, never to the screen.
_LOG_FORMAT = re.compile(r'(ESP_LOG[EWIDV]\s*\(\s*TAG\s*,\s*)((?:"(?:\\.|[^"\\\n])*"\s*)+)')


def string_literals(source: str) -> list[str]:
    """Return decoded contents of on-screen C string literals, skipping comments and logs."""
    source = _LOG_FORMAT.sub(lambda m: m.group(1), source)
    out = []
    for match in _TOKEN.finditer(source):
        token = match.group(0)
        if not token.startswith('"'):
            continue
        body = token[1:-1]
        # Decode \xNN byte escapes as UTF-8 (e.g. "\xC2\xA5"); keep other text as-is.
        raw = bytearray()
        i = 0
        while i < len(body):
            if body.startswith("\\x", i) and re.match(r"[0-9A-Fa-f]{2}", body[i + 2:i + 4]):
                raw += bytes([int(body[i + 2:i + 4], 16)])
                i += 4
            elif body[i] == "\\" and i + 1 < len(body):
                raw += body[i + 1].encode("utf-8")
                i += 2
            else:
                raw += body[i].encode("utf-8")
                i += 1
        out.append(raw.decode("utf-8", errors="replace"))
    return out


def required_text_glyphs(root: Path = ROOT) -> str:
    chars = set(ASCII) | {YUAN}
    for rel in TEXT_SOURCES:
        path = root / rel
        if not path.exists():
            continue
        for literal in string_literals(path.read_text(encoding="utf-8")):
            chars.update(ch for ch in literal if ch.isprintable() and ch != "�")
    return "".join(sorted(chars))


def glyph_set(kind: str, text_glyphs: str) -> str:
    return text_glyphs if kind == "text" else NUMBER_GLYPHS


def generated_codepoints(path: Path) -> set[int]:
    """Code points present in an lv_font_conv C output (from its per-glyph comments)."""
    found = set()
    for match in re.finditer(r"/\* U\+([0-9A-F]{4,6}) ", path.read_text(encoding="utf-8")):
        found.add(int(match.group(1), 16))
    return found


def missing_glyphs(root: Path = ROOT) -> dict[str, list[str]]:
    text_glyphs = required_text_glyphs(root)
    problems: dict[str, list[str]] = {}
    for name, _weight, _size, kind in FONTS:
        path = root / "assets" / "fonts" / f"{name}.c"
        if not path.exists():
            problems[name] = ["<font file missing>"]
            continue
        have = generated_codepoints(path)
        missing = [f"U+{ord(ch):04X} {ch}" for ch in glyph_set(kind, text_glyphs)
                   if ord(ch) not in have and ch != " "]
        if missing:
            problems[name] = missing
    return problems


def source_font(weight: str) -> Path:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    path = CACHE_DIR / f"NotoSansSC-{weight}.otf"
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != NOTO_SHA256[weight]:
        url = NOTO_URL.format(weight=weight)
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=300) as response:
            data = response.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != NOTO_SHA256[weight]:
            sys.exit(f"SHA-256 mismatch for {url}: {digest}")
        path.write_bytes(data)
    return path


def generate() -> None:
    text_glyphs = required_text_glyphs()
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    INVENTORY.write_text(
        "# Generated by tools/gen_badge_fonts.py; glyphs in the badge text fonts.\n"
        + text_glyphs.replace("\n", "") + "\n",
        encoding="utf-8",
    )
    for name, weight, size, kind in FONTS:
        symbols = glyph_set(kind, text_glyphs)
        cmd = [
            "npx", "-y", LV_FONT_CONV,
            "--font", str(source_font(weight)),
            "--symbols", symbols,
            "--size", str(size), "--bpp", "4",
            "--format", "lvgl", "--no-compress",
            "--lv-font-name", name, "--lv-include", "lvgl.h",
            "-o", str(FONT_DIR / f"{name}.c"),
        ]
        print(f"{name}: {len(symbols)} glyphs, Noto Sans SC {weight} {size}px")
        subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="only verify that the generated fonts cover every used glyph")
    args = parser.parse_args()
    if not args.check:
        generate()
    problems = missing_glyphs()
    for name, missing in problems.items():
        print(f"{name}: missing {len(missing)} glyph(s): {', '.join(missing[:20])}",
              file=sys.stderr)
    if problems:
        if args.check:
            print("Run: python3 tools/gen_badge_fonts.py", file=sys.stderr)
        return 1
    print("badge fonts cover all used glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
