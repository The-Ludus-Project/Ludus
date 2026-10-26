#!/usr/bin/env python3
"""Exercise the installed shader helper's real compiler/dependency contract."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(command, success=True):
    result = subprocess.run(list(map(str, command)), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != success:
        raise RuntimeError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('sdk', 'cmake', 'ninja', 'cxx', 'slang', 'validator', 'conan'):
        parser.add_argument('--' + name, required=True)
    # Optional: when provided, also verify the GLSL ES 3.00 (WebGL 2) backend
    # artifact. Absent, the SPIR-V/WGSL contract is exercised exactly as before.
    parser.add_argument('--spirv-cross')
    args = parser.parse_args()
    sdk = Path(args.sdk).resolve()
    with tempfile.TemporaryDirectory(prefix='ludus shader build-') as temporary:
        root = Path(temporary); source = root / 'source'; source.mkdir()
        for path in Path(__file__).with_name('shaders').iterdir():
            shutil.copy2(path, source / path.name)
        (source / 'main.cpp').write_text('#include "sample.h"\nint main() { return ludus::shaders::sample::Fragment().UniformSize == 48 ? 0 : 1; }\n')
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.29)
project(ShaderBuildContract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Ludus CONFIG REQUIRED)
add_executable(sample main.cpp)
target_link_libraries(sample PRIVATE Ludus::GraphicsRhi)
ludus_compile_shader(TARGET sample NAME sample SOURCE diagnostic.slang VERTEX VertexMain FRAGMENT FragmentMain DEFINES ${SHADER_DEFINES})
''')
        build = root / 'build'
        command = [args.cmake, '-S', source, '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_MAKE_PROGRAM=' + args.ninja, '-DCMAKE_CXX_COMPILER=' + args.cxx,
            '-DCMAKE_PREFIX_PATH=' + str(sdk) + ';' + args.conan,
            '-DLUDUS_SLANG_COMPILER=' + args.slang, '-DLUDUS_SPIRV_VALIDATOR=' + args.validator]
        if args.spirv_cross:
            command += ['-DLUDUS_SPIRV_CROSS=' + args.spirv_cross]
        run(command); run([args.cmake, '--build', build]); run([build / 'sample'])
        header = build / 'ludus-shaders/sample/sample/sample.h'
        shader = header.with_name('sample.fragment.spv')
        stamp = header.stat().st_mtime_ns
        run([args.cmake, '--build', build]); assert header.stat().st_mtime_ns == stamp, 'no-op rebuild changed shader'
        for name in ('uniforms.slang', 'diagnostic.slang'):
            with (source / name).open('a') as file: file.write('\n// dependency check\n')
            run([args.cmake, '--build', build]); new_stamp = header.stat().st_mtime_ns
            assert new_stamp != stamp, name + ' did not trigger shader rebuild'; stamp = new_stamp
        before = hashlib.sha256(shader.read_bytes()).hexdigest()
        run(command + ['-DSHADER_DEFINES=DIAGNOSTIC_SCALE=0.5']); run([args.cmake, '--build', build])
        assert hashlib.sha256(shader.read_bytes()).hexdigest() != before, 'option did not alter compiled artifact'
        manifest = json.loads(header.with_name('manifest.json').read_text())
        assert {key: manifest['uniform_sizes'][key] for key in ('vertex', 'fragment', 'wgsl')} == {'vertex':48,'fragment':48,'wgsl':48}
        assert manifest['spirv_entries'] == {'vertex':'main','fragment':'main'}
        assert manifest['wgsl_entries'] == {'vertex':'VertexMain','fragment':'FragmentMain'}
        contract = [('resolution', 0, 8), ('elapsedTime', 8, 4), ('direction', 16, 12), ('tint', 32, 16)]
        for target in ('vertex', 'fragment', 'wgsl'):
            resource = json.loads(header.with_name(target + '.reflection.json').read_text())['parameters'][0]
            fields = resource['type']['elementType']['fields']
            assert [(field['name'], field['binding']['offset'], field['binding']['size']) for field in fields] == contract
        # Derive WGSL layout separately from emitted types/alignments, without
        # borrowing the SPIR-V offsets or trusting reflection packing equality.
        code = header.with_name('sample.wgsl').read_text()
        body = re.search(r'struct DiagnosticUniforms[^\{]*\{([^}]+)\}', code).group(1)
        members = re.findall(r'@align\((\d+)\)\s+(\w+)\s*:\s*(vec[234]<f32>|f32)', body)
        occupied = 0; alignment = 1; offsets = []
        for declared, name, kind in members:
            size, natural = {'f32':(4,4), 'vec2<f32>':(8,8), 'vec3<f32>':(12,16), 'vec4<f32>':(16,16)}[kind]
            align = max(int(declared), natural); alignment = max(alignment, align)
            occupied = (occupied + align - 1) // align * align
            offsets.append((re.sub(r'_\d+$', '', name), occupied, size)); occupied += size
        assert offsets == contract
        assert alignment == 16 and (occupied + alignment - 1) // alignment * alignment == 48
        if args.spirv_cross:
            # GLSL ES 3.00 backend artifact: entries link through main; the block
            # size is reflected; the layout is derived independently from the
            # emitted ESSL std140 block (not borrowed from SPIR-V/WGSL offsets).
            assert manifest['uniform_sizes'] == {'vertex':48,'fragment':48,'wgsl':48,'glsl_es':48}
            assert manifest['glsl_es_entries'] == {'vertex':'main','fragment':'main'}
            assert manifest['profiles']['glsl_es'] == '300 es'
            assert manifest['glsl_es_layout'] == {'block':'DiagnosticUniforms_std140',
                'offsets':{'resolution':0,'elapsedTime':8,'direction':16,'tint':32},
                'size':48,'alignment':16,'binding':0}
            assert manifest['glsl_es_translator']['tag'] and manifest['glsl_es_translator']['source_sha256']
            essl = header.with_name('sample.fragment.essl').read_text()
            assert essl.lstrip().startswith('#version 300 es'), 'ESSL is not GLSL ES 3.00'
            assert 'GL_ARB_shader_draw_parameters' not in essl and 'gl_BaseVertex' not in essl
            vert_essl = header.with_name('sample.vertex.essl').read_text()
            assert 'gl_VertexID' in vert_essl and 'gl_BaseVertex' not in vert_essl, 'ESSL vertex id is not WebGL 2 safe'
            es_block = re.search(r'layout\(std140\)\s+uniform\s+\w+\s*\{([^}]+)\}', essl).group(1)
            es_members = re.findall(r'(?:highp\s+|mediump\s+|lowp\s+)?(vec[234]|float)\s+(\w+)\s*;', es_block)
            es_occupied = 0; es_align = 1; es_offsets = []
            for kind, name in es_members:
                size, natural = {'float':(4,4), 'vec2':(8,8), 'vec3':(12,16), 'vec4':(16,16)}[kind]
                es_align = max(es_align, natural)
                es_occupied = (es_occupied + natural - 1) // natural * natural
                es_offsets.append((name, es_occupied, size)); es_occupied += size
            assert es_offsets == contract, es_offsets
            assert es_align == 16 and (es_occupied + es_align - 1) // es_align * es_align == 48
        # Compile the full portable 16 KiB block, including a large std140 array,
        # then verify aggregate overflow is rejected by the installed helper.
        original_uniforms = (source / 'uniforms.slang').read_text()
        large_uniforms = original_uniforms.replace('float4 tint;', 'float4 tint;\n    float4 surface[1021];')
        (source / 'uniforms.slang').write_text(large_uniforms)
        (source / 'main.cpp').write_text('#include "sample.h"\nint main() { return ludus::shaders::sample::Fragment().UniformSize == 16384 ? 0 : 1; }\n')
        run([args.cmake, '--build', build]); run([build / 'sample'])
        large_manifest = json.loads(header.with_name('manifest.json').read_text())
        assert all(size == 16384 for size in large_manifest['uniform_sizes'].values())
        (source / 'uniforms.slang').write_text(large_uniforms.replace('float4 surface[1021];', 'float4 surface[1022];'))
        failure = run([args.cmake, '--build', build], success=False)
        assert 'Uniform exceeds fullscreen API bounds' in failure, failure
        (source / 'uniforms.slang').write_text(original_uniforms)
        (source / 'main.cpp').write_text('#include "sample.h"\nint main() { return ludus::shaders::sample::Fragment().UniformSize == 48 ? 0 : 1; }\n')
        original = (source / 'diagnostic.slang').read_text()
        (source / 'diagnostic.slang').write_text(original.replace('vk::binding(0, 0)', 'vk::binding(0, 1)'))
        failure = run([args.cmake, '--build', build], success=False)
        assert 'binding contract' in failure or 'group 0 binding 0' in failure, failure
        (source / 'diagnostic.slang').write_text(original)
        if args.spirv_cross:
            bad_cross = root / 'bad-cross'
            shutil.copy2(args.spirv_cross, bad_cross)
            shutil.copy2(str(Path(args.spirv_cross).resolve()) + '.build.json', str(bad_cross) + '.build.json')
            with bad_cross.open('ab') as file: file.write(b'changed')
            run(command + ['-DLUDUS_SPIRV_CROSS=' + str(bad_cross)])
            failure = run([args.cmake, '--build', build], success=False)
            assert 'SPIRV-Cross binary differs from verified build' in failure, failure
        bad_validator = root / 'bad-validator'; shutil.copy2(args.validator, bad_validator)
        with bad_validator.open('ab') as file: file.write(b'changed')
        run(command + ['-DLUDUS_SPIRV_VALIDATOR=' + str(bad_validator)])
        failure = run([args.cmake, '--build', build], success=False)
        assert 'validator differs' in failure
    from verify_raster_build import verify_raster, verify_native_interop
    verify_raster(args, run, metal=False)
    verify_native_interop(args, run, metal=False)
    print('Installed shader helper: no-op, source/include/option rebuild, reflected contracts, binding and integrity rejection passed')

if __name__ == '__main__': main()
