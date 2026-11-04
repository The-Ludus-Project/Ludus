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


def make_capture(path, pixels, *, graph=False, partial=False):
    header = bytearray(256)
    if graph:
        header[:8] = b'LURGCAP\0'
        struct.pack_into('<12I', header, 8, 1, 256, 16, int(partial), len(pixels), 1, 1, 1, 0, 2, 3, 1)
    else:
        header[:8] = b'LUHDR01\0'
        struct.pack_into('<10I', header, 8, 1, 1, 1, 256, len(pixels), 1, 1, 3, 2, 3 | (32 if partial else 0))
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
        self.assertEqual(len(images), 3)
        with Image.open(BytesIO(base64.b64decode(images[-1]))) as heatmap:
            self.assertEqual(heatmap.getpixel((0, 0)), (64, 64, 64))
            self.assertEqual(heatmap.getpixel((1, 0)), (64, 64, 64))
            self.assertEqual(heatmap.getpixel((2, 0)), (255, 128, 0))

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
        self.assertEqual(html.count('data:image/png;base64,'), 5)

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
