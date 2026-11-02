"""Geometry and quadrature checks independent of the Metal sampler driver."""
from pathlib import Path
import tempfile
import unittest

import numpy as np

from benchmark import NORMALS, PROBES, Panel, write_room
from check_room_reference import primary, quadrature, trace
from render_comparison import load_triangles


class RoomReferenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.room = Path(self.directory.name) / 'room'
        write_room(self.room, Panel())
        self.triangles = load_triangles(self.room).astype(np.float64)

    def test_loaded_room_and_primary_hits_match_declared_bounds(self):
        self.assertEqual(self.triangles.shape, (40, 4))
        position, normal, owner = primary(self.triangles, 32)
        hit = owner >= 0
        self.assertTrue(np.any(hit))
        self.assertTrue(np.all(np.abs(position[hit]) <= 1.000001))
        np.testing.assert_allclose(np.linalg.norm(normal[hit], axis=1), 1)

    def test_raised_receiver_has_independently_matching_area_integral(self):
        point, normal = PROBES[4], NORMALS[4]
        panel = Panel()
        expected = panel.reference(point, normal, 128)
        actual = quadrature(panel, self.triangles, point, normal, 8, 128)
        np.testing.assert_allclose(actual, expected, rtol=1e-9)

    def test_raised_plate_blocks_a_floor_to_panel_segment(self):
        point = np.array([.1, -1., -.2])
        endpoint = np.array([[.1, .97, -.2]])
        delta = endpoint-point
        distance = np.linalg.norm(delta,axis=1)
        _, owner, _ = trace(self.triangles, point + [0,.0001,0], delta/distance[:,None],
                            maximum=distance-.0001)
        self.assertGreaterEqual(owner[0], 8)

    def test_quadrature_converges_at_raised_plate_shadow_boundary(self):
        position, normal, owner = primary(self.triangles, 128)
        pixel = 114*128+59
        panel = Panel()
        first = quadrature(panel,self.triangles,position[pixel],normal[pixel],owner[pixel],64)
        second = quadrature(panel,self.triangles,position[pixel],normal[pixel],owner[pixel],128)
        self.assertGreater(np.linalg.norm(second), 0)
        np.testing.assert_allclose(first,second,rtol=1e-8)


if __name__ == '__main__':
    unittest.main()
