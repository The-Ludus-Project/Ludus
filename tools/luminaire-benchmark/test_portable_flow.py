"""Change-of-variables, encoding and export regressions without CUDA."""
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from benchmark import Panel, PROBES, write_room
from export_compute import export
from portable_flow import (BINS, HIDDEN, INPUT_COUNT, LAYERS, SHAPES, PortableFlow,
                           cap_coordinates, cap_directions, condition, grid_corners, one_blob, spline)


def make_model(path, panel, randomize=False):
    rng = np.random.default_rng(123)
    keys = np.unique(np.concatenate([grid_corners(condition(point))[0] for point in PROBES]))
    arrays = dict(grid_keys=keys.astype(np.uint32), grid_features=rng.normal(0, .01, (len(keys), 8)).astype(np.float32))
    for coupling in range(LAYERS):
        for layer, shape in enumerate(SHAPES):
            arrays[f'w{coupling}_{layer}'] = (rng.normal(0, .05, shape) if randomize else np.zeros(shape)).astype(np.float32)
            arrays[f'b{coupling}_{layer}'] = (rng.normal(0, .1, shape[0]) if randomize else np.zeros(shape[0])).astype(np.float32)
    arrays['metadata'] = np.array(json.dumps(dict(fixture_sha256=panel.identity(), steps=0, scope='test weights')))
    np.savez(path, **arrays)


class PortableTests(unittest.TestCase):
    def test_blob_wraps_and_partitions_unity(self):
        x = np.linspace(0, 1, 1001)
        encoded = one_blob(x)
        np.testing.assert_allclose(encoded.sum(axis=1), 1, atol=2e-6)
        self.assertGreaterEqual(encoded.min(), -2e-6)
        np.testing.assert_allclose(encoded[0], encoded[-1], atol=2e-6)

    def test_spline_inverse_pdf_and_uniform_limit(self):
        rng = np.random.default_rng(44)
        x = rng.uniform(.00001, .99999, 10000).astype(np.float32)
        parameters = rng.uniform(-1, 1, (len(x), BINS*2+1)).astype(np.float32)
        y, forward_pdf = spline(x, parameters)
        restored, inverse_pdf = spline(y, parameters, True)
        np.testing.assert_allclose(restored, x, atol=3e-6, rtol=3e-6)
        np.testing.assert_allclose(forward_pdf, inverse_pdf, atol=2e-5, rtol=2e-5)
        uniform, density = spline(x, np.zeros_like(parameters), True)
        np.testing.assert_allclose(uniform, x, atol=2e-7)
        np.testing.assert_allclose(density, 0, atol=2e-7)

    def test_cap_roundtrip_and_geometric_density(self):
        rng = np.random.default_rng(9)
        uv = rng.uniform(.0001, .9999, (5000, 2))
        for point in PROBES:
            directions, area = cap_directions(point, uv)
            np.testing.assert_allclose(np.linalg.norm(directions, axis=1), 1, atol=1e-6)
            restored = cap_coordinates(point, directions)
            np.testing.assert_allclose(restored, uv, atol=2e-6)
            self.assertGreater(area, 0)

    def test_full_conditioned_flow_roundtrip(self):
        panel = Panel()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'model.npz'
            make_model(path, panel, True)
            model = PortableFlow(path, panel)
            uv = np.random.default_rng(6).uniform(.001, .999, (128, 2))
            samples, inverse_pdf = model.transform(PROBES[0], uv, True)
            restored, forward_pdf = model.transform(PROBES[0], samples)
            np.testing.assert_allclose(restored, uv, atol=5e-6)
            np.testing.assert_allclose(forward_pdf, inverse_pdf, atol=5e-5)
            self.assertEqual(model.encode(PROBES[0]).shape, (INPUT_COUNT-BINS,))

    def test_export_keeps_same_emission_and_locks_profile(self):
        panel = Panel()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / 'model.npz'
            make_model(model, panel)
            write_room(root / 'room', panel)
            result = export(model, root / 'room', root / 'graph')
            self.assertLess(result['shader_chunks'], 64)
            manifest = json.loads((root / 'graph/main.lushader.json').read_text())
            self.assertEqual([item['id'] for item in manifest['settings']], ['SLICE_DEFENSIVE_FRACTION'])
            self.assertIn('ShadePortablePureLight', [item['id'] for item in manifest['programs']])
            self.assertEqual((root / 'graph/main_shaders/luminaire.slang').read_bytes(),
                             (root / 'room/graph/main_shaders/luminaire.slang').read_bytes())
            self.assertTrue(all(path.stat().st_size <= 128*1024 for path in (root / 'graph/main_shaders').glob('purelight_weights_*.slang')))
            with self.assertRaises(FileExistsError):
                export(model, root / 'room', root / 'graph')


if __name__ == '__main__':
    unittest.main()
