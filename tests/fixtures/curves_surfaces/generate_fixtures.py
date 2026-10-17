#!/usr/bin/env python3
"""Generate the G0 curves and surfaces reference fixture corpus.

Every reference value comes from exact rational arithmetic (``fractions``) or
50-digit ``decimal`` arithmetic and is rounded to float64 once, at emission.
Nothing here calls Ludus code, so C++ tests that consume
``reference_fixtures.hpp`` compare against an independent oracle. ADR 0024
defines the frozen semantics encoded here: parameter domains, boundary
policies, span sides, the float32 position floor and the guarantee classes.

Methods and acknowledgements:

- Real roots are isolated with Sturm sequences on exact rational polynomials,
  then bisected to a 2^-180 bracket (Sturm's theorem, as presented in any
  standard algebra text; no external code is used).
- Arc lengths integrate the exact speed polynomial's square root with the
  double-exponential (tanh-sinh) rule of H. Takahasi and M. Mori, "Double
  Exponential Formulas for Numerical Integration", Publications of the
  Research Institute for Mathematical Sciences 9 (1974), 721-741. Zero-speed
  parameters split the domain first so every piece is smooth. Closed forms
  cross-check the rule in test_fixtures.py.
- Centripetal Catmull-Rom tangents use the nonuniform formula and are checked
  against the recursive pyramid of P. J. Barry and R. N. Goldman, "A Recursive
  Evaluation Algorithm for a Class of Catmull-Rom Splines", SIGGRAPH 1988,
  Computer Graphics 22(4), 199-204. The parameterization follows C. Yuksel,
  S. Schaefer and J. Keyser, "Parameterization and Applications of Catmull-Rom
  Curves", Computer-Aided Design 43(7), 2011
  (https://www.cemyuksel.com/research/catmullrom_param/).
- Pi uses the series recipe from the Python ``decimal`` module documentation.
- Rotation-minimizing frames integrate Bishop's transport equation
  r' = -(r . T') T (R. L. Bishop, "There is More than One Way to Frame a Curve",
  American Mathematical Monthly 82(3), 1975, 246-251) with the classical
  fourth-order Runge-Kutta rule (W. Kutta, Zeitschrift fur Mathematik und
  Physik 46, 1901), at two step sizes combined by Richardson extrapolation.
  test_fixtures.py checks step-halving agreement, orthonormality and a planar
  closed form. The arctangent uses Euler's series after argument halving.

Usage:
  generate_fixtures.py --write   regenerate reference_fixtures.hpp
  generate_fixtures.py --check   fail when the committed header is stale
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from decimal import Decimal, getcontext, localcontext
from fractions import Fraction
from math import comb
from pathlib import Path

DECIMAL_DIGITS = 50
getcontext().prec = DECIMAL_DIGITS
# Quadrature levels stop once successive tanh-sinh refinements agree this well.
QUADRATURE_TOLERANCE = Decimal("1e-40")
QUADRATURE_TAU_LIMIT = Decimal("4.5")
QUADRATURE_MAX_LEVEL = 10
# Isolated roots are bisected to an interval of this many bits.
ROOT_BITS = 180
# Squared distances closer than this are one tied global minimum. Ties used by
# the corpus are additionally certified by exact polynomial identities.
TIE_TOLERANCE = Fraction(1, 2**100)
INVERSION_TOLERANCE = Decimal("1e-38")
# Runge-Kutta steps per span for frame references; Richardson combines this and twice it.
FRAME_STEPS = 1024
# Must match MAX_PROJECTION_MINIMA in the emitted header.
MAX_PROJECTION_MINIMA = 3
HEADER = Path(__file__).with_name("reference_fixtures.hpp")

Poly = list  # Power-basis coefficients, lowest degree first.


# --------------------------------------------------------------------------
# Exact polynomial algebra
# --------------------------------------------------------------------------
def poly_trim(p: Poly) -> Poly:
    result = list(p)
    while len(result) > 1 and result[-1] == 0:
        result.pop()
    return result or [Fraction(0)]


def poly_add(a: Poly, b: Poly) -> Poly:
    size = max(len(a), len(b))
    return poly_trim([(a[i] if i < len(a) else 0) + (b[i] if i < len(b) else 0) for i in range(size)])


def poly_scale(p: Poly, factor) -> Poly:
    return poly_trim([c * factor for c in p])


def poly_sub(a: Poly, b: Poly) -> Poly:
    return poly_add(a, poly_scale(b, -1))


def poly_mul(a: Poly, b: Poly) -> Poly:
    result = [Fraction(0)] * (len(a) + len(b) - 1)
    for i, x in enumerate(a):
        for j, y in enumerate(b):
            result[i + j] += x * y
    return poly_trim(result)


def poly_derivative(p: Poly) -> Poly:
    return poly_trim([c * i for i, c in enumerate(p)][1:] or [Fraction(0)])


def poly_eval(p: Poly, x):
    total = 0 * x
    for c in reversed(p):
        total = total * x + c
    return total


def poly_eval_decimal(p: list[Decimal], x: Decimal) -> Decimal:
    total = Decimal(0)
    for c in reversed(p):
        total = total * x + c
    return total


def poly_degree(p: Poly) -> int:
    p = poly_trim(p)
    return -1 if p == [0] else len(p) - 1


def poly_divmod(a: Poly, b: Poly) -> tuple[Poly, Poly]:
    a = poly_trim([Fraction(c) for c in a])
    b = poly_trim([Fraction(c) for c in b])
    if poly_degree(b) < 0:
        raise ZeroDivisionError("polynomial division by zero")
    quotient = [Fraction(0)] * max(1, len(a) - len(b) + 1)
    remainder = list(a)
    while poly_degree(remainder) >= poly_degree(b) and poly_degree(remainder) >= 0:
        shift = poly_degree(remainder) - poly_degree(b)
        factor = remainder[-1] / b[-1]
        quotient[shift] = factor
        remainder = poly_sub(remainder, [Fraction(0)] * shift + [c * factor for c in b])
    return poly_trim(quotient), remainder


def poly_gcd(a: Poly, b: Poly) -> Poly:
    a = poly_trim(a)
    b = poly_trim(b)
    while poly_degree(b) >= 0:
        a, b = b, poly_divmod(a, b)[1]
    return poly_scale(a, 1 / a[-1])


def poly_compose_linear(p: Poly, scale, offset) -> Poly:
    """Return p(scale * x + offset)."""
    result = [Fraction(0)]
    for c in reversed(p):
        result = poly_add(poly_mul(result, [Fraction(offset), Fraction(scale)]), [c])
    return result


def squarefree(p: Poly) -> Poly:
    return poly_divmod(p, poly_gcd(p, poly_derivative(p)))[0]


def sturm_sequence(p: Poly) -> list[Poly]:
    sequence = [poly_trim(p), poly_derivative(p)]
    while poly_degree(sequence[-1]) > 0:
        remainder = poly_divmod(sequence[-2], sequence[-1])[1]
        if poly_degree(remainder) < 0:
            break
        sequence.append(poly_scale(remainder, -1))
    return sequence


def sign_variations(sequence: list[Poly], x: Fraction) -> int:
    signs = [v for v in (poly_eval(p, x) for p in sequence) if v != 0]
    return sum(1 for left, right in zip(signs, signs[1:]) if (left < 0) != (right < 0))


def refine_root(p: Poly, lo: Fraction, hi: Fraction) -> Fraction:
    low_sign = poly_eval(p, lo) < 0
    width = Fraction(1, 2**ROOT_BITS)
    while hi - lo > width:
        mid = (lo + hi) / 2
        value = poly_eval(p, mid)
        if value == 0:
            return mid
        if (value < 0) == low_sign:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def isolate_open(p: Poly, sequence: list[Poly], lo: Fraction, hi: Fraction) -> list[Fraction]:
    """Roots of squarefree p in (lo, hi); p(lo) and p(hi) are nonzero."""
    count = sign_variations(sequence, lo) - sign_variations(sequence, hi)
    if count == 0:
        return []
    if count == 1:
        return [refine_root(p, lo, hi)]
    mid = (lo + hi) / 2
    if poly_eval(p, mid) == 0:
        gap = (hi - lo) / 4
        while (poly_eval(p, mid - gap) == 0 or poly_eval(p, mid + gap) == 0
               or sign_variations(sequence, mid - gap) - sign_variations(sequence, mid + gap) != 1):
            gap /= 2
        return isolate_open(p, sequence, lo, mid - gap) + [mid] + isolate_open(p, sequence, mid + gap, hi)
    return isolate_open(p, sequence, lo, mid) + isolate_open(p, sequence, mid, hi)


def roots_in_unit_interval(p: Poly) -> list[Fraction]:
    """All distinct real roots of p in [0, 1], ascending."""
    if poly_degree(p) <= 0:
        if poly_degree(p) < 0:
            raise ValueError("the zero polynomial has no isolated roots")
        return []
    reduced = squarefree(p)
    roots = []
    if poly_eval(reduced, Fraction(0)) == 0:
        roots.append(Fraction(0))
        reduced = poly_divmod(reduced, [Fraction(0), Fraction(1)])[0]
    has_one = poly_eval(reduced, Fraction(1)) == 0
    if has_one:
        reduced = poly_divmod(reduced, [Fraction(-1), Fraction(1)])[0]
    if poly_degree(reduced) > 0:
        roots.extend(isolate_open(reduced, sturm_sequence(reduced), Fraction(0), Fraction(1)))
    if has_one:
        roots.append(Fraction(1))
    return roots


# --------------------------------------------------------------------------
# Decimal helpers and tanh-sinh quadrature
# --------------------------------------------------------------------------
def decimal_pi() -> Decimal:
    with localcontext() as context:
        context.prec += 2
        three = Decimal(3)
        last, t, s, n, na, d, da = 0, three, 3, 1, 0, 0, 24
        while s != last:
            last = s
            n, na = n + na, na + 8
            d, da = d + da, da + 32
            t = (t * n) / d
            s += t
    return +s


def to_decimal(value) -> Decimal:
    if isinstance(value, Decimal):
        return value
    value = Fraction(value)
    return Decimal(value.numerator) / Decimal(value.denominator)


class TanhSinh:
    """Cached nodes for integrals over [a, b] mapped from [0, 1]."""

    def __init__(self) -> None:
        self.levels: dict[int, list[tuple[Decimal, Decimal]]] = {}
        self.half_pi = decimal_pi() / 2

    def nodes(self, level: int) -> list[tuple[Decimal, Decimal]]:
        if level not in self.levels:
            step = Decimal(1) / (2**level)
            # (distance from an endpoint as a fraction of b - a, weight); the
            # zero node sits at the midpoint and is listed once.
            nodes = [(Decimal("0.5"), step * self.half_pi / 2)]
            k = 1
            while True:
                tau = step * k
                if tau > QUADRATURE_TAU_LIMIT:
                    break
                grow = tau.exp()
                sinh = (grow - 1 / grow) / 2
                cosh = (grow + 1 / grow) / 2
                exponent = (2 * self.half_pi * sinh).exp()
                offset = 1 / (exponent + 1)
                weight = step * self.half_pi * cosh * 2 / (exponent + 2 + 1 / exponent)
                nodes.append((offset, weight))
                k += 1
            self.levels[level] = nodes
        return self.levels[level]

    def integrate_level(self, integrand, a: Decimal, b: Decimal, level: int) -> Decimal:
        width = b - a
        nodes = self.nodes(level)
        total = nodes[0][1] * integrand(a + width * nodes[0][0])
        for offset, weight in nodes[1:]:
            total += weight * (integrand(a + width * offset) + integrand(b - width * offset))
        return total * width

    def integrate(self, integrand, a: Decimal, b: Decimal) -> Decimal:
        if a == b:
            return Decimal(0)
        previous = self.integrate_level(integrand, a, b, 3)
        for level in range(4, QUADRATURE_MAX_LEVEL + 1):
            current = self.integrate_level(integrand, a, b, level)
            if abs(current - previous) <= QUADRATURE_TOLERANCE * max(Decimal(1), abs(current)):
                return current
            previous = current
        raise RuntimeError("tanh-sinh quadrature did not converge")


QUADRATURE = TanhSinh()


# --------------------------------------------------------------------------
# Bezier references (Bernstein basis, independent from de Casteljau)
# --------------------------------------------------------------------------
Point = tuple  # (x, y, z) of Fraction or Decimal


def bernstein_polys() -> list[Poly]:
    """Power-basis polynomials of the four cubic Bernstein basis functions."""
    result = []
    for i in range(4):
        poly = [Fraction(comb(3, i))]
        for _ in range(i):
            poly = poly_mul(poly, [Fraction(0), Fraction(1)])
        for _ in range(3 - i):
            poly = poly_mul(poly, [Fraction(1), Fraction(-1)])
        result.append(poly)
    return result


BERNSTEIN = bernstein_polys()
BERNSTEIN_D1 = [poly_derivative(p) for p in BERNSTEIN]
BERNSTEIN_D2 = [poly_derivative(p) for p in BERNSTEIN_D1]
BASES = (BERNSTEIN, BERNSTEIN_D1, BERNSTEIN_D2)


def cubic_coordinate_polys(controls: list[Point]) -> list[Poly]:
    """Power-basis polynomial of each coordinate of a cubic Bezier."""
    result = []
    for axis in range(3):
        poly = [Fraction(0)]
        for i in range(4):
            poly = poly_add(poly, poly_scale(BERNSTEIN[i], controls[i][axis]))
        result.append(poly)
    return result


def cubic_sample(controls: list[Point], u: Fraction, order: int) -> Point:
    basis = [poly_eval(p, u) for p in BASES[order]]
    return tuple(sum(basis[i] * controls[i][axis] for i in range(4)) for axis in range(3))


def speed_squared(controls: list[Point]) -> Poly:
    total = [Fraction(0)]
    for poly in cubic_coordinate_polys(controls):
        derivative = poly_derivative(poly)
        total = poly_add(total, poly_mul(derivative, derivative))
    return total


def squared_distance_poly(controls: list[Point], query: Point) -> Poly:
    total = [Fraction(0)]
    for axis, poly in enumerate(cubic_coordinate_polys(controls)):
        difference = poly_sub(poly, [Fraction(query[axis])])
        total = poly_add(total, poly_mul(difference, difference))
    return total


def length(point: Point) -> Decimal:
    return sum(to_decimal(c) * to_decimal(c) for c in point).sqrt()


@dataclass
class ArcLengthModel:
    """Piecewise-smooth speed with zero-speed breakpoints."""

    speed_decimal: list[Decimal]
    breakpoints: list[Decimal]
    zero_speed: list[Fraction]

    def speed(self, u: Decimal) -> Decimal:
        value = poly_eval_decimal(self.speed_decimal, u)
        return value.sqrt() if value > 0 else Decimal(0)

    def length(self, a: Decimal, b: Decimal) -> Decimal:
        total = Decimal(0)
        cuts = [a] + [p for p in self.breakpoints if a < p < b] + [b]
        for lo, hi in zip(cuts, cuts[1:]):
            total += QUADRATURE.integrate(self.speed, lo, hi)
        return total

    def invert(self, target: Decimal) -> Decimal:
        """Earliest u with length(0, u) == target, by safeguarded Newton."""
        cuts = [Decimal(0)] + [p for p in self.breakpoints if 0 < p < 1] + [Decimal(1)]
        prefix = Decimal(0)
        for lo, hi in zip(cuts, cuts[1:]):
            piece = self.length(lo, hi)
            if target <= prefix + piece or hi == 1:
                return self.invert_piece(lo, hi, target - prefix)
            prefix += piece
        raise AssertionError("unreachable")

    def invert_piece(self, lo: Decimal, hi: Decimal, target: Decimal) -> Decimal:
        start = lo
        low, high = lo, hi
        piece = self.length(lo, hi)
        u = lo + (hi - lo) * (target / piece)
        for _ in range(200):
            residual = self.length(start, u) - target
            if abs(residual) <= INVERSION_TOLERANCE:
                return u
            if residual > 0:
                high = u
            else:
                low = u
            speed = self.speed(u)
            candidate = u - residual / speed if speed > 0 else None
            if candidate is None or not low < candidate < high:
                candidate = (low + high) / 2
            u = candidate
        raise RuntimeError("distance inversion did not converge")


def arc_length_model(controls: list[Point]) -> ArcLengthModel:
    speed2 = speed_squared(controls)
    zero_speed = roots_in_unit_interval(speed2)
    return ArcLengthModel(
        speed_decimal=[to_decimal(c) for c in speed2],
        breakpoints=[to_decimal(r) for r in zero_speed],
        zero_speed=zero_speed,
    )


@dataclass
class Projection:
    query: Point
    distance: Decimal
    u: Fraction
    tie_count: int
    minima: list[Fraction]


def project_point(controls: list[Point], query: Point) -> Projection:
    """Global closest point over [0, 1], including endpoint candidates."""
    squared = squared_distance_poly(controls, query)
    first = poly_derivative(squared)
    second = poly_derivative(first)
    minima = []
    for root in roots_in_unit_interval(first):
        curvature = poly_eval(second, root)
        if curvature > 0 or (root in (0, 1) and curvature >= 0):
            minima.append(root)
        elif curvature == 0:
            raise ValueError("degenerate stationary point in projection fixture")
    if poly_eval(first, Fraction(0)) > 0 and Fraction(0) not in minima:
        minima.insert(0, Fraction(0))
    if poly_eval(first, Fraction(1)) < 0 and Fraction(1) not in minima:
        minima.append(Fraction(1))
    values = [poly_eval(squared, u) for u in minima]
    best = min(values)
    tied = [u for u, value in zip(minima, values) if value - best <= TIE_TOLERANCE]
    return Projection(query, to_decimal(best).sqrt(), min(tied), len(tied), sorted(minima))


# --------------------------------------------------------------------------
# Patch references
# --------------------------------------------------------------------------
PATCH_ORDERS = {"Position": (0, 0), "DU": (1, 0), "DV": (0, 1), "DUU": (2, 0), "DUV": (1, 1), "DVV": (0, 2)}


def patch_sample(controls, u: Fraction, v: Fraction, order_u: int, order_v: int) -> Point:
    bu = [poly_eval(p, u) for p in BASES[order_u]]
    bv = [poly_eval(p, v) for p in BASES[order_v]]
    return tuple(
        sum(bu[i] * bv[j] * controls[i][j][axis] for i in range(4) for j in range(4)) for axis in range(3)
    )


def cross(a: Point, b: Point) -> Point:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def subtract(a: Point, b: Point) -> Point:
    return tuple(x - y for x, y in zip(a, b))


def classify_normal(controls, u: Fraction, v: Fraction, minimum_length: Fraction, minimum_sin: Fraction):
    """Mirror the frozen TryPatchNormal classification in exact arithmetic."""
    du = patch_sample(controls, u, v, 1, 0)
    dv = patch_sample(controls, u, v, 0, 1)
    normal = cross(du, dv)
    zero = (Fraction(0),) * 3
    if du == zero or dv == zero or normal == zero:
        return "Degenerate", zero
    du_length = length(du)
    dv_length = length(dv)
    normal_length = length(normal)
    sin_angle = normal_length / du_length / dv_length
    if (du_length <= to_decimal(minimum_length) or dv_length <= to_decimal(minimum_length)
            or sin_angle <= to_decimal(minimum_sin)):
        return "IllConditioned", zero
    return "Success", tuple(to_decimal(c) / normal_length for c in normal)


def bilinear_controls(controls):
    """Controls of the degree-elevated bilinear interpolant of the four corners."""
    p00, p30, p03, p33 = controls[0][0], controls[3][0], controls[0][3], controls[3][3]
    result = []
    for i in range(4):
        row = []
        for j in range(4):
            s = Fraction(i, 3)
            t = Fraction(j, 3)
            row.append(tuple((1 - s) * (1 - t) * p00[a] + s * (1 - t) * p30[a] + (1 - s) * t * p03[a] + s * t * p33[a]
                             for a in range(3)))
        result.append(row)
    return result


def tessellation_reference(controls):
    twist = tuple(controls[0][0][a] - controls[3][0][a] - controls[0][3][a] + controls[3][3][a] for a in range(3))
    twist_bound = length(twist) / 4
    elevated = bilinear_controls(controls)
    net = max(length(subtract(controls[i][j], elevated[i][j])) for i in range(4) for j in range(4))
    return twist, twist_bound, net, net + twist_bound


def triangle_pair_deviation(controls, u: Fraction, v: Fraction) -> Decimal:
    """Distance between the patch and the P00-P33 diagonal triangle pair at (u, v)."""
    p00, p30, p03, p33 = controls[0][0], controls[3][0], controls[0][3], controls[3][3]
    if v <= u:
        linear = tuple(p00[a] + u * (p30[a] - p00[a]) + v * (p33[a] - p30[a]) for a in range(3))
    else:
        linear = tuple(p00[a] + v * (p03[a] - p00[a]) + u * (p33[a] - p03[a]) for a in range(3))
    return length(subtract(patch_sample(controls, u, v, 0, 0), linear))


# --------------------------------------------------------------------------
# Float formatting
# --------------------------------------------------------------------------
def float64_literal(value) -> str:
    number = float(value) if not isinstance(value, Fraction) else value.numerator / value.denominator
    if number != number or number in (float("inf"), float("-inf")):
        raise ValueError("fixture references must be finite")
    text = repr(number)
    if "." not in text and "e" not in text:
        text += ".0"
    return text


def is_float32(value: Fraction) -> bool:
    number = value.numerator / value.denominator
    rounded = struct.unpack("<f", struct.pack("<f", number))[0]
    return Fraction(rounded) == value


def float32_literal(value) -> str:
    value = Fraction(value)
    if not is_float32(value):
        raise ValueError(f"fixture control {value} is not exactly representable in float32")
    return float64_literal(value) + "f"


def float32_ulp(magnitude: Fraction) -> Fraction:
    """Spacing of float32 values at a positive magnitude in the normal range."""
    if magnitude <= 0:
        raise ValueError("ulp needs a positive magnitude")
    exponent = 0
    while Fraction(2) ** (exponent + 1) <= magnitude:
        exponent += 1
    while Fraction(2) ** exponent > magnitude:
        exponent -= 1
    if exponent < -126:
        return Fraction(1, 2**149)
    return Fraction(2) ** (exponent - 23)


def position_floor(extent: Fraction) -> Fraction:
    """ADR 0024 float32 position floor: two ulps at the chunk extent."""
    return 2 * float32_ulp(extent)


def as_fraction_point(point) -> Point:
    return tuple(Fraction(c) for c in point)


# --------------------------------------------------------------------------
# Fixture definitions
# --------------------------------------------------------------------------
STANDARD_PARAMETERS = [Fraction(0), Fraction(1, 4), Fraction(1, 2), Fraction(3, 4), Fraction(1)]


@dataclass
class CubicSpec:
    name: str
    purpose: str
    controls: list
    parameters: list = field(default_factory=lambda: list(STANDARD_PARAMETERS))
    distance_fractions: list = field(default_factory=lambda: [Fraction(1, 4), Fraction(1, 2), Fraction(3, 4)])
    queries: list = field(default_factory=list)


CUBIC_SPECS = [
    CubicSpec(
        "line_uniform",
        "G1-G2: collinear evenly spaced controls; constant speed 3, length 3 and exact u(s) = s/3.",
        [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0)],
    ),
    CubicSpec(
        "line_ease",
        "G2-G3: collinear ease-in/out with zero speed at both endpoints; length 3 and an undefined endpoint "
        "tangent for frames.",
        [(0, 0, 0), (0, 0, 0), (3, 0, 0), (3, 0, 0)],
    ),
    CubicSpec(
        "line_backtrack",
        "G2-G3: collinear reversal; two interior zero-speed parameters, exact opposite tangents and a three-way "
        "projection tie.",
        [(0, 0, 0), (2, 0, 0), (-1, 0, 0), (1, 0, 0)],
        queries=[(Fraction(1, 2), 1, 0)],
    ),
    CubicSpec(
        "cusp",
        "G2-G3: semicubical cusp x=t^3, y=3t^2 (t=2u-1); zero speed at u=1/2, closed-form length 2(5 sqrt 5 - 8).",
        [(-1, 3, 0), (1, -1, 0), (-1, -1, 0), (1, 3, 0)],
    ),
    CubicSpec(
        "parabola",
        "G2: degree-elevated parabola (3u, 3u^2); closed-form length 3(sqrt 5/2 + asinh(2)/4).",
        [(0, 0, 0), (1, 0, 0), (2, 1, 0), (3, 3, 0)],
    ),
    CubicSpec(
        "inflection",
        "G3: planar S curve with an inflection at u=1/2; a Frenet frame flips there, a transported frame must not.",
        [(0, 0, 0), (1, 1, 0), (2, -1, 0), (3, 0, 0)],
    ),
    CubicSpec(
        "twisted",
        "G3: regular non-planar cubic with torsion for rotation-minimizing frame comparisons.",
        [(0, 0, 0), (1, 0, 0), (1, 1, 0), (1, 1, 1)],
    ),
    CubicSpec(
        "vertical_start",
        "G3: start tangent parallel to +Y, so a +Y up seed projects to zero and needs the declared fallback.",
        [(0, 0, 0), (0, 1, 0), (1, 2, 0), (2, 2, 0)],
    ),
    CubicSpec(
        "arch",
        "G3: mirror-symmetric parabolic arch y=x(3-x); symmetric interior minima, apex minima and an endpoint "
        "minimum.",
        [(0, 0, 0), (1, 3, 0), (2, 3, 0), (3, 0, 0)],
        queries=[
            (Fraction(3, 2), 0, 0),
            (Fraction(3, 2), 1, 0),
            (Fraction(3, 2), 2, 0),
            (Fraction(3, 2), 4, 0),
            (-1, -1, 0),
        ],
    ),
]


@dataclass
class PatchSpec:
    name: str
    purpose: str
    controls: list
    samples: list
    normals: list  # (u, v, minimum length, minimum sin, expected status)
    true_max: tuple | None = None  # (u, v) where the triangle-pair deviation is maximal, derived analytically


def grid_patch(function) -> list:
    return [[tuple(Fraction(c) for c in function(i, j)) for j in range(4)] for i in range(4)]


HALF = Fraction(1, 2)
PATCH_PARAMETERS = [(Fraction(0), Fraction(0)), (Fraction(1, 4), Fraction(3, 4)), (HALF, HALF), (Fraction(1), Fraction(1))]
EDGE_HEIGHT = [0, 1, -1, 0]

PATCH_SPECS = [
    PatchSpec(
        "plane",
        "G1/G4: degree-elevated planar grid (3u, 3v, 0); normal +Z and zero tessellation error at level 0.",
        grid_patch(lambda i, j: (i, j, 0)),
        PATCH_PARAMETERS,
        [(HALF, HALF, 0, 0, "Success")],
        true_max=(HALF, HALF),
    ),
    PatchSpec(
        "saddle",
        "G1/G4: hyperbolic paraboloid z=9uv as a twisted bilinear patch; the P00-P33 triangle pair deviates by "
        "exactly the 9/4 twist bound at the centre.",
        grid_patch(lambda i, j: (i, j, i * j)),
        PATCH_PARAMETERS,
        [(HALF, HALF, 0, 0, "Success"), (Fraction(0), Fraction(1), 0, 0, "Success")],
        true_max=(HALF, HALF),
    ),
    PatchSpec(
        "bulge",
        "G4: flat corners with raised interior controls; control-net bound 3 versus true deviation 27/16.",
        grid_patch(lambda i, j: (i, j, 3 if i in (1, 2) and j in (1, 2) else 0)),
        PATCH_PARAMETERS,
        [(HALF, HALF, 0, 0, "Success")],
        true_max=(HALF, HALF),
    ),
    PatchSpec(
        "apex",
        "G1/G4: cone with its u=0 edge collapsed to an apex; Degenerate normals on that edge and IllConditioned "
        "normals beside it under a positive length threshold.",
        grid_patch(lambda i, j: (i * (2, 2, 1, 0)[j], i * (0, 1, 2, 2)[j], 3 - i)),
        PATCH_PARAMETERS,
        [
            (Fraction(0), HALF, 0, 0, "Degenerate"),
            (Fraction(1, 2**20), HALF, Fraction(1, 1000), 0, "IllConditioned"),
            (Fraction(1, 2**20), HALF, 0, 0, "Success"),
            (HALF, HALF, Fraction(1, 1000), Fraction(1, 10), "Success"),
        ],
    ),
    PatchSpec(
        "edge_first",
        "G4: first patch of a shared-edge pair; its u=1 boundary is the canonical curved edge.",
        grid_patch(lambda i, j: (i, j, i * EDGE_HEIGHT[j])),
        PATCH_PARAMETERS,
        [],
    ),
    PatchSpec(
        "edge_second",
        "G4: second patch of the pair; its u=0 boundary is the same edge with reversed v orientation.",
        grid_patch(lambda i, j: (3 + i, 3 - j, (3 - i) * EDGE_HEIGHT[3 - j])),
        PATCH_PARAMETERS,
        [],
    ),
]

SHARED_EDGE_SPECS = [
    {
        "name": "reversed_shared_edge",
        "purpose": "G4: edge_first u=1 equals edge_second u=0 with v reversed; corners and matched samples agree.",
        "first": "edge_first",
        "second": "edge_second",
        "first_u": Fraction(1),
        "second_u": Fraction(0),
        "reversed": True,
        "parameters": [Fraction(0), Fraction(1, 4), HALF, Fraction(3, 4), Fraction(1)],
    }
]

CATMULL_ROM_SPECS = [
    {
        "name": "centripetal_uneven",
        "purpose": "G2: chords 1, 4, 9 give centripetal knot steps 1, 2, 3; the middle span uses the nonuniform "
        "tangent formula.",
        "waypoints": [(0, 0, 0), (1, 0, 0), (1, 4, 0), (10, 4, 0)],
        "status": "Success",
    },
    {
        "name": "duplicate_waypoint",
        "purpose": "G2: consecutive coincident waypoints are rejected rather than producing an infinite tangent.",
        "waypoints": [(0, 0, 0), (1, 0, 0), (1, 0, 0), (2, 0, 0)],
        "status": "InvalidDomain",
    },
]

TRACK_SPECS = [
    {
        "name": "mixed_track",
        "purpose": "G2: Linear, Hermite and Step spans of uneven durations; Hermite overshoot, per-second slopes, "
        "span sides and every boundary policy.",
        "keys": [
            (Fraction(0), 0, 0, 0, "Linear"),
            (Fraction(1), 1, 0, 2, "Hermite"),
            (Fraction(3), 1, 0, 0, "Step"),
            (Fraction(7, 2), 0, 0, 0, "Step"),
        ],
        "status": "Success",
        "samples": [
            (Fraction(0), "Reject", "Outgoing"),
            (Fraction(1, 2), "Reject", "Outgoing"),
            (Fraction(1), "Reject", "Outgoing"),
            (Fraction(1), "Reject", "Incoming"),
            (Fraction(2), "Reject", "Outgoing"),
            (Fraction(3), "Reject", "Outgoing"),
            (Fraction(3), "Reject", "Incoming"),
            (Fraction(13, 4), "Reject", "Outgoing"),
            (Fraction(7, 2), "Reject", "Outgoing"),
            (Fraction(-1, 2), "Reject", "Outgoing"),
            (Fraction(-1, 2), "Clamp", "Outgoing"),
            (Fraction(4), "Clamp", "Outgoing"),
            (Fraction(-1, 2), "Hold", "Outgoing"),
            (Fraction(4), "Hold", "Outgoing"),
            (Fraction(4), "Loop", "Outgoing"),
            (Fraction(-1, 2), "Loop", "Outgoing"),
            (Fraction(7), "Loop", "Outgoing"),
            (Fraction(9, 2), "PingPong", "Outgoing"),
            (Fraction(-1, 2), "PingPong", "Outgoing"),
        ],
    },
    {
        "name": "duplicate_time",
        "purpose": "G2: equal key times are rejected; the runtime never resolves an undocumented tie.",
        "keys": [
            (Fraction(0), 0, 0, 0, "Linear"),
            (Fraction(1), 1, 0, 0, "Linear"),
            (Fraction(1), 2, 0, 0, "Linear"),
        ],
        "status": "InvalidDomain",
        "samples": [],
    },
]

PRECISION_SPECS = [
    {
        "name": "far_origin",
        "purpose": "G0/G2: controls 2^30 from the origin keep their dyadic offsets only after subtracting the "
        "chunk origin in float64.",
        "origin": (Fraction(2**30), Fraction(0), Fraction(-(2**30))),
        "offsets": [(0, 0, 0), (Fraction(1, 2), Fraction(1, 4), 0), (1, Fraction(1, 2), Fraction(1, 8)),
                    (Fraction(3, 2), Fraction(3, 4), Fraction(1, 4))],
        "max_abs_component": Fraction(2),
    }
]


# --------------------------------------------------------------------------
# Reference computation
# --------------------------------------------------------------------------
def compute_cubic(spec: CubicSpec) -> dict:
    controls = [as_fraction_point(p) for p in spec.controls]
    model = arc_length_model(controls)
    total = model.length(Decimal(0), Decimal(1))
    distances = [(total * to_decimal(f), model.invert(total * to_decimal(f))) for f in spec.distance_fractions]
    extent = max(abs(c) for p in controls for c in p)
    return {
        "spec": spec,
        "controls": controls,
        "samples": [(u, *(cubic_sample(controls, u, order) for order in range(3))) for u in spec.parameters],
        "length": total,
        "zero_speed": model.zero_speed,
        "distances": distances,
        "projections": [project_point(controls, as_fraction_point(q)) for q in spec.queries],
        "floor": position_floor(extent),
    }


def compute_patch(spec: PatchSpec) -> dict:
    samples = []
    for u, v in spec.samples:
        samples.append((u, v, {name: patch_sample(spec.controls, u, v, *orders) for name, orders in PATCH_ORDERS.items()}))
    normals = []
    for u, v, minimum_length, minimum_sin, expected in spec.normals:
        status, normal = classify_normal(spec.controls, u, v, Fraction(minimum_length), Fraction(minimum_sin))
        if status != expected:
            raise AssertionError(f"{spec.name} normal at ({u}, {v}) is {status}, expected {expected}")
        normals.append((u, v, Fraction(minimum_length), Fraction(minimum_sin), status, normal))
    twist, twist_bound, net, interior = tessellation_reference(spec.controls)
    true_max = None
    if spec.true_max is not None:
        u, v = spec.true_max
        true_max = (u, v, triangle_pair_deviation(spec.controls, u, v))
    return {
        "spec": spec,
        "samples": samples,
        "normals": normals,
        "twist": twist,
        "twist_bound": twist_bound,
        "net": net,
        "interior": interior,
        "true_max": true_max,
    }


def compute_shared_edge(spec: dict, patches: dict) -> dict:
    first = patches[spec["first"]].controls
    second = patches[spec["second"]].controls
    samples = []
    for v in spec["parameters"]:
        second_v = 1 - v if spec["reversed"] else v
        a = patch_sample(first, spec["first_u"], v, 0, 0)
        b = patch_sample(second, spec["second_u"], second_v, 0, 0)
        if a != b:
            raise AssertionError(f"{spec['name']} edge samples disagree at v={v}")
        samples.append((v, second_v, a))
    return {"spec": spec, "samples": samples}


def catmull_rom_tangents(points: list[Point], knots: list[Fraction]) -> tuple[Point, Point]:
    def tangent(previous, current, following, t_previous, t_current, t_following):
        return tuple(
            (current[a] - previous[a]) / (t_current - t_previous)
            - (following[a] - previous[a]) / (t_following - t_previous)
            + (following[a] - current[a]) / (t_following - t_current)
            for a in range(3)
        )

    start = tangent(points[0], points[1], points[2], knots[0], knots[1], knots[2])
    end = tangent(points[1], points[2], points[3], knots[1], knots[2], knots[3])
    return start, end


def barry_goldman(points: list[Point], knots: list[Fraction], t: Fraction) -> Point:
    def mix(p, q, t0, t1):
        return tuple(((t1 - t) * p[a] + (t - t0) * q[a]) / (t1 - t0) for a in range(3))

    a1 = mix(points[0], points[1], knots[0], knots[1])
    a2 = mix(points[1], points[2], knots[1], knots[2])
    a3 = mix(points[2], points[3], knots[2], knots[3])
    b1 = mix(a1, a2, knots[0], knots[2])
    b2 = mix(a2, a3, knots[1], knots[3])
    return mix(b1, b2, knots[1], knots[2])


def exact_sqrt(value: Fraction) -> Fraction | None:
    numerator = value.numerator
    denominator = value.denominator
    root_n = int(numerator**0.5)
    root_d = int(denominator**0.5)
    for n in (root_n - 1, root_n, root_n + 1):
        for d in (root_d - 1, root_d, root_d + 1):
            if n >= 0 and d > 0 and n * n == numerator and d * d == denominator:
                return Fraction(n, d)
    return None


def compute_catmull_rom(spec: dict) -> dict:
    points = [as_fraction_point(p) for p in spec["waypoints"]]
    result = {"spec": spec, "points": points, "status": spec["status"]}
    chords = [sum((b[a] - p[a]) ** 2 for a in range(3)) for p, b in zip(points, points[1:])]
    if any(chord == 0 for chord in chords):
        if spec["status"] != "InvalidDomain":
            raise AssertionError(f"{spec['name']} has coincident waypoints")
        return result
    # Centripetal alpha = 1/2: knot step = chord^(1/2) = (squared chord)^(1/4).
    steps = []
    for chord in chords:
        root = exact_sqrt(chord)
        root = exact_sqrt(root) if root is not None else None
        if root is None:
            raise AssertionError(f"{spec['name']} needs chord lengths whose square roots are rational")
        steps.append(root)
    knots = [Fraction(0)]
    for step in steps:
        knots.append(knots[-1] + step)
    start, end = catmull_rom_tangents(points, knots)
    width = knots[2] - knots[1]
    controls = [
        points[1],
        tuple(points[1][a] + width * start[a] / 3 for a in range(3)),
        tuple(points[2][a] - width * end[a] / 3 for a in range(3)),
        points[2],
    ]
    for k in range(5):
        u = Fraction(k, 4)
        if cubic_sample(controls, u, 0) != barry_goldman(points, knots, knots[1] + width * u):
            raise AssertionError(f"{spec['name']} Bezier conversion disagrees with the Barry-Goldman pyramid")
    result.update({"knots": knots, "start": start, "end": end, "width": width, "controls": controls})
    return result


def hermite_value(p0, p1, m0, m1, h, s, order):
    """Cubic Hermite value (order 0) or derivative per second (order 1) at s in [0, 1]."""
    b = [p0, p0 + h * m0 / 3, p1 - h * m1 / 3, p1]
    basis = [poly_eval(p, s) for p in BASES[order]]
    value = sum(basis[i] * b[i] for i in range(4))
    return value if order == 0 else value / h


class Track:
    """Frozen ADR 0024 scalar track semantics, written for exact arithmetic."""

    def __init__(self, keys):
        self.keys = keys

    def span_value(self, index, t, order):
        t0, v0, _, out0, mode = self.keys[index]
        t1, v1, in1, _, _ = self.keys[index + 1]
        h = t1 - t0
        s = (t - t0) / h
        if mode == "Step":
            if order == 1:
                return Fraction(0)
            return Fraction(v1 if s == 1 else v0)
        if mode == "Linear":
            return Fraction(v0) + (Fraction(v1) - v0) * s if order == 0 else (Fraction(v1) - v0) / h
        return hermite_value(Fraction(v0), Fraction(v1), Fraction(out0), Fraction(in1), h, s, order)

    def inside(self, t, side, order):
        times = [k[0] for k in self.keys]
        last = len(self.keys) - 2
        index = 0
        while index < last and t >= times[index + 1]:
            index += 1
        if side == "Incoming" and index > 0 and t == times[index]:
            index -= 1
        return self.span_value(index, t, order)

    def evaluate(self, t, policy, side):
        start = self.keys[0][0]
        end = self.keys[-1][0]
        duration = end - start
        if start <= t <= end:
            return "Success", self.inside(t, side, 0), self.inside(t, side, 1)
        if policy == "Reject":
            return "InvalidDomain", Fraction(0), Fraction(0)
        if policy in ("Clamp", "Hold"):
            clamped = start if t < start else end
            derivative = self.inside(clamped, "Outgoing" if t < start else "Incoming", 1)
            return "Success", self.inside(clamped, side, 0), derivative if policy == "Clamp" else Fraction(0)
        phase = t - start
        if policy == "Loop":
            wrapped = phase - duration * (phase // duration)
            return "Success", self.inside(start + wrapped, side, 0), self.inside(start + wrapped, side, 1)
        period = 2 * duration
        wrapped = phase - period * (phase // period)
        if wrapped <= duration:
            return "Success", self.inside(start + wrapped, side, 0), self.inside(start + wrapped, side, 1)
        reflected = start + period - wrapped
        return "Success", self.inside(reflected, side, 0), -self.inside(reflected, side, 1)


def compute_track(spec: dict) -> dict:
    keys = spec["keys"]
    times = [k[0] for k in keys]
    valid = all(a < b for a, b in zip(times, times[1:]))
    if valid != (spec["status"] == "Success"):
        raise AssertionError(f"{spec['name']} key validation disagrees with its expected status")
    result = {"spec": spec, "samples": [], "overshoot": None}
    if not valid:
        return result
    track = Track(keys)
    for t, policy, side in spec["samples"]:
        result["samples"].append((t, policy, side, *track.evaluate(t, policy, side)))
    for index in range(len(keys) - 1):
        if keys[index][4] != "Hermite":
            continue
        t0, v0, _, out0, _ = keys[index]
        t1, v1, in1, _, _ = keys[index + 1]
        h = t1 - t0
        b = [Fraction(v0), Fraction(v0) + h * Fraction(out0) / 3, Fraction(v1) - h * Fraction(in1) / 3, Fraction(v1)]
        poly = [Fraction(0)]
        for i in range(4):
            poly = poly_add(poly, poly_scale(BERNSTEIN[i], b[i]))
        candidates = [r for r in roots_in_unit_interval(poly_derivative(poly)) if 0 < r < 1]
        best = max(candidates, key=lambda r: poly_eval(poly, r))
        result["overshoot"] = (to_decimal(t0) + to_decimal(h) * to_decimal(best), poly_eval(poly, best))
    return result


def compute_precision(spec: dict) -> dict:
    origin = spec["origin"]
    offsets = [as_fraction_point(p) for p in spec["offsets"]]
    source = [tuple(origin[a] + p[a] for a in range(3)) for p in offsets]
    for point in source:
        for coordinate in point:
            if float(coordinate) != coordinate:
                raise AssertionError("precision fixture sources must be exact float64 values")
    naive = []
    for point in source:
        rounded = tuple(Fraction(struct.unpack("<f", struct.pack("<f", float(c)))[0]) for c in point)
        naive.append(length(subtract(rounded, point)))
    relative_extent = max(abs(c) for p in offsets for c in p)
    absolute_extent = max(abs(c) for p in source for c in p)
    return {
        "spec": spec,
        "source": source,
        "relative": offsets,
        "relative_extent": relative_extent,
        "relative_floor": position_floor(relative_extent),
        "absolute_floor": position_floor(absolute_extent),
        "naive_error": max(naive),
    }


# --------------------------------------------------------------------------
# Rotation-minimizing frame references (G3)
# --------------------------------------------------------------------------
def decimal_atan(x: Decimal) -> Decimal:
    """Arctangent by halving the argument, then Euler's series for atan."""
    with localcontext() as context:
        context.prec += 10
        halvings = 0
        while abs(x) > Decimal("0.1"):
            x = x / (1 + (1 + x * x).sqrt())
            halvings += 1
        # Euler: atan(x) = sum 2^(2n) (n!)^2 / (2n + 1)! * x^(2n+1) / (1 + x^2)^(n+1)
        y = x * x / (1 + x * x)
        term = x / (1 + x * x)
        total = term
        n = 0
        while True:
            n += 1
            term = term * y * (2 * n) / (2 * n + 1)
            if abs(term) < Decimal(10) ** -(context.prec + 2):
                break
            total += term
        result = total * (2**halvings)
    return +result


