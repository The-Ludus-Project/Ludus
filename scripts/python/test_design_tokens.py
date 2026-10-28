"""config/design-tokens.json: well formed, contrast holds, and every consumer reads it."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import design_tokens  # noqa: E402
import init_theme  # noqa: E402


class TokenFileTests(unittest.TestCase):
    def setUp(self):
        self.document = design_tokens.load()

    def test_contrast_pairs_hold_in_both_appearances(self):
        self.assertEqual(design_tokens.failing_pairs(self.document), [])

    def test_malformed_tokens_are_rejected(self):
        bad_name = json.loads(json.dumps(self.document))
        bad_name["colours"]["surface"] = bad_name["colours"].pop("surface.workspace")
        with self.assertRaises(design_tokens.TokenError):
            design_tokens.validate(bad_name)
        bad_value = json.loads(json.dumps(self.document))
        bad_value["colours"]["surface.panel"]["dark"] = "#FFF"
        with self.assertRaises(design_tokens.TokenError):
            design_tokens.validate(bad_value)
        unknown = json.loads(json.dumps(self.document))
        unknown["contrast"][0]["foreground"] = "text.missing"
        with self.assertRaises(design_tokens.TokenError):
            design_tokens.validate(unknown)

    def test_a_failing_pair_is_reported(self):
        document = json.loads(json.dumps(self.document))
        document["colours"]["text.primary"]["dark"] = document["colours"]["surface.panel"]["dark"]
        self.assertTrue(any("dark" in failure for failure in design_tokens.failing_pairs(document)))

    def test_viewport_stays_dark_in_both_appearances(self):
        viewport = self.document["colours"]["surface.viewport"]
        self.assertEqual(viewport["dark"], viewport["light"])


class ConsumerTests(unittest.TestCase):
    def setUp(self):
        self.document = design_tokens.load()

    def test_browser_stylesheet_carries_the_current_tokens(self):
        stylesheet = design_tokens.BROWSER_STYLESHEET.read_text(encoding="utf-8")
        self.assertTrue(design_tokens.stylesheet_is_current(self.document, stylesheet),
                        "run python3 scripts/python/design_tokens.py --write")

    def test_stale_stylesheet_is_detected(self):
        stylesheet = design_tokens.BROWSER_STYLESHEET.read_text(encoding="utf-8")
        stale = stylesheet.replace(self.document["colours"]["surface.workspace"]["dark"], "#000000", 1)
        self.assertFalse(design_tokens.stylesheet_is_current(self.document, stale))
        self.assertFalse(design_tokens.stylesheet_is_current(self.document, "body {}"))

    def test_browser_stylesheet_uses_variables_not_literals(self):
        stylesheet = design_tokens.BROWSER_STYLESHEET.read_text(encoding="utf-8")
        hand_written = stylesheet[stylesheet.index(design_tokens.END_MARKER):]
        # The translucent dialog scrim (#000a) is not a theme colour; every opaque colour must be a token.
        self.assertNotRegex(hand_written, r"#[0-9a-fA-F]{6}\b")

    def test_css_variable_names(self):
        self.assertEqual(design_tokens.css_variable("surface.workspace"), "--ludus-surface-workspace")
        self.assertEqual(design_tokens.css_variable("accent.onPrimary"), "--ludus-accent-on-primary")

    def test_init_palettes_come_from_the_tokens(self):
        for appearance, palette in init_theme.PALETTES.items():
            with self.subTest(appearance=appearance):
                self.assertEqual(palette.workspace, design_tokens.colour(self.document, "surface.workspace", appearance))
                self.assertEqual(palette.primary, design_tokens.colour(self.document, "accent.primary", appearance))
                self.assertEqual(palette.error, design_tokens.colour(self.document, "status.danger", appearance))
        self.assertEqual(init_theme.SPACE_MEDIUM, self.document["spacing"]["medium"])


if __name__ == "__main__":
    unittest.main()
