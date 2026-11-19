"""A missing expensive segment cannot improve a budget verdict."""
import unittest

from analyze import BUDGET_MS, FRAMES, SEGMENTS, verdict


class CoverageTests(unittest.TestCase):
    def test_missing_expensive_segment(self):
        samples = {key: [BUDGET_MS] * FRAMES for key in SEGMENTS}
        self.assertEqual(verdict(samples), "pass")
        samples["authored"] = [BUDGET_MS + 1] * FRAMES
        self.assertEqual(verdict(samples), "miss")
        del samples["authored"]
        self.assertEqual(verdict(samples), "inconclusive")

    def test_incomplete_timing_is_inconclusive(self):
        samples = {key: [0] * FRAMES for key in SEGMENTS}
        samples["wide"].pop()
        self.assertEqual(verdict(samples), "inconclusive")


if __name__ == "__main__":
    unittest.main()