def decimal_atan2(y: Decimal, x: Decimal) -> Decimal:
    pi = QUADRATURE.half_pi * 2
    if x > 0:
        return decimal_atan(y / x)
    if x < 0:
        return decimal_atan(y / x) + (pi if y >= 0 else -pi)
    if y > 0:
        return pi / 2
    if y < 0:
        return -pi / 2
    return Decimal(0)


def dot(a: Point, b: Point):
    return sum(a[i] * b[i] for i in range(3))


def scale(a: Point, factor) -> Point:
    return tuple(c * factor for c in a)


def add(a: Point, b: Point) -> Point:
    return tuple(a[i] + b[i] for i in range(3))


def normalize_decimal(a: Point) -> Point:
    return scale(a, 1 / length(a))


class FrameModel:
    """Unit tangent and its parameter derivative of one cubic span, in 50-digit decimals."""

    def __init__(self, controls: list[Point]) -> None:
        polys = cubic_coordinate_polys(controls)
        self.first = [[to_decimal(c) for c in poly_derivative(p)] for p in polys]
        self.second = [[to_decimal(c) for c in poly_derivative(poly_derivative(p))] for p in polys]

    def tangent(self, u: Decimal) -> tuple[Point, Point]:
        first = tuple(poly_eval_decimal(p, u) for p in self.first)
        second = tuple(poly_eval_decimal(p, u) for p in self.second)
        speed = length(first)
        unit = scale(first, 1 / speed)
        # dT/du = (C'' - (C'' . T) T) / |C'|
        turning = scale(add(second, scale(unit, -dot(second, unit))), 1 / speed)
        return unit, turning

    def rate(self, u: Decimal, normal: Point) -> Point:
        unit, turning = self.tangent(u)
        return scale(unit, -dot(normal, turning))


