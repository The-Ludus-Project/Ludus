"""Self-checks for the G0 curves and surfaces reference corpus.

These tests validate the generator's oracles with independent closed forms and
exact polynomial identities, then confirm that the committed C++ header matches
the generator. They never call Ludus code.
"""
from __future__ import annotations

import unittest
from decimal import Decimal
from fractions import Fraction

import generate_fixtures as fixtures

CLOSED_FORM_TOLERANCE = Decimal("1e-35")


def cubic_spec(name: str) -> fixtures.CubicSpec:
    return next(spec for spec in fixtures.CUBIC_SPECS if spec.name == name)


def cubic_controls(name: str) -> list:
    return [fixtures.as_fraction_point(p) for p in cubic_spec(name).controls]


def patch_spec(name: str) -> fixtures.PatchSpec:
    return next(spec for spec in fixtures.PATCH_SPECS if spec.name == name)


class ReferenceCorpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.results = fixtures.compute_all()
        cls.cubics = {item["spec"].name: item for item in cls.results["cubics"]}
        cls.patches = {item["spec"].name: item for item in cls.results["patches"]}

    def assertDecimalClose(self, actual: Decimal, expected: Decimal, tolerance=CLOSED_FORM_TOLERANCE) -> None:
        self.assertLessEqual(abs(actual - expected), tolerance, f"{actual} != {expected}")

    def test_committed_header_is_current(self) -> None:
        committed = fixtures.HEADER.read_text(encoding="utf-8")
        self.assertEqual(fixtures.normalize(committed), fixtures.normalize(fixtures.render(self.results)))

    def test_normalization_ignores_only_formatting(self) -> None:
        self.assertEqual(fixtures.normalize('a = {\n    1,\n    "x y",\n};'), fixtures.normalize('a = {1, "x y"};'))
        self.assertNotEqual(fixtures.normalize('"x y"'), fixtures.normalize('"xy"'))
        self.assertEqual(fixtures.normalize('.Purpose = "split "\n    "literal"'), fixtures.normalize('.Purpose = "split literal"'))
        self.assertNotEqual(fixtures.normalize('"a", "b"'), fixtures.normalize('"ab"'))
        self.assertNotEqual(fixtures.normalize("{1, 2}"), fixtures.normalize("{1, 3}"))

    def test_quadrature_matches_closed_form_lengths(self) -> None:
        root5 = Decimal(5).sqrt()
        expected = {
            "line_uniform": Decimal(3),
            "line_ease": Decimal(3),
            "cusp": 2 * (5 * root5 - 8),
            "parabola": 3 * (root5 / 2 + (2 + root5).ln() / 4),
        }
        for name, length in expected.items():
            with self.subTest(name=name):
                self.assertDecimalClose(self.cubics[name]["length"], length)

        # The reversing line's length is its total variation between the exact
        # zero-speed roots (5 -+ sqrt 5)/10 of x'(u) = 6 - 30u + 30u^2.
        x = fixtures.cubic_coordinate_polys(cubic_controls("line_backtrack"))[0]
        x_decimal = [fixtures.to_decimal(c) for c in x]
        first = (5 - root5) / 10
        second = (5 + root5) / 10
        at = [fixtures.poly_eval_decimal(x_decimal, u) for u in (Decimal(0), first, second, Decimal(1))]
        variation = abs(at[1] - at[0]) + abs(at[2] - at[1]) + abs(at[3] - at[2])
        self.assertDecimalClose(self.cubics["line_backtrack"]["length"], variation)
        zero_speed = [fixtures.to_decimal(u) for u in self.cubics["line_backtrack"]["zero_speed"]]
        self.assertDecimalClose(zero_speed[0], first)
        self.assertDecimalClose(zero_speed[1], second)

    def test_zero_speed_parameters_are_exact_roots(self) -> None:
        self.assertEqual(self.cubics["line_ease"]["zero_speed"], [Fraction(0), Fraction(1)])
        self.assertEqual(self.cubics["cusp"]["zero_speed"], [Fraction(1, 2)])
        for name in ("line_uniform", "parabola", "inflection", "twisted", "vertical_start", "arch"):
            with self.subTest(name=name):
                self.assertEqual(self.cubics[name]["zero_speed"], [])

    def test_distance_inversion_has_small_residuals(self) -> None:
        for name, item in self.cubics.items():
            model = fixtures.arc_length_model(item["controls"])
            for distance, u in item["distances"]:
                with self.subTest(name=name, distance=distance):
                    self.assertLessEqual(abs(model.length(Decimal(0), u) - distance), Decimal("1e-36"))
        for distance, u in self.cubics["line_uniform"]["distances"]:
            self.assertDecimalClose(u, distance / 3)
        # line_ease: x(u) = 3(3u^2 - 2u^3) is monotone, so s = x(u).
        for distance, u in self.cubics["line_ease"]["distances"]:
            self.assertDecimalClose(3 * (3 * u * u - 2 * u * u * u), distance)

    def test_symmetric_and_overlapping_ties_are_certified(self) -> None:
        arch = cubic_controls("arch")
        mirrored = [Fraction(-1), Fraction(1)]  # u -> 1 - u
        for projection in self.cubics["arch"]["projections"]:
            if projection.query[0] != Fraction(3, 2):
                continue
            squared = fixtures.squared_distance_poly(arch, projection.query)
            with self.subTest(query=projection.query):
                self.assertEqual(squared, fixtures.poly_compose_linear(squared, mirrored[0], mirrored[1]))
        backtrack = cubic_controls("line_backtrack")
        projection = self.cubics["line_backtrack"]["projections"][0]
        squared = fixtures.squared_distance_poly(backtrack, projection.query)
        x = fixtures.cubic_coordinate_polys(backtrack)[0]
        offset = fixtures.poly_sub(x, [Fraction(1, 2)])
        self.assertEqual(fixtures.poly_sub(squared, [Fraction(1)]), fixtures.poly_mul(offset, offset))
        self.assertEqual(projection.tie_count, 3)
        self.assertEqual(projection.minima[1], Fraction(1, 2))

    def test_projection_minima_are_global_on_a_dense_grid(self) -> None:
        for name, item in self.cubics.items():
            for projection in item["projections"]:
                squared = fixtures.squared_distance_poly(item["controls"], projection.query)
                best = projection.distance * projection.distance
                samples = (fixtures.poly_eval(squared, Fraction(k, 2048)) for k in range(2049))
                with self.subTest(name=name, query=projection.query):
                    self.assertGreaterEqual(fixtures.to_decimal(min(samples)), best - Decimal("1e-40"))
                    self.assertIn(projection.u, projection.minima)

    def test_catmull_rom_conversion_matches_barry_goldman(self) -> None:
        item = self.results["catmull_rom"][0]
        for k in range(17):
            u = Fraction(k, 16)
            t = item["knots"][1] + item["width"] * u
            with self.subTest(u=u):
                self.assertEqual(fixtures.cubic_sample(item["controls"], u, 0),
                                 fixtures.barry_goldman(item["points"], item["knots"], t))
        self.assertEqual(item["knots"], [Fraction(0), Fraction(1), Fraction(3), Fraction(6)])
        self.assertEqual(self.results["catmull_rom"][1]["status"], "InvalidDomain")

    def test_tessellation_bounds_cover_true_deviation(self) -> None:
        expected = {"plane": Fraction(0), "saddle": Fraction(9, 4), "bulge": Fraction(27, 16)}
        for name, true_max in expected.items():
            item = self.patches[name]
            controls = patch_spec(name).controls
            grid = [fixtures.triangle_pair_deviation(controls, Fraction(i, 32), Fraction(j, 32))
                    for i in range(33) for j in range(33)]
            with self.subTest(name=name):
                self.assertDecimalClose(item["true_max"][2], fixtures.to_decimal(true_max))
                self.assertLessEqual(max(grid), item["true_max"][2] + Decimal("1e-40"))
                self.assertLessEqual(item["true_max"][2], item["interior"] + Decimal("1e-40"))
        # The saddle is bilinear, so its whole bound is the tight twist term.
        self.assertEqual(self.patches["saddle"]["net"], 0)
        self.assertDecimalClose(self.patches["saddle"]["twist_bound"], Decimal("2.25"))

    def test_shared_edge_corners_coincide(self) -> None:
        first = patch_spec("edge_first").controls
        second = patch_spec("edge_second").controls
        self.assertEqual(first[3][0], second[0][3])
        self.assertEqual(first[3][3], second[0][0])
        for v, second_v, position in self.results["edges"][0]["samples"]:
            self.assertEqual(second_v, 1 - v)
            self.assertEqual(position, fixtures.patch_sample(second, Fraction(0), second_v, 0, 0))

    def test_position_floor_uses_two_float32_ulps(self) -> None:
        self.assertEqual(fixtures.position_floor(Fraction(3)), Fraction(1, 2**21))
        self.assertEqual(fixtures.position_floor(Fraction(3, 2)), Fraction(1, 2**22))
        self.assertEqual(fixtures.position_floor(Fraction(2**30)), Fraction(256))
        precision = self.results["precision"][0]
        self.assertGreater(precision["naive_error"], fixtures.to_decimal(precision["relative_floor"]))

    def test_track_policies_follow_the_frozen_rules(self) -> None:
        samples = {(t, policy, side): (status, value, derivative)
                   for t, policy, side, status, value, derivative in self.results["tracks"][0]["samples"]}
        self.assertEqual(samples[(Fraction(-1, 2), "Reject", "Outgoing")][0], "InvalidDomain")
        # Loop maps outside times into [start, end); 7 = start + 2 * duration.
        self.assertEqual(samples[(Fraction(7), "Loop", "Outgoing")][1:], (0, 1))
        # PingPong reflects time and negates the derivative.
        self.assertEqual(samples[(Fraction(-1, 2), "PingPong", "Outgoing")][1:], (Fraction(1, 2), -1))
        # Hold has zero derivative; Clamp keeps the one-sided endpoint derivative.
        self.assertEqual(samples[(Fraction(-1, 2), "Hold", "Outgoing")][2], 0)
        self.assertEqual(samples[(Fraction(-1, 2), "Clamp", "Outgoing")][2], 1)
        # The terminal key of an open domain selects the last span at u = 1.
        self.assertEqual(samples[(Fraction(7, 2), "Reject", "Outgoing")][1], 0)
        self.assertEqual(samples[(Fraction(1), "Reject", "Incoming")][2], 1)
        self.assertEqual(samples[(Fraction(1), "Reject", "Outgoing")][2], 2)
        # The Hermite span's value 1 + (4/3)s(1-s)^2... peaks at s = 1/3 with 43/27.
        time, value = self.results["tracks"][0]["overshoot"]
        self.assertDecimalClose(time, Decimal(5) / 3)
        self.assertDecimalClose(fixtures.to_decimal(value), Decimal(43) / 27)


