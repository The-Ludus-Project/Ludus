#!/usr/bin/env python3
"""Benchmark compile-time macro reloads through the editor's build/cook CLI.

Copies an existing shader package; never edits the supplied sources. Each sample
launches a fresh Python process. Results exclude editor debounce and GPU activation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def toggle_sources(original, macro):
    """Return enabled/disabled source bytes for one single-line macro definition."""
    if re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', macro) is None:
        raise ValueError('macro must be an identifier')
    pattern = re.compile(rb'^[ \t]*(?://[ \t]*)?#[ \t]*define[ \t]+' + macro.encode()
                         + rb'\b[^\r\n]*', re.M)
    matches = list(pattern.finditer(original))
    if len(matches) != 1 or matches[0].group().rstrip().endswith(b'\\'):
        raise ValueError('shader must contain exactly one single-line definition of ' + macro)
    match = matches[0]
    definition = match.group()[match.group().index(b'#'):]
    before, after = original[:match.start()], original[match.end():]
    return before + definition + after, before + b'// ' + definition + after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='shader package root, copied read-only')
    parser.add_argument('--manifest', default='main.lushader.json', help='package-relative manifest')
    parser.add_argument('--shader', required=True, help='package-relative shader to toggle')
    parser.add_argument('--macro', required=True)
    parser.add_argument('--output', type=Path, required=True, help='new output directory outside source')
    parser.add_argument('--helper', type=Path, required=True, help='complete installed/staged shader helper bundle')
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--validator', type=Path)
    parser.add_argument('--spirv-cross', type=Path)
    parser.add_argument('--worker-socket', type=Path)
    parser.add_argument('--python-path', type=Path, default=ROOT / 'scripts/python',
                        help='directory containing the implementation of ludus_tools to measure')
    parser.add_argument('--configuration', default='Default')
    parser.add_argument('--samples', type=int, default=7)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if args.samples < 1 or output.is_relative_to(source):
        parser.error('use positive --samples and an output directory outside the source package')
    for relative in (args.manifest, args.shader):
        path = Path(relative)
        if path.is_absolute() or '..' in path.parts or not (source / path).resolve().is_relative_to(source):
            parser.error('manifest and shader must remain inside the source package')
    # Refuse source links rather than copying material outside the requested package.
    if any(path.is_symlink() for path in source.rglob('*')):
        parser.error('source package cannot contain symlinks')
    variants = toggle_sources((source / args.shader).read_bytes(), args.macro)
    output.mkdir(parents=True, exist_ok=False)
    copied = output / 'sources'
    shutil.copytree(source, copied)
    shader = copied / args.shader
    environment = dict(os.environ, PYTHONPATH=str(args.python_path.resolve()), PYTHONDONTWRITEBYTECODE='1')
    # The selected endpoint must be explicit; an unrelated running editor cannot enter a run.
    environment.pop('LUDUS_SHADER_WORKER_SOCKET', None)
    command = [sys.executable, '-m', 'ludus_tools', '--json', 'render-graph', 'build',
               str(copied / args.manifest), str(output / 'build'), '--root', str(copied),
               '--helper', str(args.helper.resolve()), '--compiler', str(args.compiler.resolve()),
               '--runtime-package', str(output / 'published/main.lurgrt'), '--configuration', args.configuration]
    for option in ('validator', 'spirv_cross', 'worker_socket'):
        value = getattr(args, option)
        if value:
            command += ['--' + option.replace('_', '-'), str(value.resolve())]
    rows = []

    def measure(case, sample, text):
        shader.write_bytes(text)
        started = time.perf_counter_ns()
        result = subprocess.run(command, env=environment, text=True, capture_output=True)
        elapsed = (time.perf_counter_ns() - started) / 1e6
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        report = json.loads(result.stdout)
        build = report['build']
        row = {'case': case, 'sample': sample, 'wall_ms': elapsed,
               'cache_hits': build['cache_hits'], 'cache_misses': build['cache_misses'],
               'runtime_generation': report['runtime']['generation']}
        rows.append(row)
        print(json.dumps(row), flush=True)

    measure('initial', 0, variants[0])
    for sample in range(args.samples):
        # A unique harmless comment keeps each toggle out of the artifact cache.
        # Macro state alternates; source hashing, compiler work and publication are real.
        measure('fresh_macro_variant', sample, variants[(sample + 1) % 2]
                + f'\n// macro-toggle benchmark revision {sample}\n'.encode())
    measure('prime_disabled', 0, variants[1])
    for sample in range(args.samples):
        measure('cached_macro_variant', sample, variants[sample % 2])
    summary = {}
    for case in ('fresh_macro_variant', 'cached_macro_variant'):
        times = [row['wall_ms'] for row in rows if row['case'] == case]
        summary[case] = {'median_ms': statistics.median(times), 'max_ms': max(times), 'min_ms': min(times)}
    report = {'platform': platform.platform(), 'python': sys.version, 'macro': args.macro,
              'compiler': subprocess.check_output([str(args.compiler), '-version'], text=True).strip(),
              'build_service_sha256': hashlib.sha256((args.python_path / 'ludus_tools/render_graph_build.py').read_bytes()).hexdigest(),
              'notes': 'Fresh CLI process per sample, build/cook/publication included; debounce, worker startup and GPU activation excluded. Warm OS caches. Fresh variants include a unique source comment.',
              'summary': summary, 'samples': rows}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