def transport(model: FrameModel, normal: Point, start: Fraction, end: Fraction, steps: int) -> Point:
    """Integrate r' = -(r . T') T from start to end with the classical Runge-Kutta rule."""
    h = to_decimal(end - start) / steps
    u = to_decimal(start)
    for _ in range(steps):
        k1 = model.rate(u, normal)
        k2 = model.rate(u + h / 2, add(normal, scale(k1, h / 2)))
        k3 = model.rate(u + h / 2, add(normal, scale(k2, h / 2)))
        k4 = model.rate(u + h, add(normal, scale(k3, h)))
        normal = add(normal, scale(add(add(k1, scale(k2, 2)), add(scale(k3, 2), k4)), h / 6))
        u += h
    return normal


def transport_extrapolated(model: FrameModel, normal: Point, start: Fraction, end: Fraction, steps: int) -> Point:
    """Richardson combination of steps and 2 * steps for a fourth-order rule."""
    coarse = transport(model, normal, start, end, steps)
    fine = transport(model, normal, start, end, 2 * steps)
    return add(fine, scale(add(fine, scale(coarse, -1)), Decimal(1) / 15))


@dataclass
class FrameSpec:
    name: str
    purpose: str
    spans: list
    closed: bool
    seed: Point
    parameters: list  # (span index, u) in path order


