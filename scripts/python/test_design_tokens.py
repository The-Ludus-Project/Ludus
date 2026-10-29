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

    def test_high_contrast_pairs_reach_seven_to_one_for_text(self):
        for appearance in design_tokens.APPEARANCES:
            for pair in self.document["contrast"]:
                if pair["minimum"] != 4.5:
                    continue
                with self.subTest(appearance=appearance, pair=pair["label"]):
                    self.assertGreaterEqual(pair["highContrastMinimum"], 7)
                    ratio = design_tokens.contrast_ratio(
                        design_tokens.colour(self.document, pair["foreground"], appearance, True),
                        design_tokens.colour(self.document, pair["background"], appearance, True))
                    self.assertGreaterEqual(ratio, 7)

    def test_high_contrast_never_weakens_a_pair(self):
        for pair in self.document["contrast"]:
            self.assertGreaterEqual(pair["highContrastMinimum"], pair["minimum"], pair["label"])

    def test_a_weak_high_contrast_colour_is_reported(self):
        document = json.loads(json.dumps(self.document))
        document["highContrast"]["dark"]["text.primary"] = document["colours"]["surface.panel"]["dark"]
        self.assertTrue(any("high contrast" in failure for failure in design_tokens.failing_pairs(document)))

    def test_unknown_high_contrast_colour_and_bad_type_scale_are_rejected(self):
        unknown = json.loads(json.dumps(self.document))
        unknown["highContrast"]["light"]["text.missing"] = "#000000"
        with self.assertRaises(design_tokens.TokenError):
            design_tokens.validate(unknown)
        bad_scale = json.loads(json.dumps(self.document))
        bad_scale["type"]["body"] = 0.1
        with self.assertRaises(design_tokens.TokenError):
            design_tokens.validate(bad_scale)

    def test_type_roles_grow_from_caption_to_heading(self):
        scales = self.document["type"]
        self.assertEqual(scales["body"], 1.0)
        self.assertLess(scales["caption"], scales["body"])
        self.assertLess(scales["body"], scales["title"])
        self.assertLess(scales["title"], scales["heading"])

    def test_output_shader_backdrop_matches_the_viewport_tokens(self):
        shader = (design_tokens.REPOSITORY_ROOT / "modules" / "graphics" / "renderer" / "shaders"
                  / "renderer_output.slang").read_text(encoding="utf-8")
        for constant, token in (("BACKDROP_TOP_SRGB", "surface.viewportTop"),
                                ("BACKDROP_BOTTOM_SRGB", "surface.viewport")):
            value = design_tokens.colour(self.document, token, "dark").lstrip("#")
            channels = ", ".join(f"{int(value[index:index + 2], 16)}.0" for index in (0, 2, 4))
            self.assertIn(f"{constant} = float3({channels}) / 255.0", shader, token)

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

    def test_browser_stylesheet_has_high_contrast_overrides(self):
        stylesheet = design_tokens.BROWSER_STYLESHEET.read_text(encoding="utf-8")
        self.assertIn("@media (prefers-contrast: more)", stylesheet)
        self.assertIn(self.document["highContrast"]["dark"]["text.primary"], stylesheet)

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
