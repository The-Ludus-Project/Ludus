#!/usr/bin/env python3
"""Explicit acquisition/build of the pinned authors' scalar SMS reference.

Compatibility changes affect legacy dependency/build/debugger code only. No
integrator, geometry, BSDF, sampler or scene source is patched. All commands,
patches and dependency revisions are recorded in a fresh output directory.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from scene import SOURCE, read_gold_ior, verify_assets

DEPENDENCIES = ['ext/pybind11', 'ext/enoki', 'ext/tbb', 'ext/libpng', 'ext/libjpeg',
                'ext/openexr', 'ext/tinyformat', 'ext/pugixml', 'resources/data']


def apply_compatibility_patch(path, before, after):
    text = path.read_text()
    if text.count(after) == 1:
        return
    if text.count(before) != 1:
        raise ValueError(f'Unexpected upstream source in {path}; inspect it before applying compatibility changes.')
    path.write_text(text.replace(before, after))


def acquire(upstream):
    if upstream.exists():
        raise ValueError('Acquisition requires a new directory; existing reference checkouts are preserved.')
    upstream.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'clone', '--depth', '1', '--filter=blob:none', '--sparse', SOURCE['repository'], str(upstream)], check=True)
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != SOURCE['revision']:
        subprocess.run(['git', '-C', str(upstream), 'fetch', '--depth', '1', 'origin', SOURCE['revision']], check=True)
        subprocess.run(['git', '-C', str(upstream), 'checkout', '--detach', SOURCE['revision']], check=True)
    subprocess.run(['git', '-C', str(upstream), 'sparse-checkout', 'set', SOURCE['directory'],
                    'include', 'src', 'resources', 'ext'], check=True)
    subprocess.run(['git', '-C', str(upstream), 'submodule', 'update', '--init', '--depth', '1', *DEPENDENCIES], check=True)
    verify_assets(upstream / SOURCE['directory'])
    read_gold_ior(upstream)


def build(arguments):
    upstream = arguments.upstream.resolve()
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != SOURCE['revision']:
        raise ValueError('Use the pinned reference revision in a separate checkout.')
    verify_assets(upstream / SOURCE['directory'])
    read_gold_ior(upstream)
    arguments.output.mkdir(parents=True, exist_ok=False)
    configuration = (upstream / 'resources/mitsuba.conf.template').read_text()
    configuration = configuration.replace('"scalar_rgb",\n        "scalar_spectral"', '"scalar_rgb"')
    configuration = configuration.replace('"default": "scalar_spectral"', '"default": "scalar_rgb"')
    configuration_path = upstream / 'mitsuba.conf'
    if configuration_path.exists() and configuration_path.read_text() != configuration:
        raise ValueError('The existing mitsuba.conf is different; preserve it and use a separate reference checkout.')
    configuration_path.write_text(configuration)
    if sys.platform == 'darwin':
        apply_compatibility_patch(upstream / 'ext/libpng/pngpriv.h', '#      include <fp.h>', '#      include <math.h>')
        apply_compatibility_patch(upstream / 'src/libcore/util.cpp',
            '#if defined(__i386__) || defined(__x86_64__) || defined(__OSX__)\n',
            '#if defined(__i386__) || defined(__x86_64__)\n')
        marker = 'add_executable(rgb2spec_opt rgb2spec_opt.cpp)'
        apply_compatibility_patch(upstream / 'ext/rgb2spec/CMakeLists.txt', marker,
            marker+'\nif (APPLE)\n  set_property(TARGET rgb2spec_opt APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-rpath,@loader_path")\nendif()')
    environment = dict(os.environ)
    # Keep the venv executable path: resolving its symlink selects the base
    # interpreter and loses setuptools/distutils from the reference environment.
    python = Path(arguments.python).absolute()
    environment['PATH'] = str(python.parent)+os.pathsep+str(Path(arguments.cmake).absolute().parent)+os.pathsep+environment.get('PATH', '')
    configure = [str(arguments.cmake), '-S', str(upstream), '-B', str(arguments.build_directory.resolve()), '-G', 'Ninja',
                 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DMTS_ENABLE_PYTHON=OFF', '-DMTS_ENABLE_GUI=OFF',
                 '-DMTS_ENABLE_EMBREE=OFF', '-DMTS_ENABLE_LTO=OFF', '-DMTS_ENABLE_PROFILER=OFF',
                 '-DPYTHON_EXECUTABLE='+str(python)]
    if sys.platform == 'darwin':
        configure += ['-DCMAKE_C_FLAGS=-DPNG_ARM_NEON_OPT=0', '-DCMAKE_CXX_FLAGS=-Wno-invalid-specialization']
    commands = [configure, [str(arguments.cmake), '--build', str(arguments.build_directory.resolve()), '-j', str(arguments.jobs)]]
    for index, command in enumerate(commands):
        with (arguments.output / f'command-{index}.log').open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
        if result.returncode != 0:
            raise ValueError(f'Reference build failed; inspect {arguments.output}/command-{index}.log before retrying.')
    patches = {name: subprocess.check_output(['git', '-C', str(upstream / name), 'diff'], text=True) for name in DEPENDENCIES}
    (arguments.output / 'upstream.patch').write_text(subprocess.check_output(['git', '-C', str(upstream), 'diff'], text=True))
    report = dict(source=SOURCE, commands=commands, compatibility_scope='build/dependency/debugger code; transport sources unchanged',
                  submodules=subprocess.check_output(['git', '-C', str(upstream), 'submodule', 'status'], text=True),
                  dependency_patches=patches)
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('acquire', help='Explicitly download public source, assets and pinned dependencies.')
    command.add_argument('--upstream', type=Path, required=True)
    command = commands.add_parser('build')
    command.add_argument('--upstream', type=Path, required=True)
    command.add_argument('--build-directory', type=Path, required=True)
    command.add_argument('--output', type=Path, required=True)
    command.add_argument('--cmake', type=Path, required=True)
    command.add_argument('--python', type=Path, default=Path(sys.executable))
    command.add_argument('--jobs', type=int, default=6)
    arguments = parser.parse_args()
    if arguments.command == 'acquire':
        acquire(arguments.upstream)
    else:
        if arguments.jobs < 1:
            parser.error('--jobs must be positive')
        build(arguments)


if __name__ == '__main__':
    main()