LOOP_WAYPOINTS = [(3, 0, 0), (0, 3, 3), (-3, 0, 0), (0, -3, -3)]
LOOP_TANGENTS = [(0, 3, 3), (-3, 0, 0), (0, -3, 0), (3, 0, -3)]


def hermite_loop(points: list, tangents: list) -> list:
    """Closed C1 loop with unit-width Hermite spans: B1 = P0 + T0/3, B2 = P1 - T1/3."""
    spans = []
    for k in range(len(points)):
        p0, p1 = as_fraction_point(points[k]), as_fraction_point(points[(k + 1) % len(points)])
        t0, t1 = as_fraction_point(tangents[k]), as_fraction_point(tangents[(k + 1) % len(points)])
        spans.append([p0, add(p0, scale(t0, Fraction(1, 3))), add(p1, scale(t1, Fraction(-1, 3))), p1])
    return spans


QUARTERS = [Fraction(1, 4), Fraction(1, 2), Fraction(3, 4), Fraction(1)]

FRAME_SPECS = [
    FrameSpec(
        "twisted_rmf",
        "G3: the twisted cubic with seed +Y; transported normals at quarter parameters for double-reflection "
        "accuracy.",
        [[(0, 0, 0), (1, 0, 0), (1, 1, 0), (1, 1, 1)]],
        False,
        (0, 1, 0),
        [(0, u) for u in QUARTERS],
    ),
    FrameSpec(
        "inflection_rmf",
        "G3: the planar inflection cubic with seed +Z; the transported normal stays exactly +Z where a Frenet "
        "frame flips.",
        [[(0, 0, 0), (1, 1, 0), (2, -1, 0), (3, 0, 0)]],
        False,
        (0, 0, 1),
        [(0, u) for u in QUARTERS],
    ),
    FrameSpec(
        "loop_holonomy",
        "G3: asymmetric closed C1 Hermite saddle loop without mirror symmetry; transport around it returns with a nonzero "
        "residual twist.",
        hermite_loop(LOOP_WAYPOINTS, LOOP_TANGENTS),
        True,
        (0, 0, 1),
        [(k, Fraction(1, 2)) for k in range(len(LOOP_WAYPOINTS))] + [(len(LOOP_WAYPOINTS) - 1, Fraction(1))],
    ),
]


