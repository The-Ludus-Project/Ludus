#!/usr/bin/env python3
"""Run paired native opacity benchmarks, alternating layouts for each scene.

The baseline uses the test-only original single-BLAS layout with unchanged CPU
BVH primitive order. Both modes execute the same compiled production integrator;
builds with tests disabled omit the private control and raw readback accessor.
"""
import argparse
import json
import hashlib
import os
import shutil
from pathlib import Path
import subprocess

from compare_ray_opacity import SCENES, compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-executable', type=Path, help='Saved unmodified test executable; compares whole-scene batches for legacy harness compatibility')
    parser.add_argument('--runs', type=int, default=3)
    options = parser.parse_args()
    if options.runs < 1:
        parser.error('--runs must be positive')
    options.output.mkdir(parents=True, exist_ok=True)
    executable = options.output / 'benchmark-tests'
    shutil.copy2(options.executable.resolve(), executable)
    baseline_executable = executable
    if options.baseline_executable is not None:
        baseline_executable = options.output / 'baseline-tests'
        shutil.copy2(options.baseline_executable.resolve(), baseline_executable)
    metadata = {'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                'baseline_executable_sha256': hashlib.sha256(baseline_executable.read_bytes()).hexdigest(),
                'comparison_mode': 'original-build' if options.baseline_executable is not None else 'same-kernel-layout',
                'runs': options.runs, 'scenes': SCENES}
    (options.output / 'measurement.json').write_text(json.dumps(metadata, indent=2) + '\n')
    before = [options.output / f'before-{run + 1}' for run in range(options.runs)]
    after = [options.output / f'after-{run + 1}' for run in range(options.runs)]
    scenes = (None,) if options.baseline_executable is not None else SCENES
    for scene in scenes:
        for run in range(options.runs):
            modes = (False, True) if run % 2 == 0 else (True, False)
            for should_partition in modes:
                directory = (after if should_partition else before)[run]
                directory.mkdir(parents=True, exist_ok=True)
                environment = dict(os.environ)
                environment['LUDUS_OPACITY_BENCHMARK_DIRECTORY'] = str(directory.resolve())
                if scene is None:
                    environment.pop('LUDUS_OPACITY_SCENE', None)
                else:
                    environment['LUDUS_OPACITY_SCENE'] = scene
                if should_partition:
                    environment.pop('LUDUS_OPACITY_BASELINE', None)
                else:
                    environment['LUDUS_OPACITY_BASELINE'] = '1'
                label = 'partitioned' if should_partition else 'baseline'
                print(f'{scene or "all scenes"}: run {run + 1}/{options.runs}, {label}', flush=True)
                selected_executable = executable if should_partition else baseline_executable
                subprocess.run([str(selected_executable.resolve()), '[opacity]', '--reporter', 'compact'],
                               env=environment, check=True)
    results = compare(before, after)
    report = options.output / 'comparison.json'
    report.write_text(json.dumps(results, indent=2) + '\n')
    print(f'Report: {report}', flush=True)
    for scene, result in results.items():
        gpu = result['gpu']
        print(f"{scene}: {gpu['before']['median_ms']:.3f} -> {gpu['after']['median_ms']:.3f} ms "
              f"({gpu['speedup']:.3f}x), maximum display-channel difference "
              f"{result['maximum_channel_difference']}", flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
