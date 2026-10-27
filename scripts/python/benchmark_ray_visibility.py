#!/usr/bin/env python3
"""Benchmark ray optimizations or GPU pass probes with frame-interleaved A/B pairs.

The native paired fixture owns both immutable pipelines on one device and
alternates AB, BA at matched sample addresses. No image readback occurs between
paired frames. GPU intervals include compute and output raster, not just traversal.
Each independent process repeats its selected scenes; bootstrap intervals resample run
medians, not correlated frames within a run. Timing never establishes image parity.
"""
import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import struct
import subprocess
import sys

SCENES = ('empty', 'opaque', 'masked', 'mixed')
VARIANTS = ('full', 'visibility')
CONFIGURATION_FIELDS = ('version', 'extent', 'target_samples', 'warmup_samples')
BOOTSTRAP_REPETITIONS = 10000
CONFIDENCE_TAIL = 0.025
NANOSECONDS_PER_MILLISECOND = 1e6
COLOR_CHANNELS = 4
ACCUMULATION_WORDS = 2


def read_configuration(directory, scene):
    data = (directory / f'{scene}.configuration.bin').read_bytes()
    encoding = '<' + 'I' * len(CONFIGURATION_FIELDS)
    if len(data) != struct.calcsize(encoding):
        raise ValueError(f'{scene}: invalid configuration extent')
    configuration = dict(zip(CONFIGURATION_FIELDS, struct.unpack(encoding, data)))
    if configuration['version'] != 1:
        raise ValueError(f'{scene}: unsupported benchmark format')
    if configuration['extent'] < 1 or not 0 <= configuration['warmup_samples'] < configuration['target_samples']:
        raise ValueError(f'{scene}: invalid benchmark dimensions or sample counts')
    return configuration


def read_numbers(path, encoding, count):
    data = path.read_bytes()
    if len(data) != struct.calcsize(encoding) * count:
        raise ValueError(f'{path}: expected {count} {encoding} values')
    values = struct.unpack('<' + encoding * count, data)
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError(f'{path}: invalid measurement')
    return values


def compare_images(directory, scene, configuration, variants=VARIANTS):
    pixel_count = configuration['extent'] ** 2
    paths = [directory / variant for variant in variants]
    images = [(path / f'{scene}.rgba').read_bytes() for path in paths]
    if any(len(data) != pixel_count * COLOR_CHANNELS for data in images):
        raise ValueError(f'{scene}: invalid RGBA8 extent')
    differences = [abs(first - second) for first, second in zip(*images)]
    values = []
    for path in paths:
        data = array('f')
        data.frombytes((path / f'{scene}.linear.bin').read_bytes())
        if sys.byteorder != 'little':
            data.byteswap()
        if len(data) != pixel_count * COLOR_CHANNELS * ACCUMULATION_WORDS:
            raise ValueError(f'{scene}: invalid linear accumulation extent')
        values.append(data)
    maximum = 0.0
    squared_error = 0.0
    signed_error = 0.0
    for pixel in range(pixel_count):
        offset = pixel * COLOR_CHANNELS * ACCUMULATION_WORDS
        if any(data[offset + 3] != configuration['target_samples'] for data in values):
            raise ValueError(f'{scene}: incomplete GPU sample coverage at pixel {pixel}')
        for channel in range(3):
            first, second = (data[offset + channel] for data in values)
            if not math.isfinite(first) or not math.isfinite(second):
                raise ValueError(f'{scene}: nonfinite linear RGB at pixel {pixel}')
            difference = second - first
            maximum = max(maximum, abs(difference))
            squared_error += difference * difference
            signed_error += difference
    channels = pixel_count * 3
    return {
        'changed_display_channels': sum(value != 0 for value in differences),
        'maximum_display_difference': max(differences),
        'maximum_linear_difference': maximum,
        'linear_rmse': math.sqrt(squared_error / channels),
        'mean_signed_linear_difference': signed_error / channels,
    }


PASS_INFO_ENCODING = '<QddQQQQ'
PASS_INFO_FIELDS = ('version', 'transport_tick_ns', 'output_tick_ns', 'transport_valid_bits',
                    'output_valid_bits', 'transport_scope_identity', 'output_scope_identity')