def compute_frame(spec: FrameSpec, steps: int = FRAME_STEPS) -> dict:
    spans = [[as_fraction_point(p) for p in span] for span in spec.spans]
    models = [FrameModel(span) for span in spans]
    start_tangent, _ = models[0].tangent(Decimal(0))
    seed = tuple(to_decimal(c) for c in spec.seed)
    normal = normalize_decimal(add(seed, scale(start_tangent, -dot(seed, start_tangent))))
    start_normal = normal
    references = []
    position = (0, Fraction(0))
    for span_index, u in spec.parameters:
        while position[0] < span_index:
            # Joints of these fixtures are exactly C1, so the frame carries over unchanged.
            normal = transport_extrapolated(models[position[0]], normal, position[1], Fraction(1), steps)
            position = (position[0] + 1, Fraction(0))
        normal = transport_extrapolated(models[span_index], normal, position[1], u, steps)
        position = (span_index, u)
        references.append((span_index, u, normal))
    residual = Decimal(0)
    if spec.closed:
        if position != (len(spans) - 1, Fraction(1)):
            raise AssertionError("closed frame fixtures must end at the seam")
        residual = decimal_atan2(dot(start_tangent, cross(start_normal, normal)), dot(start_normal, normal))
    return {"spec": spec, "spans": spans, "models": models, "references": references, "residual": residual,
            "start_normal": start_normal}


