"""The 16, 24 and 32 pixel Ludus icons are drawn by render_small_icons.py and stay in step with it."""
from __future__ import annotations

import unittest

import render_small_icons

try:
    import PIL  # noqa: F401
except ImportError:  # Pillow is not a managed dependency; the check runs where it is installed.
    PIL = None


@unittest.skipIf(PIL is None, "Pillow is not installed")
class SmallIconTests(unittest.TestCase):
    def test_committed_pngs_match_a_fresh_render(self):
        self.assertEqual(render_small_icons.main(["--check"]), 0)

    def test_sizes_and_transparent_corners(self):
        for size, image in render_small_icons.render_all().items():
            with self.subTest(size=size):
                image = image.convert("RGBA")
                self.assertEqual(image.size, (size, size))
                self.assertEqual(image.getpixel((0, 0))[3], 0)
                self.assertEqual(image.getpixel((size // 2, size // 2))[3], 255)

    def test_the_letter_keeps_its_three_planes(self):
        colours = {(0x5d, 0xd2, 0xc5), (0xb6, 0xf3, 0xe8), (0x22, 0x9e, 0x95)}
        for size, image in render_small_icons.render_all().items():
            with self.subTest(size=size):
                image = image.convert("RGBA")
                seen = {pixel[:3] for pixel in image.getdata() if pixel[3] == 255}
                for colour in colours:
                    self.assertIn(colour, seen)


if __name__ == "__main__":
    unittest.main()