def read_pass_timings(directory, scene, count):
    data = (directory / f'{scene}.pass-info.bin').read_bytes()
    if len(data) != struct.calcsize(PASS_INFO_ENCODING):
        raise ValueError(f'{scene}: invalid pass metadata extent')
    metadata = dict(zip(PASS_INFO_FIELDS, struct.unpack(PASS_INFO_ENCODING, data)))
    if metadata['version'] != 1:
        raise ValueError(f'{scene}: unsupported pass metadata version')
    for role in ('transport', 'output'):
        tick = metadata[f'{role}_tick_ns']
        if not math.isfinite(tick) or tick < 0 or metadata[f'{role}_valid_bits'] > 64:
            raise ValueError(f'{scene}: invalid pass timestamp precision')
        if metadata[f'{role}_scope_identity'] == 0:
            raise ValueError(f'{scene}: missing pass scope identity')
    if metadata['transport_scope_identity'] == metadata['output_scope_identity']:
        raise ValueError(f'{scene}: duplicate pass scope identities')
    names = (directory / f'{scene}.pass-names.bin').read_bytes()
    offset = 0
    for role in ('transport', 'output'):
        if offset + struct.calcsize('<I') > len(names):
            raise ValueError(f'{scene}: truncated pass name length')
        length, = struct.unpack_from('<I', names, offset)
        offset += struct.calcsize('<I')
        if length == 0 or length > len(names) - offset:
            raise ValueError(f'{scene}: invalid pass name extent')
        name = names[offset:offset + length].decode('utf-8')
        if '\0' in name:
            raise ValueError(f'{scene}: embedded zero in pass name')
        metadata[f'{role}_scope_name'] = name
        offset += length
    if offset != len(names):
        raise ValueError(f'{scene}: trailing pass name data')
    transport = read_numbers(directory / f'{scene}.transport.gpu.bin', 'd', count)
    output = read_numbers(directory / f'{scene}.output.gpu.bin', 'd', count)
    # Calibrated CPU-domain intervals locate overlap on one shared clock. A zero
    # availability word means no alignment; it never turns into a zero overlap.
    interval_words = 6
    intervals = read_numbers(directory / f'{scene}.pass-intervals.bin', 'Q', count * interval_words)
    overlaps = []
    tails = []
    uncertainty = []
    for index in range(count):
        first, last, output_first, output_last, bound, available = intervals[index * interval_words:(index + 1) * interval_words]
        if available not in (0, 1):
            raise ValueError(f'{scene}: invalid pass interval availability')
        if available == 0:
            if any((first, last, output_first, output_last, bound)):
                raise ValueError(f'{scene}: unavailable pass interval contains timestamps')
            continue
        if last < first or output_last < output_first:
            raise ValueError(f'{scene}: reversed pass interval')
        overlaps.append(max(0, min(last, output_last) - max(first, output_first)))
        tails.append(max(0, output_last - last))
        uncertainty.append(bound)
    return {
        'aligned_samples': len(overlaps),
        'unaligned_samples': count - len(overlaps),
        'overlap_median_ms': statistics.median(overlaps) / NANOSECONDS_PER_MILLISECOND if overlaps else None,
        'output_tail_median_ms': statistics.median(tails) / NANOSECONDS_PER_MILLISECOND if tails else None,
        'maximum_clock_uncertainty_ns': max(uncertainty) if uncertainty else None,
        'metadata': metadata,
        'samples': count,
        'transport_median_ms': statistics.median(transport) / NANOSECONDS_PER_MILLISECOND,
        'output_median_ms': statistics.median(output) / NANOSECONDS_PER_MILLISECOND,
        'transport_zero_samples': sum(value == 0 for value in transport),
        'output_zero_samples': sum(value == 0 for value in output),
    }


def summarize_run(directory, scene, variants=VARIANTS, pass_variants=()):
    configuration = read_configuration(directory, scene)
    count = configuration['target_samples'] - configuration['warmup_samples']
    gpu = [read_numbers(directory / variant / f'{scene}.gpu.bin', 'd', count) for variant in variants]
    wall = [read_numbers(directory / variant / f'{scene}.wall.bin', 'Q', count) for variant in variants]
    gaps = read_numbers(directory / f'{scene}.gap.bin', 'Q', count)
    order = read_numbers(directory / f'{scene}.order.bin', 'B', count)
    if any(value not in (0, 1) for value in order) or abs(sum(order) - count / 2) > 0.5:
        raise ValueError(f'{scene}: pair orders are not balanced')
    if any(value <= 0 for row in gpu for value in row):
        raise ValueError(f'{scene}: GPU measurements must be positive')
    ratios = [before / after for before, after in zip(*gpu)]
    if any(not math.isfinite(value) or value <= 0 for value in ratios):
        raise ValueError(f'{scene}: unrepresentable paired frame ratio')
    if any(variant not in variants for variant in pass_variants):
        raise ValueError(f'{scene}: unknown profiled variant')
    passes = {}
    if pass_variants:
        for variant in variants:
            if variant in pass_variants:
                passes[variant] = read_pass_timings(directory / variant, scene, count)
            elif any((directory / variant / f'{scene}{suffix}').exists()
                     for suffix in ('.pass-info.bin', '.pass-names.bin', '.pass-intervals.bin', '.transport.gpu.bin', '.output.gpu.bin')):
                raise ValueError(f'{scene}: unexpected pass data for unprobed variant {variant}')
    return {
        'configuration': configuration,
        'pass_timings': passes,
        'pairs': count,
        'gpu_median_ms': [statistics.median(row) / NANOSECONDS_PER_MILLISECOND for row in gpu],
        'wall_median_ms': [statistics.median(row) / NANOSECONDS_PER_MILLISECOND for row in wall],
        'median_paired_speedup': statistics.median(ratios),
        'median_paired_delta_ms': statistics.median(before - after for before, after in zip(*gpu)) / NANOSECONDS_PER_MILLISECOND,
        'baseline_first_speedup': statistics.median(ratio for ratio, first in zip(ratios, order) if first == 1),
        'visibility_first_speedup': statistics.median(ratio for ratio, first in zip(ratios, order) if first == 0),
        'pair_gap_median_ms': statistics.median(gaps) / NANOSECONDS_PER_MILLISECOND,
        'pair_gap_maximum_ms': max(gaps) / NANOSECONDS_PER_MILLISECOND,
        'images': compare_images(directory, scene, configuration, variants),
    }