def compute_all() -> dict:
    patches = {spec.name: spec for spec in PATCH_SPECS}
    return {
        "cubics": [compute_cubic(spec) for spec in CUBIC_SPECS],
        "patches": [compute_patch(spec) for spec in PATCH_SPECS],
        "edges": [compute_shared_edge(spec, patches) for spec in SHARED_EDGE_SPECS],
        "catmull_rom": [compute_catmull_rom(spec) for spec in CATMULL_ROM_SPECS],
        "tracks": [compute_track(spec) for spec in TRACK_SPECS],
        "precision": [compute_precision(spec) for spec in PRECISION_SPECS],
        "frames": [compute_frame(spec) for spec in FRAME_SPECS],
    }


# --------------------------------------------------------------------------
# C++ emission
# --------------------------------------------------------------------------
PRELUDE = """\
#pragma once

// GENERATED by tests/fixtures/curves_surfaces/generate_fixtures.py. Do not edit.
// Regenerate with: python3 tests/fixtures/curves_surfaces/generate_fixtures.py --write
//
// G0 representative curves and surfaces reference corpus (ADR 0024). Controls
// are exact float32 values. Every reference is computed from exact rational or
// 50-digit decimal arithmetic without Ludus code and rounded to float64 once,
// so it is an independent oracle, not a copy of a production result. Fields a
// fixture does not exercise stay at their neutral zero/empty value.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::geometry_fixtures
{
using foundation::BorrowedView;
using foundation::core::float32;
using foundation::core::float64;
using foundation::core::StaticArray;
using foundation::core::uint8;
using foundation::core::usize;

/// Controls in one cubic Bezier span.
inline constexpr usize CUBIC_CONTROL_COUNT = 4;
/// Controls along each bicubic patch direction.
inline constexpr usize PATCH_CONTROL_COUNT = 4;
/// Catmull-Rom waypoints that define one interior span.
inline constexpr usize CATMULL_ROM_WAYPOINT_COUNT = 4;
/// Largest number of local minima recorded by one projection reference.
inline constexpr usize MAX_PROJECTION_MINIMA = 3;

/// Exact float32 control coordinates.
struct ControlPoint final
{
    float32 X = 0;
    float32 Y = 0;
    float32 Z = 0;
};

/// Reference coordinates rounded once to float64 from an exact or 50-digit value.
struct ReferencePoint final
{
    float64 X = 0;
    float64 Y = 0;
    float64 Z = 0;
};

/// Expected outcome in the frozen ADR 0024 status vocabulary.
enum class ExpectedStatus : uint8
{
    Success,
    InvalidDomain,
    Degenerate,
    IllConditioned,
};

/// Frozen ADR 0024 domain boundary policy; it applies only outside the domain.
enum class BoundaryPolicy : uint8
{
    Reject,
    Clamp,
    Hold,
    Loop,
    PingPong,
};

/// Which span an exact interior key selects; Outgoing is the default.
enum class SpanSide : uint8
{
    Outgoing,
    Incoming,
};

/// Interpolation of the span that starts at a key.
enum class TrackInterpolation : uint8
{
    Step,
    Linear,
    Hermite,
};

/// Exact cubic sample; derivatives are per normalized u.
struct CubicSampleReference final
{
    float64 U = 0;
    ReferencePoint Position;
    ReferencePoint FirstDerivative;
    ReferencePoint SecondDerivative;
};

/// Earliest parameter whose prefix arc length equals Distance (asset units).
struct DistanceReference final
{
    float64 Distance = 0;
    float64 U = 0;
};

/// Global Euclidean closest point over the closed span domain, endpoints included.
struct ProjectionReference final
{
    ReferencePoint Query;
    float64 Distance = 0;     ///< Global minimum distance.
    float64 U = 0;            ///< Tie-broken answer: the smallest minimizing parameter.
    usize TieCount = 0;       ///< Number of distinct global minimizers.
    usize MinimumCount = 0;   ///< Number of local minima stored in MinimumU.
    StaticArray<float64, MAX_PROJECTION_MINIMA> MinimumU{}; ///< Ascending local minima.
};

/// One cubic span with its exact samples and G2/G3 references.
struct CubicFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    StaticArray<ControlPoint, CUBIC_CONTROL_COUNT> Control{};
    BorrowedView<const CubicSampleReference> Samples;
    float64 ArcLength = 0;                   ///< Total arc length in asset units.
    BorrowedView<const float64> ZeroSpeedU;  ///< Every parameter in [0,1] with zero speed.
    BorrowedView<const DistanceReference> Distances;
    BorrowedView<const ProjectionReference> Projections;
    float64 PositionFloor = 0;               ///< ADR 0024 floor: 2 float32 ulps at the control extent.
};

/// Exact patch sample; partials are per normalized U,V.
struct PatchSampleReference final
{
    float64 U = 0;
    float64 V = 0;
    ReferencePoint Position;
    ReferencePoint DU;
    ReferencePoint DV;
    ReferencePoint DUU;
    ReferencePoint DUV;
    ReferencePoint DVV;
};

/// Geometric normal classification under a named conditioning policy.
struct NormalReference final
{
    float64 U = 0;
    float64 V = 0;
    float64 MinDerivativeLength = 0;
    float64 MinSinAngle = 0;
    ExpectedStatus Status = ExpectedStatus::Success;
    ReferencePoint Normal; ///< Unit normal when Status is Success.
};

/// Position-error references for the P00-P33 diagonal triangle pair.
struct TessellationReference final
{
    ReferencePoint Twist;          ///< P00 - P30 - P03 + P33.
    float64 TwistBound = 0;        ///< Length(Twist) / 4.
    float64 ControlNetBound = 0;   ///< Max control distance to the degree-elevated bilinear net.
    float64 InteriorBound = 0;     ///< ControlNetBound + TwistBound.
    bool HasTrueMaxDeviation = false;
    float64 TrueMaxDeviation = 0;  ///< Analytic maximum patch-to-triangle distance.
    float64 TrueMaxU = 0;
    float64 TrueMaxV = 0;
};

/// One bicubic patch, Control[u index][v index].
struct PatchFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    StaticArray<StaticArray<ControlPoint, PATCH_CONTROL_COUNT>, PATCH_CONTROL_COUNT> Control{};
    BorrowedView<const PatchSampleReference> Samples;
    BorrowedView<const NormalReference> Normals;
    TessellationReference Tessellation;
};

/// Matched position on a shared boundary, in each patch's own V.
struct EdgeSampleReference final
{
    float64 FirstV = 0;
    float64 SecondV = 0;
    ReferencePoint Position;
};

/// Two PATCH_FIXTURES entries that share one whole boundary.
struct SharedEdgeFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    usize FirstPatch = 0;
    usize SecondPatch = 0;
    float64 FirstU = 0;
    float64 SecondU = 0;
    bool IsReversed = false;
    BorrowedView<const EdgeSampleReference> Samples;
};

/// Centripetal Catmull-Rom waypoints and the converted middle span.
struct CatmullRomFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    StaticArray<ControlPoint, CATMULL_ROM_WAYPOINT_COUNT> Waypoint{};
    ExpectedStatus Status = ExpectedStatus::Success;
    StaticArray<float64, CATMULL_ROM_WAYPOINT_COUNT> Knot{};
    ReferencePoint StartTangent;   ///< Per knot unit.
    ReferencePoint EndTangent;     ///< Per knot unit.
    float64 SpanWidth = 0;         ///< Knot width of the middle span.
    StaticArray<ReferencePoint, CUBIC_CONTROL_COUNT> SpanControl{};
};

/// Scalar track key; slopes are value units per second.
struct TrackKey final
{
    float64 Time = 0;
    float32 Value = 0;
    float32 InSlope = 0;
    float32 OutSlope = 0;
    TrackInterpolation Interpolation = TrackInterpolation::Linear;
};

/// Track evaluation in seconds under a policy and span side.
struct TrackSampleReference final
{
    float64 Time = 0;
    BoundaryPolicy Policy = BoundaryPolicy::Reject;
    SpanSide Side = SpanSide::Outgoing;
    ExpectedStatus Status = ExpectedStatus::Success;
    float64 Value = 0;
    float64 DerivativePerSecond = 0;
};

/// Scalar track keys and references.
struct TrackFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    BorrowedView<const TrackKey> Keys;
    ExpectedStatus Status = ExpectedStatus::Success;
    BorrowedView<const TrackSampleReference> Samples;
    bool HasOvershoot = false;
    float64 OvershootTime = 0;
    float64 OvershootValue = 0;
};

/// Large-coordinate source controls and their chunk-relative float32 controls.
struct PrecisionFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    ReferencePoint Origin;                                     ///< Exact float64 chunk origin.
    StaticArray<ReferencePoint, CUBIC_CONTROL_COUNT> Source{}; ///< Exact float64 source controls.
    StaticArray<ControlPoint, CUBIC_CONTROL_COUNT> Relative{};
    float64 MaxAbsComponent = 0;
    float64 RelativePositionFloor = 0;
    float64 AbsolutePositionFloor = 0;
    float64 NaiveNarrowingError = 0; ///< Largest error of float32(source) without the origin.
};

/// Rotation-minimizing (Bishop) normal at one path parameter, before any closed-loop compensation.
struct FrameReference final
{
    usize SpanIndex = 0;
    float64 U = 0;
    ReferencePoint Normal; ///< Unit normal transported from the seed; integrated, not closed form.
};

/// One cubic span's controls in a frame fixture.
using SpanControls = StaticArray<ControlPoint, CUBIC_CONTROL_COUNT>;

/// Unit-width cubic path whose joints are exactly C1, with rotation-minimizing frame references.
struct FrameFixture final
{
    const char* Name = "";
    const char* Purpose = "";
    BorrowedView<const SpanControls> Spans;
    bool IsClosed = false;
    ReferencePoint SeedUp; ///< Projected perpendicular to the first tangent to start the transport.
    BorrowedView<const FrameReference> Frames;
    float64 ResidualTwist = 0; ///< Closed paths: signed angle from the start normal to the returned one, radians.
};
"""


