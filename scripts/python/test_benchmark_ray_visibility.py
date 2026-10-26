"""Validate paired timing analysis and reject incomplete/corrupt render evidence."""

import math
from pathlib import Path
import struct
import tempfile
import unittest

from benchmark_ray_visibility import bootstrap_interval, compare_images, summarize_run


class VisibilityBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.configuration = dict(version=1, extent=1, target_samples=4, warmup_samples=0)
        self.write('mixed.configuration.bin', 'I', [1, 1, 4, 0])
        self.write('mixed.gap.bin', 'Q', [100] * 4)
        self.write('mixed.order.bin', 'B', [1, 0, 1, 0])
        for variant, times in [('full', [10, 20, 30, 40]), ('visibility', [1, 2, 30, 40])]:
            (self.root / variant).mkdir()
            self.write(f'{variant}/mixed.gpu.bin', 'd', times)
            self.write(f'{variant}/mixed.wall.bin', 'Q', [100] * 4)
            (self.root / variant / 'mixed.rgba').write_bytes(bytes([128, 128, 128, 255]))
            self.write(f'{variant}/mixed.linear.bin', 'f', [0.5, 0.5, 0.5, 4, 0, 0, 0, 0])

    def write(self, name, encoding, values):
        (self.root / name).write_bytes(struct.pack('<' + encoding * len(values), *values))

    def test_ratios_use_matched_frames_and_report_order_bias(self):
        result = summarize_run(self.root, 'mixed')
        self.assertEqual(result['median_paired_speedup'], 5.5)
        self.assertEqual(result['baseline_first_speedup'], 5.5)
        self.assertEqual(result['visibility_first_speedup'], 5.5)
        self.assertEqual(result['images']['maximum_linear_difference'], 0)
        self.assertEqual(result['pair_gap_maximum_ms'], 0.0001)

    def test_truncated_timing_and_unbalanced_orders_are_rejected(self):
        self.write('full/mixed.gpu.bin', 'd', [10])
        with self.assertRaisesRegex(ValueError, 'expected 4'):
            summarize_run(self.root, 'mixed')
        self.write('full/mixed.gpu.bin', 'd', [10, 20, 30, 40])
        self.write('mixed.order.bin', 'B', [1, 1, 1, 1])
        with self.assertRaisesRegex(ValueError, 'not balanced'):
            summarize_run(self.root, 'mixed')

    def test_positive_finite_gpu_measurements_are_required(self):
        for invalid in [0, -1, math.nan, math.inf]:
            self.write('full/mixed.gpu.bin', 'd', [invalid, 20, 30, 40])
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                summarize_run(self.root, 'mixed')

    def test_display_equality_cannot_hide_missing_samples_or_nonfinite_rgb(self):
        for values, diagnostic in [
            ([0.5, 0.5, 0.5, 3, 0, 0, 0, 0], 'sample coverage'),
            ([math.nan, 0.5, 0.5, 4, 0, 0, 0, 0], 'nonfinite linear RGB'),
        ]:
            self.write('visibility/mixed.linear.bin', 'f', values)
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(ValueError, diagnostic):
                compare_images(self.root, 'mixed', self.configuration)

    def test_linear_errors_are_reported_even_when_display_is_equal(self):
        self.write('visibility/mixed.linear.bin', 'f', [1, 0.5, 0.5, 4, 0, 0, 0, 0])
        result = compare_images(self.root, 'mixed', self.configuration)
        self.assertEqual(result['maximum_display_difference'], 0)
        self.assertEqual(result['maximum_linear_difference'], 0.5)
        self.assertAlmostEqual(result['linear_rmse'], math.sqrt(0.25 / 3))

    def test_bootstrap_requires_independent_runs_and_is_reproducible(self):
        self.assertIsNone(bootstrap_interval([1]))
        self.assertEqual(bootstrap_interval([1, 1, 1]), [1, 1])
        self.assertEqual(bootstrap_interval([0.9, 1, 1.1]), bootstrap_interval([0.9, 1, 1.1]))


if __name__ == '__main__':
    unittest.main()
