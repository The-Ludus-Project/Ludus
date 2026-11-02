#!/usr/bin/env python3
"""Controlled luminaire room, CPU oracle and upstream PureLight proposal experiment.

Thanks to Shlomi Steinberg, Slice Sampling: Importance sampling with an auxiliary
map (draft), sections 6.2 and 6.4: affine Gaussian emission and finite branch law.
Thanks to Figueiredo et al., PureLight: Learning Complex Luminaires with Light
Tracing, SIGGRAPH Asia 2026, sections 3.4 and 3.5, https://arxiv.org/abs/2606.04319:
the optional adapter trains their released receiver flow against our analytic
radiance. This diagnostic does not reproduce their learned fixture appearance.
"""
import argparse
import hashlib
import json
import math
import platform
from pathlib import Path
import shutil
import sys
import time

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / 'modules/graphics/renderer/render_graphs/slice_sampling_luminaire'
UPSTREAM_REVISION = 'd0969834eef28f1453056508ba39eaef0471a5a2'
CENTER = np.array([0., .97, 0.])
HALF_EXTENT = .35
EPSILON = .1
PROBES = np.array([[.1, -1, -.2], [.2, -1, .4], [-.4, -1, .3],
                   [.07, -1, -.05], [.1, -.3, -.2], [.3, -1, .2],
                   [.6, -1, -.6], [-.99, 0, .1]])
NORMALS = np.array([[0., 1, 0]] * 5 + [[.4472135955, .894427191, 0],
                                      [0, 1, 0], [1, 0, 0]])


def fixture():
    """Read GPU-owned tile tables rather than maintain a second emission model."""
    import re
    source = (TEMPLATE / 'main_shaders/luminaire.slang').read_text()
    def table(name, width):
        body = re.search(r'LUMINAIRE_TILE_' + name + r'\[.*?\] = \{(.*?)\};', source, re.S)[1]
        values = re.findall(r'float[23]\(([^)]+)\)', body) if width > 1 else body.split(',')
        return np.array([[float(x) for x in value.split(',')] for value in values if value.strip()]).reshape(-1, width)
    return dict(steering=table('STEERING', 2), widths=table('WIDTH', 3),
                strength=table('STRENGTH', 1)[:, 0], tint=table('TINT', 3))


