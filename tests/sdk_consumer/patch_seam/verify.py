"""Verify SDK or World viewport readbacks against indexed triangles and direct surface hits."""
import math
from pathlib import Path
import subprocess
import sys

from surface_oracle import (CAMERAS, EXTENTS, NORMAL_EPSILON, SHAPES, TOLERANCES, Projection,
                            geometry, projected_deviation, read_mesh, require, unit)


def raster(shape, records, path, view):
    # Scan independent vertical slices of the indexed triangles. Adjacent faces share
    # endpoint samples. Use a half-open rule for the horizontal pixel centre.
    extent = view.extent
    raw = path.read_bytes()
    require(len(raw) == extent*extent*4, 'truncated frame')
    # Display the captured normal channel at native resolution. The adjacent
    # RGBA8 readback retains the depth bytes and coverage mask.
    image = bytes(red if 126 <= alpha <= 193 else 0 for red, alpha in zip(raw[::4], raw[3::4]))
    path.with_suffix('.pgm').write_bytes(f'P5\n{extent} {extent}\n255\n'.encode('ascii') + image)
    vertices = records['v']
    projected = [view.project(v[4:7]) for v in vertices]
    normals = [unit(tuple(n/s for n, s in zip(v[7:], view.scale))) for v in vertices]
    faces = [tuple(map(int, f)) for f in records['f']]
    depth_error = 0.
    analytic_depth = 0.
    interior_hits = 0
    analytic_probes = 0
    lit_halves = set()
    center_top = {}
    for column in range(extent//4, 3*extent//4+1, extent//16):
        x = column+.5
        spans = []
        for face in faces:
            crossings = []
            for i, j in zip(face, (*face[1:], face[0])):
                a, b = projected[i], projected[j]
                if min(a[0], b[0]) <= x < max(a[0], b[0]):
                    t = (x-a[0])/(b[0]-a[0])
                    crossings.append(((1-t)*a[1]+t*b[1], (1-t)*a[2]+t*b[2],
                                      *( (1-t)*na/a[3]+t*nb/b[3] for na, nb in zip(normals[i], normals[j]))))
            if len(crossings) == 2:
                low, high = sorted(crossings)
                spans.append((low, high, vertices[face[0]][0]))
        covered = []
        for row in range(extent):
            y = row+.5
            candidates = []
            for low, high, p in spans:
                if low[0] <= y < high[0]:
                    t = (y-low[0])/(high[0]-low[0])
                    z = low[1]+t*(high[1]-low[1])
                    # Normalization cancels the common perspective denominator.
                    normal = unit(tuple(a+t*(b-a) for a, b in zip(low[2:], high[2:])))
                    candidates.append((z, normal[0], p))
            offset = (row*extent+column)*4
            red, green, blue, alpha = raw[offset:offset+4]
            hit = 126 <= alpha <= 193
            if hit:
                covered.append(row)
            if hit != bool(candidates):
                # Allow fixed-function vertex snapping at the external outline only.
                require(spans and min(abs(y-min(lo[0] for lo, _, _ in spans)),
                                      abs(y-max(hi[0] for _, hi, _ in spans))) < .04,
                        f'coverage gap at {column},{row}, expected {bool(candidates)}')
            if shape in ('smooth', 'mixed') and column == extent//2 and not hit:
                # A one-pixel silhouette allowance applies only at an outline.
                # A hole surrounded by analytic hits must fail even if the
                # emitted index buffer and the raster both lost the same face.
                inside = all(view.intersect(shape, x, y+offset) is not None for offset in (-1, 0, 1))
                require(not inside, f'missing analytic interior hit at {column},{row}')
            if not hit or not candidates:
                continue
            z, nx, p = min(candidates)
            # Edge-on silhouette triangles amplify subpixel vertex snapping. Other
            # depth comparisons retain the full precision of the two output bytes.
            actual_depth = (green*256+blue)/65535
            depth_error = max(depth_error, abs(z-actual_depth))
            require(abs(z-actual_depth) < .001, f'triangle depth at {column},{row}')
            # The shader renormalizes the interpolated transformed normal.
            # Compare its sign and magnitude with the CPU field away from a discontinuity.
            near_edge = any(abs(y-end[0]) < .04 for lo, hi, _ in spans for end in (lo, hi))
            if not near_edge:
                require(abs((red/255*2-1)-nx) < .018, f'normal at {column},{row}: {red} vs {nx:.3f}')
                if p == 2 and abs(nx) > .15:
                    lit_halves.add(column < extent/2)
            interior_hits += 1
            if shape in ('smooth', 'mixed') and row % 3 == 0 and column == extent//2:
                w = view.intersect(shape, x, y)
                if w is not None:
                    analytic_depth = max(analytic_depth, abs(view.depth(w)-actual_depth))
                    analytic_probes += 1
        if column == extent//2:
            require(covered, 'empty center column')
            center_top = {p: min(lo[0] for lo, _, patch in spans if patch == p) for p in (1, 2)}
            first_row = min(covered)
    require(interior_hits > extent, 'insufficient coverage probes')
    if view.side == -1 and shape in ('smooth', 'crease', 'plane', 'unequal', 'unequal-crease',
                                     'unequal-reversed', 'unequal-crease-reversed'):
        # At this pose the two rows straddle x=0. Check every interior seam
        # pixel, including both sides' shading signal across a visible crease.
        for column in range(3*extent//8, 5*extent//8):
            a = ((extent//2-1)*extent+column)*4
            b = a+extent*4
            require(all(126 <= raw[i+3] <= 193 for i in (a, b)), 'open seam in raster')
            jump = abs(raw[a]-raw[b])
            if 'crease' in shape:
                require(jump > 60, 'crease disappeared from raster')
            elif shape == 'plane':
                require(jump <= 1, 'coplanar speed change introduced shading seam')
            else:
                require(jump < 55, 'smooth join developed a visible crease')
    if shape == 'sparse' and view.side == -1:
        require(len(lit_halves) == 2, 'both opposite-slope halves must draw')
    if shape not in ('smooth', 'mixed'):
        return
    # The coarse mixed control has only two triangles on the near, almost
    # planar patch. The grazing ray amplifies their height error at this view.
    depth_budget = .025 if path.parent.name.endswith('-0') else .004
    require(analytic_probes > 1 and analytic_depth < depth_budget,
            f'analytic probes={analytic_probes}, depth error={analytic_depth:.6f}, budget={depth_budget}')
    analytic = [view.silhouette(shape, p) for p in (1, 2)]
    top, tangent_x = min(analytic)
    if shape == 'smooth':
        require(.001 < abs(tangent_x) < 1, 'expected interior tangent')
        # Exact tangent is a double root. Bracket it independently, without giving a
        # rounded pixel a vote on whether the exact tangent counts as a hit.
        require(view.intersect(shape, extent/2+.5, top-.0001) is None and
                view.intersect(shape, extent/2+.5, top+.0001) is not None, 'tangent classification')
    measurements = projected_deviation(shape, records, view)
    error = abs(first_row-top)
    triangle_count = sum(data[-1] for data in measurements.values())
    require(error <= 1 and triangle_count <= 32 and abs(first_row-min(center_top.values())) <= .54,
            f'silhouette budget: {error:.3f}px, {triangle_count} triangles')
    by_patch = [abs(center_top[p]-analytic[p-1][0]) for p in (1, 2)]
    require(max(by_patch) <= 1, f'tessellated silhouette exceeds one pixel: {by_patch}')
    if shape == 'mixed' and view.side == 1 and view.scale == (1, 1, 1):
        require(measurements[1][0] > measurements[2][0] and by_patch[1] > by_patch[0],
                'projected extent must rank the flatter patch first while the curved silhouette needs more detail')
    if shape == 'mixed' and path.parent.name.endswith('-1') and view.distance == CAMERAS[-1][2]:
        _, observed, center_estimate, _, _ = measurements[1]
        require(observed > center_estimate, 'near grazing control must expose center-depth underestimation')
    print(f'{path.parent.name} {path.stem} raster={error:.3f}px tessellated={by_patch[0]:.3f}/{by_patch[1]:.3f}px '
          f'interior depth={analytic_depth:.6f} triangle depth={depth_error:.6f}')
    for p, (span, peak, naive, conservative, cost) in measurements.items():
        print(f'  patch {int(p)} extent={span:.1f}px deviation={peak:.3f}px '
              f'center={naive:.3f}px bound={conservative:.3f}px triangles={cost}')


def verify(mode, root):
    seams = {}
    for shape in SHAPES:
        for level, tolerance in enumerate(TOLERANCES):
            directory = root/f'{shape}-{level}'
            records = read_mesh(directory)
            if mode == 'geometry':
                seam, normals = geometry(shape, records, tolerance)
                print(f'{directory.name}: {len(seam)} shared samples, {len(records["f"])} triangles')
                if shape.endswith('-reversed'):
                    original, original_normals = seams[shape.removesuffix('-reversed'), level]
                    require(original == seam, 'declaration order changed seam bits')
                    require(original_normals.keys() == normals.keys() and
                            all(math.dist(value, original_normals[key]) < NORMAL_EPSILON
                                for key, value in normals.items()), 'declaration order changed seam normals')
                seams[shape, level] = seam, normals
            else:
                for extent in EXTENTS:
                    for i, camera in enumerate(CAMERAS if shape in ('smooth', 'mixed') else CAMERAS[:2]):
                        path = directory/f'{extent}-{i}.rgba'
                        try:
                            raster(shape, records, path, Projection(camera, extent))
                        except ValueError as error:
                            raise ValueError(f'{path}: {error}') from error


if __name__ == '__main__':
    mode, root, *producer = sys.argv[1:]
    if mode not in ('geometry', 'raster'):
        raise SystemExit('Expected geometry or raster, an evidence directory, and optionally the SDK executable')
    if producer:
        subprocess.run([*producer, mode, root], check=True)
    verify(mode, Path(root))
