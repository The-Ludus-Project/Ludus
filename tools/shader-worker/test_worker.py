#!/usr/bin/env python3
"""Exercise the real native worker, client, CLI package publication and source failures.

Windows workers serve a named pipe and emit DXIL; there the protocol, DXIL, stop and
recovery checks run, while render-graph package publication waits for graph DXIL (D6a).
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
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
# A forced unroll that keeps Slang busy for tens of seconds, so a stop request lands mid-compile.
SLOW_SOURCE = '''[shader("compute")] [numthreads(1, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID, RWStructuredBuffer<float> values)
{
    float value = values[id.x];
    [ForceUnroll] for (int i = 0; i < 2000; ++i) { value = sin(value * 1.0001 + float(i)); }
    values[id.x] = value;
}
'''
STOP_SECONDS = 5
IS_WINDOWS = sys.platform == 'win32'
PIPE_PREFIX = '\\\\.\\pipe\\'
# Wire status values of ludus::tooling::shader_compiler::Status and Target.
STATUS_READY = 0
STATUS_INVALID_REQUEST = 2
STATUS_COMPILE_FAILED = 3
TARGET_DXIL = 4
STAGE_FRAGMENT = 1
STAGE_COMPUTE = 2


def load_native_client():
    spec = importlib.util.spec_from_file_location('native_worker', ROOT / 'cmake/shaders/native_worker.py')
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    return native


NATIVE = load_native_client()


def endpoint_exists(endpoint):
    if IS_WINDOWS:
        # Listing the pipe namespace never connects, so it cannot occupy a listening instance.
        return str(endpoint)[len(PIPE_PREFIX):] in os.listdir(PIPE_PREFIX)
    return Path(endpoint).exists()


def wait_until_absent(endpoint):
    deadline = time.monotonic() + STOP_SECONDS
    while endpoint_exists(endpoint) and time.monotonic() < deadline:
        time.sleep(0.05)
    return not endpoint_exists(endpoint)


def wait_for_endpoint(process, endpoint):
    deadline = time.monotonic() + 10
    while not endpoint_exists(endpoint):
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError('Native worker failed to start')
        time.sleep(0.01)


def send_slow_request(endpoint):
    """Start a long compile and return the open connection without waiting for its reply."""
    payload = json.dumps({'version': 1, 'identity': 1, 'source': 'slow.slang', 'entry': 'ComputeMain',
                          'stage': 2, 'target': 2, 'files': {'slow.slang': SLOW_SOURCE}}).encode('utf-8')
    peer = NATIVE.connect(endpoint)
    peer.sendall(struct.pack('<I', len(payload)) + payload)
    time.sleep(1)
    return peer


def is_running(pid):
    if IS_WINDOWS:
        # os.kill(pid, 0) would terminate the process on Windows; query it instead.
        import ctypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel.WaitForSingleObject.restype = ctypes.c_uint32
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        synchronize, query_limited_information, wait_timeout = 0x00100000, 0x1000, 0x102
        handle = kernel.OpenProcess(synchronize | query_limited_information, 0, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == wait_timeout
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def check_stops_during_compile(worker, make_endpoint):
    """A stop request and the death of an owner each stop a worker inside a compile that would not finish soon."""
    endpoint = make_endpoint('terminated')
    process = subprocess.Popen([str(worker), '--serve', str(endpoint)])
    try:
        wait_for_endpoint(process, endpoint)
        with send_slow_request(endpoint):
            assert process.poll() is None
            process.terminate()
            process.wait(timeout=STOP_SECONDS)
        assert wait_until_absent(endpoint)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    # The owner is killed outright, so nothing forwards SIGTERM or a console event to its worker.
    endpoint = make_endpoint('orphaned')
    launcher = 'import subprocess, sys, time; print(subprocess.Popen(sys.argv[1:]).pid, flush=True); time.sleep(600)'
    # A Windows virtual environment's python.exe is a redirector that runs the interpreter as
    # its child; run the base interpreter so the killed owner is the worker's parent.
    python = getattr(sys, '_base_executable', sys.executable) if IS_WINDOWS else sys.executable
    owner = subprocess.Popen([python, '-c', launcher, str(worker), '--serve', str(endpoint), '--stop-with-parent'],
                             stdout=subprocess.PIPE, text=True)
    pid = int(owner.stdout.readline())
    try:
        deadline = time.monotonic() + 10
        while not endpoint_exists(endpoint):
            if not is_running(pid) or time.monotonic() >= deadline:
                raise RuntimeError('Owned native worker failed to start')
            time.sleep(0.01)
        with send_slow_request(endpoint):
            assert is_running(pid)
            owner.kill()
            owner.wait()
            deadline = time.monotonic() + STOP_SECONDS
            while is_running(pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            assert not is_running(pid), 'Worker outlived its killed owner'
        assert wait_until_absent(endpoint)
    finally:
        owner.stdout.close()
        if owner.poll() is None:
            owner.kill()
            owner.wait()
        if is_running(pid):
            os.kill(pid, signal.SIGTERM if IS_WINDOWS else signal.SIGKILL)


def check_protocol_recovery(endpoint):
    """Truncated/oversized frames and abandoned clients cannot poison the worker."""
    for data in (struct.pack('<I', 17 * 1024 * 1024), struct.pack('<I', 20) + b'{'):
        with NATIVE.connect(endpoint) as peer:
            peer.sendall(data)
    assert NATIVE.exchange(endpoint, {'version': 99, 'identity': 10})['status'] == STATUS_INVALID_REQUEST


def is_signed_dxil(artifact):
    # DXBC header: code, 16-byte hash, version and total size. Unsigned output has a constant hash.
    return (len(artifact) >= 32 and artifact[:4] == b'DXBC' and
            struct.unpack_from('<I', artifact, 24)[0] == len(artifact) and len(set(artifact[4:20])) > 1)


def check_dxil(endpoint):
    """Edit/save/compile through the worker: success, include edits, syntax and interface errors."""
    files = {'root.slang': '#include "color.slang"\n'
                           '[shader("fragment")] float4 FragmentMain() : SV_Target { return GetColor(); }\n',
             'color.slang': 'float4 GetColor() { return float4(1, 0, 0, 1); }\n',
             'unused.slang': '// not included\n'}
    request = {'version': 1, 'identity': 21, 'source': 'root.slang', 'entry': 'FragmentMain',
               'stage': STAGE_FRAGMENT, 'target': TARGET_DXIL, 'files': files}
    first = NATIVE.exchange(endpoint, request)
    assert first['status'] == STATUS_READY, first.get('diagnostics')
    artifact = bytes.fromhex(first['artifact'])
    assert is_signed_dxil(artifact)
    assert bytes.fromhex(first['source_read_mask']) == b'\x01\x01\x00'
    entry = json.loads(bytes.fromhex(first['reflection']))['entryPoints'][0]
    assert entry['name'] == 'FragmentMain' and entry['stage'] == 'fragment'
    files['color.slang'] = 'float4 GetColor() { return float4(0, 1, 0, 1); }\n'
    edited = NATIVE.exchange(endpoint, dict(request, identity=22))
    assert edited['status'] == STATUS_READY and edited['identity'] == 22
    assert is_signed_dxil(bytes.fromhex(edited['artifact'])) and bytes.fromhex(edited['artifact']) != artifact
    files['color.slang'] = 'float4 GetColor() { return }\n'
    broken = NATIVE.exchange(endpoint, dict(request, identity=23))
    assert broken['status'] == STATUS_COMPILE_FAILED and broken['artifact'] == ''
    assert 'color.slang' in broken['diagnostics'], broken['diagnostics']
    files['color.slang'] = 'float4 GetColor() { return float4(1, 0, 0, 1); }\n'
    missing = NATIVE.exchange(endpoint, dict(request, identity=24, entry='MissingMain'))
    assert missing['status'] == STATUS_COMPILE_FAILED and missing['artifact'] == ''
    compute = {'kernel.slang': '#ifndef LUDUS_DXIL\n#error expected DXIL\n#endif\n'
                               'RWStructuredBuffer<float> Values;\n'
                               '[numthreads(1, 1, 1)] void ComputeMain() { Values[0] = 1; }\n'}
    kernel = NATIVE.exchange(endpoint, dict(request, identity=25, source='kernel.slang', entry='ComputeMain',
                                            stage=STAGE_COMPUTE, files=compute))
    assert kernel['status'] == STATUS_READY, kernel.get('diagnostics')
    assert is_signed_dxil(bytes.fromhex(kernel['artifact']))


def run_windows(args):
    """Named-pipe worker: client, protocol, DXIL, recovery, stop and worker-exit journeys."""
    worker = args.worker.resolve()
    prefix = PIPE_PREFIX + 'ludus-shader-test-{}-'.format(os.getpid())
    endpoint = prefix + 'compiler'
    assert not endpoint_exists(endpoint)
    process = subprocess.Popen([str(worker), '--serve', endpoint])
    try:
        wait_for_endpoint(process, endpoint)
        environment = dict(os.environ, LUDUS_TEST_SHADER_SOCKET=endpoint)
        subprocess.run([str(args.client.resolve()), '[shader][client]'], env=environment, check=True)
        reply = NATIVE.exchange(endpoint, {'version': 1, 'identity': 9})
        assert reply['status'] == STATUS_INVALID_REQUEST and reply['identity'] == 9
        assert len(reply['implementation']) == 64
        duplicate = subprocess.run([str(worker), '--serve', endpoint], timeout=10)
        assert duplicate.returncode != 0
        for invalid in ('C:\\worker.sock', PIPE_PREFIX, '\\\\server\\pipe\\worker'):
            assert subprocess.run([str(worker), '--serve', invalid], timeout=10).returncode == 2
        check_dxil(endpoint)
        check_protocol_recovery(endpoint)
        check_stops_during_compile(worker, lambda name: prefix + name)
        # A worker that exits fails the next exchange; the caller keeps its published state.
        process.terminate()
        process.wait(timeout=10)
        assert wait_until_absent(endpoint)
        try:
            NATIVE.exchange(endpoint, {'version': 1, 'identity': 11})
        except (OSError, RuntimeError):
            pass
        else:
            raise AssertionError('Unavailable worker was accepted')
        print('Named-pipe client, DXIL, protocol recovery, stop and worker exit passed.')
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--cmake', type=Path, default=ROOT / 'out/host-tools/venv/bin/cmake')
    parser.add_argument('--ninja', type=Path, default=ROOT / 'out/host-tools/venv/bin/ninja')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'out/shader-tools/slang/bin/slangc')
    parser.add_argument('--validator', type=Path, default=ROOT / 'out/shader-tools/spirv-tools/usr/bin/spirv-val')
    parser.add_argument('--spirv-cross', type=Path, default=ROOT / 'out/shader-tools/spirv-cross/bin/spirv-cross')
    args = parser.parse_args()
    if IS_WINDOWS:
        run_windows(args)
        return
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
            native = NATIVE
            reply = native.exchange(str(endpoint), {'version': 1, 'identity': 9})
            assert reply['status'] == 2 and reply['identity'] == 9
            assert len(reply['implementation']) == 64
            duplicate = subprocess.run([str(args.worker.resolve()), '--serve', str(endpoint)], timeout=10)
            assert duplicate.returncode != 0
            check_stops_during_compile(args.worker.resolve(), lambda name: root / (name + '.sock'))
            check_protocol_recovery(str(endpoint))
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
            # Metal ray stages always translate through SPIR-V Cross. Elsewhere the
            # translator only adds optional GLSL ES artifacts, and naming it makes
            # it required (cmake/LudusShaders.cmake), so pass it only when installed.
            should_use_spirv_cross = sys.platform == 'darwin' or args.spirv_cross.is_file()
            if should_use_spirv_cross:
                command += ['-DLUDUS_SPIRV_CROSS=' + str(args.spirv_cross)]
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
            if should_use_spirv_cross:
                bound_options['spirv_cross'] = args.spirv_cross
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
                             'modules/content/render_graph_format/include/ludus/content/render_graph_format',
                             'modules/content/scene_format/include/ludus/content/scene_format',
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
                closest = ray_sources / 'main_shaders/closest.slang'
                original_closest = closest.read_text()
                closest.write_text('#define LUDUS_BENCH_MACRO 1\n' + original_closest)
                edited_ray = render_graph_reload.rebuild(manifest, root / (fixture + '-native'),
                    root / (fixture + '-native.lurgrt'), worker_socket=str(endpoint), **bound_options)
                assert edited_ray['build']['cache_misses'] == 1
                assert edited_ray['build']['cache_hits'] == native_ray['build']['cache_misses'] - 1
                published_ray = (root / (fixture + '-native.lurgrt')).read_bytes()
                closest.write_text('invalid shader source')
                try:
                    render_graph_reload.rebuild(manifest, root / (fixture + '-native'),
                        root / (fixture + '-native.lurgrt'), worker_socket=str(endpoint), **bound_options)
                except ToolingError:
                    pass
                else:
                    raise AssertionError('Broken native ray callback was accepted')
                assert (root / (fixture + '-native.lurgrt')).read_bytes() == published_ray
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