class Panel:
    def __init__(self, width=.02, steering=.12, spread=.08, twist=.04):
        self.tables = fixture()
        self.matrix = np.array([[spread, -twist], [twist, spread]])
        self.offset = steering * self.tables['steering']
        self.cholesky = np.zeros((9, 2, 2))
        self.cholesky[:, 0, 0] = width * self.tables['widths'][:, 0]
        self.cholesky[:, 1, 0] = width * self.tables['widths'][:, 1]
        self.cholesky[:, 1, 1] = width * self.tables['widths'][:, 2]
        self.lower = np.array([[-1 + 2 * (i % 3) / 3, -1 + 2 * (i // 3) / 3] for i in range(9)])
        self.profile = dict(width=width, steering=steering, spread=spread, twist=twist)

    def identity(self):
        payload = dict(profile=self.profile, tables={k: v.tolist() for k, v in self.tables.items()},
                       center=CENTER.tolist(), half_extent=HALF_EXTENT, radiance_scale=6)
        return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()

    def receiver(self, point):
        return point[[0, 2]] / HALF_EXTENT, (CENTER[1] - point[1]) / HALF_EXTENT

    def evaluate(self, point, area):
        """RGB radiance and area-to-solid-angle Jacobian; area in panel units."""
        tangent, height = self.receiver(point)
        cell = np.floor((area + 1) * 1.5).astype(int)
        valid = np.all((cell >= 0) & (cell < 3), axis=-1)
        tile = np.clip(cell[:, 0] + 3 * cell[:, 1], 0, 8)
        slope = (tangent - area) / height
        residual = slope - area @ self.matrix.T - self.offset[tile]
        standard = np.linalg.solve(self.cholesky[tile], residual[..., None])[..., 0]
        density = np.exp(-.5 * np.sum(standard**2, axis=-1)) / (2 * math.pi * np.linalg.det(self.cholesky[tile]))
        radiance = 6 * self.tables['strength'][tile, None] * self.tables['tint'][tile] * density[:, None]
        radiance[~valid] = 0
        displacement = np.column_stack((-slope[:, 0], np.ones(len(area)), -slope[:, 1]))
        directions = displacement / np.linalg.norm(displacement, axis=-1)[:, None]
        jacobian = 1 / (height**2 * (1 + np.sum(slope**2, axis=-1))**1.5)
        return radiance, jacobian, directions

    def slice_density(self, point, area):
        tangent, height = self.receiver(point)
        slope = (tangent - area) / height
        cell = np.floor((area + 1) * 1.5).astype(int)
        valid = np.all((cell >= 0) & (cell < 3), axis=-1)
        tile = np.clip(cell[:, 0] + 3 * cell[:, 1], 0, 8)
        residual = slope - area @ self.matrix.T - self.offset[tile]
        standard = np.linalg.solve(self.cholesky[tile], residual[..., None])[..., 0]
        _, weights = self.roots(point, standard)
        total = weights.sum(axis=1)
        density = np.exp(-.5 * np.sum(standard**2, axis=-1)) / (2 * math.pi * np.linalg.det(self.cholesky[tile]))
        return np.divide(density * self.tables['strength'][tile], total * height**2,
                         out=np.zeros(len(area)), where=valid & (total > 0))

    def roots(self, point, standard):
        tangent, height = self.receiver(point)
        matrix = np.eye(2) + height * self.matrix
        right = tangent @ self.matrix.T + self.offset[None] + np.einsum('tij,nj->nti', self.cholesky, standard)
        slopes = right @ np.linalg.inv(matrix).T
        areas = tangent - height * slopes
        valid = np.all((areas >= self.lower) & (areas < self.lower + 2 / 3), axis=-1)
        weights = valid * self.tables['strength'] / np.linalg.det(matrix)
        return areas, weights

    def sample(self, point, count, rng, method):
        area = rng.uniform(-1, 1, (count, 2))
        if method == 'area':
            return area, np.full(count, .25), np.ones(count, dtype=bool)
        candidates, weights = self.roots(point, rng.standard_normal((count, 2)))
        total = weights.sum(axis=1)
        tile = np.minimum(np.sum(np.cumsum(weights, axis=1) < (rng.random(count) * total)[:, None], axis=1), 8)
        use_slice = rng.random(count) >= EPSILON
        area[use_slice] = candidates[np.arange(count), tile][use_slice]
        valid = (~use_slice) | (total > 0)
        density = (1 - EPSILON) * self.slice_density(point, area) + EPSILON * .25
        return area, density, valid

    def contribution(self, point, normal, area, density, valid):
        radiance, jacobian, directions = self.evaluate(point, area)
        value = radiance * (jacobian * np.maximum(directions @ normal, 0) / math.pi)[:, None]
        return np.divide(value, density[:, None], out=np.zeros_like(value), where=valid[:, None] & (density[:, None] > 0))

    def reference(self, point, normal, order):
        nodes, weights = np.polynomial.legendre.leggauss(order)
        x, y = np.meshgrid(nodes, nodes)
        areas = np.stack((x.ravel(), y.ravel()), axis=1) / 3
        product = (weights[:, None] * weights).ravel() / 9
        result = np.zeros(3)
        for lower in self.lower:
            samples = areas + lower + 1 / 3
            result += np.sum(self.contribution(point, normal, samples, np.ones(len(samples)), np.ones(len(samples), dtype=bool)) * product[:, None], axis=0)
        return result


def write_room(destination, panel):
    destination.mkdir(parents=True, exist_ok=False)
    # The same vertices are emitted to both interchange formats, in metres, Y up.
    quads = [([[-1,-1,-1],[-1,-1,1],[1,-1,1],[1,-1,-1]], [0,1,0]),
             ([[-1,-1,1],[-1,1,1],[1,1,1],[1,-1,1]], [0,0,-1]),
             ([[-1,-1,-1],[-1,1,-1],[-1,1,1],[-1,-1,1]], [1,0,0]),
             ([[1,-1,1],[1,1,1],[1,1,-1],[1,-1,-1]], [-1,0,0]),
             ([[-.2,-.3,-.35],[-.2,-.3,-.05],[.4,-.3,-.05],[.4,-.3,-.35]], [0,1,0])]
    pbrt = ['LookAt 0 0 -3.2  0 -0.1 0  0 1 0', 'Camera "perspective" "float fov" [45]',
            'Film "rgb" "integer xresolution" [512] "integer yresolution" [512]', 'WorldBegin',
            'Material "diffuse" "rgb reflectance" [1 1 1]']
    import xml.etree.ElementTree as xml
    scene = xml.Element('scene', version='3.0.0')
    integrator = xml.SubElement(scene, 'integrator', type='path')
    xml.SubElement(integrator, 'integer', name='max_depth', value='2')
    sensor = xml.SubElement(scene, 'sensor', type='perspective')
    xml.SubElement(sensor, 'float', name='fov', value='45')
    transform = xml.SubElement(sensor, 'transform', name='to_world')
    xml.SubElement(transform, 'lookat', origin='0 0 -3.2', target='0 -0.1 0', up='0 1 0')
    film = xml.SubElement(sensor, 'film', type='hdrfilm')
    for dimension in ('width', 'height'):
        xml.SubElement(film, 'integer', name=dimension, value='512')
    for index, (vertices, normal) in enumerate(quads):
        points = ' '.join(str(v) for vertex in vertices for v in vertex)
        normals = ' '.join(str(v) for _ in vertices for v in normal)
        pbrt += ['Shape "trianglemesh" "integer indices" [0 1 2 0 2 3]',
                 f'"point3 P" [{points}] "normal N" [{normals}]']
        # OBJ avoids renderer-specific rectangle orientation conventions.
        name = f'receiver-{index}.obj'
        obj = '\n'.join('v ' + ' '.join(map(str, vertex)) for vertex in vertices)
        obj += '\nvn ' + ' '.join(map(str, normal)) + '\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n'
        (destination / name).write_text(obj)
        shape = xml.SubElement(scene, 'shape', type='obj')
        xml.SubElement(shape, 'string', name='filename', value=name)
        bsdf = xml.SubElement(shape, 'bsdf', type='diffuse')
        xml.SubElement(bsdf, 'rgb', name='reflectance', value='1 1 1')
    (destination / 'scene-v4.pbrt').write_text('\n'.join(pbrt + ['WorldEnd']) + '\n')
    xml.indent(scene)
    xml.ElementTree(scene).write(destination / 'scene.xml', encoding='unicode')
    shutil.copytree(TEMPLATE, destination / 'graph')
    manifest = destination / 'graph/main.lushader.json'
    manifest.write_text((destination / 'graph/main.lushader.json.in').read_text().replace('@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', '["metal", "wgsl"]'))
    (destination / 'graph/main.lushader.json.in').unlink()
    (destination / 'comparison.json').write_text(json.dumps(dict(schema=1, fixture_sha256=panel.identity(),
        scope='analytic-panel diagnostic; shared exact radiance; direct illumination; no authored scene emitters',
        purelight_revision=UPSTREAM_REVISION, purelight_status='requires retrained upstream flow; not pretrained rough-flower',
        radiance_owner='graph/main_shaders/luminaire.slang', profiles=panel.profile,
        probes_world=PROBES.tolist(), normals_world=NORMALS.tolist()), indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'run'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--room', type=Path, default=Path('out/luminaire-room'),
                        help='prepared room whose immutable radiance source is checked before a run')
    parser.add_argument('--samples', type=int, default=131072)
    parser.add_argument('--seed', type=int, default=871)
    parser.add_argument('--purelight', type=Path, help='trained adapter checkpoint (see purelight_adapter.py)')
    parser.add_argument('--upstream', type=Path)
    parser.add_argument('--portable', type=Path, help='CPU-trained portable flow archive; also exportable to Slang')
    args = parser.parse_args()
    panel = Panel()
    if args.samples < 2:
        parser.error('--samples must be at least 2')
    if args.action == 'prepare':
        write_room(args.output, panel)
        return
    room = json.loads((args.room / 'comparison.json').read_text())
    shader = 'main_shaders/luminaire.slang'
    if (room['fixture_sha256'] != panel.identity() or
            (args.room / 'graph' / shader).read_bytes() != (TEMPLATE / shader).read_bytes()):
        raise ValueError('Prepared room differs from the shared radiance evaluator. Prepare a fresh room before comparing.')
    sampler = None
    if args.purelight and args.portable:
        parser.error('Select one learned sampler per run.')
    if args.portable:
        from portable_flow import PortableFlow
        sampler = PortableFlow(args.portable, panel)
    if args.purelight:
        from purelight_adapter import ReceiverFlow
        sampler = ReceiverFlow(args.upstream, args.purelight, panel)
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for index, (point, normal) in enumerate(zip(PROBES, NORMALS)):
        reference = panel.reference(point, normal, 128)
        refined = panel.reference(point, normal, 256)
        if not np.allclose(reference, refined, rtol=1e-5, atol=1e-300):
            raise ValueError(f'probe {index}: quadrature has not converged; increase reference orders')
        for method in ('area', 'slice-defensive') + ((('portable-purelight',) if args.portable else ('purelight-flow',)) if sampler else ()):
            rng = np.random.default_rng(args.seed + index)
            if method == 'purelight-flow':
                sampler.sample(point, 256)
                sampler.torch.manual_seed(args.seed + index)
            elif method == 'portable-purelight':
                sampler.sample(point, 256, args.seed + index)
            else:
                panel.sample(point, 256, np.random.default_rng(0), method)
            start = time.perf_counter()
            if method == 'purelight-flow':
                sampler.torch.manual_seed(args.seed + index)
            area, density, valid = (sampler.sample(point, args.samples, args.seed + index) if method == 'portable-purelight' else
                                   sampler.sample(point, args.samples) if method == 'purelight-flow'
                                     else panel.sample(point, args.samples, rng, method))
            values = panel.contribution(point, normal, area, density, valid)
            seconds = time.perf_counter() - start
            rows.append(dict(probe=index, method=method, mean=values.mean(axis=0).tolist(),
                reference=refined.tolist(), standard_error=(values.std(axis=0, ddof=1) / math.sqrt(args.samples)).tolist(),
                variance=values.var(axis=0, ddof=1).tolist(), seconds=seconds, null_rate=float(1-valid.mean())))
    report = dict(schema=1, fixture_sha256=panel.identity(), samples=args.samples, seed=args.seed,
        scope='Unoccluded CPU point-estimator diagnostic; NumPy evaluation and adapter transfers included; not Ludus GPU timing',
        host=platform.platform(), python=sys.version, numpy=np.__version__,
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        shader_sha256=hashlib.sha256((TEMPLATE / shader).read_bytes()).hexdigest(),
        portable_flow_status='measured' if args.portable else 'not run',
        purelight_status='measured' if args.purelight else 'not run; provide --upstream and --purelight', results=rows)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(args.output / 'report.json')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        print(f'Benchmark failed: {error}', file=sys.stderr)
        raise SystemExit(1) from error
