#!/usr/bin/env python3
"""Generate RT1's GGX directional-albedo table for the multiple-scattering material profile.

The table tabulates the directional albedo E(mu, alpha) of RT1's single-scattering GGX lobe at unit Fresnel:
isotropic GGX, height-correlated Smith visibility 0.5 / max(n.l Lv + n.v Ll, 1e-6), exactly as
modules/graphics/renderer/src/reference_bsdf.cpp evaluates it. Rows are linear in sqrt(alpha) over [0, 1]; row 0
(alpha = 0) is a mirror with unit albedo. Columns are uniform in x = 1 - sqrt(1 - 2 theta_s / pi), where
theta_s = atan2(alpha sin theta, cos theta) is the inclination of the stretched view, which concentrates columns
near grazing. After the grid come one average deficit per row, 2 * integral (1 - E(mu)) mu dmu of the bilinear
interpolant that the renderer looks up, so the compensation lobe's normalization matches its numerator. Storing the
deficit rather than the average albedo keeps the small deficits of smooth rows precise in float32.

Each entry integrates over Eric Heitz's visible-normal sample space with a midpoint grid; below-horizon reflections
count as zero. Thanks to Eric Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018, sections
3-4 and Appendix B, https://jcgt.org/published/0007/04/01/, and to Christopher Kulla and Alejandro Conty,
"Revisiting Physically Based Shading at Imageworks", SIGGRAPH 2017, slides 9-14, for the tabulated compensation.
The grazing-resolved parameterization follows the experiment recorded in docs/development/ltc-table.md.

Writes little-endian float32 values. See docs/architecture/renderer-systems.md#multiple-scattering-ggx-profile.
"""

import argparse
import json
import math
import multiprocessing
import struct
from pathlib import Path

TABLE_VERSION = 1
ROUGHNESS_COUNT = 64
VIEW_COUNT = 64
SMITH_DENOMINATOR_FLOOR = 1e-6
DEFAULT_STRATA = 256
AVERAGE_SUBDIVISIONS = 4096


def view_cosine(alpha, column):
    """Return cos(theta) of the view addressed by a column at alpha > 0."""
    x = column / (VIEW_COUNT - 1)
    stretched = 0.5 * math.pi * (1 - (1 - x) * (1 - x))
    if stretched >= 0.5 * math.pi:
        return 0.0
    return math.cos(math.atan(math.tan(stretched) / alpha))


def column_coordinate(alpha, cosine):
    """Return the fractional column of a view cosine at alpha, the lookup's inverse of view_cosine."""
    sine = math.sqrt(max(0.0, 1 - cosine * cosine))
    stretched = math.atan2(alpha * sine, cosine)
    x = 1 - math.sqrt(max(0.0, 1 - stretched / (0.5 * math.pi)))
    return x * (VIEW_COUNT - 1)


def directional_albedo(alpha, cosine, strata):
    """Integrate the RT1 lobe at unit Fresnel from a view at cosine, by visible-normal midpoint samples."""
    view = (math.sqrt(max(0.0, 1 - cosine * cosine)), 0.0, cosine)
    alpha_squared = alpha * alpha
    lambda_view = math.sqrt(cosine * cosine * (1 - alpha_squared) + alpha_squared)
    stretched = (alpha * view[0], 0.0, view[2])
    length = math.sqrt(stretched[0] * stretched[0] + stretched[2] * stretched[2])
    stretched = (stretched[0] / length, 0.0, stretched[2] / length)
    tangent_length = stretched[0] * stretched[0]
    tangent = (0.0, 1.0, 0.0) if tangent_length > 0 else (1.0, 0.0, 0.0)
    bitangent = (
        stretched[1] * tangent[2] - stretched[2] * tangent[1],
        stretched[2] * tangent[0] - stretched[0] * tangent[2],
        stretched[0] * tangent[1] - stretched[1] * tangent[0],
    )
    blend = 0.5 * (1 + stretched[2])
    total = 0.0
    for i in range(strata):
        radius = math.sqrt((i + 0.5) / strata)
        for j in range(strata):
            angle = 2 * math.pi * (j + 0.5) / strata
            disk_x = radius * math.cos(angle)
            disk_y = radius * math.sin(angle)
            warped_y = (1 - blend) * math.sqrt(max(0.0, 1 - disk_x * disk_x)) + blend * disk_y
            height = math.sqrt(max(0.0, 1 - disk_x * disk_x - warped_y * warped_y))
            hemisphere = (
                tangent[0] * disk_x + bitangent[0] * warped_y + stretched[0] * height,
                tangent[1] * disk_x + bitangent[1] * warped_y + stretched[1] * height,
                tangent[2] * disk_x + bitangent[2] * warped_y + stretched[2] * height,
            )
            half = (alpha * hemisphere[0], alpha * hemisphere[1], max(0.0, hemisphere[2]))
            half_length = math.sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2])
            if half_length == 0:
                continue
            half = (half[0] / half_length, half[1] / half_length, half[2] / half_length)
            view_half = view[0] * half[0] + view[2] * half[2]
            light_cosine = 2 * view_half * half[2] - view[2]
            if light_cosine <= 0:
                continue
            lambda_light = math.sqrt(light_cosine * light_cosine * (1 - alpha_squared) + alpha_squared)
            denominator = max(light_cosine * lambda_view + cosine * lambda_light, SMITH_DENOMINATOR_FLOOR)
            total += light_cosine * (cosine + lambda_view) / denominator
    return total / (strata * strata)


