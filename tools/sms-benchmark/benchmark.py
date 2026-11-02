#!/usr/bin/env python3
"""Prepare the original SMS Figure 17 and check its CPU/Metal material parity."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np

from scene import SOURCE, Scene, normalize, read_gold_ior, verify_assets
from layout import write_shader_layout

ROOT = Path(__file__).resolve().parents[2]
OWNER = Path(__file__).parent


def prepare(upstream, output):
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != SOURCE['revision']:
        raise ValueError(f'Expected upstream {SOURCE["revision"]}; create a separate checkout of that revision.')
    directory = upstream / SOURCE['directory']
    hashes = verify_assets(directory)
    scene = Scene(directory)
    eta, extinction = read_gold_ior(upstream)
    output.mkdir(parents=True, exist_ok=False)
    for name in SOURCE['files']:
        target = output / 'original' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(directory / name, target)
    shutil.copyfile(upstream / 'LICENSE', output / 'original/LICENSE')
    np.savez(output / 'scene.npz', transforms=scene.transforms, inverse_transforms=scene.inverse_transforms,
             normals=scene.normals, areas=scene.areas, camera=scene.camera, slopes=scene.slopes,
             eta=eta, extinction=extinction)
    report = dict(source=SOURCE, upstream_revision=revision, asset_hashes=hashes,
                  rectangle_roles=['area light', 'rough gold reflector', 'diffuse floor'],
                  rectangle_areas=scene.areas.tolist(), camera_position=scene.camera[:3, 3].tolist(),
                  field_of_view_degrees=scene.field_of_view, emitter_radiance=scene.radiance,
                  eta_rgb=eta.tolist(), extinction_rgb=extinction.tolist(),
                  normal_map_resolution=list(scene.slopes.shape[:2]),
                  original_resolution=[1080, 1080], original_filter='gaussian',
                  original_path_depth=6, rendering_status='not rendered',
                  source_unchanged=True, material_model='isotropic Beckmann, slope-interpolated normal map',
                  note='Published assets are unchanged; scene.npz is a derived research representation, not an engine import.')
    (output / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def validate(upstream, output, compiler, count):
    """Compare raw scene intersections, shading normals and cos-weighted BSDFs."""
    scene = Scene(upstream / SOURCE['directory'])
    eta, extinction = read_gold_ior(upstream)
    output.mkdir(parents=True, exist_ok=False)
    source_directory = output / 'source'
    source_directory.mkdir()
    for source in [ROOT / 'modules/foundation/math/shaders/math.slang',
                   ROOT / 'modules/graphics/rhi/shaders/raster_bindings.slang',
                   OWNER / 'shaders/figure17.slang']:
        shutil.copyfile(source, source_directory / source.name)
    write_shader_layout(source_directory)
    subprocess.run([sys.executable, str(ROOT / 'cmake/shaders/compile_shader.py'),
                    '--source', str(source_directory / 'figure17.slang'), '--output', str(output / 'compile'),
                    '--name', 'ValidateFigure17', '--compiler', str(compiler),
                    '--lock', str(ROOT / 'config/shader_toolchain.json'),
                    '--graph-stage', 'compute', '--graph-target', 'wgsl'], check=True)
    random = np.random.default_rng(92026)
    inputs = np.zeros((count, 5, 4), dtype=np.float32)
    inputs[:, 0, :2] = random.uniform(0, 1, (count, 2))
    inputs[:, 0, 2] = np.resize(SOURCE['roughness'], count)
    # Include narrow-lobe samples rather than accepting a test of all zero BSDFs.
    frame = scene.make_frame(inputs[:, 0, :2])
    view_shading = normalize(np.c_[random.uniform(-.7, .7, (count, 2)), np.ones(count)])
    offset = random.normal(size=(count, 2))*inputs[:, 0, 2, None]/np.sqrt(2)
    microfacet = normalize(np.c_[offset, np.ones(count)])
    light_shading = 2*np.sum(view_shading*microfacet, axis=-1, keepdims=True)*microfacet-view_shading
    inputs[:, 1, :3] = np.einsum('...ij,...i->...j', frame, view_shading)
    inputs[:, 2, :3] = np.einsum('...ij,...i->...j', frame, light_shading)
    ray_origins, ray_directions = scene.make_primary_rays(int(np.ceil(np.sqrt(count))))
    inputs[:, 3, :3] = ray_origins.reshape(-1, 3)[:count]
    inputs[:, 4, :3] = ray_directions.reshape(-1, 3)[:count]
    expected = np.zeros((count, 3, 4), dtype=np.float64)
    expected[:, 0, :3] = scene.make_frame(inputs[:, 0, :2])[:, 2]
    expected[:, 1, :3] = scene.evaluate_beckmann(inputs[:, 0, :2], inputs[:, 1, :3], inputs[:, 2, :3], inputs[:, 0, 2], eta, extinction)
    distance, identity, coordinates = scene.intersect(inputs[:, 3, :3], inputs[:, 4, :3])
    expected[:, 2] = np.c_[np.minimum(distance, 1e30), identity, coordinates]
    packed_scene = np.zeros((14, 4), dtype=np.float32)
    packed_scene[0, :3] = eta
    packed_scene[1, :3] = extinction
    packed_scene[2:5, :3] = normalize(scene.transforms[1, :3, :3].T)
    packed_scene[2, 3] = scene.slopes.shape[0]
    packed_scene[5:] = scene.inverse_transforms[:, :3].reshape(-1, 4)
    slopes = np.zeros(scene.slopes.shape[:2]+(4,), dtype=np.float32)
    slopes[..., :2] = scene.slopes
    import wgpu
    from wgpu.utils import get_default_device
    from wgpu.utils.compute import compute_with_buffers
    device = get_default_device()
    adapter = dict(device.adapter.info)
    if adapter['adapter_type'] == 'CPU':
        raise ValueError('A physical GPU is required; no software fallback is accepted.')
    shader = (output / 'compile/artifact.wgsl').read_text().replace('fn ValidateFigure17(', 'fn main(')
    result = compute_with_buffers({0: inputs, 2: slopes, 3: packed_scene}, {1: count*3*4*4}, shader,
                                  n=((count+31)//32, 1, 1))
    observed = np.frombuffer(result[1], dtype=np.float32).reshape(count, 3, 4)
    np.savez(output / 'parity.npz', inputs=inputs, expected=expected, observed=observed)
    tolerances = [(2e-5, 2e-6), (5e-4, 2e-5), (2e-5, 2e-5)]
    checks = [bool(np.allclose(observed[:, index], expected[:, index], rtol=relative, atol=absolute))
              for index, (relative, absolute) in enumerate(tolerances)]
    nonzero = int(np.count_nonzero(np.max(expected[:, 1, :3], axis=1) > 1))
    report = dict(adapter=adapter, count=count, scope='raw material and geometry parity; no SMS, slice sampling, or timing claim',
                  records=['shading normal', 'Beckmann BSDF times outgoing shading cosine', 'ray hit distance/shape/UV'],
                  records_passed=checks, tolerances=tolerances, nonzero_highlight_samples=nonzero,
                  maximum_absolute_errors=np.max(np.abs(observed-expected), axis=(0, 2)).tolist(),
                  shader_sha256=hashlib.sha256(shader.encode()).hexdigest(), source=SOURCE)
    (output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    if not all(checks) or nonzero < count//2:
        raise ValueError('Material/geometry parity failed; inspect the preserved report and arrays.')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('prepare', 'validate'):
        command = commands.add_parser(name)
        command.add_argument('--upstream', type=Path, required=True)
        command.add_argument('--output', type=Path, required=True)
        if name == 'validate':
            command.add_argument('--compiler', type=Path, required=True)
            command.add_argument('--count', type=int, default=1024)
    arguments = parser.parse_args()
    if arguments.command == 'prepare':
        report = prepare(arguments.upstream.resolve(), arguments.output)
    else:
        if arguments.count < 1:
            parser.error('--count must be positive')
        report = validate(arguments.upstream.resolve(), arguments.output, arguments.compiler, arguments.count)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
