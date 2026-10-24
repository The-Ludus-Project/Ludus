#!/usr/bin/env python3
"""Offline, pinned Slang CLI driver for installed Ludus consumers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


def run(command):
    print('+ ' + ' '.join(command), flush=True)
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def entry(path):
    data = path.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    index = 5
    names = []
    while index < len(words):
        count, op = words[index] >> 16, words[index] & 65535
        if count == 0 or index + count > len(words):
            raise RuntimeError('Malformed SPIR-V instruction')
        if op == 15:
            names.append(struct.pack('<' + 'I' * (count - 3), *words[index + 3:index + count]).split(b'\0')[0].decode())
        index += count
    if len(names) != 1:
        raise RuntimeError('Expected one entry per SPIR-V module')
    return names[0]


def uniform_size(path):
    parameters = json.loads(path.read_text())["parameters"]
    if not parameters:
        return 0
    if len(parameters) != 1:
        raise RuntimeError("Fullscreen API supports only one uniform resource")
    resource = parameters[0]
    binding = resource["binding"]
    if (binding["kind"] != "descriptorTableSlot" or binding["index"] != 0 or
            binding.get("space", 0) != 0 or resource["type"]["kind"] != "constantBuffer"):
        raise RuntimeError("Fullscreen API requires a constant buffer at set/group 0 binding 0")
    size = resource["type"]["elementVarLayout"]["binding"]["size"]
    if size < 1 or size > 16384:
        raise RuntimeError("Uniform exceeds fullscreen API bounds")
    return size


# Independent std140 derivation from the emitted GLSL ES block. Deliberately
# narrow for the fullscreen contract, not a general GLSL parser. Does not borrow
# SPIR-V/WGSL offsets; the backend re-checks against the real linked program.
GLSL_ES_KINDS = {'float': (4, 4), 'vec2': (8, 8), 'vec3': (12, 16), 'vec4': (16, 16), 'mat4': (64, 16)}


def glsl_es_layout(code):
    match = re.search(r'uniform\s+(\w+)\s*\{([^}]+)\}', code)
    if match is None:
        return {'block': None, 'offsets': {}, 'size': 0, 'alignment': 16, 'binding': 0}
    if re.search(r'layout\(std140\)\s+uniform\s+' + re.escape(match.group(1)), code) is None:
        raise RuntimeError('GLSL ES uniform block is not std140')
    # Slang can lower a fixed array to a one-field std140 wrapper struct.
    # Accept exactly that shape; arbitrary/nested structs remain unsupported.
    primitive = r'(?:highp\s+|mediump\s+|lowp\s+)?(vec[234]|mat4|float)\s+(\w+)\s*(?:\[\s*([0-9]+)\s*\])?'
    wrappers = {}
    for type_name, body in re.findall(r'struct\s+(\w+)\s*\{([^}]+)\}\s*;', code):
        field = re.fullmatch(r'\s*' + primitive + r'\s*;\s*', body)
        if field and field.group(2) == 'data' and field.group(3):
            if type_name in wrappers:
                raise RuntimeError('Duplicate GLSL ES array wrapper')
            wrappers[type_name] = (field.group(1), field.group(3))
    offset = 0
    alignment = 16
    offsets = {}
    for declaration in match.group(2).split(';'):
        if not declaration.strip():
            continue
        major = re.match(r'\s*layout\((row_major|column_major)\)\s*', declaration)
        if major:
            declaration = declaration[major.end():]
        field = re.fullmatch(r'\s*' + primitive + r'\s*', declaration)
        if field:
            kind, name, count = field.groups()
        else:
            wrapper = re.fullmatch(r'\s*(\w+)\s+(\w+)\s*', declaration)
            if wrapper is None or wrapper.group(1) not in wrappers:
                raise RuntimeError('GLSL ES layout contains unsupported field types; refusing an inferred layout')
            kind, count = wrappers[wrapper.group(1)]
            name = wrapper.group(2)
        if major and kind != 'mat4':
            raise RuntimeError('GLSL ES matrix order qualifier requires mat4 or its fixed array wrapper')
        if name in offsets:
            raise RuntimeError("Duplicate GLSL ES uniform member")
        size, align = GLSL_ES_KINDS[kind]
        if count:
            length = int(count)
            if length < 1 or length > 1024:
                raise RuntimeError("GLSL ES uniform array exceeds bounded layout support")
            align = 16
            size = (size + 15) // 16 * 16 * length
        alignment = max(alignment, align)
        offset = (offset + align - 1) // align * align
        offsets[name] = offset
        offset += size
    size = (offset + alignment - 1) // alignment * alignment
    if size > 16384:
        raise RuntimeError('GLSL ES uniform exceeds fullscreen bounds')
    return {'block': match.group(1), 'offsets': offsets, 'size': size, 'alignment': alignment, 'binding': 0}


def glsl_es_contract(code):
    # One std140 uniform block at binding 0, a `main` entry, no desktop-only
    # extensions or base-vertex/instance builtins unsupported by WebGL 2.
    for forbidden in ('GL_ARB_shader_draw_parameters', 'gl_BaseVertex', 'gl_BaseInstance'):
        if forbidden in code:
            raise RuntimeError('GLSL ES uses a feature outside the WebGL 2 contract: ' + forbidden)
    if not code.lstrip().startswith('#version 300 es'):
        raise RuntimeError('GLSL ES artifact is not version 300 es')
    if re.search(r'\bvoid\s+main\s*\(', code) is None:
        raise RuntimeError('GLSL ES entry point is not main')
    blocks = re.findall(r'uniform\s+\w+\s*\{', code)
    if len(blocks) > 1:
        raise RuntimeError('Emitted GLSL ES exceeds the single-block fullscreen contract')


def constant_buffer_uniform_size(path, api, slot):
    """Uniform size of a target whose reflection names constant buffer slots directly."""
    parameters = json.loads(path.read_text())["parameters"]
    if not parameters:
        return 0
    if len(parameters) != 1:
        raise RuntimeError(f"{api} fullscreen API supports only one uniform resource")
    resource = parameters[0]
    binding = resource["binding"]
    if (binding["kind"] != "constantBuffer" or binding["index"] != 0 or
            binding.get("space", 0) != 0 or resource["type"]["kind"] != "constantBuffer"):
        raise RuntimeError(f"{api} fullscreen API requires a constant buffer at {slot}")
    size = resource["type"]["elementVarLayout"]["binding"]["size"]
    if not 1 <= size <= 16384:
        raise RuntimeError(f"{api} uniform exceeds fullscreen API bounds")
    return size


def metal_uniform_size(path):
    return constant_buffer_uniform_size(path, "Metal", "buffer 0")


def relative_line_directives(code, roots):
    """Name the files in emitted ``#line`` directives relative to a captured root.

    Slang spells the compiled source and its includes by absolute path. Embedded
    in a generated header, that text differs per checkout, and a compiler cache
    shared between checkouts can then pair one checkout's literal with another's
    folded length: when ccache hashes preprocessed output it relativizes every
    line-start ``#line N "path"`` under its base_dir, and it cannot tell a raw
    string literal from a line marker. Keep the directives for Metal diagnostics,
    but name each file by its shortest path relative to the source or include
    directories, so every checkout emits identical bytes. An absolute path
    outside every root fails: pass its directory through INCLUDE.
    """
    bases = []
    for root in roots:
        spelled = Path(root)
        bases.append(spelled)
        resolved = spelled.resolve()
        if resolved != spelled:
            bases.append(resolved)

    def remap(match):
        filename = Path(match[2])
        if not filename.is_absolute():
            return match[0]
        candidates = []
        for spelling in (filename, filename.resolve()):
            for base in bases:
                try:
                    candidates.append(spelling.relative_to(base).as_posix())
                except ValueError:
                    continue
        if not candidates:
            raise RuntimeError(f'Emitted line directive names {filename} outside the captured source and include directories')
        return match[1] + min(candidates, key=len) + match[3]

    code = re.sub(r'(^[ \t]*#line\s+\d+\s+")([^"]+)(")', remap, code, flags=re.M)
    for base in bases:
        if str(base) in code:
            raise RuntimeError(f'Emitted shader text still names the checkout directory {base}')
    return code


def compile_metal(args, lock):
    # Thanks to the Slang team, "Metal-Specific Functionalities", User Guide:
    # https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-02-metal-target-specific.html
    # ConstantBuffer maps to constant pointers / buffer indices, and vertex ID
    # and position semantics map to Metal attributes. Reflect this target's
    # layout independently; float3 occupies 16 bytes in the Metal fixture.
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    extra = [part for path in args.include for part in ('-I', path)] + ['-D' + value for value in args.define]
    roots = [Path(args.source).parent, *args.include]
    commands, dependencies, entries, sizes, sources = [], [], {}, {}, {}
    for stage, source_entry in (('vertex', args.vertex), ('fragment', args.fragment)):
        artifact = output / f'{args.name}.{stage}.metal'
        reflection = output / f'{stage}.reflection.json'
        depfile = output / f'{stage}.d'
        command = [args.compiler, args.source, '-target', 'metal', '-profile', 'sm_6_0',
                   '-entry', source_entry, '-stage', stage, '-o', str(artifact),
                   '-reflection-json', str(reflection), '-depfile', str(depfile), *extra]
        commands.append(command)
        run(command)
        dependencies.append(depfile.read_text().split(':', 1)[1].strip())
        reflected = json.loads(reflection.read_text())['entryPoints']
        code = relative_line_directives(artifact.read_text(), roots)
        artifact.write_text(code)
        emitted = re.findall(r'\[\[' + stage + r'\]\]\s+\w+\s+(\w+)\s*\(', code)
        if len(reflected) != 1 or reflected[0]['stage'] != stage or len(emitted) != 1:
            raise RuntimeError('Expected one Metal entry with the requested stage')
        name = emitted[0]
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,62}', name):
            raise RuntimeError('Metal entry exceeds the fullscreen entry contract')
        if any(index != '0' for index in re.findall(r'\[\[buffer\((\d+)\)\]\]', code)):
            raise RuntimeError('Metal resource outside buffer 0')
        sizes[stage] = metal_uniform_size(reflection)
        entries[stage] = name
        sources[stage] = code
    header = '#pragma once\n#include <ludus/graphics/rhi/render.h>\n'
    header += f'namespace ludus::shaders::{args.name} {{\n'
    for stage in ('vertex', 'fragment'):
        delimiter = 'LUDUS_MSLV' if stage == 'vertex' else 'LUDUS_MSLF'
        code = sources[stage]
        if f'){delimiter}"' in code:
            raise RuntimeError('Metal source conflicts with header delimiter')
        header += f'inline constexpr char {stage.upper()}_MSL[] = R"{delimiter}(' + code + f'){delimiter}";\n'
        header += f'inline graphics::rhi::ShaderDescription {stage.title()}() noexcept {{\n'
        header += f' graphics::rhi::ShaderDescription result; result.Stage = graphics::rhi::ShaderStage::{stage.title()};\n'
        header += f' result.UniformSize = {sizes[stage]}; result.Msl = {stage.upper()}_MSL; result.MslEntry = "{entries[stage]}";\n return result;\n}}\n'
    header += '}\n'
    manifest = {'compiler': lock['slang'], 'commands': commands, 'uniform_sizes': sizes,
                'metal_entries': entries, 'profiles': {'metal': 'sm_6_0'},
                'sha256': {f'{args.name}.{stage}.metal': hashlib.sha256(sources[stage].encode()).hexdigest()
                           for stage in ('vertex', 'fragment')},
                'layout': 'See separate Metal reflection JSON; never assume packing equality.'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    target = output / f'{args.name}.h'
    escaped_target = str(target).replace(' ', '\\ ').replace('#', '\\#')
    (output / 'shader.d').write_text(escaped_target + ': ' + ' '.join(dependencies) + '\n')
    # Header last: compilation/reflection failures cannot complete the command.
    target.write_text(header)


# DXBC container header: code, 16-byte hash, version, total size and part count.
DXIL_CONTAINER_HEADER_BYTES = 32
DXIL_HASH = slice(4, 20)
DXIL_SIZE_OFFSET = 24


def dxil_container(path):
    """Read a DXC container and require the DXIL validator's signature."""
    data = path.read_bytes()
    if len(data) < DXIL_CONTAINER_HEADER_BYTES or data[:4] != b'DXBC':
        raise RuntimeError(f'{path.name} is not a DXIL container')
    if struct.unpack_from('<I', data, DXIL_SIZE_OFFSET)[0] != len(data):
        raise RuntimeError(f'{path.name} container size differs from its file size')
    digest = data[DXIL_HASH]
    # Unsigned output carries a zero or a constant "bypass" hash, which retail
    # Direct3D 12 runtimes reject. dxil.dll must sign every artifact.
    if len(set(digest)) == 1:
        raise RuntimeError(f'{path.name} is not signed; dxil.dll must sit beside dxcompiler.dll')
    return data


