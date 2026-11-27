"""Exercise the offline comparison entry point with independently encoded inputs."""
import base64
import importlib.util
from io import BytesIO
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'compare-render'
HAS_IMAGE_TOOLS = all(importlib.util.find_spec(module) is not None for module in ('numpy', 'PIL'))
# A full-scale error is near white, and the smallest nonzero error stands clear of an exact match's black.
FULL_SCALE_LIGHTNESS = 90
LOWEST_ERROR_LIGHTNESS = 15
# Severity 1.0 protanopia and deuteranopia matrices for linear RGB, with thanks to Gustavo M. Machado,
# Manuel M. Oliveira and Leandro A. F. Fernandes, "A Physiologically-based Model for Simulation of Color
# Vision Deficiency", IEEE TVCG 15(6), pp. 1291-1298, 2009, https://doi.org/10.1109/TVCG.2009.113.
# The values are those reproduced in colorspacious (cvd.py, MACHADO_ET_AL_MATRICES).
COLOR_VISION_DEFICIENCIES = {
    'protanopia': ((0.152286, 1.052583, -0.204868), (0.114503, 0.786281, 0.099216),
                   (-0.003882, -0.048116, 1.051998)),
    'deuteranopia': ((0.367322, 0.860646, -0.227968), (0.280085, 0.672501, 0.047413),
                     (-0.011820, 0.042940, 0.968881)),
}
# Patch columns of the synthetic report fixture: exact, low, medium and saturated errors, then missing samples.
PATCH_PIXELS = 8
PATCH_ERRORS = (0, 5, 26, 128)  # added to every channel of an 8-bit baseline value
PATCH_BASE = 100
FIXTURE_HEIGHT = 8
DIFF_SCALE = 0.25


def read_heatmap(html):
    import numpy as np
    from PIL import Image

    images = re.findall(r'class="error-source" src="data:image/png;base64,([\w+/=]+)"', html)
    with Image.open(BytesIO(base64.b64decode(images[-1]))) as heatmap:
        return np.asarray(heatmap.convert('RGB'))


def decode_srgb(rgb):
    import numpy as np

    value = np.asarray(rgb, dtype=np.float64) / 255
    return np.where(value <= 0.04045, value / 12.92, ((value + 0.055) / 1.055) ** 2.4)


def get_lightness(rgb, deficiency=None):
    """CIELAB L* of 8-bit sRGB colours, as seen with an optional colour-vision deficiency."""
    import numpy as np

    linear = decode_srgb(rgb)
    if deficiency is not None:
        linear = np.clip(linear @ np.asarray(COLOR_VISION_DEFICIENCIES[deficiency]).T, 0, 1)
    y = linear @ (0.2126729, 0.7151522, 0.0721750)
    return np.where(y > (6 / 29) ** 3, 116 * np.cbrt(y) - 16, y * (29 / 3) ** 3)


def is_neutral(rgb):
    return rgb[0] == rgb[1] == rgb[2]


def make_capture(path, pixels, *, graph=False, partial=False, failed=False, width=None):
    width = width or len(pixels)
    height = len(pixels) // width
    header = bytearray(256)
    if graph:
        header[:8] = b'LURGCAP\0'
        flags = int(partial) | (2 if failed else 0)
        struct.pack_into('<12I', header, 8, 1, 256, 16, flags, width, height, 1, 1, int(failed), 2, 3, 1)
    else:
        header[:8] = b'LUHDR01\0'
        struct.pack_into('<10I', header, 8, 1, 1, 1, 256, width, height, 1, 3, 2, 3 | (32 if partial else 0))
    path.write_bytes(header + b''.join(struct.pack('<fffI', *pixel) for pixel in pixels))


