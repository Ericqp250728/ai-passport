#!/usr/bin/env python3
"""Generate the notification-mirror LVGL fonts.

Notification text is dynamic, so the fonts carry a fixed travel character set
rather than only the firmware's own strings:

- printable ASCII and Latin-1
- GB2312 level-1 Chinese (3,755 common simplified characters)
- JIS X 0208 level-1 kanji (2,965 common Japanese characters, e.g. 駅)
- hiragana, katakana, CJK punctuation, full-width forms, arrows
- every character in the firmware's own on-screen strings

All glyphs come from Noto Sans SC (it covers the whole set). Emoji are not
included; main/notif_model.c strips them before display.

    python3 tools/gen_mirror_fonts.py            # regenerate assets/fonts/mirror_*.c
    python3 tools/gen_mirror_fonts.py --check    # coverage check only, no Node.js needed

Generation needs Node.js (npx). Source fonts are downloaded once into
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
EXTRA_INVENTORY = FONT_DIR / "mirror_glyphs_extra.txt"

LV_FONT_CONV = "lv_font_conv@1.5.3"
NOTO_COMMIT = "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
NOTO_URL = ("https://raw.githubusercontent.com/notofonts/noto-cjk/"
            f"{NOTO_COMMIT}/Sans/SubsetOTF/SC/NotoSansSC-{{weight}}.otf")
NOTO_SHA256 = {
    "Regular": "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9",
    "Bold": "c6cb5a93abaa9edc8ee7463b7ebb7f42d618d40e6ed2f7a5371c97b0b64767c0",
}

# Files whose string literals end up on screen.
TEXT_SOURCES = ("main/mirror_ui.c", "main/main.c", "main/notif_model.c")

# Rare punctuation in the ranges below that Noto Sans SC does not provide.
NOT_IN_SOURCE = {0x2017, 0x201B, 0x201F, 0x2023, 0x2024, 0x2031, 0x2034, 0x2036, 0x2037, 0x2038}

# (output name, weight, size, bits per pixel). 24 px uses 2 bpp to halve its size.
FONTS = (
    ("mirror_font_16", "Regular", 16, 4),
    ("mirror_font_24", "Bold", 24, 2),
)

_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
_LOG_FORMAT = re.compile(r'(ESP_LOG[EWIDV]\s*\(\s*TAG\s*,\s*)((?:"(?:\\.|[^"\\\n])*"\s*)+)')


def string_literals(source: str) -> list[str]:
    """Decoded contents of on-screen C string literals, skipping comments and logs."""
    source = _LOG_FORMAT.sub(lambda m: m.group(1), source)
    out = []
    for match in _TOKEN.finditer(source):
        token = match.group(0)
        if not token.startswith('"'):
            continue
        body = token[1:-1]
        raw = bytearray()
        i = 0
        while i < len(body):
            if body.startswith("\\x", i) and re.match(r"[0-9A-Fa-f]{2}", body[i + 2:i + 4]):
                raw += bytes([int(body[i + 2:i + 4], 16)])
                i += 4
            elif body[i] == "\\" and i + 1 < len(body):
                raw += {"n": b"\n", "t": b"\t"}.get(body[i + 1], body[i + 1].encode())
                i += 2
            else:
                raw += body[i].encode("utf-8")
                i += 1
        out.append(raw.decode("utf-8", errors="replace"))
    return out


def _double_byte(codec: str, rows: range) -> set[int]:
    found = set()
    for hi in rows:
        for lo in range(0xA1, 0xFF):
            try:
                found.add(ord(bytes([hi, lo]).decode(codec)))
            except UnicodeDecodeError:
                pass
    return found


def base_codepoints() -> set[int]:
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100))
    cps |= _double_byte("gb2312", range(0xB0, 0xD8))    # GB2312 level 1
    cps |= _double_byte("euc_jp", range(0xB0, 0xD0))    # JIS X 0208 level 1 kanji
    cps |= set(range(0x3000, 0x3040))                   # CJK symbols and punctuation
    cps |= set(range(0x3041, 0x3097)) | set(range(0x309B, 0x309F))   # hiragana
    cps |= set(range(0x30A0, 0x3100))                   # katakana
    cps |= set(range(0xFF01, 0xFF5F)) | set(range(0xFFE0, 0xFFE7))   # full-width forms
    cps |= set(range(0x2010, 0x2028)) | set(range(0x2030, 0x203C))   # general punctuation
    cps -= NOT_IN_SOURCE
    cps |= set(range(0x2190, 0x219A))                   # arrows
    cps |= {0x2103, 0x2116, 0x2121, 0x25B2, 0x25BC, 0x25CB, 0x25CF, 0x2605, 0x2606}
    return cps


def literal_codepoints(root: Path = ROOT) -> set[int]:
    cps = set()
    for rel in TEXT_SOURCES:
        path = root / rel
        if path.exists():
            for literal in string_literals(path.read_text(encoding="utf-8")):
                cps.update(ord(ch) for ch in literal if ch.isprintable() and ch != "�")
    return cps


def required_codepoints(root: Path = ROOT) -> set[int]:
    return base_codepoints() | literal_codepoints(root)


def generated_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    return {int(m.group(1), 16) for m in re.finditer(r"/\* U\+([0-9A-F]{4,6}) ", text)}


def missing_glyphs(root: Path = ROOT) -> dict[str, list[str]]:
    """Firmware strings must be fully covered; the base set may lack glyphs the font lacks."""
    literals = literal_codepoints(root)
    problems: dict[str, list[str]] = {}
    for name, *_ in FONTS:
        path = root / "assets" / "fonts" / f"{name}.c"
        if not path.exists():
            problems[name] = ["<font file missing>"]
            continue
        have = generated_codepoints(path)
        missing = [f"U+{cp:04X} {chr(cp)}" for cp in sorted(literals)
                   if cp not in have and cp != 0x20]
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
    base = base_codepoints()
    literals = literal_codepoints()
    extra = sorted(literals - base)
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    EXTRA_INVENTORY.write_text(
        "# Generated by tools/gen_mirror_fonts.py: on-screen characters outside the base set.\n"
        + "".join(chr(cp) for cp in extra) + "\n",
        encoding="utf-8",
    )
    symbols = "".join(chr(cp) for cp in sorted(base | literals) if cp != 0x20)
    for name, weight, size, bpp in FONTS:
        print(f"{name}: {len(symbols) + 1} glyphs, Noto Sans SC {weight} {size}px {bpp}bpp")
        subprocess.run([
            "npx", "-y", LV_FONT_CONV,
            "--font", str(source_font(weight)),
            "--symbols", symbols, "-r", "0x20",
            "--size", str(size), "--bpp", str(bpp),
            "--format", "lvgl", "--no-compress",
            "--lv-font-name", name, "--lv-include", "lvgl.h",
            "-o", str(FONT_DIR / f"{name}.c"),
        ], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="only verify that the fonts cover every firmware string")
    args = parser.parse_args()
    if not args.check:
        generate()
    problems = missing_glyphs()
    for name, missing in problems.items():
        print(f"{name}: missing {len(missing)} glyph(s): {', '.join(missing[:20])}", file=sys.stderr)
    if problems:
        if args.check:
            print("Run: python3 tools/gen_mirror_fonts.py", file=sys.stderr)
        return 1
    print("mirror fonts cover all firmware strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