class Emitter:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def line(self, text: str = "") -> None:
        self.lines.append(text)

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


def control(point) -> str:
    return "{" + ", ".join(float32_literal(c) for c in point) + "}"


def reference(point) -> str:
    return "{" + ", ".join(float64_literal(c) for c in point) + "}"


def view(namespace: str, name: str, count: int) -> str:
    if count == 0:
        return "{}"
    return f"{{{namespace}::{name}.GetData(), {namespace}::{name}.GetSize()}}"


def emit_array(out: Emitter, type_name: str, count_name: str, name: str, items: list[str]) -> None:
    out.line(f"inline constexpr usize {count_name} = {len(items)};")
    if not items:
        return
    out.line(f"inline constexpr StaticArray<{type_name}, {count_name}> {name}{{{{")
    for item in items:
        out.line(f"    {item},")
    out.line("}};")


def quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_cubics(out: Emitter, cubics: list[dict]) -> None:
    for item in cubics:
        spec = item["spec"]
        out.line(f"namespace {spec.name}")
        out.line("{")
        samples = [
            f"{{.U = {float64_literal(u)}, .Position = {reference(p)}, .FirstDerivative = {reference(d1)}, "
            f".SecondDerivative = {reference(d2)}}}"
            for u, p, d1, d2 in item["samples"]
        ]
        emit_array(out, "CubicSampleReference", "SAMPLE_COUNT", "SAMPLES", samples)
        emit_array(out, "float64", "ZERO_SPEED_COUNT", "ZERO_SPEED_U", [float64_literal(u) for u in item["zero_speed"]])
        distances = [f"{{.Distance = {float64_literal(s)}, .U = {float64_literal(u)}}}" for s, u in item["distances"]]
        emit_array(out, "DistanceReference", "DISTANCE_COUNT", "DISTANCES", distances)
        projections = []
        for projection in item["projections"]:
            if len(projection.minima) > MAX_PROJECTION_MINIMA:
                raise AssertionError(f"{spec.name} projection has more than {MAX_PROJECTION_MINIMA} minima")
            minima = ", ".join(float64_literal(u) for u in projection.minima)
            projections.append(
                f"{{.Query = {reference(projection.query)}, .Distance = {float64_literal(projection.distance)}, "
                f".U = {float64_literal(projection.u)}, .TieCount = {projection.tie_count}, "
                f".MinimumCount = {len(projection.minima)}, .MinimumU = {{{{{minima}}}}}}}"
            )
        emit_array(out, "ProjectionReference", "PROJECTION_COUNT", "PROJECTIONS", projections)
        out.line(f"}} // namespace {spec.name}")
        out.line()

    out.line("/// Representative cubic spans.")
    out.line(f"inline constexpr usize CUBIC_FIXTURE_COUNT = {len(cubics)};")
    out.line("inline constexpr StaticArray<CubicFixture, CUBIC_FIXTURE_COUNT> CUBIC_FIXTURES{{")
    for item in cubics:
        spec = item["spec"]
        n = spec.name
        controls = ", ".join(control(p) for p in item["controls"])
        out.line("    {")
        out.line(f"        .Name = {quote(n)},")
        out.line(f"        .Purpose = {quote(spec.purpose)},")
        out.line(f"        .Control = {{{{{controls}}}}},")
        out.line(f"        .Samples = {view(n, 'SAMPLES', len(item['samples']))},")
        out.line(f"        .ArcLength = {float64_literal(item['length'])},")
        out.line(f"        .ZeroSpeedU = {view(n, 'ZERO_SPEED_U', len(item['zero_speed']))},")
        out.line(f"        .Distances = {view(n, 'DISTANCES', len(item['distances']))},")
        out.line(f"        .Projections = {view(n, 'PROJECTIONS', len(item['projections']))},")
        out.line(f"        .PositionFloor = {float64_literal(item['floor'])},")
        out.line("    },")
    out.line("}};")
    out.line()


def emit_patches(out: Emitter, patches: list[dict]) -> None:
    for item in patches:
        spec = item["spec"]
        out.line(f"namespace {spec.name}")
        out.line("{")
        samples = []
        for u, v, values in item["samples"]:
            fields = ", ".join(f".{name} = {reference(values[name])}" for name in PATCH_ORDERS)
            samples.append(f"{{.U = {float64_literal(u)}, .V = {float64_literal(v)}, {fields}}}")
        emit_array(out, "PatchSampleReference", "SAMPLE_COUNT", "SAMPLES", samples)
        normals = [
            f"{{.U = {float64_literal(u)}, .V = {float64_literal(v)}, .MinDerivativeLength = {float64_literal(length_)}, "
            f".MinSinAngle = {float64_literal(sin_)}, .Status = ExpectedStatus::{status}, .Normal = {reference(normal)}}}"
            for u, v, length_, sin_, status, normal in item["normals"]
        ]
        emit_array(out, "NormalReference", "NORMAL_COUNT", "NORMALS", normals)
        out.line(f"}} // namespace {spec.name}")
        out.line()

    out.line("/// Representative bicubic patches; SHARED_EDGE_FIXTURES index this array.")
    out.line(f"inline constexpr usize PATCH_FIXTURE_COUNT = {len(patches)};")
    out.line("inline constexpr StaticArray<PatchFixture, PATCH_FIXTURE_COUNT> PATCH_FIXTURES{{")
    for item in patches:
        spec = item["spec"]
        n = spec.name
        out.line("    {")
        out.line(f"        .Name = {quote(n)},")
        out.line(f"        .Purpose = {quote(spec.purpose)},")
        out.line("        .Control = {{")
        for row in spec.controls:
            out.line(f"            {{{{{', '.join(control(p) for p in row)}}}}},")
        out.line("        }},")
        out.line(f"        .Samples = {view(n, 'SAMPLES', len(item['samples']))},")
        out.line(f"        .Normals = {view(n, 'NORMALS', len(item['normals']))},")
        true_max = item["true_max"]
        fields = [
            f".Twist = {reference(item['twist'])}",
            f".TwistBound = {float64_literal(item['twist_bound'])}",
            f".ControlNetBound = {float64_literal(item['net'])}",
            f".InteriorBound = {float64_literal(item['interior'])}",
        ]
        has_true_max = true_max is not None
        true_max = true_max or (0, 0, 0)
        fields += [
            f".HasTrueMaxDeviation = {'true' if has_true_max else 'false'}",
            f".TrueMaxDeviation = {float64_literal(true_max[2])}",
            f".TrueMaxU = {float64_literal(true_max[0])}",
            f".TrueMaxV = {float64_literal(true_max[1])}",
        ]
        out.line(f"        .Tessellation = {{{', '.join(fields)}}},")
        out.line("    },")
    out.line("}};")
    out.line()