def bootstrap_interval(run_medians):
    if len(run_medians) < 2:
        return None
    generator = random.Random(0)
    estimates = sorted(statistics.median(generator.choices(run_medians, k=len(run_medians)))
                       for _ in range(BOOTSTRAP_REPETITIONS))
    return [estimates[int(BOOTSTRAP_REPETITIONS * CONFIDENCE_TAIL)],
            estimates[int(BOOTSTRAP_REPETITIONS * (1 - CONFIDENCE_TAIL)) - 1]]


def summarize(directories, scenes=SCENES, variants=VARIANTS, pass_variants=()):
    results = {}
    for scene in scenes:
        runs = [summarize_run(directory, scene, variants, pass_variants) for directory in directories]
        if any(run['configuration'] != runs[0]['configuration'] for run in runs):
            raise ValueError(f'{scene}: configurations differ across runs')
        medians = [run['median_paired_speedup'] for run in runs]
        ratio = statistics.median(medians)
        pass_summaries = {}
        for variant in pass_variants:
            stage_runs = [run['pass_timings'][variant] for run in runs]
            # Scope identities are producer/device-generation local. Preserve them
            # per run; only precision metadata can be compared across processes.
            precision_fields = tuple(name for name in PASS_INFO_FIELDS if not name.endswith('scope_identity'))
            precision = {name: stage_runs[0]['metadata'][name] for name in precision_fields}
            scope_names = {role: stage_runs[0]['metadata'][f'{role}_scope_name'] for role in ('transport', 'output')}
            if any({name: run['metadata'][name] for name in precision_fields} != precision for run in stage_runs):
                raise ValueError(f'{scene}: pass timestamp precision differs across runs')
            if any({role: run['metadata'][f'{role}_scope_name'] for role in ('transport', 'output')} != scope_names
                   for run in stage_runs):
                raise ValueError(f'{scene}: pass scope names differ across runs')
            pass_summaries[variant] = {
                'precision': precision,
                'samples': sum(run['samples'] for run in stage_runs),
                'aligned_samples': sum(run['aligned_samples'] for run in stage_runs),
                'unaligned_samples': sum(run['unaligned_samples'] for run in stage_runs),
                'overlap_median_ms': statistics.median(run['overlap_median_ms'] for run in stage_runs
                                                      if run['overlap_median_ms'] is not None)
                    if any(run['overlap_median_ms'] is not None for run in stage_runs) else None,
                'output_tail_median_ms': statistics.median(run['output_tail_median_ms'] for run in stage_runs
                                                         if run['output_tail_median_ms'] is not None)
                    if any(run['output_tail_median_ms'] is not None for run in stage_runs) else None,
                'maximum_clock_uncertainty_ns': max(run['maximum_clock_uncertainty_ns'] for run in stage_runs
                                                   if run['maximum_clock_uncertainty_ns'] is not None)
                    if any(run['maximum_clock_uncertainty_ns'] is not None for run in stage_runs) else None,
                'scope_names': scope_names,
                'transport_median_ms': statistics.median(run['transport_median_ms'] for run in stage_runs),
                'output_median_ms': statistics.median(run['output_median_ms'] for run in stage_runs),
                'transport_run_bootstrap_95_percent_interval_ms': bootstrap_interval(
                    [run['transport_median_ms'] for run in stage_runs]),
                'output_run_bootstrap_95_percent_interval_ms': bootstrap_interval(
                    [run['output_median_ms'] for run in stage_runs]),
                'transport_zero_samples': sum(run['transport_zero_samples'] for run in stage_runs),
                'output_zero_samples': sum(run['output_zero_samples'] for run in stage_runs),
            }
        results[scene] = {
            'runs': runs,
            'pass_timings': pass_summaries,
            'median_run_paired_speedup': ratio,
            'median_run_paired_gpu_time_reduction_percent': 100 * (1 - 1 / ratio),
            'run_bootstrap_95_percent_interval': bootstrap_interval(medians),
            'maximum_display_difference': max(run['images']['maximum_display_difference'] for run in runs),
            'maximum_linear_difference': max(run['images']['maximum_linear_difference'] for run in runs),
        }
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--experiment', choices=('visibility', 'hit-cache', 'pass-timings'), default='visibility')
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--runs', type=int, default=5)
    options = parser.parse_args()
    if options.runs < 1:
        parser.error('--runs must be positive')
    if options.output.exists():
        parser.error('--output must be a new directory to preserve measurement evidence')
    options.output.mkdir(parents=True)
    executable = options.output / 'benchmark-tests'
    shutil.copy2(options.executable.resolve(), executable)
    is_cache = options.experiment == 'hit-cache'
    is_pass = options.experiment == 'pass-timings'
    variants = ('unprobed', 'profiled') if is_pass else ('uncached', 'cached') if is_cache else VARIANTS
    pass_variants = ('profiled',) if is_pass else ()
    scenes = SCENES + ('textured-nearest', 'textured-linear') if is_cache or is_pass else SCENES
    prefix = 'LUDUS_RAY_PASS' if is_pass else 'LUDUS_HIT_CACHE' if is_cache else 'LUDUS_VISIBILITY'
    tag = '[ray-pass-timings]' if is_pass else '[hit-cache]' if is_cache else '[visibility]'
    metadata = {
        'created_at_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(),
        'machine': platform.machine(),
        'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
        'runs': options.runs,
        'experiment': options.experiment,
        'variants': variants,
        'per_pass_variants': pass_variants,
        'scenes': scenes,
        'pairing': 'One device/queue/surface per scene; both pipelines ready; AB,BA per matched sample; reverse initial order between processes.',
        'timing_scope': 'Frame: compute plus output raster. Optional pass timestamps: transport compute encoder and output raster stage envelope, including stalls. Pass intervals are not an additive partition of frame time; CPU wall time is separate.',
        'interval_method': '95% percentile bootstrap of independent process run-median paired ratios; correlated frames are not resampled as independent observations.',
    }
    (options.output / 'measurement.json').write_text(json.dumps(metadata, indent=2) + '\n')
    directories = []
    for run in range(options.runs):
        directory = options.output / f'run-{run + 1}'
        for variant in variants:
            (directory / variant).mkdir(parents=True)
        environment = dict(os.environ)
        environment[prefix + '_BENCHMARK_DIRECTORY'] = str(directory.resolve())
        if run % 2:
            environment[prefix + '_REVERSE_ORDER'] = '1'
        else:
            environment.pop(prefix + '_REVERSE_ORDER', None)
        print(f'Interleaved A/B run {run + 1}/{options.runs}', flush=True)
        subprocess.run([str(executable.resolve()), tag, '--reporter', 'compact'],
                       env=environment, check=True)
        directories.append(directory)
    results = summarize(directories, scenes, variants, pass_variants)
    report = options.output / 'comparison.json'
    report.write_text(json.dumps(results, indent=2) + '\n')
    print(f'Report: {report}', flush=True)
    for scene, result in results.items():
        if is_pass:
            passes = result['pass_timings']['profiled']
            ratio = result['median_run_paired_speedup']
            alignment = (f"overlap {passes['overlap_median_ms']:.4f} ms, "
                         f"output tail {passes['output_tail_median_ms']:.4f} ms"
                         if passes['overlap_median_ms'] is not None else 'alignment unavailable')
            print(f"{scene}: transport {passes['transport_median_ms']:.4f} ms, "
                  f"output envelope {passes['output_median_ms']:.4f} ms, "
                  f"{alignment} ({passes['aligned_samples']}/{passes['samples']} aligned; "
                  f"max clock uncertainty {passes['maximum_clock_uncertainty_ns']} ns), "
                  f"profiled frame overhead {100 * (1 / ratio - 1):.2f}%, "
                  f"95% unprobed/profiled interval {result['run_bootstrap_95_percent_interval']}, "
                  f"maximum RGBA8 difference {result['maximum_display_difference']}", flush=True)
        else:
            print(f"{scene}: paired speedup {result['median_run_paired_speedup']:.4f}x, "
                  f"GPU time reduction {result['median_run_paired_gpu_time_reduction_percent']:.2f}%, "
                  f"95% run interval {result['run_bootstrap_95_percent_interval']}, "
                  f"maximum RGBA8 difference {result['maximum_display_difference']}", flush=True)
    return int(any(result['maximum_display_difference'] != 0 for result in results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
