#!/usr/bin/env python3
"""Execute exported Slang flow on a real GPU and compare with NumPy inference.

wgpu is a verification driver only. The identical Slang implementation is also
linked into the Ludus render graph; this check does not time the editor renderer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import sys
import time

import numpy as np

from benchmark import CENTER, Panel, PROBES, ROOT
from portable_flow import PortableFlow, RADIUS, cap_directions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graph', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples-per-receiver', type=int, default=32)
    args = parser.parse_args()
    if args.samples_per_receiver < 1:
        parser.error('--samples-per-receiver must be positive')
    args.output.mkdir(parents=True, exist_ok=False)
    source_root = args.output / 'source'
    shutil.copytree(args.graph / 'main_shaders', source_root)
    shutil.copyfile(ROOT / 'modules/foundation/math/shaders/math.slang', source_root / 'math.slang')
    shutil.copyfile(ROOT / 'modules/graphics/rhi/shaders/raster_bindings.slang', source_root / 'raster_bindings.slang')
    shader = source_root / 'validate_flow.slang'
    shutil.copyfile(Path(__file__).parent / 'shaders/validate_flow.slang', shader)
    command = [sys.executable, str(ROOT / 'cmake/shaders/compile_shader.py'),
               '--source', str(shader), '--output', str(args.output), '--name', 'ValidatePortableFlow',
               '--compiler', str(args.compiler), '--lock', str(ROOT / 'config/shader_toolchain.json'),
               '--graph-stage', 'compute', '--graph-target', 'wgsl']
    subprocess.run(command, check=True)
    sources = list(args.output.glob('*.wgsl'))
    if len(sources) != 1:
        raise ValueError('Expected exactly one compiled WGSL validation artifact.')
    source = sources[0].read_text()
    # compute_with_buffers requires the entry named main; Slang's generated entry
    # may keep the source name. Rename only its declaration, preserving the body.
    source = source.replace('fn ValidatePortableFlow(', 'fn main(')
    from wgpu.utils import get_default_device
    from wgpu.utils.compute import compute_with_buffers
    device = get_default_device()
    adapter = dict(device.adapter.info)
    if adapter.get('adapter_type') == 'CPU':
        raise RuntimeError('A physical GPU is required for this parity check; software execution is not accepted.')
    rng = np.random.default_rng(871)
    inputs = np.zeros((len(PROBES) * args.samples_per_receiver, 2, 4), dtype=np.float32)
    expected = np.zeros((len(inputs), 5, 4), dtype=np.float32)
    panel = Panel()
    model = PortableFlow(args.graph / 'portable-model.npz', panel)
    for index, point in enumerate(PROBES):
        begin = index * args.samples_per_receiver
        end = begin + args.samples_per_receiver
        seeds = rng.uniform(.0001, .9999, (args.samples_per_receiver, 2)).astype(np.float32)
        inputs[begin:end, 0, :3] = point
        inputs[begin:end, 0, 3] = seeds[:, 0]
        inputs[begin:end, 1, 0] = seeds[:, 1]
        uv, log_pdf = model.transform(point, seeds, True)
        restored, replay_pdf = model.transform(point, uv)
        directions, cap_area = cap_directions(point, uv)
        expected[begin:end, 0] = np.c_[uv, log_pdf, replay_pdf]
        expected[begin:end, 1] = np.c_[restored, directions[:, :2]]
        expected[begin:end, 2, :2] = np.c_[directions[:, 2], np.exp(log_pdf)/cap_area]
        distance = np.divide(CENTER[1]-point[1], directions[:, 1], out=np.zeros(len(uv)), where=directions[:, 1] > 0)
        area = (point[None, [0, 2]] + distance[:, None]*directions[:, [0, 2]]) / .35
        radiance, _, _ = panel.evaluate(point, area)
        valid = (distance > 0) & np.all((area >= -1) & (area < 1), axis=1)
        expected[begin:end, 3, :3] = radiance / (np.exp(log_pdf)/cap_area)[:, None]
        expected[begin:end, 3, :3][~valid] = 0
        expected[begin:end, 3, 3] = valid
        expected[begin:end, 4, :3] = np.where(valid[:, None], directions, 0)
    start = time.perf_counter()
    output = compute_with_buffers({0: inputs}, {1: expected.nbytes}, source, n=((len(inputs)+31)//32, 1, 1))
    seconds = time.perf_counter()-start
    observed = np.frombuffer(output[1], dtype=np.float32).reshape(expected.shape)
    np.savez(args.output / 'parity.npz', inputs=inputs, expected=expected, observed=observed)
    if not np.all(np.isfinite(observed)):
        raise ValueError('GPU produced nonfinite results; raw parity arrays preserved.')
    # Loop-based GPU MLP accumulation differs from CPU BLAS reduction order.
    tolerances = [(2e-3, 3e-4), (2e-3, 3e-4), (3e-3, 3e-4), (1e-2, 1e-5), (2e-3, 3e-4)]
    checks = [bool(np.allclose(observed[:, i], expected[:, i], rtol=relative, atol=absolute))
              for i, (relative, absolute) in enumerate(tolerances)]
    report = dict(adapter=adapter, samples=len(inputs), scope='raw GPU/CPU parity; not renderer throughput',
        seconds_including_compilation_upload_readback=seconds, records_passed=checks,
        maximum_absolute_errors=np.max(np.abs(observed-expected), axis=(0,2)).tolist(),
        tolerances=tolerances, shader_sha256=hashlib.sha256(source.encode()).hexdigest(),
        model_sha256=hashlib.sha256((args.graph / 'portable-model.npz').read_bytes()).hexdigest())
    (args.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if not all(checks):
        raise ValueError('GPU/CPU parity failed; inspect preserved arrays and report.')


if __name__ == '__main__':
    main()
