#!/usr/bin/env python3
"""Measure real shader package builds; never initialize an editor or graphics device."""
import argparse
from contextlib import ExitStack
import tempfile
import json
import os
from pathlib import Path
import platform
import statistics
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/python'))
from ludus_tools import render_graph, render_graph_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=7)
    parser.add_argument('--worker', type=Path)
    args = parser.parse_args()
    if args.samples < 1:
        parser.error('--samples must be positive')
    args.output.mkdir(parents=True, exist_ok=False)
    graph = render_graph.new_workspace(args.output / 'sources')
    manifest = render_graph_build.new_build_sources(graph, profiles=('metal', 'wgsl'), renderer_static=True)
    color = graph.parent / 'main_shaders/color.slang'
    shared = color.with_name('shared.slang')
    shared.write_text('float4 Adjust(float4 value) { return value; }\n')
    color.write_text('#include "shared.slang"\n' + color.read_text().replace('return value.Color;', 'return Adjust(value.Color);'))
    environment = dict(os.environ, PYTHONPATH=str(ROOT / 'scripts/python'), PYTHONDONTWRITEBYTECODE='1')
    command = [sys.executable, '-m', 'ludus_tools', '--json', 'render-graph', 'build', str(manifest),
               str(args.output / 'build'), '--helper', str(ROOT / 'cmake/shaders'), '--compiler', str(args.compiler)]
    if args.worker:
        command += ['--worker-socket', os.environ['LUDUS_BENCH_WORKER_SOCKET']]
    helper = args.output / 'helper'
    helper.mkdir()
    for name in render_graph_build.HELPERS:
        shutil.copyfile(ROOT / ('config' if name.endswith('.json') else 'cmake/shaders') / name, helper / name)
    command[9] = str(helper)
    cases = {}
    for case in ('cold_artifacts', 'warm_artifacts', 'shader_edit', 'include_edit', 'failed_edit'):
        measurements = []
        if case == 'warm_artifacts':
            command[7] = str(args.output / 'build')
            subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
        for sample in range(args.samples):
            if case == 'cold_artifacts':
                command[7] = str(args.output / f'cold-{sample}')
            else:
                command[7] = str(args.output / 'build')
            if case == 'shader_edit':
                color.write_text(color.read_text() + f'\n// edit {sample}\n')
            elif case == 'include_edit':
                shared.write_text(f'float4 Adjust(float4 value) {{ return value * {1 + sample * 0.01}; }}\n')
            elif case == 'failed_edit':
                color.write_text(color.read_text() + '\nthis is invalid shader syntax\n')
            start = time.perf_counter_ns()
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            payload = json.loads(result.stdout)
            if (result.returncode == 0) != (case != 'failed_edit'):
                raise RuntimeError(result.stdout + result.stderr)
            measurements.append({'wall_ms': elapsed, 'result': payload.get('build', payload.get('error'))})
        times = sorted(m['wall_ms'] for m in measurements)
        cases[case] = {'median_ms': statistics.median(times), 'max_ms': max(times), 'samples': measurements}
        print(case, round(cases[case]['median_ms'], 2), 'ms', flush=True)
    report = {'version': 1, 'platform': platform.platform(), 'python': sys.version,
              'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'compiler': subprocess.check_output([str(args.compiler), '-version'], text=True).strip(),
              'worker_startup_ms': float(os.environ['LUDUS_BENCH_WORKER_STARTUP_MS']) if args.worker else None,
              'profiles': ['metal', 'wgsl'], 'programs': 3,
              'notes': 'Fresh Python CLI process per sample; cold means empty artifact cache, not cold OS page cache. No GPU pipeline timing.',
              'cases': cases}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    with ExitStack() as cleanup:
        if '--worker' in sys.argv:
            executable = Path(sys.argv[sys.argv.index('--worker') + 1]).resolve()
            temporary = cleanup.enter_context(tempfile.TemporaryDirectory(prefix='ludus-shader-', dir='/tmp'))
            endpoint = Path(temporary) / 'compiler.sock'
            worker_start = time.perf_counter_ns()
            process = subprocess.Popen([str(executable), '--serve', str(endpoint)])
            def stop():
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            cleanup.callback(stop)
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Native worker failed to start')
                time.sleep(0.01)
            os.environ['LUDUS_BENCH_WORKER_SOCKET'] = str(endpoint)
            os.environ['LUDUS_BENCH_WORKER_STARTUP_MS'] = str((time.perf_counter_ns() - worker_start) / 1e6)
        main()
