import math
import unittest
import xml.etree.ElementTree as ET

import numpy as np

from affine_slice import AffineSlice, DEFENSIVE_FRACTION
from scene import compute_fresnel, compute_smith_beckmann, interpolate_map, parse_transform


class SceneTests(unittest.TestCase):
    def test_listed_transforms_left_multiply(self):
        matrix = parse_transform(ET.fromstring('<transform><scale x="2"/><translate x="3"/></transform>'))
        np.testing.assert_allclose(matrix @ [1, 0, 0, 1], [5, 0, 0, 1])

    def test_texel_centres_and_wrap_clamp(self):
        pixels = np.array([[[0, 1], [2, 3]], [[4, 5], [6, 7]]])
        coordinates = np.array([[.25, .25], [.5, .5], [1, 1], [1.25, -.75]])
        np.testing.assert_allclose(interpolate_map(pixels, coordinates), [[0, 1], [3, 4], [0, 1], [0, 1]])

    def test_normal_incidence_fresnel_and_masking(self):
        eta = np.array([.2, .4, 1.4])
        extinction = np.array([4., 2., 1.6])
        expected = ((eta-1)**2+extinction**2)/((eta+1)**2+extinction**2)
        np.testing.assert_allclose(compute_fresnel(1., eta, extinction), expected)
        self.assertEqual(compute_smith_beckmann(1., .02), 1)


class AffineTests(unittest.TestCase):
    def make_identity(self):
        row, column = np.mgrid[:3, :3]/2
        return AffineSlice(np.stack((column-.5, row-.5), axis=-1))

    def test_identity_density_keeps_null_probability(self):
        proposal = self.make_identity()
        alpha = .2
        point, density, count = proposal.sample(np.array([.1, -.1]), .6, alpha)
        self.assertEqual(count, 1)
        np.testing.assert_allclose(point, [.6, .4])
        expected = DEFENSIVE_FRACTION+(1-DEFENSIVE_FRACTION)*math.exp(-.02/alpha**2)/(math.pi*alpha**2)
        self.assertAlmostEqual(density, expected)
        self.assertAlmostEqual(density, proposal.evaluate_pdf(point, alpha))
        self.assertEqual(proposal.sample(np.array([2., 2.]), .5, alpha), (None, 0., 0))
        # Subdensity plus null atom, not a retried/renormalized Gaussian.
        nodes, weights = np.polynomial.legendre.leggauss(16)
        other_nodes, other_weights = np.polynomial.legendre.leggauss(17)
        midpoint = (nodes+1)/2
        # Unequal quadrature orders avoid placing quadrature mass on the
        # piecewise map's measure-zero diagonal chart boundaries.
        other_midpoint = (other_nodes+1)/2
        integral = sum(proposal.evaluate_pdf([x, y], alpha)*weights[i]*other_weights[j]/4
                       for i, x in enumerate(midpoint) for j, y in enumerate(other_midpoint))
        expected_integral = DEFENSIVE_FRACTION+(1-DEFENSIVE_FRACTION)*math.erf(.5/alpha)**2
        self.assertAlmostEqual(integral, expected_integral, places=6)

    def test_both_fold_branches_are_counted(self):
        row, column = np.mgrid[:3, :3]/2
        proposal = AffineSlice(np.stack((np.abs(2*column-1), row-.5), axis=-1))
        identities, coordinates, weights = proposal.enumerate_roots([.3, .13])
        self.assertEqual(len(identities), 2)
        np.testing.assert_allclose(sorted(coordinates[:, 0]), [.35, .65])
        self.assertAlmostEqual(weights.sum(), 1)
        self.assertEqual(proposal.sample(np.array([-.3, .13]), .5, .2), (None, 0., 0))

    def test_collapsed_map_retains_defensive_support(self):
        proposal = AffineSlice(np.zeros((3, 3, 2)))
        self.assertEqual(proposal.sample(np.zeros(2), .5, .2), (None, 0., 0))
        self.assertEqual(proposal.evaluate_pdf([.4, .6], .2), DEFENSIVE_FRACTION)

    def test_seed_bins_cover_every_root_without_duplicates(self):
        row, column = np.mgrid[:17, :17]/16
        proposal = AffineSlice(np.stack((np.sin(column*4*np.pi)*.3+column*.1,
                                         np.sin(row*2*np.pi)*.3+row*.1), axis=-1).astype(np.float32))
        bounds, bins, identities = proposal.build_acceleration()
        for seed in np.random.default_rng(8).uniform(-.3, .3, (128, 2)):
            roots, _, _ = proposal.enumerate_roots(seed)
            if np.any(seed < bounds[:2]) or np.any(seed > bounds[2:]):
                self.assertEqual(len(roots), 0)
                continue
            cell = np.minimum(((seed-bounds[:2])/(bounds[2:]-bounds[:2])*16).astype(int), 15)
            offset, count = bins[cell[1]*16+cell[0]]
            candidates = identities[offset:offset+count]
            self.assertEqual(len(candidates), len(np.unique(candidates)))
            self.assertTrue(set(roots).issubset(set(candidates)))


if __name__ == '__main__':
    unittest.main()
