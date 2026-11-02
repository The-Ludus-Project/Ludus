#!/usr/bin/env python3
"""Execute the finite-branch affine proposal on Metal and replay its CPU PDF."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np

from affine_slice import AffineSlice
from benchmark import OWNER, ROOT
from scene import SOURCE, Scene
from layout import write_shader_layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--resolution', type=int, default=64)
    arguments = parser.parse_args()
    if arguments.resolution < 1:
        parser.error('--resolution must be positive')
    scene = Scene(arguments.upstream / SOURCE['directory'])
    arguments.output.mkdir(parents=True, exist_ok=False)
    source_directory = arguments.output / 'source'
    source_directory.mkdir()
    for source in [ROOT / 'modules/foundation/math/shaders/math.slang',
                   ROOT / 'modules/graphics/rhi/shaders/raster_bindings.slang',
                   OWNER / 'shaders/figure17.slang', OWNER / 'shaders/affine_slice.slang']:
        shutil.copyfile(source, source_directory / source.name)
    write_shader_layout(source_directory)
    subprocess.run([sys.executable, str(ROOT / 'cmake/shaders/compile_shader.py'),
                    '--source', str(source_directory / 'affine_slice.slang'), '--output', str(arguments.output / 'compile'),
                    '--name', 'ValidateAffineSlice', '--compiler', str(arguments.compiler),
                    '--lock', str(ROOT / 'config/shader_toolchain.json'),
                    '--graph-stage', 'compute', '--graph-target', 'wgsl'], check=True)
    random = np.random.default_rng(924)
    origins, directions = scene.make_primary_rays(32)
    distances, identities, _ = scene.intersect(origins, directions)
    receivers = (origins+distances[..., None]*directions)[identities == 2][::87][:8]
    inputs = []
    expected = []
    vertices = []
    acceleration = []
    bins = []
    triangles = []
    for guide, receiver in enumerate(receivers):
        proposal = AffineSlice.from_scene(scene, receiver, arguments.resolution)
        # Packing to FP32 is part of the proposal definition on GPU. The oracle
        # evaluates this same represented map independently in FP64.
        proposal = AffineSlice(proposal.vertices.astype(np.float32))
        bounds, records, identities = proposal.build_acceleration()
        bin_offset = sum(len(value) for value in bins)
        triangle_offset = sum(len(value) for value in triangles)
        records[:, 0] += triangle_offset
        acceleration.extend([bounds, [bin_offset, 0, 0, 0]])
        bins.append(records)
        triangles.append(identities)
        offset = sum(len(value) for value in vertices)
        vertices.append(np.c_[proposal.vertices.reshape(-1, 2), np.zeros(((arguments.resolution+1)**2, 2))])
        for alpha in SOURCE['roughness']:
            for _ in range(32):
                residual = (random.normal(size=2)*alpha/np.sqrt(2)).astype(np.float32)
                branch = np.float32(random.uniform(.001, .999))
                coordinates = random.uniform(.001, .999, 2).astype(np.float32)
                data = np.zeros((5, 4), dtype=np.float32)
                data[0] = [*residual, branch, alpha]
                data[1] = [*coordinates, arguments.resolution, offset]
                data[2, 0] = guide
                inputs.append(data)
                sample, density, count = proposal.sample(residual, branch, data[0, 3])
                _, _, weights = proposal.enumerate_roots(residual)
                record = np.zeros((3, 4))
                if sample is not None:
                    record[0] = [*sample, density, 1]
                record[1] = [count, weights.sum(), proposal.evaluate_pdf(coordinates, data[0, 3]), 0]
                expected.append(record)
    inputs = np.asarray(inputs, dtype=np.float32)
    expected = np.asarray(expected)
    packed_vertices = np.concatenate(vertices).astype(np.float32)
    from wgpu.utils import get_default_device
    from wgpu.utils.compute import compute_with_buffers
    device = get_default_device()
    adapter = dict(device.adapter.info)
    if adapter['adapter_type'] == 'CPU':
        raise ValueError('A physical GPU is required for this check.')
    shader = (arguments.output / 'compile/artifact.wgsl').read_text().replace('fn ValidateAffineSlice(', 'fn main(')
    # Exercise the same accelerated shader path used by the timed renderer.
    result = compute_with_buffers({0: inputs, 4: packed_vertices,
        6: np.asarray(acceleration, dtype=np.float32), 7: np.concatenate(bins).astype(np.uint32),
        8: np.concatenate(triangles).astype(np.uint32)}, {1: expected.size*4}, shader,
                                  n=((len(inputs)+31)//32, 1, 1))
    observed = np.frombuffer(result[1], dtype=np.float32).reshape(expected.shape)
    np.savez(arguments.output / 'parity.npz', inputs=inputs, expected=expected, observed=observed)
    checks = dict(coordinates=bool(np.allclose(observed[:, 0, :2], expected[:, 0, :2], rtol=3e-5, atol=3e-6)),
                  density=bool(np.allclose(observed[:, 0, 2], expected[:, 0, 2], rtol=3e-4, atol=2e-5)),
                  support=bool(np.array_equal(observed[:, 0, 3], expected[:, 0, 3])),
                  root_count=bool(np.array_equal(observed[:, 1, 0], expected[:, 1, 0])),
                  total_weight=bool(np.allclose(observed[:, 1, 1], expected[:, 1, 1], rtol=3e-4, atol=2e-5)),
                  replay_density=bool(np.allclose(observed[:, 1, 2], expected[:, 1, 2], rtol=3e-4, atol=2e-5)))
    report = dict(adapter=adapter, source=SOURCE, scope='accelerated affine surrogate proposal and PDF parity; no image-quality or SMS performance claim',
                  draws=len(inputs), receivers=len(receivers), resolution=arguments.resolution,
                  roots_maximum=int(expected[:, 1, 0].max()), checks=checks,
                  maximum_absolute_errors=np.max(np.abs(observed-expected), axis=(0, 2)).tolist(),
                  shader_sha256=hashlib.sha256(shader.encode()).hexdigest())
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if not all(checks.values()):
        raise ValueError('Affine proposal parity failed; inspect preserved raw arrays.')


if __name__ == '__main__':
    main()