if __name__ == "__main__":
    unittest.main()


class FrameReferenceTests(unittest.TestCase):
    """Rotation-minimizing frame references: closed forms, invariants and step-halving convergence."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.frames = {spec.name: fixtures.compute_frame(spec) for spec in fixtures.FRAME_SPECS}

    def test_arctangent_matches_closed_forms(self) -> None:
        pi = fixtures.QUADRATURE.half_pi * 2
        self.assertLessEqual(abs(fixtures.decimal_atan(Decimal(1)) - pi / 4), Decimal("1e-45"))
        self.assertLessEqual(abs(fixtures.decimal_atan(Decimal(3).sqrt()) - pi / 3), Decimal("1e-45"))
        self.assertLessEqual(abs(fixtures.decimal_atan2(Decimal(-1), Decimal(-1)) + 3 * pi / 4), Decimal("1e-45"))

    def test_planar_transport_keeps_the_plane_normal(self) -> None:
        # In a plane T' stays in the plane, so r' = -(r . T') T vanishes for the out-of-plane normal.
        for _, _, normal in self.frames["inflection_rmf"]["references"]:
            self.assertEqual(normal, (Decimal(0), Decimal(0), Decimal(1)))

    def test_references_stay_orthonormal(self) -> None:
        for name, item in self.frames.items():
            for index, u, normal in item["references"]:
                with self.subTest(name=name, span=index, u=u):
                    tangent, _ = item["models"][index].tangent(fixtures.to_decimal(u))
                    # Integration error, not rounding: far below one float64 unit at emission.
                    self.assertLessEqual(abs(fixtures.length(normal) - 1), Decimal("1e-15"))
                    self.assertLessEqual(abs(fixtures.dot(normal, tangent)), Decimal("1e-15"))

    def test_step_halving_agrees(self) -> None:
        for name in ("twisted_rmf", "loop_holonomy"):
            coarse = fixtures.compute_frame(next(s for s in fixtures.FRAME_SPECS if s.name == name),
                                            fixtures.FRAME_STEPS // 2)
            for (_, _, fine), (_, _, rough) in zip(self.frames[name]["references"], coarse["references"]):
                with self.subTest(name=name):
                    self.assertLessEqual(fixtures.length(fixtures.add(fine, fixtures.scale(rough, -1))),
                                         Decimal("1e-13"))
            self.assertLessEqual(abs(self.frames[name]["residual"] - coarse["residual"]), Decimal("1e-13"))

    def test_loop_has_nonzero_holonomy(self) -> None:
        self.assertGreater(abs(self.frames["loop_holonomy"]["residual"]), Decimal("0.1"))