def depfile_dependencies(path):
    """Dependencies of a one-target depfile; Windows drive colons are not separators."""
    parts = re.split(r':\s', path.read_text(), maxsplit=1)
    if len(parts) != 2:
        raise RuntimeError(f'{path.name} is not a one-target depfile')
    return parts[1].strip()


def compile_dxil(args, lock):
    # Thanks to the Slang team, "Supported Compilation Targets" (DXIL through DXC),
    # User Guide: https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-targets.html
    # Slang emits HLSL and has the pinned DXC compile and sign it. Reflect this
    # target independently: register b0, space 0 is the root constant buffer view.
    if not args.dxc_path:
        raise RuntimeError('DXIL builds require --dxc-path')
    pin = lock['dxc']
    for library, key in (('dxcompiler.dll', 'dxcompiler_sha256'), ('dxil.dll', 'dxil_sha256')):
        if hashlib.sha256((Path(args.dxc_path) / library).read_bytes()).hexdigest() != pin[key]:
            raise RuntimeError(f'{library} differs from SDK pin')
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    extra = [part for path in args.include for part in ('-I', path)] + ['-D' + value for value in args.define]
    commands, dependencies, entries, sizes, containers = [], [], {}, {}, {}
    for stage, source_entry in (('vertex', args.vertex), ('fragment', args.fragment)):
        artifact = output / f'{args.name}.{stage}.dxil'
        reflection = output / f'{stage}.reflection.json'
        depfile = output / f'{stage}.d'
        command = [args.compiler, args.source, '-target', 'dxil', '-profile', 'sm_6_0',
                   '-entry', source_entry, '-stage', stage, '-D', 'LUDUS_DXIL=1', '-o', str(artifact),
                   '-reflection-json', str(reflection), '-depfile', str(depfile),
                   '-dxc-path', args.dxc_path, *extra]
        commands.append(command)
        if '52001' in run(command):
            raise RuntimeError('Slang could not load dxil.dll to sign the DXIL output')
        dependencies.append(depfile_dependencies(depfile))
        reflected = json.loads(reflection.read_text())['entryPoints']
        if len(reflected) != 1 or reflected[0]['stage'] != stage or reflected[0]['name'] != source_entry:
            raise RuntimeError('Expected one DXIL entry with the requested stage')
        sizes[stage] = constant_buffer_uniform_size(reflection, 'DXIL', 'register b0, space 0')
        entries[stage] = source_entry
        containers[stage] = dxil_container(artifact)
    header = '#pragma once\n#include <ludus/foundation/base/types.h>\n#include <ludus/graphics/rhi/render.h>\n'
    header += f'namespace ludus::shaders::{args.name} {{\n'
    for stage in ('vertex', 'fragment'):
        data = containers[stage]
        header += f'alignas(4) inline constexpr foundation::uint8 {stage.upper()}_DXIL[] = {{\n'
        header += ',\n'.join('    ' + ', '.join(f'0x{x:02x}U' for x in data[i:i + 16]) for i in range(0, len(data), 16))
        header += '\n};\n'
        header += f'inline graphics::rhi::ShaderDescription {stage.title()}() noexcept {{\n'
        header += f' graphics::rhi::ShaderDescription result; result.Stage = graphics::rhi::ShaderStage::{stage.title()};\n'
        header += f' result.UniformSize = {sizes[stage]}; result.Dxil = {stage.upper()}_DXIL; result.DxilEntry = "{entries[stage]}";\n return result;\n}}\n'
    header += '}\n'
    manifest = {'compiler': lock['slang'], 'dxc': {'version': pin['version'], 'sha256': pin['sha256']},
                'commands': commands, 'uniform_sizes': sizes, 'dxil_entries': entries, 'profiles': {'dxil': 'sm_6_0'},
                'sha256': {f'{args.name}.{stage}.dxil': hashlib.sha256(containers[stage]).hexdigest()
                           for stage in ('vertex', 'fragment')},
                'layout': 'See separate DXIL reflection JSON; never assume packing equality.'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    target = output / f'{args.name}.h'
    escaped_target = str(target).replace(' ', '\\ ').replace('#', '\\#')
    (output / 'shader.d').write_text(escaped_target + ': ' + ' '.join(dependencies) + '\n')
    # Header last: compilation/reflection failures cannot complete the command.
    target.write_text(header)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('source', 'output', 'name', 'compiler', 'lock'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--vertex')
    parser.add_argument('--fragment')
    parser.add_argument('--compute')
    parser.add_argument('--validator')
    parser.add_argument('--worker-socket')
    parser.add_argument('--worker-identity')
    parser.add_argument('--metal', action='store_true')
    # Windows hosts: compile and sign DXIL with the pinned DXC libraries in this directory.
    parser.add_argument('--dxil', action='store_true')
    parser.add_argument('--dxc-path')
    parser.add_argument('--raster', action='store_true')
    parser.add_argument('--fragment-storage', action='store_true')
    parser.add_argument('--native-ray-queries', action='store_true')
    parser.add_argument('--native-binding-arrays', action='store_true')
    parser.add_argument('--runtime-binding-arrays', action='store_true')
    parser.add_argument('--include', action='append', default=[])
    parser.add_argument('--define', action='append', default=[])
    # Optional build-time SPIR-V -> GLSL ES 3.00 translator for the WebGL 2
    # browser backend artifact. When absent, SPIR-V/WGSL builds are unchanged.
    parser.add_argument('--spirv-cross')
    parser.add_argument('--spirv-cross-lock')
    parser.add_argument("--graph-forward-material", action="store_true", help="Admit the verified renderer forward material resource interface")
    parser.add_argument("--graph-stage", choices=("vertex", "fragment", "compute"))
    parser.add_argument("--graph-target", choices=("spirv", "wgsl", "metal", "glsl"))
    args = parser.parse_args()
    if args.graph_forward_material and args.graph_stage not in ('vertex', 'fragment'):
        raise RuntimeError('Forward material resources require a closed generated raster graph stage')
    lock = json.loads(Path(args.lock).read_text())
    uses_native_worker = args.worker_socket and (not args.native_ray_queries or args.graph_stage)
    if not uses_native_worker and run([args.compiler, '-version']).strip() != lock['slang']['version']:
        raise RuntimeError('Slang version differs from SDK pin')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', args.name):
        raise RuntimeError('Invalid shader name')
    if args.fragment_storage and not ((args.raster and not args.compute) or args.graph_stage == 'fragment'):
        raise RuntimeError('Fragment storage requires raster stages')
    if args.runtime_binding_arrays and (not args.native_binding_arrays or not args.raster or args.metal):
        raise RuntimeError("Runtime binding arrays require native Vulkan raster stages")
    if args.graph_stage or args.graph_target:
        if not args.graph_stage or not args.graph_target or args.compute or args.vertex or args.fragment or args.raster or args.metal or args.include or args.define or args.native_binding_arrays or args.runtime_binding_arrays:
            raise RuntimeError("Graph stages require a closed generated source and exactly one target/stage")
        from compile_graph_stage import compile_graph_stage
        compile_graph_stage(args, lock)
        return
    if args.worker_socket and not (args.raster or args.compute):
        raise RuntimeError('Native worker requires graph, raster or compute stages; fullscreen compatibility stays offline')
    if args.native_binding_arrays and not (args.raster or args.compute):
        raise RuntimeError("Native binding arrays require raster or compute stages")
    if args.compute and (args.vertex or args.fragment or args.raster):
        raise RuntimeError("Compute is mutually exclusive with raster/fullscreen stages")
    if not args.compute and (not args.vertex or not args.fragment):
        raise RuntimeError("Raster/fullscreen compilation requires vertex and fragment entries")
    if args.dxil:
        if (args.raster or args.compute or args.metal or args.spirv_cross or args.validator or args.worker_socket or
                args.native_binding_arrays or args.fragment_storage):
            raise RuntimeError('DXIL builds support only fullscreen vertex/fragment stages')
        compile_dxil(args, lock)
        return
    if args.raster or args.compute:
        from compile_raster import compile_raster
        compile_raster(args, lock)
        return
    if args.metal:
        compile_metal(args, lock)
        return
    if not args.validator:
        raise RuntimeError('SPIR-V/WGSL builds require --validator')
    if hashlib.sha256(Path(args.validator).read_bytes()).hexdigest() != lock['spirv_tools']['val_sha256']:
        raise RuntimeError('SPIR-V validator differs from SDK pin')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', args.name):
        raise RuntimeError('Invalid shader name')
    cross_lock = None
    if args.spirv_cross:
        if not args.spirv_cross_lock:
            raise RuntimeError('GLSL ES build requires --spirv-cross-lock to pin the translator')
        cross_lock = json.loads(Path(args.spirv_cross_lock).read_text())['spirv_cross']
        tool = Path(args.spirv_cross).resolve()
        build = json.loads(Path(str(tool) + '.build.json').read_text())
        for key in ('tag', 'commit', 'source_sha256'):
            if build.get(key) != cross_lock[key]:
                raise RuntimeError('SPIRV-Cross source identity differs from SDK pin')
        if build.get('binary_sha256') != hashlib.sha256(tool.read_bytes()).hexdigest():
            raise RuntimeError('SPIRV-Cross binary differs from verified build')
    output = Path(args.output); output.mkdir(parents=True, exist_ok=True)
    extra = [part for path in args.include for part in ('-I', path)] + ['-D' + value for value in args.define]
    commands = []; dependencies = []
    entries = {}
    for stage, source_entry in (('vertex', args.vertex), ('fragment', args.fragment)):
        binary = output / f'{args.name}.{stage}.spv'
        depfile = output / f'{stage}.d'
        command = [args.compiler, args.source, '-target', 'spirv', '-profile', 'spirv_1_3', '-entry', source_entry,
                   '-stage', stage, '-o', str(binary), '-reflection-json', str(output / f'{stage}.reflection.json'), '-depfile', str(depfile), *extra]
        commands.append(command); run(command)
        run([args.validator, '--target-env', 'vulkan1.1', str(binary)])
        dependencies.append(depfile.read_text().split(':', 1)[1].strip())
        entries[stage] = entry(binary)
    wgsl = output / f'{args.name}.wgsl'; depfile = output / 'wgsl.d'
    command = [args.compiler, args.source, '-target', 'wgsl', '-profile', 'sm_6_0', '-entry', args.vertex,
               '-entry', args.fragment, '-o', str(wgsl), '-reflection-json', str(output / 'wgsl.reflection.json'), '-depfile', str(depfile), *extra]
    commands.append(command); run(command); dependencies.append(depfile.read_text().split(':', 1)[1].strip())
    sizes = {stage: uniform_size(output / f'{stage}.reflection.json') for stage in ('vertex', 'fragment', 'wgsl')}
    code = wgsl.read_text()
    bindings = re.findall(r'@binding\((\d+)\)\s+@group\((\d+)\)\s+var<(\w+)>', code)
    if any(binding != ('0', '0', 'uniform') for binding in bindings) or len(bindings) > 1:
        raise RuntimeError('Emitted WGSL resources do not fit the fullscreen binding contract')
    wgsl_entries = dict(re.findall(r'@(vertex|fragment)\s+fn\s+(\w+)', code))
    if set(wgsl_entries) != {'vertex', 'fragment'}:
        raise RuntimeError('Expected one vertex and one fragment WGSL entry')

    # Optional GLSL ES 3.00 artifacts (WebGL 2). Each stage is translated from the
    # GLSL-specific validated SPIR-V by the pinned translator. LUDUS_GLSL_ES
    # permits the authored source to normalize fragment coordinates without
    # patching generated text. Its depfiles also participate in rebuilds.
    glsl_es = {}
    glsl_es_entries = {}
    glsl_es_layout_data = None
    if args.spirv_cross:
        for stage in ('vertex', 'fragment'):
            essl = output / f'{args.name}.{stage}.essl'
            binary = output / f'{args.name}.{stage}.glsl-es.spv'
            depfile = output / f'{stage}.glsl-es.d'
            command = [args.compiler, args.source, '-target', 'spirv', '-profile', 'spirv_1_3',
                       '-entry', args.vertex if stage == 'vertex' else args.fragment, '-stage', stage,
                       '-D', 'LUDUS_GLSL_ES=1', '-o', str(binary), '-reflection-json',
                       str(output / f'{stage}.glsl-es.reflection.json'), '-depfile', str(depfile), *extra]
            commands.append(command); run(command)
            run([args.validator, '--target-env', 'vulkan1.1', str(binary)])
            dependencies.append(depfile.read_text().split(':', 1)[1].strip())
            command = [args.spirv_cross, '--version', '300', '--es', '--fixup-clipspace', '--output', str(essl), str(binary)]
            commands.append(command); run(command)
            text = essl.read_text()
            glsl_es_contract(text)
            glsl_es[stage] = text
            # Generated GLSL ES always links through main.
            glsl_es_entries[stage] = 'main'
        glsl_es_layout_data = glsl_es_layout(glsl_es['fragment'])
        if glsl_es_layout_data['size'] != sizes['fragment']:
            raise RuntimeError('GLSL ES std140 block size differs from the reflected uniform size')
        if glsl_es_layout_data['binding'] != 0:
            raise RuntimeError('GLSL ES uniform block is not at binding 0')
        sizes['glsl_es'] = glsl_es_layout_data['size']
        if sizes['glsl_es'] != sizes['wgsl']:
            raise RuntimeError('GLSL ES and WGSL uniform upload sizes differ')
        for target in ('vertex', 'fragment', 'wgsl'):
            reflected = json.loads((output / f'{target}.reflection.json').read_text())
            if reflected.get('parameters'):
                fields = reflected['parameters'][0]['type']['elementType']['fields']
                offsets = {field['name']: field['binding']['offset'] for field in fields}
                if offsets != glsl_es_layout_data['offsets']:
                    raise RuntimeError('Per-target uniform member offsets differ from GLSL ES std140')
        vertex_layout = glsl_es_layout(glsl_es['vertex'])
        if vertex_layout['size'] and vertex_layout != glsl_es_layout_data:
            raise RuntimeError('GLSL ES stage uniform layouts differ')

    header = '#pragma once\n#include <ludus/foundation/base/config.h>\n#include <ludus/graphics/rhi/render.h>\n'
    header += f'namespace ludus::shaders::{args.name} {{\n#if !defined(LUDUS_PLATFORM_WEB)\n'
    for stage in ('vertex', 'fragment'):
        data = (output / f'{args.name}.{stage}.spv').read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        header += f'inline constexpr foundation::uint32 {stage.upper()}_SPIRV[] = {{\n'
        header += ',\n'.join('    ' + ', '.join(f'0x{x:08x}U' for x in words[i:i+8]) for i in range(0, len(words), 8)) + '\n};\n'
    if ')LUDUS_WGSL"' in code:
        raise RuntimeError('WGSL conflicts with header delimiter')
    header += '#else\ninline constexpr char WGSL[] = R"LUDUS_WGSL(' + code + ')LUDUS_WGSL";\n'
    if args.spirv_cross:
        # C++ raw-string delimiters are limited to 16 characters; keep them short.
        delimiters = {'vertex': 'LUDUS_ESV', 'fragment': 'LUDUS_ESF'}
        for stage in ('vertex', 'fragment'):
            text = glsl_es[stage]
            delimiter = delimiters[stage]
            if f'){delimiter}"' in text:
                raise RuntimeError('GLSL ES conflicts with header delimiter')
            header += f'inline constexpr char {stage.upper()}_GLSL_ES[] = R"{delimiter}(' + text + f'){delimiter}";\n'
    header += '#endif\n'
    for stage in ('vertex', 'fragment'):
        header += f'inline graphics::rhi::ShaderDescription {stage.title()}() noexcept {{\n graphics::rhi::ShaderDescription result; result.Stage = graphics::rhi::ShaderStage::{stage.title()};\n'
        header += '#if defined(LUDUS_PLATFORM_WEB)\n'
        header += f' result.UniformSize = {sizes["wgsl"]}; result.Wgsl = WGSL; result.WgslEntry = "{wgsl_entries[stage]}";\n'
        if args.spirv_cross:
            header += f' result.GlslEs = {stage.upper()}_GLSL_ES; result.GlslEsEntry = "{glsl_es_entries[stage]}";\n'
        header += f'#else\n result.UniformSize = {sizes[stage]}; result.Spirv = {stage.upper()}_SPIRV; result.SpirvEntry = "{entries[stage]}";\n#endif\n return result;\n}}\n'
    header += '}\n'
    target = output / f'{args.name}.h'
    # Header last: failed compiler/validator never presents a completed build output.
    target.write_text(header)
    escaped_target = str(target).replace(' ', '\\ ').replace('#', '\\#')
    (output / 'shader.d').write_text(escaped_target + ': ' + ' '.join(dependencies) + '\n')
    manifest = {'compiler': lock['slang'], 'commands': commands,
        'uniform_sizes': sizes, 'spirv_entries': entries, 'wgsl_entries': wgsl_entries, 'profiles': {'spirv': 'spirv_1_3', 'wgsl': 'sm_6_0'},
        'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir() if p.suffix in ('.spv', '.wgsl', '.essl')},
        'layout': 'See separate per-target reflection JSON; never assume packing equality.'}
    if args.spirv_cross:
        manifest['profiles']['glsl_es'] = '300 es'
        manifest['glsl_es_entries'] = glsl_es_entries
        manifest['glsl_es_translator'] = {'tag': cross_lock.get('tag'), 'commit': cross_lock.get('commit'),
                                          'source_sha256': cross_lock.get('source_sha256')}
        manifest['glsl_es_layout'] = glsl_es_layout_data
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print('Ludus shader build: ' + str(error), file=sys.stderr)
        sys.exit(1)
