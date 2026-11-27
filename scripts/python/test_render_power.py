"""Exercise the render power check with independently encoded raw captures."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'check-render-power'
# x_ndc=x/(1+x/4), y_ndc=y/(1+x/4), z_ndc=1/2, stored by column.
TILTED_PROJECTION = (1, 0, 0.125, 0.25, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.5, 1)
FLAT_PROJECTION = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.5, 1)


def write_capture(path, values, *, graph=False, projection=TILTED_PROJECTION, state='complete'):
    header = bytearray(256)
    samples = 0 if state == 'partial' else 1
    if graph:
        failed = int(state == 'failed')
        flags = int(state == 'partial') | (2 * failed)
        header[:8] = b'LURGCAP\0'
        struct.pack_into('<12I', header, 8, 1, 256, 16, flags, len(values), 1, 1, samples, failed, 2, 3, 1)
        struct.pack_into('<16f', header, 128, *projection)
    else:
        flags = 1 | (32 if state == 'partial' else 0)
        header[:8] = b'LUHDR01\0'
        struct.pack_into('<10I', header, 8, 1, 1, 1, 256, len(values), 1, 1, 1, 1, flags)
        struct.pack_into('<16f', header, 96, *projection)
    path.write_bytes(header + b''.join(struct.pack('<fffI', red, 0, 0, samples) for red in values))


class RenderPowerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.reference = Path(directory.name) / 'open.luhdr'
        self.candidate = Path(directory.name) / 'masked.lurgcap'
        write_capture(self.reference, [1, 1])

    def check_power(self, expected=81 / 256, *, receiver_z=0, tolerance=0.01, channel='red'):
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(self.candidate), str(self.reference),
             '--receiver-z', str(receiver_z), '--channel', channel, '--expected-fraction', str(expected),
             '--tolerance', str(tolerance)],
            capture_output=True, text=True, check=False)

    def test_area_weighted_power_for_unequal_perspective_pixels(self):
        # Left pixel width 4/5, heights 8/5 and 2. Right width 4/3, heights 2 and 8/3.
        write_capture(self.candidate, [1, 0], graph=True)
        result = self.check_power()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertAlmostEqual(report['open_radiance_area_integral'], 1024 / 225)
        self.assertAlmostEqual(report['measured_radiance_area_integral'], 36 / 25)
        self.assertAlmostEqual(report['measured_fraction'], 81 / 256)
        self.assertAlmostEqual(report['residual_fraction_of_open_power'], 0)

    def test_residual_sign_and_tolerance_for_added_and_lost_power(self):
        for pixels, sign in (([1, 0.2], 1), ([0.8, 0], -1)):
            with self.subTest(pixels=pixels):
                write_capture(self.candidate, pixels, graph=True)
                result = self.check_power()
                self.assertEqual(result.returncode, 1, result.stderr)
                residual = json.loads(result.stdout)['residual_fraction_of_open_power']
                self.assertGreater(sign * residual, 0.01)

    def test_zero_expected_power_is_supported(self):
        write_capture(self.candidate, [0, 0], graph=True)
        result = self.check_power(expected=0, tolerance=0)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['measured_fraction'], 0)
        self.assertIsNone(report['candidate_border_fraction_of_measured_power'])

    def test_rejects_unmatched_or_unusable_captures(self):
        for options, message in ((dict(projection=FLAT_PROJECTION), 'different cameras'),
                                 (dict(state='partial'), 'incomplete'),
                                 (dict(state='failed'), 'incomplete')):
            with self.subTest(options=options):
                write_capture(self.candidate, [1, 0], graph=True, **options)
                result = self.check_power()
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
        write_capture(self.candidate, [1, 0], graph=True)
        self.assertIn('Receiver plane does not cover', self.check_power(receiver_z=2).stderr)
        self.assertIn('dark in this channel', self.check_power(channel='blue').stderr)
        self.candidate.write_bytes(b'\x89PNG\r\n\x1a\n')
        self.assertIn('Export a raw .luhdr or .lurgcap', self.check_power().stderr)


if __name__ == '__main__':
    unittest.main()