@unittest.skipUnless(HAS_IMAGE_TOOLS, 'NumPy and Pillow are required for the offline comparison')
class RenderComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / 'report.html'

    def compare(self, first, second, *options, check=True):
        return subprocess.run([sys.executable, str(SCRIPT), str(first), str(second), '-o', str(self.output), *options],
                              capture_output=True, text=True, check=check)

    def test_hdr_masks_unsampled_pixels_on_either_side_and_uses_raw_radiance(self):
        from PIL import Image

        reference = self.root / 'expected.luhdr'
        graph = self.root / 'candidate.lurgcap'
        make_capture(reference, [(10, 10, 10, 1), (0, 0, 0, 0), (2, 3, 4, 1)], partial=True)
        make_capture(graph, [(0, 0, 0, 0), (10, 10, 10, 1), (4, 3, 4, 1)], graph=True, partial=True)
        report = json.loads(self.compare(reference, graph).stdout)
        self.assertEqual(report['comparison_space'], 'scene-linear RGB')
        self.assertEqual(report['baseline']['minimum_samples'], 0)
        self.assertEqual(report['candidate']['output_resource'], 2)
        self.assertEqual(len(report['warnings']), 2)
        metrics = report['metrics']
        self.assertEqual(metrics['compared_pixels'], 1)
        self.assertAlmostEqual(metrics['coverage'], 1 / 3)
        self.assertAlmostEqual(metrics['mae'], 2 / 3)
        self.assertAlmostEqual(metrics['rmse'], math.sqrt(4 / 3))
        self.assertAlmostEqual(metrics['relative_mse'], 4 / (4.01 * 3))
        self.assertAlmostEqual(metrics['luminance_bias'], 2 * 0.2126)
        self.assertEqual(metrics['worst_pixel_xy'], [2, 0])
        html = self.output.read_text(encoding='utf-8')
        images = re.findall(r'data:image/png;base64,([\w+/=]+)', html)
        self.assertEqual(len(images), 6)  # baseline, candidate, heatmap and three legend images
        heatmap = read_heatmap(html)
        self.assertTrue(is_neutral(heatmap[0, 0]) and is_neutral(heatmap[0, 1]))
        self.assertGreater(get_lightness(heatmap[0, 2]), FULL_SCALE_LIGHTNESS)

    def test_display_comparison_composites_alpha_and_reads_embedded_profile(self):
        from PIL import Image, ImageCms

        baseline = self.root / 'base&line.png'
        candidate = self.root / 'candidate.png'
        profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
        Image.new('RGBA', (2, 1), (255, 0, 0, 0)).save(baseline, icc_profile=profile)
        image = Image.new('RGB', (2, 1))
        image.putpixel((1, 0), (255, 0, 0))
        image.save(candidate)
        report = json.loads(self.compare(baseline, candidate, '--diff-scale', '0.5').stdout)
        self.assertEqual(report['baseline']['color_profile'], 'ICC converted to sRGB')
        self.assertEqual(report['comparison_space'], 'sRGB display values')
        self.assertEqual(report['heatmap_full_scale'], 0.5)
        self.assertEqual(report['metrics']['max_channel_error'], 1)
        self.assertEqual(report['metrics']['worst_pixel_xy'], [1, 0])
        self.assertAlmostEqual(report['metrics']['mae'], 1 / 6)
        self.assertAlmostEqual(report['metrics']['psnr_db'], 10 * math.log10(6))
        html = self.output.read_text(encoding='utf-8')
        self.assertIn('base&amp;line.png', html)
        result = self.compare(baseline, candidate, check=False)
        self.assertIn('already exists', result.stderr)
        self.assertEqual(self.output.read_text(encoding='utf-8'), html)

    def test_many_saved_graphs_share_one_linear_baseline(self):
        reference = self.root / 'reference.luhdr'
        first = self.root / 'first.lurgcap'
        second = self.root / 'second.lurgcap'
        make_capture(reference, [(2, 2, 2, 1)])
        make_capture(first, [(2, 2, 2, 1)], graph=True)
        make_capture(second, [(4, 4, 4, 1)], graph=True)
        report = json.loads(self.compare(reference, first, str(second)).stdout)
        self.assertEqual(report['baseline']['path'], str(reference))
        self.assertEqual([entry['metrics']['mae'] for entry in report['comparisons']], [0, 2])
        html = self.output.read_text(encoding='utf-8')
        self.assertIn('<option value="1">second.lurgcap</option>', html)
        self.assertEqual(html.count('data:image/png;base64,'), 8)

    def test_regression_checks_every_candidate_and_keeps_failure_reports(self):
        baseline = self.root / 'baseline.lurgcap'
        first = self.root / 'at-limit.lurgcap'
        second = self.root / 'above-limit.lurgcap'
        make_capture(baseline, [(2, 2, 2, 1)], graph=True)
        make_capture(first, [(2.5, 2.5, 2.5, 1)], graph=True)
        make_capture(second, [(3, 3, 3, 1)], graph=True)

        passing = self.compare(baseline, first, '--max-rmse', '0.5')
        self.assertEqual(json.loads(passing.stdout)['regression'],
                         {'max_rmse': 0.5, 'passed': True, 'failures': []})
        self.output.unlink()

        failing = self.compare(baseline, first, str(second), '--max-rmse', '0.5', check=False)
        self.assertEqual(failing.returncode, 1)
        comparisons = json.loads(failing.stdout)['comparisons']
        self.assertTrue(comparisons[0]['regression']['passed'])
        self.assertFalse(comparisons[1]['regression']['passed'])
        self.assertEqual(comparisons[1]['metrics']['rmse'], 1)
        self.assertIn('RMSE 1 exceeds the limit 0.5', comparisons[1]['regression']['failures'][0])
        html = self.output.read_text(encoding='utf-8')
        self.assertIn('Image regression check passed', html)
        self.assertIn('Image regression check failed', html)

    def test_regression_rejects_incomplete_hdr_with_matching_pixels(self):
        baseline = self.root / 'baseline.luhdr'
        candidate = self.root / 'candidate.lurgcap'
        pixels = [(2, 2, 2, 1)]
        make_capture(baseline, pixels, partial=True)
        make_capture(candidate, pixels, graph=True, failed=True)

        result = self.compare(baseline, candidate, '--max-rmse', '0', check=False)
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertEqual(report['metrics']['rmse'], 0)
        self.assertEqual(report['metrics']['coverage'], 1)
        self.assertFalse(report['regression']['passed'])
        self.assertEqual(len(report['regression']['failures']), 2)
        self.assertIn('Baseline capture is incomplete', report['regression']['failures'][0])
        self.assertIn('Candidate capture is incomplete', report['regression']['failures'][1])

    def test_display_regression_uses_normalized_srgb_and_validates_tolerance(self):
        from PIL import Image

        baseline = self.root / 'baseline.png'
        candidate = self.root / 'candidate.png'
        Image.new('RGB', (1, 1), (0, 0, 0)).save(baseline)
        Image.new('RGB', (1, 1), (1, 1, 1)).save(candidate)
        for tolerance in ('-1', 'nan', 'inf'):
            with self.subTest(tolerance=tolerance):
                result = self.compare(baseline, candidate, '--max-rmse', tolerance, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertIn('--max-rmse must be finite and nonnegative', result.stderr)
                self.assertFalse(self.output.exists())

        result = self.compare(baseline, candidate, '--max-rmse', '0.004')
        report = json.loads(result.stdout)
        self.assertTrue(report['regression']['passed'])
        self.assertAlmostEqual(report['metrics']['rmse'], 1 / 255)

    def write_patch_fixture(self):
        """Write display and linear pairs whose patches have known errors; the linear candidate lacks samples last."""
        import numpy as np
        from PIL import Image

        width = PATCH_PIXELS * (len(PATCH_ERRORS) + 1)
        offset = np.zeros((FIXTURE_HEIGHT, width), dtype=np.int32)
        for patch, error in enumerate(PATCH_ERRORS):
            offset[:, patch * PATCH_PIXELS:(patch + 1) * PATCH_PIXELS] = error
        paths = {name: self.root / name for name in ('expected.png', 'actual.png', 'expected.luhdr', 'actual.lurgcap')}
        Image.new('RGB', (width, FIXTURE_HEIGHT), (PATCH_BASE,) * 3).save(paths['expected.png'])
        actual = np.repeat((PATCH_BASE + offset)[..., None], 3, axis=2).astype(np.uint8)
        Image.fromarray(actual).save(paths['actual.png'])
        missing = width - PATCH_PIXELS
        baseline = [(PATCH_BASE / 255,) * 3 + (1,)] * (width * FIXTURE_HEIGHT)
        candidate = [(value / 255,) * 3 + (int(index % width < missing),)
                     for index, value in enumerate((PATCH_BASE + offset).flat)]
        make_capture(paths['expected.luhdr'], baseline, width=width)
        make_capture(paths['actual.lurgcap'], candidate, graph=True, partial=True, width=width)
        return paths, width

    def test_heatmap_orders_patch_errors_by_lightness_for_common_vision(self):
        import numpy as np

        paths, width = self.write_patch_fixture()
        reports = {}
        for space, (baseline, candidate) in {'display': ('expected.png', 'actual.png'),
                                              'linear': ('expected.luhdr', 'actual.lurgcap')}.items():
            with self.subTest(space=space):
                self.output = self.root / f'{space}.html'
                result = self.compare(paths[baseline], paths[candidate], '--diff-scale', str(DIFF_SCALE),
                                      '--max-rmse', '0', check=False)
                self.assertEqual(result.returncode, 1)
                reports[space] = json.loads(result.stdout)
                html = self.output.read_text(encoding='utf-8')
                for tick in ('0', '0.0625', '0.125', '0.1875', '0.25'):
                    self.assertIn(f'<span>{tick}</span>', html)
                self.assertIn('normalized sRGB, 0 to 1' if space == 'display' else '(scene-linear RGB)', html)
                heatmap = read_heatmap(html)
                patches = [heatmap[:, i * PATCH_PIXELS:(i + 1) * PATCH_PIXELS].reshape(-1, 3)
                           for i in range(len(PATCH_ERRORS))]
                for patch in patches:
                    self.assertTrue(np.all(patch == patch[0]), 'each patch has one error and one colour')
                colors = np.asarray([patch[0] for patch in patches])
                self.assertEqual(colors[0].tolist(), [0, 0, 0])
                for deficiency in (None, *COLOR_VISION_DEFICIENCIES):
                    lightness = get_lightness(colors, deficiency)
                    self.assertTrue(np.all(np.diff(lightness) > 0), f'{deficiency}: {lightness}')
                    self.assertGreater(lightness[1], LOWEST_ERROR_LIGHTNESS)
                self.assertGreater(get_lightness(colors[-1]), FULL_SCALE_LIGHTNESS)
                self.assertFalse(any(is_neutral(color) for color in colors[1:]))
                missing = heatmap[:, width - PATCH_PIXELS:].reshape(-1, 3)
                if space == 'linear':
                    # Neutral stripes of two greys: a texture that no ramp colour or exact match can show.
                    greys = np.unique(missing, axis=0)
                    self.assertEqual(len(greys), 2)
                    self.assertTrue(all(is_neutral(grey) and grey[0] > 0 for grey in greys))
                else:
                    self.assertTrue(np.all(missing == 0))

        # The presentation leaves the metrics and the verdict to the errors alone.
        display = reports['display']['metrics']
        errors = np.asarray(PATCH_ERRORS, dtype=np.float64) / 255
        self.assertEqual(display['compared_pixels'], width * FIXTURE_HEIGHT)
        self.assertAlmostEqual(display['mae'], errors.sum() / (len(PATCH_ERRORS) + 1))
        self.assertAlmostEqual(display['rmse'], math.sqrt(np.sum(errors ** 2) / (len(PATCH_ERRORS) + 1)))
        self.assertEqual(display['max_channel_error'], PATCH_ERRORS[-1] / 255)
        linear = reports['linear']['metrics']
        self.assertEqual(linear['compared_pixels'], (width - PATCH_PIXELS) * FIXTURE_HEIGHT)
        self.assertAlmostEqual(linear['rmse'], math.sqrt(np.sum(errors ** 2) / len(PATCH_ERRORS)), places=6)
        for report in reports.values():
            self.assertEqual(report['heatmap_full_scale'], DIFF_SCALE)
            self.assertFalse(report['regression']['passed'])

    def test_incompatible_inputs_return_actionable_errors_without_output(self):
        from PIL import Image

        first = self.root / 'first.png'
        second = self.root / 'second.png'
        capture = self.root / 'candidate.lurgcap'
        Image.new('RGB', (1, 1)).save(first)
        Image.new('RGB', (2, 1)).save(second)
        make_capture(capture, [(0, 0, 0, 0)], graph=True, partial=True)
        for candidate, message in ((second, 'same resolution'), (capture, 'PNG saved alongside')):
            with self.subTest(candidate=candidate):
                result = self.compare(first, candidate, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
                self.assertFalse(self.output.exists())
        result = self.compare(capture, capture, check=False)
        self.assertIn('no pixels have committed samples in both captures', result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
