#!/usr/bin/env python3
"""Check that the generated notification-mirror fonts cover the travel character set."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_mirror_fonts as fonts  # noqa: E402


class MirrorFontTest(unittest.TestCase):
    def test_firmware_strings_are_covered(self) -> None:
        self.assertEqual(fonts.missing_glyphs(), {}, "run: python3 tools/gen_mirror_fonts.py")

    def test_travel_set_is_covered(self) -> None:
        for name, *_ in fonts.FONTS:
            have = fonts.generated_codepoints(ROOT / "assets" / "fonts" / f"{name}.c")
            missing = fonts.base_codepoints() - have - {0x20}
            self.assertEqual(sorted(missing)[:10], [], name)
            # 常用中文、日文假名与日本汉字都在
            for ch in "新宿駅東口ありがとうカタカナ明治通り右转左折→":
                self.assertIn(ord(ch), have, f"{name}: {ch}")
            self.assertNotIn(0x1F600, have)          # 表情不在字库,由 notif_sanitize 去掉

    def test_literal_scan_skips_comments_and_logs(self) -> None:
        src = '// "注释"\nESP_LOGW(TAG, "日志");\nlabel("显示");\n'
        self.assertEqual(fonts.string_literals(src), ["显示"])


if __name__ == "__main__":
    unittest.main()
