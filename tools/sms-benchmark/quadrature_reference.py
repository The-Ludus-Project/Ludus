#!/usr/bin/env python3
"""Independent randomized-QMC reference, parallelized over pixels and sample blocks.

Each independent shift estimates the same four-dimensional caustic integral.
Uncertainty is measured across complete shifted images, not from correlated
point samples. The GPU evaluates transport; CPU FP64 reduces the partial sums.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np

from gpu import compile_entry
import layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=262144)
    parser.add_argument('--replicates', type=int, default=8)
    parser.add_argument('--alpha', type=float, nargs='+', default=[.005, .02, .1])
    arguments = parser.parse_args()
    if arguments.samples < 2 or arguments.samples > layout.MAXIMUM_QMC_SAMPLES or arguments.samples & (arguments.samples-1):
        parser.error(f'--samples must be a power of two in [2, {layout.MAXIMUM_QMC_SAMPLES}].')
    if any(not np.isfinite(alpha) or alpha <= 0 for alpha in arguments.alpha):
        parser.error('--alpha values must be finite and positive.')
    if arguments.replicates < 4:
        parser.error('Use at least four independent complete-image shifts.')
    arguments.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((arguments.prepared / 'manifest.json').read_text())
    shader = compile_entry(arguments.output / 'renderer', arguments.compiler, 'quadrature_reference.slang', 'IntegrateConnections')
    import wgpu
    adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
    adapter_info = dict(adapter.info)
    if adapter_info['adapter_type'] == 'CPU':
        raise ValueError('A physical GPU is required; use check_integral.py for the CPU reference.')
    device = adapter.request_device_sync()
    queue = device.queue
    module = device.create_shader_module(code=shader)
    pipeline = device.create_compute_pipeline(layout='auto', compute=dict(module=module, entry_point='IntegrateConnections'))
    entries = []
    buffers = []
    for binding, name in [(0, 'inputs'), (2, 'slopes'), (3, 'scene')]:
        data = np.load(arguments.prepared / f'{name}.npy', mmap_mode='r')
        buffer = device.create_buffer_with_data(data=data, usage=wgpu.BufferUsage.STORAGE)
        buffers.append(buffer)
        entries.append(dict(binding=binding, resource=dict(buffer=buffer)))
    samples_per_block = 32
    blocks = (arguments.samples+samples_per_block-1)//samples_per_block
    pixels_per_batch = 16
    partials = device.create_buffer(size=pixels_per_batch*blocks*16, usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC)
    parameters = device.create_buffer(size=layout.QMC_PARAMETER_CODEC.size, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
    entries.extend([dict(binding=1, resource=dict(buffer=partials)), dict(binding=5, resource=dict(buffer=parameters))])
    bindings = device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=entries)
    extent = manifest['extent']
    pixel_count = extent*extent
    records = []
    for alpha in arguments.alpha:
        beginning = time.perf_counter()
        images = []
        for replicate in range(arguments.replicates):
            image = np.zeros((pixel_count, 3), dtype=np.float64)
            for offset in range(0, pixel_count, pixels_per_batch):
                count = min(pixels_per_batch, pixel_count-offset)
                values = layout.QMC_PARAMETER_CODEC.pack(offset, count, arguments.samples, samples_per_block,
                    671229+replicate*1729, blocks, 0, 0, alpha, 0, 0, 0)
                queue.write_buffer(parameters, 0, values)
                encoder = device.create_command_encoder()
                compute = encoder.begin_compute_pass()
                compute.set_pipeline(pipeline)
                compute.set_bind_group(0, bindings)
                compute.dispatch_workgroups((count*blocks+31)//32, 1, 1)
                compute.end()
                queue.submit([encoder.finish()])
                raw = np.frombuffer(queue.read_buffer(partials, 0, count*blocks*16), dtype=np.float32).reshape(count, blocks, 4)
                if not np.all(np.isfinite(raw)):
                    np.save(arguments.output / f'invalid-{alpha}-{replicate}-{offset}.npy', raw)
                    raise ValueError('Nonfinite quadrature contribution; inspect the preserved partial sums.')
                image[offset:offset+count] = raw[:, :, :3].sum(axis=1, dtype=np.float64)/arguments.samples
            images.append(image.reshape(extent, extent, 3))
            print('QMC reference', alpha, 'shift', replicate+1, '/', arguments.replicates, flush=True)
        replicas = np.asarray(images)
        image = replicas.mean(axis=0)
        variance = replicas.var(axis=0, ddof=1)/len(replicas)
        uncertainty = float(np.sqrt(variance.sum()/max(np.sum(image*image), 1e-30)))
        np.savez(arguments.output / f'reference-{alpha}.npz', image=image, variance=variance, replicates=replicas)
        records.append(dict(alpha=alpha, estimated_relative_standard_error=uncertainty,
                            wall_seconds=time.perf_counter()-beginning))
        report = dict(source=manifest['source'], prepared=manifest, adapter=adapter_info,
                      reference_samples=arguments.samples*arguments.replicates*2,
                      samples_per_chart_per_shift=arguments.samples, independent_shifts=arguments.replicates,
                      reference_quality_target=.02, records=records,
                      reference_method='shifted Hammersley UV/direction charts, balance MIS, independently uniform area-light endpoints; no slice guide or Newton roots',
                      shader_sha256=hashlib.sha256(shader.encode()).hexdigest(),
                      limitations=[f'Uncertainty across {arguments.replicates} shifts is empirical, not a certified global error bound.',
                                   'GPU evaluates transport in FP32; partial sums are reduced on CPU in FP64.'])
        (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
        print(records[-1], flush=True)


if __name__ == '__main__':
    main()
