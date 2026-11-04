#!/usr/bin/env python3
"""Run a self-contained two-reflection transport experiment on a physical GPU."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

import numpy as np
import wgpu

from gpu import ROOT, compile_entry
import rough_pair

GROUP_SIZE = 32
REPLICAS_PER_RECEIVER = 32
LIGHT = np.array([0.26, -0.18], dtype=np.float32)
WIDTH = float(np.float32(0.17))
CURVATURE = float(np.float32(0.85))
HALF_EXTENT = float(np.float32(0.68))
SOURCE_OFFSET = np.array([rough_pair.SOURCE_RECEIVER_OFFSET, 0], dtype=np.float32).astype(np.float64)
PARAMETER_LAYOUT = struct.Struct('<4If3I')
TRANSPORT_SOURCE = (ROOT / 'modules/graphics/renderer/render_graphs/rough_pair_slice'
                    / 'main_shaders/rough_pair_transport.slang')
MAX_SAMPLES_PER_LANE = 2**24
ENTRIES = {
    'slice': 'RenderRoughPairSlice',
    'bdpt-mis': 'RenderRoughPairBdpt',
    'shift-mixture': 'RenderRoughPairShift',
    'probe': 'ProbeRoughPair',
    'cached-neighbors': 'RenderRoughPairCache',
    'probe-cache': 'ProbeRoughPairCache',
}
DIRECT_METHODS = ('slice', 'bdpt-mis', 'shift-mixture')
METHODS = (*DIRECT_METHODS, 'cached-neighbors')


class Compute:
    def __init__(self, output, compiler):
        adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
        self.adapter = dict(adapter.info)
        if self.adapter['adapter_type'] == 'CPU':
            raise RuntimeError('The rough-pair benchmark requires a physical GPU.')
        self.device = adapter.request_device_sync(required_features=['timestamp-query'])
        self.queue = self.device.queue
        self.parameters = self.device.create_buffer(size=PARAMETER_LAYOUT.size, usage=wgpu.BufferUsage.UNIFORM |
                                                    wgpu.BufferUsage.COPY_DST)
        self.pipelines = {}
        self.hashes = {}
        for method, entry in ENTRIES.items():
            source = 'rough_pair_cache.slang' if method in ('cached-neighbors', 'probe-cache') else 'rough_pair.slang'
            code = compile_entry(output / method, compiler, source, entry, includes=(TRANSPORT_SOURCE,))
            self.hashes[method] = hashlib.sha256(code.encode()).hexdigest()
            module = self.device.create_shader_module(code=code)
            self.pipelines[method] = self.device.create_compute_pipeline(
                layout='auto', compute=dict(module=module, entry_point=entry))
        self.query = self.device.create_query_set(type='timestamp', count=2)
        self.timestamps = self.device.create_buffer(size=16, usage=wgpu.BufferUsage.QUERY_RESOLVE |
                                                    wgpu.BufferUsage.COPY_SRC)

    def storage(self, data):
        return self.device.create_buffer_with_data(data=np.ascontiguousarray(data),
            usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC | wgpu.BufferUsage.COPY_DST)

    def bind(self, method, buffers):
        entries = [dict(binding=index, resource=dict(buffer=buffer))
                   for index, buffer in {1: self.parameters, **buffers}.items()]
        return self.device.create_bind_group(layout=self.pipelines[method].get_bind_group_layout(0),
                                             entries=entries)

    def dispatch(self, method, bindings, count, batch, offset, seed, grid_width=0):
        self.queue.write_buffer(self.parameters, 0, PARAMETER_LAYOUT.pack(
            count, batch, offset, seed, SOURCE_OFFSET[0], REPLICAS_PER_RECEIVER, grid_width, 0))
        encoder = self.device.create_command_encoder()
        compute = encoder.begin_compute_pass(timestamp_writes=dict(query_set=self.query,
            beginning_of_pass_write_index=0, end_of_pass_write_index=1))
        compute.set_pipeline(self.pipelines[method])
        compute.set_bind_group(0, bindings)
        compute.dispatch_workgroups((count+GROUP_SIZE-1)//GROUP_SIZE, 1, 1)
        compute.end()
        encoder.resolve_query_set(self.query, 0, 2, self.timestamps, 0)
        self.queue.submit([encoder.finish()])
        stamps = np.frombuffer(self.queue.read_buffer(self.timestamps), dtype=np.uint64)
        return (int(stamps[1])-int(stamps[0])) * 1e-9

    def probe(self, seed):
        rng = np.random.default_rng(seed)
        count = 4096
        receivers = rng.uniform(-1.1, 1.1, (count, 2)).astype(np.float32)
        draws = rng.random((count, 8), dtype=np.float32)
        draws = np.clip(draws, 2**-23, 1-2**-23)
        records = np.zeros((count, 2 * len(DIRECT_METHODS) + 1, 4), dtype=np.float32)
        gpu = self.storage(records)
        bindings = self.bind('probe', {0: self.storage(receivers), 3: self.storage(draws), 4: gpu})
        self.dispatch('probe', bindings, count, 1, 0, seed)
        raw = np.frombuffer(self.queue.read_buffer(gpu), dtype=np.float32).reshape(records.shape)
        actual = raw[:, :-1].reshape(count, len(DIRECT_METHODS), 8)
        restored = raw[:, -1]
        if not np.isfinite(actual).all() or np.any(actual[..., 6]):
            raise RuntimeError('GPU probe produced invalid values or an unresolved root.')
        report = {}
        arguments = (draws[:, :5].astype(np.float64), receivers.astype(np.float64),
                     LIGHT.astype(np.float64), WIDTH, CURVATURE, HALF_EXTENT)
        for index, method in enumerate(DIRECT_METHODS):
            if method == 'shift-mixture':
                *oracle, jacobian = rough_pair.sample_shift(*arguments)
                np.testing.assert_allclose(actual[:, index, 7], jacobian, rtol=0.001, atol=0.000025)
                accepted = jacobian > 0
                target_density = rough_pair.evaluate(oracle[0], oracle[1], arguments[1],
                                                       LIGHT, WIDTH, CURVATURE)[1]
                fresh_only = (draws[:, 4] >= 0.5) & (oracle[2] > 0) & (oracle[2] < target_density)
                if not accepted.any() or not fresh_only.any():
                    raise RuntimeError('Shift probe needs both accepted shifts and fresh-only support.')
                target = arguments[1][accepted]
                source = target - SOURCE_OFFSET
                source_first, source_second, _, _ = rough_pair.sample('slice', arguments[0][accepted], source,
                    LIGHT.astype(np.float64), WIDTH, CURVATURE, HALF_EXTENT)
                expected_source = np.column_stack((source_first, source_second))
                np.testing.assert_allclose(restored[accepted], expected_source, rtol=0, atol=0.000025)
                # Differentiate the full CPU shift on independent area-coordinate
                # perturbations. Neither the CPU density ratio nor the GPU
                # determinant enters this finite-difference reference.
                coordinates = expected_source[:128]
                step = 1e-5
                columns = []
                for axis in np.eye(4):
                    above = coordinates + step * axis
                    below = coordinates - step * axis
                    mapped_above = rough_pair.shift(above[:, :2], above[:, 2:], source[:128], target[:128],
                                                    LIGHT, CURVATURE)
                    mapped_below = rough_pair.shift(below[:, :2], below[:, 2:], source[:128], target[:128],
                                                    LIGHT, CURVATURE)
                    columns.append((np.column_stack(mapped_above)-np.column_stack(mapped_below))/(2*step))
                finite_difference = np.linalg.det(np.stack(columns, axis=-1))
                np.testing.assert_allclose(jacobian[accepted][:128], finite_difference, rtol=0, atol=1e-7)
            else:
                oracle = rough_pair.sample(method, *arguments)
            expected = np.column_stack(oracle)
            np.testing.assert_array_equal(actual[:, index, 4] > 0, expected[:, 4] > 0)
            np.testing.assert_allclose(actual[:, index, :6], expected, rtol=0.001, atol=0.000025)
            report[method] = dict(draws=count, nulls=int(np.count_nonzero(expected[:, 4] == 0)),
                                  max_absolute_error=np.max(np.abs(actual[:, index, :6]-expected),
                                                            axis=0).tolist())
            if method == 'shift-mixture':
                report[method].update(
                    accepted_shifts=int(accepted.sum()), fresh_only_support=int(fresh_only.sum()),
                    jacobian_max_absolute_error=float(np.max(np.abs(actual[:, index, 7]-jacobian))),
                    round_trip_max_absolute_error=float(np.max(np.abs(restored[accepted]-expected_source))))
        return report

    def probe_cache(self, seed):
        # An odd grid tests the fresh-only tail of every row and ensures that
        # padding lanes take part in barriers without writing a receiver.
        grid_width = 9
        receivers = make_receivers(grid_width)
        rng = np.random.default_rng(seed)
        probe_rounds = 4
        shape = (len(receivers), probe_rounds * REPLICAS_PER_RECEIVER)
        draws = np.clip(rng.random((*shape, 4), dtype=np.float32), 2**-23, 1-2**-23)
        gpu = self.storage(np.zeros((*shape, 5, 4), dtype=np.float32))
        bindings = self.bind('probe-cache', {0: self.storage(receivers), 3: self.storage(draws), 4: gpu})
        self.dispatch('probe-cache', bindings, cache_lane_count(grid_width), probe_rounds, 0, seed, grid_width)
        raw = np.frombuffer(self.queue.read_buffer(gpu), dtype=np.float32).reshape(*shape, 5, 4)
        if not np.isfinite(raw).all() or np.any(raw[..., 4, 2]):
            raise RuntimeError('Cached GPU probe produced invalid values or an unresolved root.')
        expected = rough_pair.sample_cached_grid(draws.astype(np.float64), receivers.astype(np.float64),
                                                grid_width, LIGHT.astype(np.float64), WIDTH, CURVATURE, HALF_EXTENT)
        for index, kind in enumerate(('fresh', 'shifted')):
            actual_density = raw[..., 2 * index + 1, 0]
            expected_density = expected[f'{kind}_density']
            np.testing.assert_array_equal(actual_density > 0, expected_density > 0)
            np.testing.assert_allclose(actual_density, expected_density, rtol=0.001, atol=0.000025)
            np.testing.assert_allclose(raw[..., 2 * index + 1, 1], expected[kind], rtol=0.001, atol=0.000025)
            if kind == 'shifted':
                present = expected_density > 0
                np.testing.assert_allclose(raw[..., 2 * index, :][present], expected[f'{kind}_positions'][present],
                                           rtol=0, atol=0.000025)
            else:
                np.testing.assert_allclose(raw[..., 0, :], expected['fresh_positions'], rtol=0, atol=0.000025)
        np.testing.assert_array_equal(raw[..., 4, 0], expected['weight'])
        np.testing.assert_allclose(raw[..., 3, 3], expected['jacobian'], rtol=0.001, atol=0.000025)
        np.testing.assert_allclose(raw[..., 4, 1], expected['radiance'], rtol=0.001, atol=0.000025)
        paired = expected['has_neighbor'][:, None]
        fresh_only = paired & (expected['fresh_density'] > 0) & (expected['weight'] == 1)
        accepted = expected['shifted_density'] > 0
        null_source = expected['null_source']
        off_target = expected['off_target']
        if not (fresh_only.any() and accepted.any() and null_source.any() and off_target.any() and np.any(~paired)):
            raise RuntimeError('Cache probe requires accepted shifts, both null cases, fresh-only support and singletons.')
        return dict(rounds=int(np.prod(shape)), singleton_rounds=int(np.count_nonzero(~paired) * shape[1]),
                    accepted_shifts=int(accepted.sum()), fresh_only_support=int(fresh_only.sum()),
                    null_sources=int(null_source.sum()), off_target_shifts=int(off_target.sum()),
                    max_radiance_error=float(np.max(np.abs(raw[..., 4, 1]-expected['radiance']))),
                    max_jacobian_error=float(np.max(np.abs(raw[..., 3, 3]-expected['jacobian']))))

    def render(self, method, receivers, grid_width, seconds, budget, batch, seed):
        lane_count = len(receivers) * REPLICAS_PER_RECEIVER
        is_cached = method == 'cached-neighbors'
        dispatch_count = cache_lane_count(grid_width) if is_cached else lane_count
        zeros = np.zeros((lane_count, 4), dtype=np.float32)
        accumulation = self.storage(zeros)
        bindings = self.bind(method, {0: self.storage(receivers), 2: accumulation})
        # Warm the pipeline before taking timestamps and discard the warm-up.
        self.dispatch(method, bindings, dispatch_count, batch, 0, seed, grid_width)
        self.queue.write_buffer(accumulation, 0, zeros)
        samples = 0
        elapsed = 0.
        beginning = time.perf_counter()
        wall_seconds = 0.
        while (elapsed if budget == 'gpu' else wall_seconds) < seconds:
            if samples + batch > MAX_SAMPLES_PER_LANE:
                raise RuntimeError('Reduce the time budget to keep counts exact in FP32.')
            duration = self.dispatch(method, bindings, dispatch_count, batch, samples, seed, grid_width)
            if duration <= 0:
                raise RuntimeError('GPU timestamp query did not advance.')
            elapsed += duration
            samples += batch
            wall_seconds = time.perf_counter() - beginning
        raw = np.frombuffer(self.queue.read_buffer(accumulation), dtype=np.float32).reshape(-1, 4)
        if not np.isfinite(raw).all() or np.any(raw[:, 3]):
            raise RuntimeError(f'{method} produced an invalid contribution or unresolved root.')
        raw = raw.reshape(len(receivers), REPLICAS_PER_RECEIVER, 4).sum(axis=1, dtype=np.float64)
        samples *= REPLICAS_PER_RECEIVER
        mean = raw[:, 0] / samples
        second = raw[:, 1] / samples
        round_variance = np.maximum(0, second-mean**2) * samples / (samples-1)
        return mean, dict(seed=seed, samples_per_receiver=samples, gpu_seconds=elapsed, wall_seconds=wall_seconds,
                          null_fraction=float(raw[:, 2].sum() / (samples * len(receivers))),
                          shifted_draws_per_round=(grid_width - grid_width % 2) / grid_width if is_cached else 0,
                          mean_round_variance=float(round_variance.mean()),
                          standard_error=np.sqrt(round_variance / samples))


def cache_lane_count(grid_width):
    return grid_width * (grid_width + grid_width % 2) * REPLICAS_PER_RECEIVER


def make_receivers(grid_width):
    axis = np.linspace(-0.9, 0.9, grid_width, dtype=np.float32)
    return np.stack(np.meshgrid(axis, axis), axis=-1).reshape(-1, 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=0.25, help='Seconds per estimator per seed.')
    parser.add_argument('--budget', choices=('gpu', 'wall'), default='gpu')
    parser.add_argument('--extent', type=int, default=8)
    parser.add_argument('--batch', type=int, default=128)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--methods', choices=METHODS, nargs='+', default=DIRECT_METHODS)
    parser.add_argument('--probe-only', action='store_true')
    arguments = parser.parse_args()
    if (not np.isfinite(arguments.seconds) or min(arguments.seconds, arguments.extent, arguments.runs) <= 0
            or not 2 <= arguments.batch <= MAX_SAMPLES_PER_LANE):
        parser.error('Use positive benchmark parameters and a batch of at least two samples.')
    arguments.output.mkdir(parents=True, exist_ok=False)
    compute = Compute(arguments.output, arguments.compiler)
    report = dict(adapter=compute.adapter, shader_sha256=compute.hashes, probe=compute.probe(9137))
    report['probe']['cached-neighbors'] = compute.probe_cache(9137)
    print(json.dumps(report), flush=True)
    if not arguments.probe_only:
        receivers = make_receivers(arguments.extent)
        reference = rough_pair.quadrature(receivers, LIGHT, WIDTH, CURVATURE, HALF_EXTENT, 30)
        refined = rough_pair.quadrature(receivers, LIGHT, WIDTH, CURVATURE, HALF_EXTENT, 34)
        report['quadrature_max_absolute_difference'] = float(np.max(np.abs(reference-refined)))
        np.testing.assert_allclose(reference, refined, rtol=0, atol=1e-7)
        results = []
        arrays = dict(receivers=receivers, reference=refined)
        for trial in range(arguments.runs):
            first = trial % len(arguments.methods)
            methods = arguments.methods[first:] + arguments.methods[:first]
            for method in methods:
                image, result = compute.render(method, receivers, arguments.extent, arguments.seconds,
                                               arguments.budget, arguments.batch, 1000+trial)
                error = image-refined
                standard_error = result.pop('standard_error')
                result.update(method=method, rmse=float(np.sqrt(np.mean(error**2))),
                              mean_error=float(np.mean(error)),
                              max_standardized_error=float(np.max(np.abs(error) / np.maximum(standard_error, 1e-9))))
                if np.any(np.abs(error) > 6 * standard_error + 0.00001):
                    raise RuntimeError(f'{method} seed {1000+trial} disagrees with independent quadrature.')
                arrays[f'{method}-{trial}'] = image
                arrays[f'{method}-standard-error-{trial}'] = standard_error
                results.append(result)
                print(json.dumps(result), flush=True)
        report['runs'] = results
        report['settings'] = dict(extent=arguments.extent, batch=arguments.batch, seconds=arguments.seconds,
                                  budget=arguments.budget, methods=arguments.methods, width=WIDTH, curvature=CURVATURE, half_extent=HALF_EXTENT, light=LIGHT.tolist())
        np.savez(arguments.output / 'linear.npz', **arrays)
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
