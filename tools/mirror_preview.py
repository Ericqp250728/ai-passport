#!/usr/bin/env python3
"""Render the notification-mirror pages on the host and save PNG screenshots.

Compiles LVGL plus the real main/mirror_ui.c with tools/mirror_preview/lv_conf.h,
renders every page with made-up sample notifications, applies the device's 30 px
corner mask, and writes build/mirror-preview/*.png and a side-by-side preview.png:

    python3 tools/mirror_preview.py
    python3 tools/mirror_preview.py --lvgl /path/to/lvgl   # LVGL 9.5 source tree

By default the LVGL source comes from managed_components/lvgl__lvgl, which the
first `idf.py build` downloads.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "mirror-preview"
CC = os.environ.get("CC", "cc")


def compile_objects(sources: list[Path], obj_dir: Path, flags: list[str]) -> list[Path]:
    obj_dir.mkdir(parents=True, exist_ok=True)
    jobs = []
    for src in sources:
        obj = obj_dir / (str(src.relative_to(src.anchor)).replace(os.sep, "_") + ".o")
        jobs.append((src, obj))

    def build(job: tuple[Path, Path]) -> Path:
        src, obj = job
        if not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime:
            subprocess.run([CC, *flags, "-c", str(src), "-o", str(obj)], check=True)
        return obj

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        return list(pool.map(build, jobs))


def ppm_to_png(ppm: Path, png: Path) -> tuple[int, int, bytes]:
    data = ppm.read_bytes()
    header, rest = data.split(b"\n255\n", 1)
    width, height = map(int, header.split()[1:3])
    write_png(png, width, height, rest)
    return width, height, rest


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 9))
                     + chunk(b"IEND", b""))


def contact_sheet(images: list[tuple[int, int, bytes]], gap: int = 16) -> tuple[int, int, bytes]:
    width = sum(w for w, _, _ in images) + gap * (len(images) + 1)
    height = max(h for _, h, _ in images) + gap * 2
    bg = bytes((0x33, 0x33, 0x33))
    out = bytearray(bg * (width * height))
    x0 = gap
    for w, h, rgb in images:
        for y in range(h):
            start = ((y + gap) * width + x0) * 3
            out[start:start + w * 3] = rgb[y * w * 3:(y + 1) * w * 3]
        x0 += w + gap
    return width, height, bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lvgl", type=Path, default=ROOT / "managed_components" / "lvgl__lvgl",
                        help="LVGL 9.5 source tree (default: managed_components/lvgl__lvgl)")
    args = parser.parse_args()
    lvgl = args.lvgl.resolve()
    if not (lvgl / "lvgl.h").exists():
        print(f"LVGL source not found at {lvgl}; run `idf.py build` once or pass --lvgl",
              file=sys.stderr)
        return 1

    conf_dir = ROOT / "tools" / "mirror_preview"
    flags = ["-std=gnu11", "-O1", "-w", "-DLV_CONF_INCLUDE_SIMPLE",
             f"-I{conf_dir}", f"-I{lvgl}", f"-I{lvgl.parent}", f"-I{ROOT / 'main'}",
             f"-I{ROOT / 'components' / 'bsp' / 'src'}"]
    lvgl_sources = sorted((lvgl / "src").rglob("*.c"))
    app_sources = [
        conf_dir / "preview.c",
        ROOT / "main" / "mirror_ui.c",
        ROOT / "main" / "mirror_nav.c",
        ROOT / "main" / "notif_model.c",
        ROOT / "main" / "ancs_proto.c",
        ROOT / "components" / "bsp" / "src" / "bsp_display_rounding.c",
        *sorted((ROOT / "assets" / "fonts").glob("mirror_font_*.c")),
    ]
    print(f"compiling {len(lvgl_sources)} LVGL sources (cached after the first run)...")
    objs = compile_objects(lvgl_sources, OUT / "obj" / "lvgl", flags)
    objs += compile_objects(app_sources, OUT / "obj" / "app", flags)
    exe = OUT / "mirror_preview"
    subprocess.run([CC, *map(str, objs), "-lm", "-o", str(exe)], check=True)

    frames = OUT / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe), str(frames)], check=True)

    images = []
    for ppm in sorted(frames.glob("*.ppm")):
        images.append(ppm_to_png(ppm, OUT / f"{ppm.stem}.png"))
    write_png(OUT / "preview.png", *contact_sheet(images))
    print(f"wrote {OUT / 'preview.png'} and {len(images)} page screenshots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
