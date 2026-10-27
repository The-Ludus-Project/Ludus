"""Validate paired timing analysis and reject incomplete/corrupt render evidence."""

import math
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

from benchmark_ray_visibility import bootstrap_interval, compare_images, summarize, summarize_run


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

    def write_passes(self, metadata=(1, 1, 1, 64, 64, 11, 12)):
        (self.root / 'visibility/mixed.pass-info.bin').write_bytes(struct.pack('<QddQQQQ', *metadata))
        self.write('visibility/mixed.transport.gpu.bin', 'd', [30, 40, 50, 60])
        self.write('visibility/mixed.output.gpu.bin', 'd', [5, 6, 7, 8])
        self.write('visibility/mixed.pass-intervals.bin', 'Q', [100, 140, 110, 150, 1, 1] * 4)
        names = (b'GPU preview transport', b'GPU preview image')
        (self.root / 'visibility/mixed.pass-names.bin').write_bytes(
            b''.join(struct.pack('<I', len(name)) + name for name in names))

    def write_both_passes(self):
        self.write_passes()
        for suffix in ('.pass-info.bin', '.pass-names.bin', '.pass-intervals.bin',
                       '.transport.gpu.bin', '.output.gpu.bin'):
            shutil.copy2(self.root / 'visibility' / ('mixed' + suffix),
                         self.root / 'full' / ('mixed' + suffix))
        self.write('full/mixed.transport.gpu.bin', 'd', [60, 80, 100, 120])

    def test_workgroup_transport_ratios_pair_frames_independently_of_frame_intervals(self):
        self.write_both_passes()
        result = summarize([self.root], scenes=('mixed',), pass_variants=('full', 'visibility'))['mixed']
        transport = result['transport_pairs']
        self.assertEqual(transport['median_run_paired_speedup'], 2)
        self.assertEqual(transport['median_run_paired_gpu_time_reduction_percent'], 50)
        self.assertEqual(transport['ratio_pairs'], 4)
        self.assertEqual(result['median_run_paired_speedup'], 5.5)
        self.assertEqual(result['runs'][0]['transport_pairs']['baseline_first_speedup'], 2)
        self.assertEqual(result['runs'][0]['transport_pairs']['candidate_first_speedup'], 2)

    def test_workgroup_zero_duration_pairs_do_not_manufacture_transport_speedup(self):
        self.write_both_passes()
        self.write('visibility/mixed.transport.gpu.bin', 'd', [0, 40, 50, 60])
        result = summarize([self.root], scenes=('mixed',), pass_variants=('full', 'visibility'))['mixed']
        self.assertEqual(result['transport_pairs']['ratio_pairs'], 3)
        self.assertEqual(result['transport_pairs']['zero_duration_pairs'], 1)
        self.assertEqual(result['transport_pairs']['median_run_paired_speedup'], 2)
        self.write('visibility/mixed.transport.gpu.bin', 'd', [0] * 4)
        result = summarize([self.root], scenes=('mixed',), pass_variants=('full', 'visibility'))['mixed']
        self.assertEqual(result['transport_pairs']['runs_with_ratios'], 0)
        self.assertIsNone(result['transport_pairs']['median_run_paired_speedup'])
        self.assertIsNone(result['transport_pairs']['run_bootstrap_95_percent_interval'])

    def test_unrepresentable_transport_ratio_is_rejected(self):
        self.write_both_passes()
        self.write('full/mixed.transport.gpu.bin', 'd', [1e308] * 4)
        self.write('visibility/mixed.transport.gpu.bin', 'd', [1e-308] * 4)
        with self.assertRaisesRegex(ValueError, 'unrepresentable paired transport ratio'):
            summarize_run(self.root, 'mixed', pass_variants=('full', 'visibility'))

    def test_passes_are_separate_intervals_and_need_not_sum_to_frame_time(self):
        self.write_passes()
        result = summarize_run(self.root, 'mixed', pass_variants=('visibility',))
        passes = result['pass_timings']['visibility']
        self.assertEqual(passes['samples'], 4)
        self.assertEqual(passes['transport_median_ms'], 0.000045)
        self.assertEqual(passes['output_median_ms'], 0.0000065)
        self.assertGreater(passes['transport_median_ms'], result['gpu_median_ms'][1])
        self.assertEqual(passes['metadata']['transport_scope_identity'], 11)

    def test_alignment_measures_overlap_and_tail_without_requiring_it(self):
        self.write_passes()
        result = summarize_run(self.root, 'mixed', pass_variants=('visibility',))['pass_timings']['visibility']
        self.assertEqual(result['aligned_samples'], 4)
        self.assertEqual(result['overlap_median_ms'], 0.00003)
        self.assertEqual(result['output_tail_median_ms'], 0.00001)
        self.write('visibility/mixed.pass-intervals.bin', 'Q', [0] * 24)
        result = summarize_run(self.root, 'mixed', pass_variants=('visibility',))['pass_timings']['visibility']
        self.assertEqual(result['unaligned_samples'], 4)
        self.assertIsNone(result['overlap_median_ms'])
        self.assertIsNone(result['output_tail_median_ms'])
        self.assertGreater(result['transport_median_ms'], 0)

    def test_unrepresentable_frame_ratio_is_rejected(self):
        self.write('full/mixed.gpu.bin', 'd', [1e308, 20, 30, 40])
        self.write('visibility/mixed.gpu.bin', 'd', [1e-308, 2, 30, 40])
        with self.assertRaisesRegex(ValueError, 'unrepresentable paired frame ratio'):
            summarize_run(self.root, 'mixed')

    def test_invalid_alignment_cannot_report_false_overlap(self):
        self.write_passes()
        for record in ([140, 100, 110, 150, 1, 1], [100, 140, 110, 150, 1, 0], [0, 0, 0, 0, 0, 2]):
            self.write('visibility/mixed.pass-intervals.bin', 'Q', record * 4)
            with self.subTest(record=record), self.assertRaises(ValueError):
                summarize_run(self.root, 'mixed', pass_variants=('visibility',))

    def test_incomplete_or_invalid_pass_labels_are_rejected(self):
        for data in (b'', struct.pack('<I', 0), struct.pack('<I', 100) + b'x',
                     struct.pack('<I', 1) + b'\xff', struct.pack('<I', 1) + b'\0'):
            self.write_passes()
            (self.root / 'visibility/mixed.pass-names.bin').write_bytes(data)
            with self.subTest(data=data), self.assertRaises(ValueError):
                summarize_run(self.root, 'mixed', pass_variants=('visibility',))

    def test_quantized_zero_passes_and_unknown_precision_are_preserved(self):
        self.write_passes((1, 0, 0, 0, 0, 11, 12))
        self.write('visibility/mixed.output.gpu.bin', 'd', [0] * 4)
        result = summarize_run(self.root, 'mixed', pass_variants=('visibility',))
        passes = result['pass_timings']['visibility']
        self.assertEqual(passes['output_median_ms'], 0)
        self.assertEqual(passes['output_zero_samples'], 4)
        self.assertEqual(passes['metadata']['output_valid_bits'], 0)

    def test_missing_truncated_or_invalid_pass_samples_are_rejected(self):
        with self.assertRaises(FileNotFoundError):
            summarize_run(self.root, 'mixed', pass_variants=('visibility',))
        self.write_passes()
        self.write('visibility/mixed.transport.gpu.bin', 'd', [30])
        with self.assertRaisesRegex(ValueError, 'expected 4'):
            summarize_run(self.root, 'mixed', pass_variants=('visibility',))
        for invalid in [-1, math.nan, math.inf]:
            self.write('visibility/mixed.transport.gpu.bin', 'd', [invalid, 40, 50, 60])
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'invalid measurement'):
                summarize_run(self.root, 'mixed', pass_variants=('visibility',))

    def test_invalid_pass_metadata_and_unprobed_pass_data_are_rejected(self):
        for metadata in [
            (2, 1, 1, 64, 64, 11, 12),
            (1, math.nan, 1, 64, 64, 11, 12),
            (1, 1, 1, 65, 64, 11, 12),
            (1, 1, 1, 64, 64, 0, 12),
            (1, 1, 1, 64, 64, 11, 11),
        ]:
            self.write_passes(metadata)
            with self.subTest(metadata=metadata), self.assertRaises(ValueError):
                summarize_run(self.root, 'mixed', pass_variants=('visibility',))
        self.write_passes()
        self.write('full/mixed.transport.gpu.bin', 'd', [30, 40, 50, 60])
        with self.assertRaisesRegex(ValueError, 'unexpected pass data'):
            summarize_run(self.root, 'mixed', pass_variants=('visibility',))

    def test_scope_identities_remain_local_to_each_process(self):
        self.write_passes()
        with tempfile.TemporaryDirectory() as temporary:
            other = Path(temporary) / 'run'
            shutil.copytree(self.root, other)
            (other / 'visibility/mixed.pass-info.bin').write_bytes(struct.pack('<QddQQQQ', 1, 1, 1, 64, 64, 21, 22))
            result = summarize([self.root, other], scenes=('mixed',), pass_variants=('visibility',))['mixed']
            self.assertNotIn('transport_scope_identity', result['pass_timings']['visibility']['precision'])
            self.assertEqual(result['runs'][1]['pass_timings']['visibility']['metadata']['transport_scope_identity'], 21)

    def test_ratios_use_matched_frames_and_report_order_bias(self):
        result = summarize_run(self.root, 'mixed')
        self.assertEqual(result['median_paired_speedup'], 5.5)
        self.assertEqual(result['baseline_first_speedup'], 5.5)
        self.assertEqual(result['visibility_first_speedup'], 5.5)
        self.assertEqual(result['images']['maximum_linear_difference'], 0)
        self.assertEqual(result['pair_gap_maximum_ms'], 0.0001)

    def test_cache_experiment_uses_its_selected_scenes_and_variant_labels(self):
        (self.root / 'full').rename(self.root / 'uncached')
        (self.root / 'visibility').rename(self.root / 'cached')
        result = summarize([self.root], scenes=('mixed',), variants=('uncached', 'cached'))
        self.assertEqual(tuple(result), ('mixed',))
        self.assertEqual(result['mixed']['median_run_paired_speedup'], 5.5)
        self.assertEqual(result['mixed']['maximum_linear_difference'], 0)

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
