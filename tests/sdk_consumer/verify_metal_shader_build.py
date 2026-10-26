#!/usr/bin/env python3
"""Verify installed Metal shader generation, layout and dependency rebuilds."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

from verify_shader_build import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('sdk', 'cmake', 'ninja', 'cxx', 'slang', 'conan', 'sysroot', 'libcxx'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ludus metal sdk-') as temporary:
        root = Path(temporary)
        source = root / 'source'
        source.mkdir()
        for path in Path(__file__).with_name('shaders').iterdir():
            shutil.copy2(path, source / path.name)
        main_source = '''#include "sample.h"
#include <ludus/graphics/rhi/rhi.h>
int main() {
    namespace rhi = ludus::graphics::rhi;
    if (ludus::shaders::sample::Fragment().UniformSize != EXPECTED_SIZE) { return 1; }
    const auto started = rhi::Start({}, {});
    if (started != rhi::StartStatus::Ready) {
        const bool absent = rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable;
        rhi::Shutdown(); return absent ? 0 : 2;
    }
    rhi::ShaderHandle shader;
    const auto status = rhi::CreateShader(ludus::shaders::sample::Fragment(), shader);
    rhi::Shutdown(); return status == rhi::ResourceStatus::Ready ? 0 : 3;
}
'''
        (source / 'main.cpp').write_text(main_source.replace('EXPECTED_SIZE', '48'))
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.29)
project(MetalShaderContract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi)
add_executable(sample main.cpp)
target_link_libraries(sample PRIVATE Ludus::GraphicsRhi)
target_compile_options(sample PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET sample NAME sample SOURCE diagnostic.slang VERTEX VertexMain FRAGMENT FragmentMain DEFINES ${SHADER_DEFINES})
''')
        build = root / 'build'
        command = [args.cmake, '-S', source, '-B', build, '-G', 'Ninja',
                   '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                   '-DCMAKE_CXX_COMPILER=' + args.cxx, '-DCMAKE_OSX_SYSROOT=' + args.sysroot,
                   '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0',
                   '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -nostdinc++ -isystem "' + args.libcxx + '"',
                   '-DCMAKE_PREFIX_PATH=' + str(Path(args.sdk).resolve()) + ';' + args.conan,
                   '-DLUDUS_SLANG_COMPILER=' + args.slang]
        run(command)
        run([args.cmake, '--build', build])
        run([build / 'sample'])
        header = build / 'ludus-shaders/sample/sample/sample.h'
        shader = header.with_name('sample.fragment.metal')
        manifest = json.loads(header.with_name('manifest.json').read_text())
        assert manifest['uniform_sizes'] == {'vertex': 48, 'fragment': 48}
        assert manifest['metal_entries'] == {'vertex': 'VertexMain', 'fragment': 'FragmentMain'}
        contract = [('resolution', 0, 8), ('elapsedTime', 8, 4), ('direction', 16, 16), ('tint', 32, 16)]
        for stage in ('vertex', 'fragment'):
            resource = json.loads(header.with_name(stage + '.reflection.json').read_text())['parameters'][0]
            assert resource['binding'] == {'kind': 'constantBuffer', 'index': 0}
            fields = resource['type']['elementType']['fields']
            assert [(f['name'], f['binding']['offset'], f['binding']['size']) for f in fields] == contract
        # Independently derive this fixture's layout from emitted Metal types.
        code = shader.read_text()
        body = re.search(r'struct DiagnosticUniforms_\w+\s*\{([^}]+)\}', code).group(1)
        members = re.findall(r'(float[234]?)\s+(\w+)\s*;', body)
        occupied, alignment, offsets = 0, 1, []
        for kind, name in members:
            size, align = {'float': (4, 4), 'float2': (8, 8), 'float3': (16, 16), 'float4': (16, 16)}[kind]
            alignment = max(alignment, align)
            occupied = (occupied + align - 1) // align * align
            offsets.append((re.sub(r'_\d+$', '', name), occupied, size))
            occupied += size
        assert offsets == contract and occupied == 48 and alignment == 16
        stamp = header.stat().st_mtime_ns
        run([args.cmake, '--build', build])
        assert header.stat().st_mtime_ns == stamp, 'No-op rebuild rewrote shader'
        for name in ('uniforms.slang', 'diagnostic.slang'):
            with (source / name).open('a') as file:
                file.write('\n// dependency check\n')
            run([args.cmake, '--build', build])
            next_stamp = header.stat().st_mtime_ns
            assert next_stamp != stamp, name + ' did not rebuild'
            stamp = next_stamp
        before = hashlib.sha256(shader.read_bytes()).hexdigest()
        run(command + ['-DSHADER_DEFINES=DIAGNOSTIC_SCALE=0.5'])
        run([args.cmake, '--build', build])
        assert hashlib.sha256(shader.read_bytes()).hexdigest() != before
        original_uniforms = (source / 'uniforms.slang').read_text()
        large = original_uniforms.replace('float4 tint;', 'float4 tint;\n float4 surface[1021];')
        (source / 'uniforms.slang').write_text(large)
        (source / 'main.cpp').write_text(main_source.replace('EXPECTED_SIZE', '16384'))
        run([args.cmake, '--build', build])
        run([build / 'sample'])
        large_manifest = json.loads(header.with_name('manifest.json').read_text())
        assert set(large_manifest['uniform_sizes'].values()) == {16384}
        stamp = header.stat().st_mtime_ns
        (source / 'uniforms.slang').write_text(large.replace('surface[1021]', 'surface[1022]'))
        failure = run([args.cmake, '--build', build], success=False)
        assert 'uniform exceeds fullscreen API bounds' in failure, failure
        assert header.stat().st_mtime_ns == stamp, 'Failed generation completed a header'
        (source / 'uniforms.slang').write_text(original_uniforms)
        original_shader = (source / 'diagnostic.slang').read_text()
        (source / 'diagnostic.slang').write_text(original_shader.replace('settings;', 'settings : register(b1);'))
        failure = run([args.cmake, '--build', build], success=False)
        assert 'buffer 0' in failure, failure
    from verify_raster_build import verify_raster, verify_native_interop
    verify_raster(args, run, metal=True)
    verify_native_interop(args, run, metal=True)
    print('Installed Metal shader helper: native link/runtime, per-target layout, rebuilds, capacity and binding rejection passed')


if __name__ == '__main__':
    main()
