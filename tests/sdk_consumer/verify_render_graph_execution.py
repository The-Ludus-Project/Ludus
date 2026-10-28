#!/usr/bin/env python3
"""Qualify RG2/RG3 installed CLI and relocated CMake helper without engine sources.

The installed CLI and relocated helper must emit identical headers and identical
`.lurgrt` runtime generations, and both must match the source-tree tool that cooks
the generation decoded by the native GPU activation test.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

SOURCE_TOOLS = Path(__file__).resolve().parents[2] / 'scripts/python'
# Mirrors render_graph_format/runtime_package.h: ten uint32 header fields, then the identities.
PACKAGE_MAGIC = 0x4752554C
FORMAT_HEADER = (Path(__file__).resolve().parents[2]
                 / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/runtime_package.h')
HEADER_FIELDS = 10
IDENTITY_BYTES = 32


def read_package_version():
    """The generation version the cooker writes, as its C++ format owner declares it."""
    constants = dict(re.findall(r'inline constexpr foundation::uint32 (\w+) = (\w+);', FORMAT_HEADER.read_text()))
    version = constants['PACKAGE_VERSION']
    return int(constants.get(version, version))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('cli', 'sdk', 'slang', 'cxx', 'cmake', 'output'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--profiles', nargs='+', default=['metal', 'wgsl'])
    parser.add_argument('--validator')
    parser.add_argument('--spirv-cross')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.exists():
        raise RuntimeError('Acceptance output must be a new directory')
    output.mkdir(parents=True)
    sdk = Path(args.sdk).resolve()
    relocated = output / 'relocated'
    shutil.copytree(sdk / 'lib/cmake/Ludus', relocated / 'cmake')
    shutil.copytree(sdk / 'include', relocated / 'include')
    cli = str(Path(args.cli).resolve())
    def run_cli(*arguments):
        result = subprocess.run([cli, '--json', 'render-graph', *map(str, arguments)], cwd=output,
                                check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode != 0:
            raise RuntimeError(f"RG2 CLI failed ({result.returncode}): {result.stdout}\n{result.stderr}")
        return json.loads(result.stdout)
    source = output / 'sources'
    run_cli('new', source / 'graphs', '--output', 'depth')
    graph = source / 'graphs/main.lurender.json'
    value = json.loads(graph.read_text())
    for node in value['configurations'][0]['nodes']:
        if node['kind'] == 'Subgraph':
            node['asset'] = 'graphs/' + node['asset']
    graph.write_text(json.dumps(value))
    manifest = run_cli('new-build', graph, '--root', source, '--renderer-static', '--profiles', *args.profiles)['created']
    helper = relocated / 'cmake/shaders'
    flags = ['--helper', helper, '--compiler', Path(args.slang).resolve(), '--root', source]
    if args.validator:
        flags += ['--validator', Path(args.validator).resolve()]
    if args.spirv_cross:
        flags += ['--spirv-cross', Path(args.spirv_cross).resolve()]
    package = run_cli('build', manifest, output / 'package', *flags)['build']
    run_cli('emit-runtime', package['package'], output / 'cli.h', '--name', 'InstalledGraph')
    runtime = run_cli('cook-runtime', package['package'], output / 'cli.lurgrt')['runtime']
    # The installed CLI ships the RG3 authoring services, not only build/execution.
    difference = run_cli('schema-diff', package['package'], package['package'])['schema_diff']
    assert difference['cpu_interface'] == 'Compatible', difference
    # Force a nonvoid DepthOnly function through the real compiler: never silently discard its result.
    depth = source / 'graphs/main_shaders/depth.slang'
    original = depth.read_text()
    depth.write_text('float4 DepthMain(RasterVarying value) { return float4(1, 1, 1, 1); }\n')
    prior = (output / 'package/current.json').read_bytes()
    failed = subprocess.run([cli, '--json', 'render-graph', 'build', manifest, str(output / 'package'), *map(str, flags)],
                            cwd=output, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert failed.returncode != 0 and (output / 'package/current.json').read_bytes() == prior
    assert json.loads(failed.stdout)['error']['code'] == 'RenderGraphBuildShader'
    depth.write_text(original)
    consumer = output / 'consumer'
    consumer.mkdir()
    (consumer / 'main.cpp').write_text(
        '#include "InstalledGraph.h"\n'
        'static_assert(ludus::render_graph_packages::InstalledGraph::PASSES[0].Program == ludus::content::render_graph_format::Technique::DepthOnly);\n'
        'int main()\n{\n'
        '    const ludus::graphics::renderer::RenderGraphPlan plan = ludus::render_graph_packages::InstalledGraph::MakePlan();\n'
        '    return plan.Description.CameraCount == 1 && plan.Passes.GetSize() == 1 ? 0 : 1;\n}\n')
    cmake = ['cmake_minimum_required(VERSION 3.29)', 'project(RG2Consumer LANGUAGES CXX)',
             'set(CMAKE_CXX_STANDARD 23)', 'set(CMAKE_CXX_STANDARD_REQUIRED ON)', 'set(CMAKE_CXX_EXTENSIONS OFF)',
             f'include("{relocated.as_posix()}/cmake/LudusRenderGraph.cmake")', 'add_executable(consumer main.cpp)',
             f'target_include_directories(consumer PRIVATE "{relocated.as_posix()}/include")',
             'target_compile_options(consumer PRIVATE -fno-exceptions -Wall -Wextra -Werror)',
             f'ludus_compile_render_graph(TARGET consumer NAME InstalledGraph MANIFEST "{Path(manifest).as_posix()}" ROOT "{source.as_posix()}" RUNTIME_PACKAGE)']
    (consumer / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n')
    command = [args.cmake, '-S', str(consumer), '-B', str(output / 'build'),
               '-DCMAKE_CXX_COMPILER=' + str(Path(args.cxx).resolve()),
               '-DLUDUS_SLANG_COMPILER=' + str(Path(args.slang).resolve())]
    if args.validator:
        command.append('-DLUDUS_SPIRV_VALIDATOR=' + str(Path(args.validator).resolve()))
    if args.spirv_cross:
        command.append('-DLUDUS_SPIRV_CROSS=' + str(Path(args.spirv_cross).resolve()))
    subprocess.run(command, check=True, cwd=output)
    subprocess.run([args.cmake, '--build', str(output / 'build'), '--parallel', '2'], check=True, cwd=output)
    subprocess.run([str(output / 'build/consumer')], check=True, timeout=30)
    header = next((output / 'build').rglob('InstalledGraph.h'))
    assert header.read_bytes() == (output / 'cli.h').read_bytes(), 'installed CLI/CMake plans differ'
    verify_runtime_package(output, header, runtime, package['package'])
    print('RG2 installed CLI/CMake, relocation, numeric header and nonvoid depth rejection passed.')
    print('RG3 installed CLI, relocated CMake RUNTIME_PACKAGE and source tool cook identical .lurgrt bytes.')


def verify_runtime_package(output, header, runtime, package):
    """Require one `.lurgrt` image from every tool path, consistent with the emitted header."""
    cli = (output / 'cli.lurgrt').read_bytes()
    generated = sorted((output / 'build').rglob('InstalledGraph.lurgrt'))
    assert len(generated) == 1, generated
    assert generated[0].read_bytes() == cli, 'installed CLI and relocated CMake runtime generations differ'
    source = output / 'source.lurgrt'
    environment = dict(os.environ, PYTHONPATH=str(SOURCE_TOOLS), PYTHONDONTWRITEBYTECODE='1')
    subprocess.run([sys.executable, '-m', 'ludus_tools', '--json', 'render-graph', 'cook-runtime', str(package), str(source)],
                   check=True, cwd=output, env=environment, stdout=subprocess.DEVNULL)
    assert source.read_bytes() == cli, 'installed and source-tree runtime generations differ'
    magic, version, *_, stages = struct.unpack_from('<' + 'I' * HEADER_FIELDS, cli)
    assert (magic, version) == (PACKAGE_MAGIC, read_package_version()) and stages == runtime['stages'], \
        (hex(magic), version, stages, runtime)
    identity = cli[HEADER_FIELDS * 4:HEADER_FIELDS * 4 + IDENTITY_BYTES]
    generation = cli[HEADER_FIELDS * 4 + IDENTITY_BYTES:HEADER_FIELDS * 4 + 2 * IDENTITY_BYTES]
    assert identity.hex() == runtime['interface_identity'] and generation.hex() == runtime['generation'], runtime
    # The host header compiled against this generation names the same CPU interface identity.
    text = header.read_text()
    block = text[text.index('INTERFACE_IDENTITY'):]
    block = block[:block.index('};')]
    assert bytes(int(value, 16) for value in re.findall(r'0x[0-9a-f]+', block)) == identity, 'header/package identity differ'


if __name__ == '__main__':
    main()
