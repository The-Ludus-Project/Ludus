#!/usr/bin/env python3
"""Exercise the real native worker, client, CLI package publication and source failures."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/python'))
sys.dont_write_bytecode = True
from ludus_tools import render_graph, render_graph_build, render_graph_reload
from ludus_tools.errors import ToolingError

# Worker builds hash and load the native client beside the ordinary helper bundle.
WORKER_HELPERS = render_graph_build.HELPERS + (render_graph_build.NATIVE_WORKER_HELPER,)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--cmake', type=Path, default=ROOT / 'out/host-tools/venv/bin/cmake')
    parser.add_argument('--ninja', type=Path, default=ROOT / 'out/host-tools/venv/bin/ninja')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'out/shader-tools/slang/bin/slangc')
    parser.add_argument('--validator', type=Path, default=ROOT / 'out/shader-tools/spirv-tools/usr/bin/spirv-val')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ludus-shader-', dir='/tmp') as directory:
        root = Path(directory)
        endpoint = root / 'compiler.sock'
        process = subprocess.Popen([str(args.worker.resolve()), '--serve', str(endpoint)])
        try:
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Native worker failed to start')
                time.sleep(0.01)
            environment = dict(os.environ, LUDUS_TEST_SHADER_SOCKET=str(endpoint))
            subprocess.run([str(args.client.resolve()), '[shader][client]'], env=environment, check=True)
            spec = importlib.util.spec_from_file_location('native_worker', ROOT / 'cmake/shaders/native_worker.py')
            native = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(native)
            reply = native.exchange(str(endpoint), {'version': 1, 'identity': 9})
            assert reply['status'] == 2 and reply['identity'] == 9
            assert len(reply['implementation']) == 64
            duplicate = subprocess.run([str(args.worker.resolve()), '--serve', str(endpoint)], timeout=10)
            assert duplicate.returncode != 0
            # Truncated/oversized frames and abandoned clients cannot poison the worker.
            for data in (struct.pack('<I', 17 * 1024 * 1024), struct.pack('<I', 20) + b'{'):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                    peer.connect(str(endpoint))
                    peer.sendall(data)
            assert native.exchange(str(endpoint), {'version': 99, 'identity': 10})['status'] == 2
            # Exercise the canonical renderer recipe owner through real CMake,
            # including shared include roots and independently checked reflection.
            recipes = root / 'recipes'
            recipes.mkdir()
            (recipes / 'dummy.cpp').write_text('void Fixture() {}\n')
            (recipes / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.29)
project(NativeShaderRecipes LANGUAGES CXX)
include("{ROOT / 'cmake/LudusShaders.cmake'}")
include("{ROOT / 'cmake/LudusRendererShaders.cmake'}")
add_library(recipe STATIC dummy.cpp)
ludus_compile_renderer_shaders(TARGET recipe SHADERS renderer_flat renderer_composite renderer_lit renderer_gpu_preview)
''')
            command = [str(args.cmake), '-S', str(recipes), '-B', str(root / 'recipe-build'), '-G', 'Ninja',
                '-DCMAKE_MAKE_PROGRAM=' + str(args.ninja), '-DPython3_EXECUTABLE=' + sys.executable,
                '-DCMAKE_CXX_COMPILER=' + str(ROOT / 'out/host-tools/bin/clang++'),
                '-DLUDUS_SLANG_COMPILER=' + str(args.compiler), '-DLUDUS_SHADER_WORKER_SOCKET=' + str(endpoint)]
            if sys.platform != 'darwin':
                command += ['-DLUDUS_SPIRV_VALIDATOR=' + str(args.validator)]
            configured = subprocess.run(command, capture_output=True, text=True)
            if configured.returncode:
                raise RuntimeError(configured.stdout + configured.stderr)
            built = subprocess.run([str(args.cmake), '--build', str(root / 'recipe-build'), '--parallel', '2'], capture_output=True, text=True)
            if built.returncode:
                raise RuntimeError(built.stdout + built.stderr)
            bound = root / 'bound'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/bound', bound)
            profiles = ['metal', 'wgsl'] if sys.platform == 'darwin' else ['spirv', 'wgsl']
            template = (bound / 'main.lushader.json.in').read_text()
            (bound / 'main.lushader.json').write_text(template.replace('@RG2_TEST_PROFILES@', json.dumps(profiles)))
            bound_options = dict(helper=ROOT / 'cmake/shaders', compiler=args.compiler)
            # The shared helper's lock files are installed beside it; stage the
            # same verified helper bundle used by the normal build adapter.
            bound_helper = root / 'bound-helper'
            bound_helper.mkdir()
            for name in WORKER_HELPERS:
                origin = ROOT / ('config' if name.endswith('.json') else 'cmake/shaders') / name
                (bound_helper / name).write_bytes(origin.read_bytes())
            bound_options['helper'] = bound_helper
            if sys.platform != 'darwin':
                bound_options['validator'] = args.validator
            # Mirror the installed helper bundle with its renderer/math/RHI-owned sources.
            for relative in ('modules/graphics/renderer/shaders', 'modules/foundation/math/shaders',
                             'modules/graphics/rhi/shaders',
                             'modules/graphics/renderer/include/ludus/graphics/renderer'):
                for source_file in (ROOT / relative).iterdir():
                    if source_file.suffix in ('.slang', '.h') or source_file.name == 'recipes.json':
                        (bound_helper / source_file.name).write_bytes(source_file.read_bytes())
            # Both graph ray profiles wrap the renderer compute kernel. Compile through
            # the real native API: nested annotated entry points used to hang Slang's
            # parameter transform, despite the offline compiler accepting these sources.
            for fixture in ('raytracing', 'scene_ray'):
                ray_sources = root / fixture
                shutil.copytree(ROOT / 'tests/sdk_consumer/rg2' / fixture, ray_sources)
                template = (ray_sources / 'main.lushader.json.in').read_text()
                manifest = ray_sources / 'main.lushader.json'
                manifest.write_text(template.replace('@RG2_TEST_PROFILES@', json.dumps(profiles)))
                native_ray = render_graph_reload.rebuild(manifest, root / (fixture + '-native'),
                    root / (fixture + '-native.lurgrt'), worker_socket=str(endpoint), **bound_options)
                offline_ray = render_graph_reload.rebuild(manifest, root / (fixture + '-offline'),
                    root / (fixture + '-offline.lurgrt'), **bound_options)
                assert native_ray['runtime']['interface_identity'] == offline_ray['runtime']['interface_identity']
            native_bound = render_graph_reload.rebuild(bound / 'main.lushader.json', root / 'bound-native', root / 'bound-native.lurgrt',
                worker_socket=str(endpoint), **bound_options)
            legacy_bound = render_graph_reload.rebuild(bound / 'main.lushader.json', root / 'bound-legacy', root / 'bound-legacy.lurgrt', **bound_options)
            assert native_bound['runtime']['interface_identity'] == legacy_bound['runtime']['interface_identity']
            # Reload publishes the resource names beside the package, for the editor texture viewer.
            assert Path(native_bound['runtime']['resource_names']) == (root / 'bound-native.lurgrt.resources.json').absolute()
            assert json.loads(Path(native_bound['runtime']['resource_names']).read_text())['generation'] == native_bound['runtime']['generation']
            source = render_graph.new_workspace(root / 'sources', output='presentation')
            graph = json.loads(source.read_text())
            for configuration in graph['configurations']:
                for node in configuration['nodes']:
                    if node['kind'] == 'Target':
                        node['extent'] = [96, 64]
            source.write_text(json.dumps(graph))
            manifest = render_graph_build.new_build_sources(source, profiles=('metal', 'wgsl') if sys.platform == 'darwin' else ('spirv', 'wgsl'), renderer_static=True)
            helper = root / 'helper'
            helper.mkdir()
            for name in WORKER_HELPERS:
                origin = ROOT / ('config' if name.endswith('.json') else 'cmake/shaders') / name
                (helper / name).write_bytes(origin.read_bytes())
            options = dict(helper=helper, compiler=args.compiler, worker_socket=str(endpoint))
            if sys.platform != "darwin":
                options["validator"] = args.validator
            destination = root / 'published/main.lurgrt'
            result = render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
            original = destination.read_bytes()
            (destination.parent / 'initial.lurgrt').write_bytes(original)
            assert result['build']['cache_misses'] == 6
            warm = render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
            assert warm['build']['cache_misses'] == 0
            assert destination.read_bytes() == original
            color = source.parent / 'main_shaders/color.slang'
            valid = color.read_text()
            color.write_text('invalid source')
            try:
                render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
            except ToolingError:
                pass
            else:
                raise AssertionError('Broken shader was accepted')
            assert destination.read_bytes() == original
            color.write_text(valid)
            actual_build = render_graph_build.build_package
            def edit_after_build(*arguments, **keywords):
                result = actual_build(*arguments, **keywords)
                color.write_text(valid + '\n// source changed after compilation\n')
                return result
            with patch.object(render_graph_build, 'build_package', edit_after_build):
                try:
                    render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
                except ToolingError as error:
                    assert error.code == 'RenderGraphBuildStale'
                else:
                    raise AssertionError('Stale build published a runtime generation')
            assert destination.read_bytes() == original
            color.write_text(valid.replace('return value.Color;', 'return float4(0,1,0,1);'))
            changed = render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
            assert changed['build']['cache_misses'] == 2
            assert destination.read_bytes() != original
            assert changed['runtime']['interface_identity'] == result['runtime']['interface_identity']
            (destination.parent / 'changed.lurgrt').write_bytes(destination.read_bytes())
            if args.renderer:
                result = subprocess.run([str(args.renderer.resolve()), '[shader][reload]'],
                    env=dict(os.environ, LUDUS_NATIVE_RELOAD_ROOT=str(destination.parent),
                        LUDUS_NATIVE_RELOAD_METRICS=str(root / 'pipeline-metrics.json')),
                    capture_output=True, text=True, timeout=60)
                print(result.stdout, result.stderr)
                assert result.returncode == 0 and 'skipped' not in result.stdout.lower()
                print((root / 'pipeline-metrics.json').read_text())
            # A worker crash fails the next uncached build and preserves the live publication.
            last = destination.read_bytes()
            process.terminate()
            process.wait(timeout=10)
            color.write_text(valid + '\n// force cache miss after worker exit\n')
            try:
                render_graph_reload.rebuild(manifest, root / 'build', destination, **options)
            except (ToolingError, OSError, RuntimeError):
                pass
            else:
                raise AssertionError('Unavailable worker was accepted')
            assert destination.read_bytes() == last
            print('Native client, protocol recovery, cached builds, shader edits, failures and worker exit passed.')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
