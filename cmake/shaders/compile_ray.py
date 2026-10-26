#!/usr/bin/env python3
"""Cook the fixed native RT0 kernel with verified existing host-tool pins."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from compile_shader import entry, run


def verify_spirv(path):
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        raise RuntimeError('Malformed ray SPIR-V')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203 or words[1] > 0x00010500:
        raise RuntimeError('Ray SPIR-V exceeds the Vulkan 1.2 profile')
    capabilities, bindings, sets, local = set(), {}, {}, None
    index = 5
    while index < len(words):
        count, op = words[index] >> 16, words[index] & 65535
        if not count or index + count > len(words):
            raise RuntimeError('Malformed ray SPIR-V instruction')
        fields = words[index + 1:index + count]
        if op == 17:
            if len(fields) != 1:
                raise RuntimeError("Malformed SPIR-V capability")
            capabilities.add(fields[0])
        elif op == 71 and len(fields) == 3:
            if fields[1] == 33:
                bindings[fields[0]] = fields[2]
            elif fields[1] == 34:
                sets[fields[0]] = fields[2]
        elif op == 16 and len(fields) == 5 and fields[1] == 17:
            local = tuple(fields[2:])
        index += count
    if capabilities != {1, 4472} or set(bindings.values()) != set(range(5)) or len(bindings) != 5:
        raise RuntimeError('Ray kernel does not have the fixed five-binding RayQueryKHR ABI')
    if any(sets.get(resource) != 0 for resource in bindings) or local != (64, 1, 1):
        raise RuntimeError('Ray kernel has wrong descriptor sets or workgroup dimensions')
    return words


def verify_metal(source):
    bindings = {name: int(index) for name, index in re.findall(r'(\w+)\s+\[\[buffer\((\d+)\)\]\]', source)}
    if 'intersection_query' not in source or bindings != {'scene': 0, 'rays': 1, 'hits': 2, 'opacity': 3, 'parameters': 4}:
        raise RuntimeError('Ray MSL variant differs from the fixed native ABI')
    names = re.findall(r'kernel\s+void\s+(\w+)\s*\(', source)
    if len(names) != 1 or ')LUDUS_RAY"' in source:
        raise RuntimeError('Invalid ray MSL entry or literal delimiter')
    return names[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('source', 'output', 'name', 'compiler', 'lock', 'spirv-cross', 'spirv-cross-lock'):
        parser.add_argument('--' + field, required=True)
    parser.add_argument('--validator')
    parser.add_argument('--metal', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', args.name):
        raise RuntimeError('Invalid ray artifact name')
    lock = json.loads(Path(args.lock).read_text())
    if run([args.compiler, '-version']).strip() != lock['slang']['version']:
        raise RuntimeError('Ray compiler differs from the SDK pin')
    cross_pin = json.loads(Path(args.spirv_cross_lock).read_text())['spirv_cross']
    tool = Path(args.spirv_cross).resolve()
    build = json.loads(Path(str(tool) + '.build.json').read_text())
    if any(build.get(key) != cross_pin[key] for key in ('tag', 'commit', 'source_sha256')):
        raise RuntimeError('Ray translator source differs from SDK pin')
    if build.get('binary_sha256') != hashlib.sha256(tool.read_bytes()).hexdigest():
        raise RuntimeError('Ray translator binary differs from verified build')
    if not args.metal:
        if not args.validator or hashlib.sha256(Path(args.validator).read_bytes()).hexdigest() != lock['spirv_tools']['val_sha256']:
            raise RuntimeError('Native Vulkan ray artifacts require the pinned SPIR-V validator')
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    binary = output / (args.name + '.spv')
    metal = output / (args.name + '.metal')
    commands = [[args.compiler, args.source, '-entry', 'QueryMain', '-stage', 'compute', '-target', 'spirv',
                 '-profile', 'spirv_1_5', '-capability', 'spvRayQueryKHR', '-o', str(binary)]]
    run(commands[-1])
    words = verify_spirv(binary)
    if not args.metal:
        commands.append([args.validator, '--target-env', 'vulkan1.2', str(binary)])
        run(commands[-1])
    commands.append([str(tool), str(binary), '--msl', '--msl-version', '20400', '--msl-decoration-binding', '--output', str(metal)])
    run(commands[-1])
    source = metal.read_text()
    metal_entry = verify_metal(source)
    header = '#pragma once\n#include <ludus/graphics/rhi/render.h>\n'
    header += f'namespace ludus::shaders::{args.name} {{\n'
    header += 'inline constexpr ludus::foundation::uint32 SPIRV[] = {' + ','.join(hex(word) for word in words) + '};\n'
    header += 'inline constexpr char MSL[] = R"LUDUS_RAY(' + source + ')LUDUS_RAY";\n'
    header += 'inline graphics::rhi::ShaderDescription Query() noexcept {\n graphics::rhi::ShaderDescription result;\n'
    header += ' result.Stage = graphics::rhi::ShaderStage::Compute; result.Spirv = SPIRV;\n'
    header += f' result.SpirvEntry = "{entry(binary)}"; result.Msl = MSL; result.MslEntry = "{metal_entry}";\n return result;\n}}\n}}\n'
    manifest = {'profile': {'vulkan': '1.2 + KHR_acceleration_structure + KHR_ray_query', 'metal': 'MSL 2.4',
                            'workgroup': [64, 1, 1], 'ray_bytes': 48, 'result_bytes': 32},
                'source_sha256': hashlib.sha256(Path(args.source).read_bytes()).hexdigest(),
                'commands': commands, 'slang': lock['slang']['version'], 'spirv_cross': build,
                'spirv_validation': 'native Metal compiler/runtime only' if args.metal else 'pinned spirv-val Vulkan 1.2',
                'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (binary, metal)}}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / (args.name + '.h')).write_text(header)


if __name__ == '__main__':
    main()
