#!/usr/bin/env python3
"""Qualify the installed CLI/SDK RG1 packages, incremental builds and CPU packing."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('cli', 'sdk', 'slang', 'cxx', 'output'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--profiles', nargs='+', default=['metal', 'wgsl'])
    parser.add_argument('--validator')
    parser.add_argument('--spirv-cross')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.exists():
        raise RuntimeError('Acceptance output must be a new directory')
    output.mkdir(parents=True)
    sdk = Path(args.sdk).resolve()
    helper = sdk / 'lib/cmake/Ludus/shaders'
    cli = str(Path(args.cli).resolve())
    def run_cli(*arguments):
        result = subprocess.run([cli, '--json', 'render-graph', *map(str, arguments)], cwd=output,
                                check=True, text=True, stdout=subprocess.PIPE)
        return json.loads(result.stdout)
    root = output / 'source'
    run_cli('new', root)
    manifest = run_cli('new-build', root / 'main.lurender.json', '--profiles', *args.profiles)['created']
    flags = ['--helper', str(helper), '--compiler', str(Path(args.slang).resolve())]
    if args.validator:
        flags += ['--validator', str(Path(args.validator).resolve())]
    if args.spirv_cross:
        flags += ['--spirv-cross', str(Path(args.spirv_cross).resolve())]
    first = run_cli('build', manifest, output / 'first', *flags)['build']
    second = run_cli('build', manifest, output / 'second', *flags)['build']
    assert first['generation'] == second['generation'], 'independent clean builds differ'
    warm = run_cli('build', manifest, output / 'first', *flags)['build']
    assert warm['cache_misses'] == 0
    package = run_cli('verify-package', first['package'])['package']
    assert package['counts']['configurations'] == 2 and package['counts']['programs'] == 3
    assert package['counts']['artifacts'] == 3 * len(args.profiles)
    # Exercise real transitive invalidation, compiler failure and saved output.
    color = root / 'main_shaders/color.slang'
    include = color.with_name('factor.slang')
    include.write_text('float Factor() { return 1; }\n')
    original = color.read_text()
    color.write_text('#include "factor.slang"\n' + original.replace('material.gain', 'material.gain * Factor()'))
    run_cli('build', manifest, output / 'first', *flags)
    include.write_text('float Factor() { return 2; }\n')
    changed = run_cli('build', manifest, output / 'first', *flags)['build']
    assert changed['cache_misses'] == len(args.profiles) and changed['cache_hits'] == 2 * len(args.profiles)
    prior = (output / 'first/current.json').read_bytes()
    # The pinned compiler's diagnostic arrives through the helper's prefixed stderr report and
    # must still name the authored function file and line the editor opens.
    broken = original.replace('material.tint', 'undefinedTint')
    broken_line, broken_column = next((i, line.index('undefinedTint') + 1)
                                      for i, line in enumerate(broken.splitlines(), 1) if 'undefinedTint' in line)
    color.write_text(broken)
    failed = subprocess.run([cli, '--json', 'render-graph', 'build', manifest, str(output / 'first'), *flags], cwd=output,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert failed.returncode != 0 and (output / 'first/current.json').read_bytes() == prior
    error = json.loads(failed.stdout)['error']
    assert error['code'] == 'RenderGraphBuildShader'
    # The caret under the echoed line also marks the undefined token's column and length.
    assert error['location'] == {'source': 'main_shaders/color.slang', 'line': broken_line, 'column': broken_column,
                                 'length': len('undefinedTint')}, error.get('location')
    # The report names no machine path: no helper command echoes or temporary build roots.
    assert str(output) not in error['message'] and str(output.resolve()) not in error['message'], error['message']
    assert not any(line.startswith('+ ') for line in error['message'].splitlines()), error['message']
    package_path = Path(first['package'])
    cpp = ['#include "schemas.h"', '#include <cstring>', 'using namespace ludus;',
           'using namespace ludus::render_graph_generated;', 'int main()', '{',
           '    Material value{};', '    value.tint[0] = 1.0F;', '    value.tint[1] = 2.0F;',
           '    value.tint[2] = 3.0F;', '    value.gain = 4.0F;', '    foundation::uint8 bytes[64]{};']
    for program in package['programs']:
        if program['id'] != 'Color':
            continue
        layout = program['descriptor']['layouts'][0]
        size = layout['size']
        offset = layout['fields'][1]['offset']
        assert offset == (16 if program['profile'] == 'metal' else 12)
        function = 'Pack_Color_' + program['profile'] + '_material'
        cpp += ['    for (auto& byte : bytes)', '    {', '        byte = 123;', '    }',
                f'    if ({function}(value, bytes, {size - 1}) || {function}(value, nullptr, {size}))',
                '    {', '        return 1;', '    }',
                '    for (auto byte : bytes)', '    {', '        if (byte != 123)', '        {', '            return 2;', '        }', '    }',
                f'    if (!{function}(value, bytes, {size}) ||',
                '        std::memcmp(bytes, value.tint, sizeof(value.tint)) != 0 ||',
                f'        std::memcmp(bytes + {offset}, &value.gain, sizeof(value.gain)) != 0 || bytes[{size}] != 123)',
                '    {', '        return 3;', '    }']
        if offset > 12:
            cpp += ['    if (bytes[12] != 0 || bytes[13] != 0 || bytes[14] != 0 || bytes[15] != 0)',
                    '    {', '        return 4;', '    }']
    cpp += ['    return 0;', '}']
    (output / 'packing.cpp').write_text('\n'.join(cpp) + '\n')
    command = [args.cxx, '-std=c++23', '-fno-exceptions', '-Wall', '-Wextra', '-Werror',
               '-I' + str(sdk / 'include'), '-I' + str(package_path), str(output / 'packing.cpp'), '-o', str(output / 'packing')]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True)
    subprocess.run([str(output / 'packing')], check=True, timeout=30)
    # Preserve a valid package path for the pinned browser stage-admission fixture.
    (output / 'acceptance.json').write_text(json.dumps({'package': first['package'], 'generation': first['generation'],
                                                      'profiles': args.profiles, 'clean_builds_match': True,
                                                      'transitive_misses': changed['cache_misses'], 'sanitized': args.sanitize}, indent=2) + '\n')
    # Relocated cooked package verification requires neither SDK nor compiler.
    relocated = output / 'relocated' / first['generation']
    relocated.parent.mkdir()
    shutil.copytree(package_path, relocated)
    run_cli('verify-package', relocated)
    print('Installed RG1 CLI/SDK, clean/warm/invalidation/failure, relocation and CPU packing passed.')


if __name__ == '__main__':
    main()
