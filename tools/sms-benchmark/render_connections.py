#!/usr/bin/env python3
"""Matched Metal estimates of the isolated floor-gold-area-light contribution.

This is a headless research harness, not an editor/BVH frame benchmark. It uses
the published scene assets but pixel-centre rays and level-zero texture lookup.
The full authors' integrator is run separately by render_original.py.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
import time

import numpy as np
import layout

from affine_slice import AffineSlice
from connection import make_receivers
from gpu import compile_entry, pack_scene
from scene import SOURCE, Scene

METHODS = {'connection-mis': layout.METHOD_CONNECTION_MIS, 'slice': layout.METHOD_SLICE, 'sms': layout.METHOD_SMS}
ALL_METHODS = dict(METHODS, reference=layout.METHOD_REFERENCE)


def prepare(upstream, output, extent, resolution):
    output.mkdir(parents=True, exist_ok=False)
    beginning = time.perf_counter()
    scene = Scene(upstream / SOURCE['directory'])
    records, slopes = pack_scene(scene, upstream)
    positions, colors, mask = make_receivers(scene, extent)
    inputs = np.zeros((len(positions), 5, 4), dtype=np.float32)
    inputs[:, 0, :3] = positions
    inputs[:, 0, 3] = mask
    inputs[:, 1, :3] = colors
    vertices = []
    acceleration = []
    bins = []
    identities = []
    triangle_offset = 0
    bin_offset = 0
    for index in np.flatnonzero(mask):
        proposal = AffineSlice.from_scene(scene, positions[index], resolution)
        proposal = AffineSlice(proposal.vertices.astype(np.float32))
        bounds, bin_records, triangle_identities = proposal.build_acceleration()
        guide = len(vertices)
        inputs[index, 1, 3] = guide
        vertices.append(np.c_[proposal.vertices.reshape(-1, 2), np.zeros(((resolution+1)**2, 2))])
        acceleration.extend([bounds, [bin_offset, 0, 0, 0]])
        bin_records[:, 0] += triangle_offset
        bins.append(bin_records)
        identities.append(triangle_identities)
        triangle_offset += len(triangle_identities)
        bin_offset += len(bin_records)
        if guide % 32 == 0:
            print(f'Preparing receiver {guide+1}/{int(mask.sum())}', flush=True)
    arrays = dict(inputs=inputs, slopes=slopes, scene=records,
                  vertices=np.concatenate(vertices).astype(np.float32),
                  acceleration=np.asarray(acceleration, dtype=np.float32),
                  bins=np.concatenate(bins).astype(np.uint32), identities=np.concatenate(identities).astype(np.uint32))
    for name, value in arrays.items():
        np.save(output / f'{name}.npy', value)
    report = dict(source=SOURCE, extent=extent, resolution=resolution, floor_receivers=int(mask.sum()),
                  preprocessing_seconds=time.perf_counter()-beginning,
                  buffer_bytes={name: value.nbytes for name, value in arrays.items()},
                  buffer_sha256={name: hashlib.sha256(value.tobytes()).hexdigest() for name, value in arrays.items()},
                  scope='published assets; isolated camera-floor-gold-light integral, pixel centres and level-zero textures',
                  slice_proposal='affine inverse-map surrogate, light centre conditioning, complete inverse-triangle enumeration, 10% defensive area sampling',
                  sms_proposal='portable two-stage offset-normal SMS with analytic slope-map Jacobian and capped independent re-hit trials')
    (output / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')


class Renderer:
    def __init__(self, prepared, output, compiler, minimum_guide_alpha, methods=None):
        import wgpu
        self.wgpu = wgpu
        self.manifest = json.loads((prepared / 'manifest.json').read_text())
        self.extent = self.manifest['extent']
        self.pixel_count = self.extent*self.extent
        self.resolution = self.manifest['resolution']
        self.minimum_guide_alpha = minimum_guide_alpha
        self.output = output
        beginning = time.perf_counter()
        adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
        self.adapter = dict(adapter.info)
        if self.adapter['adapter_type'] == 'CPU':
            raise ValueError('Physical GPU required; no software fallback.')
        largest = max(self.manifest['buffer_bytes'].values())
        limit = adapter.limits['max-storage-buffer-binding-size']
        if largest > limit:
            raise ValueError(f'A guide buffer requires {largest} bytes, but this adapter supports {limit}; prepare a new fixture with lower extent/resolution.')
        self.device = adapter.request_device_sync(required_features=['timestamp-query'],
            required_limits={'max-storage-buffer-binding-size': max(128*1024*1024, largest),
                             'max-buffer-size': max(256*1024*1024, largest)})
        self.queue = self.device.queue
        self.buffers = {}
        entries = []
        for binding, name in [(0, 'inputs'), (2, 'slopes'), (3, 'scene'), (4, 'vertices'),
                              (6, 'acceleration'), (7, 'bins'), (8, 'identities')]:
            data = np.load(prepared / f'{name}.npy', mmap_mode='r')
            buffer = self.device.create_buffer_with_data(data=data, usage=wgpu.BufferUsage.STORAGE)
            self.buffers[name] = buffer
            entries.append(dict(binding=binding, resource=dict(buffer=buffer)))
        self.accumulation = self.device.create_buffer(size=self.pixel_count*3*4*4,
            usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC | wgpu.BufferUsage.COPY_DST)
        self.parameters = self.device.create_buffer(size=layout.RENDER_PARAMETER_BYTES, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
        entries.extend([dict(binding=1, resource=dict(buffer=self.accumulation)),
                        dict(binding=5, resource=dict(buffer=self.parameters))])
        self.pipelines = {}
        self.bindings = {}
        self.shader_hash = {}
        for method, entry in [('connection-mis', 'RenderConnectionsMis'), ('slice', 'RenderConnectionsSlice'), ('sms', 'RenderConnectionsSms'), ('reference', 'RenderConnectionsReference')]:
            if methods is not None and method not in methods:
                continue
            shader = compile_entry(output / method, compiler, 'render_connections.slang', entry)
            self.shader_hash[method] = hashlib.sha256(shader.encode()).hexdigest()
            module = self.device.create_shader_module(code=shader)
            pipeline = self.device.create_compute_pipeline(layout='auto', compute=dict(module=module, entry_point=entry))
            self.pipelines[method] = pipeline
            used_bindings = set(range(9)) if method in ('slice', 'reference') else {0, 1, 2, 3, 5}
            self.bindings[method] = self.device.create_bind_group(layout=pipeline.get_bind_group_layout(0),
                entries=[value for value in entries if value['binding'] in used_bindings])
        self.query = self.device.create_query_set(type='timestamp', count=2)
        self.timestamps = self.device.create_buffer(size=16, usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC)
        self.setup_seconds = time.perf_counter()-beginning
        self.reset()

    def reset(self):
        self.queue.write_buffer(self.accumulation, 0, np.zeros((self.pixel_count, 3, 4), dtype=np.float32))
        self.queue.read_buffer(self.accumulation, 0, 16)

    def dispatch(self, method, alpha, seed, batch, offset=0, count=None):
        count = self.pixel_count if count is None else count
        beginning = time.perf_counter()
        self.queue.write_buffer(self.parameters, 0, layout.PARAMETER_CODEC.pack(count, offset, batch, seed, ALL_METHODS[method], self.resolution, alpha, self.minimum_guide_alpha))
        encoder = self.device.create_command_encoder()
        compute = encoder.begin_compute_pass(timestamp_writes=dict(query_set=self.query,
            beginning_of_pass_write_index=0, end_of_pass_write_index=1))
        compute.set_pipeline(self.pipelines[method])
        compute.set_bind_group(0, self.bindings[method])
        compute.dispatch_workgroups((count+31)//32, 1, 1)
        compute.end()
        encoder.resolve_query_set(self.query, 0, 2, self.timestamps, 0)
        self.queue.submit([encoder.finish()])
        stamps = np.frombuffer(self.queue.read_buffer(self.timestamps), dtype=np.uint64)
        return time.perf_counter()-beginning, (int(stamps[1])-int(stamps[0]))*1e-9

    def read(self):
        raw = np.frombuffer(self.queue.read_buffer(self.accumulation), dtype=np.float32).reshape(self.extent, self.extent, 3, 4).copy()
        counts = raw[:, :, 0, 3]
        if np.any(counts <= 0):
            raise ValueError('Some pixels received no samples; increase the time budget.')
        image = raw[:, :, 0, :3]/counts[..., None]
        second = raw[:, :, 1, :3]/counts[..., None]
        variance = np.maximum(0, second-image*image)/np.maximum(1, counts[..., None]-1)
        if not np.all(np.isfinite(raw)) or np.any(raw[:, :, 2, 3] > 0):
            np.save(self.output / 'invalid-accumulation.npy', raw)
            raise ValueError('Invalid transport contribution; preserve the raw accumulation and investigate.')
        return image, variance, raw

    def fixed(self, method, alpha, seed, samples, batch=16):
        self.reset()
        wall = 0.
        gpu = 0.
        for start in range(0, samples, batch):
            first, second = self.dispatch(method, alpha, seed, min(batch, samples-start))
            wall += first
            gpu += second
            if start % 1024 == 0:
                print(f'{method} alpha={alpha}: {start+min(batch,samples-start)}/{samples} spp', flush=True)
        image, variance, raw = self.read()
        return image, variance, raw, dict(method=method, alpha=alpha, seed=seed, samples=samples, wall_seconds=wall, gpu_seconds=gpu)

    def timed(self, method, alpha, seed, seconds, batch, initial=None):
        if initial is None:
            self.reset()
        else:
            self.queue.write_buffer(self.accumulation, 0, initial)
            self.queue.read_buffer(self.accumulation, 0, 16)
        wall = 0.
        gpu = 0.
        offset = 0
        dispatches = 0
        while wall < seconds:
            first, second = self.dispatch(method, alpha, seed, batch, offset, min(32, self.pixel_count-offset))
            wall += first
            gpu += second
            offset = (offset+32) % self.pixel_count
            dispatches += 1
        image, variance, raw = self.read()
        counts = raw[:, :, 0, 3]
        return image, variance, raw, dict(method=method, alpha=alpha, seed=seed, wall_seconds=wall, gpu_seconds=gpu,
            dispatches=dispatches, spp_mean=float(counts.mean()), spp_min=float(counts.min()), spp_max=float(counts.max()),
            pixel_samples=int(counts.sum()), samples_per_wall_second=float(counts.sum()/wall),
            solver_calls=float(raw[:, :, 2, 0].sum()), capped_bernoulli=float(raw[:, :, 2, 1].sum()),
            nulls=float(raw[:, :, 2, 2].sum()), positive_samples=float(raw[:, :, 1, 3].sum()))


def make_preview(output, rows, references):
    from PIL import Image, ImageDraw
    methods = list(METHODS)
    display = 256
    margin = 40
    canvas = Image.new('RGB', ((len(methods)+1)*display, len(references)*(display+margin)), '#15191f')
    draw = ImageDraw.Draw(canvas)
    exposure = 1/max(np.percentile(np.concatenate([value.ravel() for value in references.values()]), 99), 1e-8)
    for row_index, (alpha, reference) in enumerate(references.items()):
        for column, method in enumerate(methods+['reference']):
            if method == 'reference':
                image = reference
                label = f'Alpha {alpha}: MC reference'
            else:
                record = next(record for record in rows if record['alpha'] == alpha and record['method'] == method)
                image = np.load(output / record['arrays'])['image']
                label = f'{method}: {record["nrmse"]*100:.1f}% NRMSE'
            value = np.maximum(image*exposure, 0)
            value /= 1+value
            value = np.where(value <= .0031308, value*12.92, 1.055*value**(1/2.4)-.055)
            bitmap = Image.fromarray(np.asarray(np.clip(value*255, 0, 255), dtype=np.uint8)).resize((display, display), Image.Resampling.NEAREST)
            canvas.paste(bitmap, (column*display, row_index*(display+margin)+margin))
            draw.text((column*display+8, row_index*(display+margin)+8), label, fill='white')
    canvas.save(output / 'connection-comparison.png')


def run(arguments):
    arguments.output.mkdir(parents=True, exist_ok=False)
    renderer = Renderer(arguments.prepared, arguments.output / 'renderer', arguments.compiler, arguments.minimum_guide_alpha,
                        set(METHODS) if arguments.reference_directory is not None else None)
    rows = []
    references = {}
    reference_report = None
    if arguments.reference_directory is not None:
        reference_report = json.loads((arguments.reference_directory / 'report.json').read_text())
        if reference_report['prepared']['buffer_sha256'] != renderer.manifest['buffer_sha256']:
            raise ValueError('Reference representation/proposal settings differ; generate a new independent reference.')
        if reference_report.get('minimum_guide_alpha', arguments.minimum_guide_alpha) != arguments.minimum_guide_alpha:
            raise ValueError('Reference proposal settings differ; generate a new independent reference.')
        if reference_report['reference_samples'] != arguments.reference_samples:
            raise ValueError('Reference sample count differs; use its recorded count or generate a new reference.')
    for alpha in arguments.alpha:
        calibration = {}
        minimum = arguments.seconds
        for method in METHODS:
            renderer.reset()
            # First use can include lazy backend pipeline work. Calibrate from
            # a second, batched dispatch so the cheap baseline is not assigned
            # a tiny batch based on compilation latency.
            renderer.dispatch(method, alpha, 100, 1)
            duration, gpu = renderer.dispatch(method, alpha, 100, 8)
            image, variance, raw = renderer.read()
            np.savez(arguments.output / f'warmup-{method}-{alpha}.npz', image=image, raw=raw)
            calibration[method] = max(1, min(256, int(.01*8*renderer.pixel_count/(32*duration))))
            minimum = max(minimum, 2*duration)
            print('warmup', method, alpha, duration, 'batch', calibration[method], flush=True)
        for trial in range(arguments.runs):
            methods = list(METHODS)
            methods = methods[trial % len(methods):]+methods[:trial % len(methods)]
            for method in methods:
                image, variance, raw, record = renderer.timed(method, alpha, 1001+trial, minimum, calibration[method])
                name = f'run-{trial}-{method}-{alpha}.npz'
                np.savez(arguments.output / name, image=image, variance=variance, raw=raw)
                record.update(trial=trial, budget_seconds=minimum, arrays=name)
                rows.append(record)
                print(json.dumps(record), flush=True)
            # Matching depends only on completed timing observations, never on
            # image error. Preserve initial and extension arrays separately.
            trial_rows = rows[-len(METHODS):]
            for iteration in range(3):
                target = max(value['wall_seconds'] for value in trial_rows)
                for index, previous in enumerate(trial_rows):
                    if previous['wall_seconds'] >= target*.995:
                        continue
                    initial = np.load(arguments.output / previous['arrays'])['raw']
                    image, variance, raw, extension = renderer.timed(previous['method'], alpha, previous['seed'],
                        target-previous['wall_seconds'], calibration[previous['method']], initial)
                    for key in ('wall_seconds', 'gpu_seconds', 'dispatches'):
                        extension[key] += previous[key]
                    extension.update(trial=trial, budget_seconds=minimum,
                        arrays=f'match-{iteration}-run-{trial}-{previous["method"]}-{alpha}.npz',
                        timing_extensions=previous.get('timing_extensions', [])+[target-previous['wall_seconds']])
                    extension['samples_per_wall_second'] = extension['pixel_samples']/extension['wall_seconds']
                    np.savez(arguments.output / extension['arrays'], image=image, variance=variance, raw=raw)
                    trial_rows[index] = extension
                rows[-len(METHODS):] = trial_rows
            elapsed = [value['wall_seconds'] for value in trial_rows]
            for value in trial_rows:
                value['relative_time_mismatch'] = max(elapsed)/min(elapsed)-1
        if reference_report is None:
            image, variance, raw, record = renderer.fixed('reference', alpha, 671229, arguments.reference_samples, 64)
        else:
            reference_path = arguments.reference_directory / f'reference-{alpha}.npz'
            data = np.load(reference_path)
            image, variance = data['image'], data['variance']
        references[alpha] = image
        if reference_report is None:
            np.savez(arguments.output / f'reference-{alpha}.npz', image=image, variance=variance, raw=raw)
        else:
            shutil.copyfile(reference_path, arguments.output / reference_path.name)
        reference_uncertainty = float(np.sqrt(variance.sum()/max(np.sum(image*image), 1e-30)))
        for row in rows:
            if row['alpha'] != alpha:
                continue
            candidate = np.load(arguments.output / row['arrays'])['image']
            row['mse'] = float(np.mean((candidate-image)**2))
            row['nrmse'] = float(np.sqrt(np.sum((candidate-image)**2)/max(np.sum(image*image), 1e-30)))
            row['reference_estimated_relative_standard_error'] = reference_uncertainty
        report = dict(adapter=renderer.adapter, prepared=renderer.manifest, source=SOURCE,
                      setup_seconds=renderer.setup_seconds, shader_sha256=renderer.shader_hash,
                      scope='isolated caustic integral on original assets; portable SMS adaptation, not original Mitsuba timing',
                      reference_samples=arguments.reference_samples, runs=rows,
                      minimum_guide_alpha=arguments.minimum_guide_alpha,
                      reused_reference=str(arguments.reference_directory) if reference_report is not None else None,
                      limitations=['Reference is an independent-seed Monte Carlo estimate; reported uncertainty is empirical.',
                                   'Slice preprocessing and GPU setup are separate; include them for cold-start comparisons.',
                                   'SMS uses finite-work Bernoulli trials and a numerical re-hit threshold; caps must be reported.',
                                   'No PSMS-ReSTIR comparison or claim of present-day SOTA superiority.'])
        (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    make_preview(arguments.output, rows, references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('prepare')
    command.add_argument('--upstream', type=Path, required=True)
    command.add_argument('--output', type=Path, required=True)
    command.add_argument('--extent', type=int, default=32)
    command.add_argument('--resolution', type=int, default=64)
    command = commands.add_parser('run')
    command.add_argument('--prepared', type=Path, required=True)
    command.add_argument('--compiler', type=Path, required=True)
    command.add_argument('--output', type=Path, required=True)
    command.add_argument('--seconds', type=float, default=10)
    command.add_argument('--runs', type=int, default=3)
    command.add_argument('--reference-samples', type=int, default=16384)
    command.add_argument('--minimum-guide-alpha', type=float, default=.05)
    command.add_argument('--reference-directory', type=Path)
    command.add_argument('--alpha', type=float, nargs='+', default=SOURCE['roughness'])
    command = commands.add_parser('reference')
    command.add_argument('--prepared', type=Path, required=True)
    command.add_argument('--compiler', type=Path, required=True)
    command.add_argument('--output', type=Path, required=True)
    command.add_argument('--samples', type=int, default=65536)
    command.add_argument('--minimum-guide-alpha', type=float, default=.05)
    command.add_argument('--alpha', type=float, nargs='+', default=SOURCE['roughness'])
    arguments = parser.parse_args()
    if arguments.command == 'prepare':
        if arguments.extent < 1 or arguments.resolution < 1:
            parser.error('Extent and resolution must be positive.')
        prepare(arguments.upstream, arguments.output, arguments.extent, arguments.resolution)
    elif arguments.command == 'run':
        if min(arguments.seconds, arguments.runs, arguments.reference_samples, *arguments.alpha) <= 0:
            parser.error('Budgets, sample counts and roughness must be positive.')
        run(arguments)
    else:
        if arguments.samples < 2:
            parser.error('Reference requires at least two samples.')
        arguments.output.mkdir(parents=True, exist_ok=False)
        renderer = Renderer(arguments.prepared, arguments.output / 'renderer', arguments.compiler,
                            arguments.minimum_guide_alpha, {'reference'})
        records = []
        for alpha in arguments.alpha:
            image, variance, raw, record = renderer.fixed('reference', alpha, 671229, arguments.samples, 64)
            np.savez(arguments.output / f'reference-{alpha}.npz', image=image, variance=variance, raw=raw)
            record['estimated_relative_standard_error'] = float(np.sqrt(variance.sum()/max(np.sum(image*image), 1e-30)))
            records.append(record)
            report = dict(source=SOURCE, adapter=renderer.adapter, prepared=renderer.manifest,
                          minimum_guide_alpha=arguments.minimum_guide_alpha, reference_samples=arguments.samples,
                          reference_method='50/50 mixture of explicit-PDF slice and connection-MIS proposals',
                          reference_quality_target=.02, records=records, shader_sha256=renderer.shader_hash)
            (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