def emit_edges(out: Emitter, edges: list[dict], patches: list[dict]) -> None:
    index = {item["spec"].name: i for i, item in enumerate(patches)}
    for item in edges:
        spec = item["spec"]
        out.line(f"namespace {spec['name']}")
        out.line("{")
        samples = [
            f"{{.FirstV = {float64_literal(a)}, .SecondV = {float64_literal(b)}, .Position = {reference(p)}}}"
            for a, b, p in item["samples"]
        ]
        emit_array(out, "EdgeSampleReference", "SAMPLE_COUNT", "SAMPLES", samples)
        out.line(f"}} // namespace {spec['name']}")
        out.line()
    out.line("/// Shared whole-boundary patch pairs.")
    out.line(f"inline constexpr usize SHARED_EDGE_FIXTURE_COUNT = {len(edges)};")
    out.line("inline constexpr StaticArray<SharedEdgeFixture, SHARED_EDGE_FIXTURE_COUNT> SHARED_EDGE_FIXTURES{{")
    for item in edges:
        spec = item["spec"]
        out.line("    {")
        out.line(f"        .Name = {quote(spec['name'])},")
        out.line(f"        .Purpose = {quote(spec['purpose'])},")
        out.line(f"        .FirstPatch = {index[spec['first']]},")
        out.line(f"        .SecondPatch = {index[spec['second']]},")
        out.line(f"        .FirstU = {float64_literal(spec['first_u'])},")
        out.line(f"        .SecondU = {float64_literal(spec['second_u'])},")
        out.line(f"        .IsReversed = {'true' if spec['reversed'] else 'false'},")
        out.line(f"        .Samples = {view(spec['name'], 'SAMPLES', len(item['samples']))},")
        out.line("    },")
    out.line("}};")
    out.line()


def emit_catmull_rom(out: Emitter, items: list[dict]) -> None:
    out.line("/// Centripetal Catmull-Rom waypoint sets.")
    out.line(f"inline constexpr usize CATMULL_ROM_FIXTURE_COUNT = {len(items)};")
    out.line("inline constexpr StaticArray<CatmullRomFixture, CATMULL_ROM_FIXTURE_COUNT> CATMULL_ROM_FIXTURES{{")
    for item in items:
        spec = item["spec"]
        out.line("    {")
        out.line(f"        .Name = {quote(spec['name'])},")
        out.line(f"        .Purpose = {quote(spec['purpose'])},")
        out.line(f"        .Waypoint = {{{{{', '.join(control(p) for p in item['points'])}}}}},")
        out.line(f"        .Status = ExpectedStatus::{item['status']},")
        zero_point = (0, 0, 0)
        knots = item.get("knots", [0] * 4)
        controls = item.get("controls", [zero_point] * 4)
        out.line(f"        .Knot = {{{{{', '.join(float64_literal(k) for k in knots)}}}}},")
        out.line(f"        .StartTangent = {reference(item.get('start', zero_point))},")
        out.line(f"        .EndTangent = {reference(item.get('end', zero_point))},")
        out.line(f"        .SpanWidth = {float64_literal(item.get('width', 0))},")
        out.line(f"        .SpanControl = {{{{{', '.join(reference(p) for p in controls)}}}}},")
        out.line("    },")
    out.line("}};")
    out.line()


def emit_tracks(out: Emitter, tracks: list[dict]) -> None:
    for item in tracks:
        spec = item["spec"]
        out.line(f"namespace {spec['name']}")
        out.line("{")
        keys = [
            f"{{.Time = {float64_literal(t)}, .Value = {float32_literal(v)}, .InSlope = {float32_literal(i)}, "
            f".OutSlope = {float32_literal(o)}, .Interpolation = TrackInterpolation::{mode}}}"
            for t, v, i, o, mode in spec["keys"]
        ]
        emit_array(out, "TrackKey", "KEY_COUNT", "KEYS", keys)
        samples = [
            f"{{.Time = {float64_literal(t)}, .Policy = BoundaryPolicy::{policy}, .Side = SpanSide::{side}, "
            f".Status = ExpectedStatus::{status}, .Value = {float64_literal(value)}, "
            f".DerivativePerSecond = {float64_literal(derivative)}}}"
            for t, policy, side, status, value, derivative in item["samples"]
        ]
        emit_array(out, "TrackSampleReference", "SAMPLE_COUNT", "SAMPLES", samples)
        out.line(f"}} // namespace {spec['name']}")
        out.line()
    out.line("/// Scalar tracks in seconds.")
    out.line(f"inline constexpr usize TRACK_FIXTURE_COUNT = {len(tracks)};")
    out.line("inline constexpr StaticArray<TrackFixture, TRACK_FIXTURE_COUNT> TRACK_FIXTURES{{")
    for item in tracks:
        spec = item["spec"]
        n = spec["name"]
        out.line("    {")
        out.line(f"        .Name = {quote(n)},")
        out.line(f"        .Purpose = {quote(spec['purpose'])},")
        out.line(f"        .Keys = {view(n, 'KEYS', len(spec['keys']))},")
        out.line(f"        .Status = ExpectedStatus::{spec['status']},")
        out.line(f"        .Samples = {view(n, 'SAMPLES', len(item['samples']))},")
        time, value = item["overshoot"] or (0, 0)
        out.line(f"        .HasOvershoot = {'true' if item['overshoot'] is not None else 'false'},")
        out.line(f"        .OvershootTime = {float64_literal(time)},")
        out.line(f"        .OvershootValue = {float64_literal(value)},")
        out.line("    },")
    out.line("}};")
    out.line()


def emit_precision(out: Emitter, items: list[dict]) -> None:
    out.line("/// Large-coordinate narrowing fixtures.")
    out.line(f"inline constexpr usize PRECISION_FIXTURE_COUNT = {len(items)};")
    out.line("inline constexpr StaticArray<PrecisionFixture, PRECISION_FIXTURE_COUNT> PRECISION_FIXTURES{{")
    for item in items:
        spec = item["spec"]
        out.line("    {")
        out.line(f"        .Name = {quote(spec['name'])},")
        out.line(f"        .Purpose = {quote(spec['purpose'])},")
        out.line(f"        .Origin = {reference(spec['origin'])},")
        out.line(f"        .Source = {{{{{', '.join(reference(p) for p in item['source'])}}}}},")
        out.line(f"        .Relative = {{{{{', '.join(control(p) for p in item['relative'])}}}}},")
        out.line(f"        .MaxAbsComponent = {float64_literal(spec['max_abs_component'])},")
        out.line(f"        .RelativePositionFloor = {float64_literal(item['relative_floor'])},")
        out.line(f"        .AbsolutePositionFloor = {float64_literal(item['absolute_floor'])},")
        out.line(f"        .NaiveNarrowingError = {float64_literal(item['naive_error'])},")
        out.line("    },")
    out.line("}};")


def emit_frames(out: Emitter, items: list[dict]) -> None:
    for item in items:
        spec = item["spec"]
        out.line(f"namespace {spec.name}")
        out.line("{")
        spans = ["{{" + ", ".join(control(p) for p in span) + "}}" for span in item["spans"]]
        emit_array(out, "SpanControls", "SPAN_COUNT", "SPANS", spans)
        frames = [
            f"{{.SpanIndex = {index}, .U = {float64_literal(u)}, .Normal = {reference(normal)}}}"
            for index, u, normal in item["references"]
        ]
        emit_array(out, "FrameReference", "FRAME_COUNT", "FRAMES", frames)
        out.line(f"}} // namespace {spec.name}")
        out.line()
    out.line("/// Rotation-minimizing frame and closed-loop holonomy fixtures.")
    out.line(f"inline constexpr usize FRAME_FIXTURE_COUNT = {len(items)};")
    out.line("inline constexpr StaticArray<FrameFixture, FRAME_FIXTURE_COUNT> FRAME_FIXTURES{{")
    for item in items:
        spec = item["spec"]
        n = spec.name
        out.line("    {")
        out.line(f"        .Name = {quote(n)},")
        out.line(f"        .Purpose = {quote(spec.purpose)},")
        out.line(f"        .Spans = {view(n, 'SPANS', len(item['spans']))},")
        out.line(f"        .IsClosed = {'true' if spec.closed else 'false'},")
        out.line(f"        .SeedUp = {reference(as_fraction_point(spec.seed))},")
        out.line(f"        .Frames = {view(n, 'FRAMES', len(item['references']))},")
        out.line(f"        .ResidualTwist = {float64_literal(item['residual'])},")
        out.line("    },")
    out.line("}};")


def render(results: dict) -> str:
    out = Emitter()
    out.lines.extend(PRELUDE.rstrip("\n").split("\n"))
    out.line()
    emit_cubics(out, results["cubics"])
    emit_patches(out, results["patches"])
    emit_edges(out, results["edges"], results["patches"])
    emit_catmull_rom(out, results["catmull_rom"])
    emit_tracks(out, results["tracks"])
    emit_precision(out, results["precision"])
    out.line()
    emit_frames(out, results["frames"])
    out.line("} // namespace ludus::geometry_fixtures")
    return out.text()


def normalize(source: str) -> str:
    """Drop formatting-only differences: whitespace, literal splits and trailing commas.

    The committed header is formatted by scripts/check, so comparisons ignore
    whitespace outside string literals, join adjacent string literals as the
    compiler does, and drop a comma directly before a brace.
    """
    result = []
    in_string = False
    escaped = False
    index = 0
    while index < len(source):
        character = source[index]
        index += 1
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                following = index
                while following < len(source) and source[following].isspace():
                    following += 1
                if following < len(source) and source[following] == '"':
                    index = following + 1
                    continue
                in_string = False
            result.append(character)
            continue
        if character == '"':
            in_string = True
            result.append(character)
        elif not character.isspace():
            result.append(character)
    text = "".join(result)
    previous = None
    while previous != text:
        previous = text
        text = text.replace(",}", "}")
    return text


def generate() -> str:
    return render(compute_all())


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="rewrite reference_fixtures.hpp")
    mode.add_argument("--check", action="store_true", help="fail when reference_fixtures.hpp is stale")
    arguments = parser.parse_args(argv)
    text = generate()
    if arguments.write:
        HEADER.write_text(text, encoding="utf-8")
        return 0
    committed = HEADER.read_text(encoding="utf-8") if HEADER.is_file() else ""
    if normalize(committed) != normalize(text):
        print(f"{HEADER} is stale; run generate_fixtures.py --write and scripts/check --format --fix",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
