"""Verify the installed portable raster helper and public-only native consumer."""
import json
from pathlib import Path
import shutil
import tempfile


def verify_raster(args, run, metal=False):
    """Build raster.cpp with the LUSL ports, then check the helper's Slang manual-binding limits.

    raster.slang is the retained Slang input of the binding checks: LUSL assigns slots in
    declaration order, so slot 15 and a nonzero group are spelled only in Slang
    (config/lusl_migration.json).
    """
    with tempfile.TemporaryDirectory(prefix='ludus raster sdk-') as temporary:
        source = Path(temporary)/'source'; source.mkdir()
        shutil.copy2(Path(__file__).with_name('raster.cpp'),source/'raster.cpp')
        shutil.copy2(Path(__file__).with_name('ordered_frame.h'),source/'ordered_frame.h')
        shutil.copy2(Path(__file__).with_name('merged_frame.h'),source/'merged_frame.h')
        for name in ('raster.lusl', 'cull.lusl', 'raster.slang'):
            shutil.copy2(Path(__file__).with_name('shaders')/name,source/name)
        (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.29)
project(RasterSdkContract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi)
add_executable(raster_consumer raster.cpp)
target_link_libraries(raster_consumer PRIVATE Ludus::GraphicsRhi)
target_compile_options(raster_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET raster_consumer NAME cull SOURCE cull.lusl COMPUTE ComputeMain)
ludus_compile_shader(TARGET raster_consumer NAME raster RASTER SOURCE raster.lusl VERTEX VertexMain FRAGMENT FragmentMain)
ludus_compile_shader(TARGET raster_consumer NAME raster_slang RASTER SOURCE raster.slang VERTEX vertexMain FRAGMENT fragmentMain)
''')
        build=Path(temporary)/'build'
        command=[args.cmake,'-S',source,'-B',build,'-G','Ninja',
                 '-DCMAKE_MAKE_PROGRAM='+args.ninja,'-DCMAKE_CXX_COMPILER='+args.cxx,
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_PREFIX_PATH='+str(Path(args.sdk).resolve())+';'+args.conan,
                 '-DLUDUS_SLANG_COMPILER='+args.slang]
        if metal:
            command += ['-DCMAKE_OSX_SYSROOT='+args.sysroot,'-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0',
                        '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -nostdinc++ -isystem "'+args.libcxx+'"']
        else:
            command += ['-DLUDUS_SPIRV_VALIDATOR='+args.validator]
            if args.spirv_cross: command += ['-DLUDUS_SPIRV_CROSS='+args.spirv_cross]
        run(command);run([args.cmake,'--build',build]);run([build/'raster_consumer'])
        shaders=build/'ludus-shaders/raster_consumer'
        output=shaders/'raster'
        slang_output=shaders/'raster_slang'
        targets=('metal',) if metal else ('spirv','wgsl') + (('glsl',) if args.spirv_cross else ())
        # The LUSL port and the Slang original reflect the same interface.
        for interface_output in (output, slang_output):
            manifest=json.loads((interface_output/'manifest.json').read_text())
            for target in targets:
                assert manifest['interfaces'][target+'_vertex']['entries']==[]
                assert [(x['location'],x['format']) for x in manifest['interfaces'][target+'_vertex']['inputs']]==[(0,'Float3'),(1,'Float2'),(2,'Float2')]
                assert [(x['binding'],x['kind'],x['size']) for x in manifest['interfaces'][target+'_fragment']['entries']]==[(0,'UniformBuffer',16),(1,'Texture2D',0),(2,'Sampler',0)]
        slots=(output/'raster_slots.h').read_text()
        for constant in ('TINT = 0;','IMAGE = 1;','IMAGE_SAMPLER = 2;','INPUT_POSITION = 0;','INPUT_UV = 1;','INPUT_OFFSET = 2;'):
            assert constant in slots, slots
        compute_output=shaders/'cull'
        compute_manifest=json.loads((compute_output/'manifest.json').read_text())
        for target in (('metal',) if metal else ('spirv','wgsl')):
            interface=compute_manifest['interfaces'][target+'_compute']
            assert interface['workgroup']==[1,1,1]
            assert [(x['binding'],x['kind'],x['size']) for x in interface['entries']]==[(0,'StorageRead',8),(1,'StorageReadWrite',8),(2,'StorageReadWrite',4)]
        compute_header=compute_output/'cull.h';compute_stamp=compute_header.stat().st_mtime_ns
        header=output/'raster.h';stamp=header.stat().st_mtime_ns
        run([args.cmake,'--build',build]);assert header.stat().st_mtime_ns==stamp
        assert compute_header.stat().st_mtime_ns==compute_stamp
        with (source/'cull.lusl').open('a') as file:file.write('\n// compute dependency regression\n')
        run([args.cmake,'--build',build]);assert compute_header.stat().st_mtime_ns!=compute_stamp
        compute_original=(source/'cull.lusl').read_text()
        (source/'cull.lusl').write_text(compute_original.replace('threads(1, 1, 1)','threads(257, 1, 1)'))
        failure=run([args.cmake,'--build',build],success=False)
        assert 'workgroup' in failure or 'thread group' in failure, failure
        (source/'cull.lusl').write_text(compute_original)
        with (source/'raster.lusl').open('a') as file:file.write('\n// raster dependency regression\n')
        run([args.cmake,'--build',build]);assert header.stat().st_mtime_ns!=stamp
        original=(source/'raster.slang').read_text()
        # R18c's final implemented slot is legal on independently reflected targets.
        (source/'raster.slang').write_text(original.replace('vk::binding(1, 0)','vk::binding(15, 0)').replace('register(t1)','register(t15)'))
        run([args.cmake,'--build',build])
        expanded=json.loads((slang_output/'manifest.json').read_text())
        for target in targets:
            assert any(entry['binding']==15 and entry['kind']=='Texture2D'
                       for entry in expanded['interfaces'][target+'_fragment']['entries'])
        # Metal has no descriptor spaces; reject its separately reflected native
        # register at 16 as well as the invalid group on SPIR-V/WGSL targets.
        (source/'raster.slang').write_text(original.replace('vk::binding(1, 0)','vk::binding(1, 1)').replace('register(t1)','register(t16)'))
        failure=run([args.cmake,'--build',build],success=False)
        assert 'group/set zero' in failure or 'bounded profile' in failure or 'remapping' in failure, failure
    print('Installed portable raster: reflected stages, LUSL slot tables, native draws, retained snapshots, rebuild and malformed binding rejection passed')


def verify_native_interop(args, run, metal=False):
    fixture = Path(__file__).parent
    with tempfile.TemporaryDirectory(prefix='ludus native interop sdk-') as temporary:
        source = Path(temporary) / 'source'
        source.mkdir()
        for name in ('native_interop.cpp', 'native_interop_fixture.h', 'native_interop_metal.mm',
                     'native_interop_vulkan.cpp', 'native_interop.cmake'):
            shutil.copy2(fixture / name, source / name)
        (source / 'shaders').mkdir()
        shutil.copy2(fixture / 'shaders/native_interop.lusl', source / 'shaders/native_interop.lusl')
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.29)
project(NativeInteropSdkContract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi FoundationLogging)
include(native_interop.cmake)
ludus_add_native_interop_fixture(native_consumer "${CMAKE_CURRENT_SOURCE_DIR}")
''')
        build = Path(temporary) / 'build'
        command = [args.cmake, '-S', source, '-B', build, '-G', 'Ninja',
                   '-DCMAKE_MAKE_PROGRAM=' + args.ninja, '-DCMAKE_CXX_COMPILER=' + args.cxx,
                   '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                   '-DCMAKE_PREFIX_PATH=' + str(Path(args.sdk).resolve()) + ';' + args.conan,
                   '-DLUDUS_SLANG_COMPILER=' + args.slang]
        if metal:
            flags = '-stdlib=libc++ -nostdinc++ -isystem "' + args.libcxx + '"'
            command += ['-DCMAKE_OBJCXX_COMPILER=' + args.cxx, '-DCMAKE_OSX_SYSROOT=' + args.sysroot,
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', '-DCMAKE_CXX_FLAGS=' + flags,
                        '-DCMAKE_OBJCXX_FLAGS=' + flags]
        else:
            command += ['-DLUDUS_SPIRV_VALIDATOR=' + args.validator]
        run(command)
        run([args.cmake, '--build', build])
        run([build / 'native_consumer'])
    print('Installed native bridge: exact device, retained source, GPU sample and shutdown retirement passed')
