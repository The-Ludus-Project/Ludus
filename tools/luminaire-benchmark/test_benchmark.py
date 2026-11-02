"""Independent invariants for the shared-radiance comparison setup."""
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as xml

import numpy as np

from benchmark import CENTER, HALF_EXTENT, NORMALS, PROBES, Panel, write_room


class BenchmarkTests(unittest.TestCase):
    def test_constant_integral_checks_subdensity_and_null_accounting(self):
        panel = Panel()
        for point in (PROBES[0], PROBES[6]):
            area, density, valid = panel.sample(point, 131072, np.random.default_rng(120), 'slice')
            values = np.where(valid, 1 / density, 0)
            self.assertAlmostEqual(values.mean(), 4, delta=6 * values.std() / np.sqrt(len(values)))
            self.assertTrue(np.all(density >= .025))

    def test_slice_matches_independent_area_quadrature(self):
        panel = Panel()
        point, normal = PROBES[0], NORMALS[0]
        reference = panel.reference(point, normal, 128)
        area, density, valid = panel.sample(point, 65536, np.random.default_rng(12), 'slice')
        values = panel.contribution(point, normal, area, density, valid)
        self.assertTrue(np.all(np.abs(values.mean(axis=0) - reference) <
                               6 * values.std(axis=0) / np.sqrt(len(values))))

    def test_solid_angle_jacobian_matches_geometric_area_conversion(self):
        panel = Panel()
        point = PROBES[0]
        area = np.array([[.2, -.1], [-.5, .4]])
        _, jacobian, directions = panel.evaluate(point, area)
        world = CENTER + HALF_EXTENT * np.column_stack((area[:, 0], np.zeros(2), area[:, 1]))
        squared_distance = np.sum((world - point)**2, axis=1)
        np.testing.assert_allclose(jacobian, directions[:, 1] * HALF_EXTENT**2 / squared_distance)

    def test_tile_emission_uses_gpu_tables_and_gaussian_normalization(self):
        panel = Panel()
        point = PROBES[0]
        tangent, height = panel.receiver(point)
        for tile in range(9):
            # At a mean-direction root the Gaussian exponent is zero.
            area, weights = panel.roots(point, np.zeros((1, 2)))
            if weights[0, tile] == 0:
                continue
            radiance, _, _ = panel.evaluate(point, area[:, tile])
            expected = (6 * panel.tables['strength'][tile] * panel.tables['tint'][tile] /
                        (2 * np.pi * np.linalg.det(panel.cholesky[tile])))
            np.testing.assert_allclose(radiance[0], expected)
            self.assertTrue(np.all(np.isfinite((tangent - area[:, tile]) / height)))

    def test_generated_room_has_no_uncontrolled_emitters_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'room'
            panel = Panel()
            write_room(output, panel)
            document = xml.parse(output / 'scene.xml')
            self.assertEqual(len(document.findall('shape')), 5)
            self.assertEqual(document.findall('.//emitter'), [])
            self.assertNotIn('AreaLightSource', (output / 'scene-v4.pbrt').read_text())
            manifest = json.loads((output / 'comparison.json').read_text())
            self.assertEqual(manifest['fixture_sha256'], panel.identity())
            self.assertEqual((output / 'graph/main_shaders/luminaire.slang').read_bytes(),
                             (Path(__file__).resolve().parents[2] /
                              'modules/graphics/renderer/render_graphs/slice_sampling_luminaire/main_shaders/luminaire.slang').read_bytes())
            with self.assertRaises(FileExistsError):
                write_room(output, panel)

    def test_fixture_identity_changes_after_edits(self):
        self.assertNotEqual(Panel().identity(), Panel(width=.04).identity())
        self.assertNotEqual(Panel().identity(), Panel(steering=.2).identity())


if __name__ == '__main__':
    unittest.main()
