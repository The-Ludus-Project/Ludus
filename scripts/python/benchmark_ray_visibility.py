#!/usr/bin/env python3
"""Benchmark native visibility or per-hit caching with frame-interleaved A/B pairs.

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


def summarize_run(directory, scene, variants=VARIANTS):
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
    return {
        'configuration': configuration,
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


def summarize(directories, scenes=SCENES, variants=VARIANTS):
    results = {}
    for scene in scenes:
        runs = [summarize_run(directory, scene, variants) for directory in directories]
        if any(run['configuration'] != runs[0]['configuration'] for run in runs):
            raise ValueError(f'{scene}: configurations differ across runs')
        medians = [run['median_paired_speedup'] for run in runs]
        ratio = statistics.median(medians)
        results[scene] = {
            'runs': runs,
            'median_run_paired_speedup': ratio,
            'median_run_paired_gpu_time_reduction_percent': 100 * (1 - 1 / ratio),
            'run_bootstrap_95_percent_interval': bootstrap_interval(medians),
            'maximum_display_difference': max(run['images']['maximum_display_difference'] for run in runs),
            'maximum_linear_difference': max(run['images']['maximum_linear_difference'] for run in runs),
        }
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--experiment', choices=('visibility', 'hit-cache'), default='visibility')
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
    variants = ('uncached', 'cached') if is_cache else VARIANTS
    scenes = SCENES + ('textured-nearest', 'textured-linear') if is_cache else SCENES
    prefix = 'LUDUS_HIT_CACHE' if is_cache else 'LUDUS_VISIBILITY'
    tag = '[hit-cache]' if is_cache else '[visibility]'
    metadata = {
        'created_at_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(),
        'machine': platform.machine(),
        'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
        'runs': options.runs,
        'experiment': options.experiment,
        'variants': variants,
        'scenes': scenes,
        'pairing': 'One device/queue/surface per scene; both pipelines ready; AB,BA per matched sample; reverse initial order between processes.',
        'timing_scope': 'GPU compute plus output raster; wall completion and CPU inter-pair submission gaps are separate.',
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
    results = summarize(directories, scenes, variants)
    report = options.output / 'comparison.json'
    report.write_text(json.dumps(results, indent=2) + '\n')
    print(f'Report: {report}', flush=True)
    for scene, result in results.items():
        print(f"{scene}: paired speedup {result['median_run_paired_speedup']:.4f}x, "
              f"GPU time reduction {result['median_run_paired_gpu_time_reduction_percent']:.2f}%, "
              f"95% run interval {result['run_bootstrap_95_percent_interval']}, "
              f"maximum RGBA8 difference {result['maximum_display_difference']}", flush=True)
    return int(any(result['maximum_display_difference'] != 0 for result in results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
