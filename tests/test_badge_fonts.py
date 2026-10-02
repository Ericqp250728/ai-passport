#!/usr/bin/env python3
"""Check that the generated stall-badge fonts cover every on-screen glyph."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_badge_fonts as fonts  # noqa: E402


class StringLiteralTest(unittest.TestCase):
    def test_skips_comments_and_log_formats(self) -> None:
        source = (
            '// "注释"\n/* "块注释" */\n'
            'ESP_LOGE(TAG, "日志 %s" "续行", x);\n'
            'toast("成功", false); char c = \'"\';\n'
            'const char *y = "\\xC2\\xA5\\"引号";\n'
        )
        self.assertEqual(fonts.string_literals(source), ["成功", '¥"引号'])

    def test_required_glyphs_include_content_and_ui_text(self) -> None:
        glyphs = fonts.required_text_glyphs()
        for ch in "扫码付款记一笔本场账本¥▲▼":
            self.assertIn(ch, glyphs)
        self.assertNotIn("龘", glyphs)

    def test_local_content_is_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "main").mkdir()
            (root / "main" / "badge_content_local.c").write_text(
                'const char *shop = "龘龘小店";\n', encoding="utf-8")
            self.assertIn("龘", fonts.required_text_glyphs(root))


class CoverageTest(unittest.TestCase):
    def test_generated_fonts_cover_every_used_glyph(self) -> None:
        problems = fonts.missing_glyphs()
        self.assertEqual(problems, {}, "run: python3 tools/gen_badge_fonts.py")

    def test_coverage_check_reports_a_missing_glyph(self) -> None:
        font = ROOT / "assets" / "fonts" / "badge_font_16.c"
        have = fonts.generated_codepoints(font)
        self.assertIn(ord("记"), have)          # 记
        self.assertNotIn(0x9F98, have)               # deliberately absent

    def test_number_font_has_digits_and_currency(self) -> None:
        have = fonts.generated_codepoints(ROOT / "assets" / "fonts" / "badge_num_48.c")
        for ch in fonts.NUMBER_GLYPHS:
            self.assertIn(ord(ch), have)


if __name__ == "__main__":
    unittest.main()