def projected_sine_squared(alpha, x):
    """Return sin^2(theta) of the view at stretched-view coordinate x in [0, 1], stably for tiny alpha."""
    stretched = 0.5 * math.pi * (1 - (1 - x) * (1 - x))
    if stretched >= 0.5 * math.pi:
        return 1.0
    tangent = math.tan(stretched)
    return tangent * tangent / (alpha * alpha + tangent * tangent)


def average_deficit(alpha, row_values, subdivisions):
    """Integrate 2 (1 - E(mu)) mu dmu = (1 - E) d(sin^2 theta) for one row's interpolant at its own alpha.

    A Stieltjes sum over exact sin^2(theta) increments: at small alpha nearly every view inclination lies in the first
    column cell, where quadrature of the Jacobian d theta / d x would fail.
    """
    if alpha == 0:
        return 0.0
    total = 0.0
    for column in range(VIEW_COUNT - 1):
        previous = projected_sine_squared(alpha, column / (VIEW_COUNT - 1))
        for step in range(subdivisions):
            fraction = (step + 1) / subdivisions
            current = projected_sine_squared(alpha, (column + fraction) / (VIEW_COUNT - 1))
            middle = (step + 0.5) / subdivisions
            value = row_values[column] * (1 - middle) + row_values[column + 1] * middle
            total += (1 - min(1.0, max(0.0, value))) * (current - previous)
            previous = current
    return total


def generate_row(arguments):
    """Return one row's albedos and their average deficit."""
    row, strata = arguments
    root = row / (ROUGHNESS_COUNT - 1)
    alpha = root * root
    if alpha == 0:
        return [1.0] * VIEW_COUNT, 0.0
    values = [directional_albedo(alpha, view_cosine(alpha, column), strata) for column in range(VIEW_COUNT)]
    return values, average_deficit(alpha, values, AVERAGE_SUBDIVISIONS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, help="table binary to write")
    parser.add_argument("--provenance", required=True, help="provenance JSON to write")
    parser.add_argument("--strata", type=int, default=DEFAULT_STRATA, help="midpoint strata per sample axis")
    parser.add_argument("--workers", type=int, default=0, help="worker processes; 0 uses every core")
    options = parser.parse_args()
    workers = options.workers if options.workers > 0 else multiprocessing.cpu_count()
    with multiprocessing.Pool(workers) as pool:
        rows = pool.map(generate_row, [(row, options.strata) for row in range(ROUGHNESS_COUNT)])
    albedos = [value for values, _ in rows for value in values]
    deficits = [deficit for _, deficit in rows]
    data = struct.pack("<" + "f" * (len(albedos) + len(deficits)), *albedos, *deficits)
    Path(options.output).write_bytes(data)
    provenance = {
        "generator": "tools/ggx-albedo/generate_ggx_albedo_table.py",
        "tableVersion": TABLE_VERSION,
        "brdf": "RT1 single-scattering GGX at unit Fresnel: height-correlated Smith visibility with a 1e-6 "
        "denominator floor",
        "rows": "linear in sqrt(alpha) over [0, 1]; row 0 is a unit-albedo mirror",
        "columns": "uniform in x = 1 - sqrt(1 - 2 theta_s / pi), theta_s = atan2(alpha sin theta, cos theta)",
        "averageDeficit": "2 * integral of (1 - E(mu)) mu dmu for the clamped bilinear interpolant E, a Stieltjes "
        "sum over exact sin^2(theta) increments of each column cell",
        "settings": {
            "roughnessCount": ROUGHNESS_COUNT,
            "viewCount": VIEW_COUNT,
            "strata": options.strata,
            "averageSubdivisions": AVERAGE_SUBDIVISIONS,
        },
        "fullyRoughNormalIncidence": rows[ROUGHNESS_COUNT - 1][0][0],
    }
    Path(options.provenance).write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
