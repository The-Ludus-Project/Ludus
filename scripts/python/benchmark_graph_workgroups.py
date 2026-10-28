#!/usr/bin/env python3
"""Compare authored compute/MIS graph workgroups with paired frames and A/A controls.

The fixture cooks the actual templates into 64x1 and 8x8 runtime packages. Both
immutable graphs share one device; paired samples alternate AB/BA, with both
frame and per-operation probes armed. Readbacks happen after the timed sequence.
Independent process medians are the bootstrap units, rather than correlated frames.
See docs/architecture/renderer-systems.md for fixture scope and acceptance limits.
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
import shutil
import statistics
import struct
import subprocess
import sys

from benchmark_ray_visibility import bootstrap_interval, read_numbers

SCENES = ('opaque', 'cutout', 'textured')
GRAPHS = {'compute': ('primary', 'shade'), 'mis': ('primary', 'mis', 'light', 'bsdf')}
VARIANTS = ('baseline', 'candidate')
EXTENT = 512
SAMPLES = 128
WARMUP = 16
MEASURED = SAMPLES - WARMUP
RAW_TOLERANCE = 1e-5
NAME_BYTES = 128
METADATA_HEADER = struct.Struct('<QQ')
METADATA_RECORD = struct.Struct('<QQQQQd128s')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_metadata(path, count):
    data = path.read_bytes()
    require(len(data) == METADATA_HEADER.size + count * METADATA_RECORD.size,
            f'{path}: invalid metadata size')
    require(METADATA_HEADER.unpack_from(data) == (1, count), f'{path}: invalid metadata header')
    result = []
    for index in range(count):
        kind, identity, logical_count, scope_index, bits, tick, encoded = METADATA_RECORD.unpack_from(
            data, METADATA_HEADER.size + index * METADATA_RECORD.size)
        require(kind == (1 if index == 0 or index % 2 else 0), f'{path}: unexpected operation kind')
        require(identity > 0 and logical_count > 0 and scope_index == index,
                f'{path}: invalid scope metadata')
        require(0 <= bits <= 64 and math.isfinite(tick) and tick >= 0,
                f'{path}: invalid timestamp precision')
        name, separator, padding = encoded.partition(b'\0')
        require(bool(name) and bool(separator) and not any(padding), f'{path}: invalid scope name')
        result.append(dict(kind=kind, identity=identity, logical_count=logical_count,
                           scope_index=scope_index, valid_bits=bits, tick_ns=tick, name=name.decode('utf-8')))
    return result


def read_raw(path):
    data = path.read_bytes()
    require(len(data) == EXTENT * EXTENT * 32, f'{path}: invalid accumulation size')
    words = array('I')
    words.frombytes(data)
    values = array('f')
    values.frombytes(data)
    if sys.byteorder != 'little':
        words.byteswap()
        values.byteswap()
    require(all(words[index] == SAMPLES for index in range(3, len(words), 8)),
            f'{path}: incomplete sample coverage')
    require(all(math.isfinite(values[index + channel]) for index in range(0, len(values), 8)
                for channel in range(3)), f'{path}: nonfinite raw RGB')
    return data, values


def summarize(root, runs):
    result = {}
    for experiment in ('ab', 'aa'):
        result[experiment] = {}
        for graph, passes in GRAPHS.items():
            operation_names = [passes[0]]
            for name in passes[1:]:
                operation_names.extend((name, name + '_raster'))
            for scene in SCENES:
                stem = graph + '-' + scene
                records = []
                for run in range(1, runs + 1):
                    directory = root / experiment / f'run-{run}'
                    order = read_numbers(directory / f'{stem}.order.bin', 'B', MEASURED)
                    require(set(order) == {0, 1} and sum(order) == MEASURED // 2,
                            f'{stem}: unbalanced paired order')
                    metadata = [read_metadata(directory / variant / f'{stem}.metadata.bin', len(operation_names))
                                for variant in VARIANTS]
                    require(metadata[0] == metadata[1], f'{stem}: variant scope or precision mismatch')
                    record = dict(operations={}, raw_maximum_difference=0.0, raw_files_identical=True,
                                  maximum_display_difference=0, metadata=metadata[0])
                    for index, name in [('frame', 'frame')] + [(f'pass{i}', name) for i, name in enumerate(operation_names)]:
                        first, second = [read_numbers(directory / variant / f'{stem}.{index}.bin', 'd', MEASURED)
                                         for variant in VARIANTS]
                        require(all(value > 0 for values in (first, second) for value in values),
                                f'{stem}/{name}: unavailable duration')
                        ratios = [a / b for a, b in zip(first, second)]
                        record['operations'][name] = dict(
                            ratio=statistics.median(ratios),
                            medians_ms=[statistics.median(values) / 1e6 for values in (first, second)],
                            orders=[statistics.median(value for value, kind in zip(ratios, order) if kind == selected)
                                    for selected in (0, 1)])
                    for index in range(len(passes)):
                        first, second = [read_raw(directory / variant / f'{stem}.raw{index}.bin') for variant in VARIANTS]
                        maximum = max(abs(first[1][offset + channel] - second[1][offset + channel]) / SAMPLES
                                      for offset in range(0, len(first[1]), 8) for channel in range(3))
                        record['raw_files_identical'] &= first[0] == second[0]
                        record['raw_maximum_difference'] = max(record['raw_maximum_difference'], maximum)
                    images = sorted(path.name for path in (directory / VARIANTS[0]).glob(f'{stem}.image*.rgba'))
                    require(len(images) == len(passes) - 1, f'{stem}: missing exported images')
                    require(images == sorted(path.name for path in (directory / VARIANTS[1]).glob(f'{stem}.image*.rgba')),
                            f'{stem}: variant export mismatch')
                    for image in images:
                        first, second = [(directory / variant / image).read_bytes() for variant in VARIANTS]
                        require(len(first) == len(second) == EXTENT * EXTENT * 4, f'{stem}: invalid RGBA8 size')
                        if first != second:
                            record['maximum_display_difference'] = max(record['maximum_display_difference'],
                                                                       max(abs(a - b) for a, b in zip(first, second)))
                    require(record['maximum_display_difference'] == 0 and record['raw_maximum_difference'] <= RAW_TOLERANCE,
                            f'{stem}: rendering changed')
                    records.append(record)
                operations = {}
                for name in records[0]['operations']:
                    ratios = [record['operations'][name]['ratio'] for record in records]
                    ratio = statistics.median(ratios)
                    operations[name] = dict(reduction_percent=100 * (1 - 1 / ratio), ratio=ratio,
                                            ratio_run_bootstrap_95=bootstrap_interval(ratios), run_ratios=ratios,
                                            run_median_ms=[record['operations'][name]['medians_ms'] for record in records])
                result[experiment][stem] = dict(runs=records, operations=operations,
                                                maximum_display_difference=max(record['maximum_display_difference'] for record in records),
                                                maximum_raw_difference=max(record['raw_maximum_difference'] for record in records))
    (root / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    for experiment, scenes in result.items():
        for scene, data in scenes.items():
            print(experiment, scene, {name: (round(value['reduction_percent'], 2), value['ratio_run_bootstrap_95'])
                                     for name, value in data['operations'].items()}, flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=5)
    arguments = parser.parse_args()
    require(arguments.runs >= 3, 'Use at least three independent A/B and A/A blocks')
    arguments.output.mkdir(parents=True, exist_ok=False)
    binary = arguments.output / 'benchmark-tests'
    shutil.copy2(arguments.executable, binary)
    root = Path(__file__).resolve().parents[2]
    metadata = dict(sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    source_base=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                    source_diff_sha256=hashlib.sha256(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=root)).hexdigest(),
                    platform=platform.platform(), utc=datetime.now(timezone.utc).isoformat(),
                    runs=arguments.runs, extent=EXTENT, samples=SAMPLES, warmup=WARMUP,
                    baseline=[64, 1, 1], candidate=[8, 8, 1],
                    order='alternate AB/BA per frame; reverse first pair and AB/AA process order in alternating blocks')
    (arguments.output / 'measurement.json').write_text(json.dumps(metadata, indent=2) + '\n')
    for run in range(arguments.runs):
        for experiment in (('ab', 'aa') if run % 2 == 0 else ('aa', 'ab')):
            directory = arguments.output / experiment / f'run-{run + 1}'
            for variant in VARIANTS:
                (directory / variant).mkdir(parents=True)
            environment = dict(os.environ)
            environment['LUDUS_GRAPH_WORKGROUP_DIRECTORY'] = str(directory.resolve())
            for key in ('LUDUS_GRAPH_WORKGROUP_CONTROL', 'LUDUS_GRAPH_WORKGROUP_REVERSE'):
                environment.pop(key, None)
            if experiment == 'aa':
                environment['LUDUS_GRAPH_WORKGROUP_CONTROL'] = '1'
            if run % 2:
                environment['LUDUS_GRAPH_WORKGROUP_REVERSE'] = '1'
            print(f'{experiment.upper()} block {run + 1}/{arguments.runs}', flush=True)
            subprocess.run([str(binary.resolve()), '[graph-workgroups]', '--reporter', 'compact'], env=environment, check=True)
    summarize(arguments.output, arguments.runs)


if __name__ == '__main__':
    main()
