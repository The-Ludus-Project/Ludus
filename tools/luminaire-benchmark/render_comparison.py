#!/usr/bin/env python3
"""Equal-time controlled-room renders using Ludus samplers on headless Metal.

The driver supplies deterministic primary rays and the generated room triangles.
It is a research compute harness, not the Ludus editor or native BVH frame loop.
Reported wall budgets include uniform update, encoding, submission, completion
and timestamp readback; compilation, warmup, training and image readback are separate.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import numpy as np

from benchmark import ROOT


def load_triangles(room):
    records = []
    for path in sorted(room.glob('receiver-*.obj')):
        vertices = []
        normal = None
        for line in path.read_text().splitlines():
            fields = line.split()
            if fields[0] == 'v':
                vertices.append(list(map(float, fields[1:])))
            elif fields[0] == 'vn':
                normal = list(map(float, fields[1:]))
            elif fields[0] == 'f':
                for field in fields[1:]:
                    records.append(vertices[int(field.split('/')[0])-1] + [0])
                records.append(normal + [0])
    return np.asarray(records, dtype=np.float32)


class Renderer:
    def __init__(self, graph, room, compiler, output, extent):
        self.extent = extent
        source_root = output / 'source'
        shutil.copytree(graph / 'main_shaders', source_root)
        for name, owner in [('math.slang', 'modules/foundation/math/shaders'),
                            ('raster_bindings.slang', 'modules/graphics/rhi/shaders')]:
            shutil.copyfile(ROOT / owner / name, source_root / name)
        shader = source_root / 'render_room.slang'
        shutil.copyfile(Path(__file__).parent / 'shaders/render_room.slang', shader)
        import wgpu
        self.wgpu = wgpu
        adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
        self.adapter = dict(adapter.info)
        if self.adapter['adapter_type'] == 'CPU':
            raise ValueError('A physical GPU is required.')
        self.device = adapter.request_device_sync(required_features=['timestamp-query'])
        self.queue = self.device.queue
        self.triangles = load_triangles(room)
        self.mesh = self.device.create_buffer_with_data(data=self.triangles, usage=wgpu.BufferUsage.STORAGE)
        self.accumulation = self.device.create_buffer(size=extent*extent*32,
            usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC | wgpu.BufferUsage.COPY_DST)
        self.parameters = self.device.create_buffer(size=32, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
        self.query = self.device.create_query_set(type='timestamp', count=2)
        self.timestamps = self.device.create_buffer(size=16, usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC)
        self.pipelines = {}
        self.bindings = {}
        self.shader_hashes = {}
        beginning = time.perf_counter()
        for method, entry in [('slice', 'RenderRoomSlice'), ('portable-purelight', 'RenderRoomPureLight')]:
            directory = output / ('compile-' + method)
            command = [sys.executable, str(ROOT/'cmake/shaders/compile_shader.py'), '--source', str(shader),
                       '--output', str(directory), '--name', entry, '--compiler', str(compiler),
                       '--lock', str(ROOT/'config/shader_toolchain.json'), '--graph-stage', 'compute', '--graph-target', 'wgsl']
            subprocess.run(command, check=True)
            source = (directory / 'artifact.wgsl').read_text()
            self.shader_hashes[method] = hashlib.sha256(source.encode()).hexdigest()
            module = self.device.create_shader_module(code=source)
            pipeline = self.device.create_compute_pipeline(layout='auto', compute=dict(module=module, entry_point=entry))
            self.pipelines[method] = pipeline
            self.bindings[method] = self.device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=[
                dict(binding=0, resource=dict(buffer=self.mesh, offset=0, size=self.triangles.nbytes)),
                dict(binding=1, resource=dict(buffer=self.accumulation, offset=0, size=extent*extent*32)),
                dict(binding=2, resource=dict(buffer=self.parameters, offset=0, size=32))])
        self.setup_seconds = time.perf_counter()-beginning
        self.reset()

    def reset(self):
        self.queue.write_buffer(self.accumulation, 0, np.zeros(self.extent*self.extent*8, dtype=np.float32))
        self.queue.read_buffer(self.accumulation, 0, 16)

    def dispatch(self, method, seed, samples, row, rows, column=0, columns=None):
        columns = self.extent if columns is None else columns
        beginning = time.perf_counter()
        self.queue.write_buffer(self.parameters, 0, struct.pack('<8I', self.extent, self.extent, samples, seed, row, rows, column, columns))
        encoder = self.device.create_command_encoder()
        compute = encoder.begin_compute_pass(timestamp_writes=dict(query_set=self.query,
            beginning_of_pass_write_index=0, end_of_pass_write_index=1))
        compute.set_pipeline(self.pipelines[method])
        compute.set_bind_group(0, self.bindings[method])
        compute.dispatch_workgroups((columns+7)//8, (rows+7)//8, 1)
        compute.end()
        encoder.resolve_query_set(self.query, 0, 2, self.timestamps, 0)
        self.queue.submit([encoder.finish()])
        timestamps = np.frombuffer(self.queue.read_buffer(self.timestamps), dtype=np.uint64)
        wall = time.perf_counter()-beginning
        gpu = int(timestamps[1])-int(timestamps[0])
        if gpu < 0:
            raise ValueError('GPU timestamp interval is reversed.')
        return wall, gpu*1e-9

    def image(self):
        data = bytes(self.queue.read_buffer(self.accumulation))
        raw = np.frombuffer(data, dtype=np.float32).reshape(self.extent, self.extent, 2, 4)
        counts = np.frombuffer(data, dtype=np.uint32).reshape(self.extent, self.extent, 2, 4)[:, :, 0, 3]
        if np.any(counts == 0):
            raise ValueError('Budget left pixels without samples; use a longer budget.')
        image = raw[:, :, 0, :3] / counts[:, :, None]
        if not np.all(np.isfinite(image)) or np.any(image < 0):
            raise ValueError('Invalid linear image; preserve the raw run.')
        return image, counts

    def timed(self, method, seed, budget, calibration, initial=None):
        if initial is None:
            self.reset()
        else:
            self.queue.write_buffer(self.accumulation, 0, initial)
            self.queue.read_buffer(self.accumulation, 0, 16)
        batch = calibration[method]
        wall = 0.
        gpu = 0.
        dispatches = 0
        zero_timestamps = 0
        bands = [(row, 0) for row in range(0, self.extent, 8)]
        rng = np.random.default_rng(seed)
        rng.shuffle(bands)
        while wall < budget:
            row, column = bands[dispatches % len(bands)]
            duration, gpu_duration = self.dispatch(method, seed, batch, row, min(8, self.extent-row), column, self.extent)
            wall += duration
            gpu += gpu_duration
            zero_timestamps += int(gpu_duration == 0)
            dispatches += 1
            if dispatches % len(bands) == 0:
                rng.shuffle(bands)
        image, counts = self.image()
        return image, counts, dict(method=method, seed=seed, budget_seconds=budget,
            wall_seconds=wall, gpu_seconds=gpu, zero_timestamp_intervals=zero_timestamps, dispatches=dispatches, samples_per_dispatch=batch,
            spp_min=int(counts.min()), spp_max=int(counts.max()), spp_mean=float(counts.mean()),
            pixel_samples=int(counts.sum()), samples_per_wall_second=float(counts.sum()/wall),
            samples_per_gpu_second=float(counts.sum()/gpu) if gpu > 0 else None)

    def reference(self, seed, samples, batch):
        self.reset()
        beginning = time.perf_counter()
        for offset in range(0, samples, batch):
            for row in range(0, self.extent, 8):
                self.dispatch('slice', seed, min(batch, samples-offset), row, min(8, self.extent-row))
            if (offset//batch) % 32 == 0:
                print(f'reference seed={seed}: {offset+min(batch,samples-offset)}/{samples} spp', flush=True)
        image, counts = self.image()
        return image, time.perf_counter()-beginning


def metrics(image, reference):
    difference = image-reference
    mse = float(np.mean(difference**2))
    return dict(mse=mse, rmse=math.sqrt(mse),
                normalized_rmse=math.sqrt(float(np.sum(difference**2)/np.sum(reference**2))),
                relative_mae=float(np.sum(np.abs(difference))/np.sum(np.abs(reference))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graph', type=Path, required=True)
    parser.add_argument('--room', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--extent', type=int, default=128)
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--reference-samples', type=int, default=16384)
    parser.add_argument('--pilot', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    renderer = Renderer(args.graph, args.room, args.compiler, args.output, args.extent)
    print('adapter',renderer.adapter, flush=True)
    calibration = {}
    timings = {}
    for method in renderer.pipelines:
        renderer.reset()
        full_wall = 0.
        full_gpu = 0.
        for row in range(0,args.extent,8):
            wall,gpu=renderer.dispatch(method,100,1,row,min(8,args.extent-row))
            full_wall+=wall;full_gpu+=gpu
            print(f'warmup {method}: rows={row+8}/{args.extent}, wall={full_wall:.3f}s',flush=True)
        image,counts=renderer.image()
        np.savez(args.output/f'warmup-{method}.npz',image=image,samples=counts)
        calibration[method] = 1
        timings[method] = dict(full_frame_wall_seconds=full_wall,full_frame_gpu_seconds=full_gpu)
    budget=max(args.seconds,math.ceil(2*max(record['full_frame_wall_seconds'] for record in timings.values())))
    print('shared wall budget',budget,flush=True)
    if args.pilot:
        print(json.dumps(dict(calibration=timings, batch=calibration), indent=2), flush=True)
        return
    rows = []
    for run in range(args.runs):
        order = ['slice', 'portable-purelight'] if run % 2 == 0 else ['portable-purelight', 'slice']
        pair = {}
        snapshots = {}
        for method in order:
            target = pair['portable-purelight'][2]['wall_seconds'] if method == 'slice' and 'portable-purelight' in pair else budget
            image, counts, record = renderer.timed(method, 1001+run, target, calibration)
            pair[method]=(image,counts,record)
            snapshots[method]=bytes(renderer.queue.read_buffer(renderer.accumulation))
            print('initial completed',json.dumps(record),flush=True)
        # Match actual completed rendering time, using only timing observations.
        # If the slice run came first, restore its raw accumulation and extend it
        # to the slower method's actual time; reference/error is not consulted.
        first=pair['slice'][2]
        second=pair['portable-purelight'][2]
        if first['wall_seconds'] < second['wall_seconds']:
            image,counts,extension=renderer.timed('slice',1001+run,second['wall_seconds']-first['wall_seconds'],calibration,snapshots['slice'])
            for key in ('wall_seconds','gpu_seconds','dispatches','zero_timestamp_intervals'):
                extension[key]+=first[key]
            extension['budget_seconds']=second['wall_seconds']
            extension['samples_per_wall_second']=extension['pixel_samples']/extension['wall_seconds']
            extension['samples_per_gpu_second']=extension['pixel_samples']/extension['gpu_seconds'] if extension['gpu_seconds'] else None
            pair['slice']=(image,counts,extension)
        difference=abs(pair['slice'][2]['wall_seconds']-pair['portable-purelight'][2]['wall_seconds'])
        if difference > .01*max(pair['slice'][2]['wall_seconds'],pair['portable-purelight'][2]['wall_seconds']):
            raise ValueError('Completed render budgets differ by more than 1%; preserve and investigate.')
        for method in ('slice','portable-purelight'):
            image,counts,record=pair[method]
            np.savez(args.output/f'{method}-{run}.npz',image=image,samples=counts)
            rows.append(record)
            print('matched completed',json.dumps(record),flush=True)
    largest = max(row['spp_max'] for row in rows)
    reference_samples = max(args.reference_samples, 8*largest)
    # Both references are independent of measured renders and each other.
    first, first_seconds = renderer.reference(80001, reference_samples, 256)
    np.save(args.output/'reference-first.npy',first)
    second, second_seconds = renderer.reference(80002, reference_samples, 256)
    reference = .5*(first+second)
    np.save(args.output/'reference.npy',reference)
    np.save(args.output/'reference-second.npy',second)
    for index, row in enumerate(rows):
        run = row['seed']-1001
        image=np.load(args.output/f'{row["method"]}-{run}.npz')['image']
        row.update(metrics(image,reference))
    report = dict(schema=1, adapter=renderer.adapter, extent=[args.extent,args.extent],
        scope=__doc__, primary_rays='fixed pixel centres; no antialiasing',
        geometry_sha256=hashlib.sha256(renderer.triangles.tobytes()).hexdigest(),
        model_sha256=hashlib.sha256((args.graph/'portable-model.npz').read_bytes()).hexdigest(),
        shader_sha256=renderer.shader_hashes, setup_seconds=renderer.setup_seconds,
        calibration=timings, reference_samples_per_half=reference_samples,
        reference_seconds=first_seconds+second_seconds,
        reference_half_difference=metrics(first,second),
        reference_estimated_normalized_rmse=math.sqrt(float(np.sum((first-second)**2)/(4*np.sum(reference**2)))),
        training=json.loads((args.graph/'comparison-model.json').read_text()), results=rows,
        limitations=['portable quantized 1000-step model trained on eight receivers',
                     'linear triangle traversal, not Ludus BVH/editor timing',
                     'CPU training and model loading excluded from steady-state budgets'])
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(args.output/'report.json',flush=True)


if __name__=='__main__':
    main()
