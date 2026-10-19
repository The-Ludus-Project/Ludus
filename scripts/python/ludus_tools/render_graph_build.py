"""RG1 shared offline build service: schemas, explicit stages, recipes and packages.

Thanks to Rémi Arnaud, The Game Asset Pipeline, Game Engine Gems 1, chapter 2,
sections 2.2-2.3, and the Slang authors, Using the Reflection API (User Guide):
retain readable intermediates/provenance and independently verified target
packing. See docs/architecture/render-graph-authoring.md. No GPU work occurs here.
"""
from __future__ import annotations

from contextlib import contextmanager

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from .buildlock import BuildTreeLock
from .errors import ToolingError
from . import render_graph as graph

VERSION = 1
TARGETS = ('spirv', 'wgsl', 'metal', 'glsl')
SHAPES = {'Float32': ('float', 1), 'Float2': ('float2', 2), 'Float3': ('float3', 3), 'Float4': ('float4', 4)}
RESERVED = {'class', 'struct', 'namespace', 'auto', 'const', 'float', 'int', 'void', 'return', 'for', 'while', 'if', 'else', 'static', 'inline', 'template', 'typename', 'private', 'public', 'protected', 'operator', 'true', 'false', 'nullptr', 'bool', 'break', 'continue', 'switch', 'case', 'default', 'delete', 'new', 'this', 'union', 'enum', 'do', 'sizeof', 'alignof', 'using', 'typedef', 'uniform', 'in', 'out', 'register', 'cbuffer', 'shader', 'rgEntry', 'value'}
RESERVED.update(('alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept bitand bitor catch char char8_t char16_t char32_t compl consteval constexpr constinit const_cast co_await co_return co_yield decltype double dynamic_cast explicit export extern friend goto inline int long mutable noexcept not not_eq or or_eq reflexpr reinterpret_cast requires short signed static_assert static_cast template thread_local throw try typeid typename unsigned virtual volatile wchar_t xor xor_eq concept final override float2 float3 float4 uint uint2 uint3 uint4').split())
NAME = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,62}\Z')
HELPERS = ('compile_shader.py', 'compile_raster.py', 'compile_graph_stage.py', 'native_worker.py', 'shader_toolchain.json', 'spirv_cross_toolchain.json')
LIMITS = {'configurations': 8, 'programs': 32, 'artifacts': 128, 'pipelines': 64}
MAX_FILE = 128 * 1024
MAX_BYTES = 4 * 1024 * 1024
MAX_FILES = 64
MAX_PACKAGE_FILES = 1024
MAX_PACKAGE_BYTES = 64 * 1024 * 1024


def fail(code, where, message):
    raise ToolingError('RenderGraphBuild' + code, (f'{where}: {message}')[:4096])


def require(condition, code, where, message):
    if not condition:
        fail(code, where, message)


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n').encode()


def digest(value):
    return hashlib.sha256(value).hexdigest()


def identity(value):
    return digest(encoded(value))


def name(value, where):
    require(type(value) is str and NAME.fullmatch(value) and value not in RESERVED, 'Schema', where, 'ASCII identifier required')


def keys(value, required, where, optional=()):
    require(type(value) is dict and set(required) <= value.keys() and not value.keys() - set(required) - set(optional),
            'Schema', where, f'expected fields {required}; optional {optional}')


def integer(value, low, high, where):
    require(type(value) is int and low <= value <= high, 'Capacity', where, f'integer in [{low},{high}] required')


def enum(value, choices, where):
    require(type(value) is str and value in choices, 'Schema', where, f'expected {choices}')


def bounded_list(value, limit, where):
    require(type(value) is list and len(value) <= limit, 'Capacity', where, f'array bounded by {limit} required')


def read(path, limit=MAX_FILE):
    try:
        with Path(path).open('rb') as stream:
            data = stream.read(limit + 1)
        require(len(data) <= limit, 'Capacity', str(path), 'file byte limit exceeded')
        return data
    except OSError as error:
        fail('Source', str(path), str(error))


def document(data, where):
    def unique(pairs):
        result = {}
        for k, v in pairs:
            require(k not in result, 'Schema', where, 'duplicate JSON member: ' + k)
            result[k] = v
        return result
    def constant(value):
        fail('Schema', where, 'nonfinite constant ' + value)
    try:
        return json.loads(data.decode('utf-8'), object_pairs_hook=unique, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        fail('Schema', where, str(error))


class Snapshot:
    """Package-contained, bounded source closure captured once before compilation."""
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.files = {}
        self.total = 0

    def path(self, relative):
        require(type(relative) is str and relative and '\\' not in relative and ':' not in relative,
                'Source', str(relative), 'package-relative POSIX path required')
        p = Path(relative)
        require(not p.is_absolute() and '..' not in p.parts and p.as_posix() == relative,
                'Source', relative, 'noncanonical or escaping package path')
        resolved = (self.root / p).resolve()
        require(resolved.is_relative_to(self.root), 'Source', relative, 'symlink escapes package')
        return resolved

    def load(self, relative):
        if relative not in self.files:
            require(len(self.files) < MAX_FILES, 'Capacity', relative, 'source file limit exceeded')
            self.files[relative] = read(self.path(relative))
            self.total += len(self.files[relative])
            require(self.total <= MAX_BYTES, 'Capacity', relative, 'source byte limit exceeded')
        return self.files[relative]

    def shader_closure(self, relative, active=()):
        require(relative not in active and len(active) < 16, 'Source', relative, 'recursive or excessive shader import')
        data = self.load(relative)
        try:
            text = data.decode('utf-8')
        except UnicodeError as error:
            fail('Source', relative, str(error))
        # Restrict source discovery to literal local includes. Conditional includes
        # are conservatively captured, so a disabled dependency still invalidates.
        text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
        require(not re.search(r'\b(?:import|__include|__import)\b|^\s*#\s*(?:line|include_next)\b', text, re.M),
                'Source', relative, 'RG1 uses literal local includes; module imports and source remapping are unsupported')
        include_lines = re.findall(r'^\s*#\s*include\b([^\n]*)', text, re.M)
        closure = {relative}
        for line in include_lines:
            match = re.fullmatch(r'\s*"([^"\n]+)"\s*', line)
            require(match is not None, 'Source', relative, 'only literal quoted includes are supported')
            child = (Path(relative).parent / match[1]).as_posix()
            self.path(child)
            closure.update(self.shader_closure(child, (*active, relative)))
        require(not re.search(r'#\s*include\s*\\', text), 'Source', relative, 'continued include directives unsupported')
        return sorted(closure)

    def verify(self):
        for relative, data in self.files.items():
            require(read(self.path(relative)) == data, 'Stale', relative, 'source changed during build; prior package retained')


def validate_manifest(value):
    keys(value, ('version', 'id', 'profiles', 'schemas', 'programs', 'techniques', 'budgets'), 'build manifest')
    require(type(value['version']) is int and value['version'] == VERSION, 'Schema', 'manifest', 'version 1 required')
    name(value['id'], 'manifest id')
    bounded_list(value['profiles'], 4, 'profiles')
    for target in value['profiles']:
        enum(target, TARGETS, 'profile')
    require(value['profiles'] and len(set(value['profiles'])) == len(value['profiles']), 'Schema', 'profiles', 'nonempty unique targets required')
    keys(value['budgets'], LIMITS, 'budgets')
    for key, maximum in LIMITS.items():
        integer(value['budgets'][key], 1, maximum, 'budget ' + key)
    schemas, programs = {}, {}
    bounded_list(value['schemas'], 16, 'schemas')
    for schema in value['schemas']:
        keys(schema, ('id', 'role', 'fields'), 'schema')
        name(schema['id'], 'schema id')
        require(schema['id'] not in schemas, 'Schema', schema['id'], 'duplicate schema')
        enum(schema['role'], ('Uniform', 'Vertex', 'Varying'), 'schema role')
        bounded_list(schema['fields'], 16, 'schema fields')
        require(schema['fields'], 'Schema', schema['id'], 'empty schema')
        seen, locations = set(), set()
        for field in schema['fields']:
            keys(field, ('id', 'type') if schema['role'] == 'Uniform' else ('id', 'type', 'semantic', 'location'), 'field')
            name(field['id'], 'field id')
            require(field['id'] not in seen, 'Schema', schema['id'], 'duplicate field identity')
            seen.add(field['id'])
            enum(field['type'], SHAPES, 'field type')
            if schema['role'] != 'Uniform':
                require(type(field['semantic']) is str and re.fullmatch(r'(?:POSITION|NORMAL|TEXCOORD[0-9]|COLOR[0-9]|SV_Position)', field['semantic']),
                        'Schema', field['id'], 'bounded semantic required')
                integer(field['location'], 0, 15, 'field location')
                if field['semantic'] != 'SV_Position':
                    require(field['location'] not in locations, 'Schema', field['id'], 'duplicate location')
                    locations.add(field['location'])
                if schema['role'] == 'Vertex':
                    enum(field['type'], ('Float2', 'Float3', 'Float4'), 'vertex field')
                    require(field['semantic'] != 'SV_Position', 'Schema', field['id'], 'vertex inputs cannot use SV_Position')
                elif field['semantic'] == 'SV_Position':
                    require(field['type'] == 'Float4', 'Schema', field['id'], 'position requires Float4')
        schemas[schema['id']] = schema
    bounded_list(value['programs'], LIMITS['programs'], 'programs')
    for program in value['programs']:
        keys(program, ('id', 'stage', 'source', 'function', 'input', 'output', 'bindings'), 'program')
        name(program['id'], 'program id')
        name(program['function'], 'source function')
        require(program['source'] != 'wrapper.slang', 'Source', program['id'], 'wrapper.slang is reserved')
        require(program['function'] != 'rgEntry', 'Schema', program['id'], 'rgEntry is generated')
        require(program['id'] not in programs, 'Schema', program['id'], 'duplicate program identity')
        enum(program['stage'], ('vertex', 'fragment'), 'stage')
        require(type(program['input']) is str and program['input'] in schemas, 'Schema', program['id'], 'unknown input schema')
        expected = 'Vertex' if program['stage'] == 'vertex' else 'Varying'
        require(schemas[program['input']]['role'] == expected, 'Schema', program['id'], 'wrong input schema role')
        if program['stage'] == 'vertex':
            require(type(program['output']) is str and program['output'] in schemas and schemas[program['output']]['role'] == 'Varying', 'Schema', program['id'], 'vertex output must be Varying')
            positions = [f for f in schemas[program['output']]['fields'] if f['semantic'] == 'SV_Position']
            require(len(positions) == 1, 'Schema', program['id'], 'vertex output needs exactly one position')
        else:
            enum(program['output'], ('Color', 'Depth'), 'fragment output')
        bounded_list(program['bindings'], 8, 'bindings')
        used, slots = set(), set()
        for binding in program['bindings']:
            keys(binding, ('id', 'schema', 'slot'), 'binding')
            name(binding['id'], 'binding id')
            integer(binding['slot'], 0, 7, 'binding slot')
            require(binding['id'] not in used and binding['slot'] not in slots, 'Schema', program['id'], 'duplicate binding')
            require(binding['id'] not in schemas, 'Schema', program['id'], 'binding identity collides with a schema type')
            used.add(binding['id']); slots.add(binding['slot'])
            require(type(binding['schema']) is str and binding['schema'] in schemas and schemas[binding['schema']]['role'] == 'Uniform', 'Schema', program['id'], 'binding requires uniform schema')
        programs[program['id']] = program
    techniques = {}
    bounded_list(value['techniques'], 8, 'techniques')
    for technique in value['techniques']:
        keys(technique, ('id', 'configuration', 'vertex', 'fragment', 'recipes'), 'technique')
        require(type(technique['configuration']) is str and
                technique['id'] in (family + '.' + technique['configuration'] for family in graph.MATERIALS.values()),
                'Schema', 'technique', 'RG0 material-family technique identity required')
        enum(technique['configuration'], graph.MODES, 'configuration')
        require(technique['id'] not in techniques, 'Schema', technique['id'], 'duplicate configuration')
        for stage in ('vertex', 'fragment'):
            require(type(technique[stage]) is str and technique[stage] in programs and programs[technique[stage]]['stage'] == stage, 'Schema', technique['id'], 'unknown or wrong stage program')
        fragment = programs[technique['fragment']]
        require(fragment['output'] == ('Color' if technique['configuration'] == 'ColorAndDepth' else 'Depth'),
                'Schema', technique['id'], 'fragment output must match explicit technique')
        require(programs[technique['vertex']]['input'] == 'RendererStatic', 'Schema', technique['id'], 'RG0 requires RendererStatic schema')
        bounded_list(technique['recipes'], 16, 'recipes')
        require(technique['recipes'], 'Schema', technique['id'], 'at least one recipe required')
        seen = set()
        for recipe in technique['recipes']:
            keys(recipe, ('id', 'color', 'depth', 'samples', 'topology', 'cull', 'front', 'depth_compare', 'depth_write', 'blend'), 'recipe')
            name(recipe['id'], 'recipe id')
            require(recipe['id'] not in seen, 'Schema', recipe['id'], 'duplicate recipe id')
            seen.add(recipe['id'])
            enum(recipe['color'], ('RGBA8Unorm',) if technique['configuration'] == 'ColorAndDepth' else ('None',), 'color format')
            enum(recipe['depth'], ('Depth32Float',), 'depth format')
            integer(recipe['samples'], 1, 1, 'samples')
            enum(recipe['topology'], ('TriangleList',), 'topology')
            enum(recipe['cull'], ('None', 'Back', 'Front'), 'cull')
            enum(recipe['front'], ('Clockwise', 'CounterClockwise'), 'front')
            enum(recipe['depth_compare'], ('Less', 'LessEqual', 'Equal', 'Always'), 'depth compare')
            require(type(recipe['depth_write']) is bool, 'Schema', recipe['id'], 'depth_write must be Boolean')
            enum(recipe['blend'], ('Opaque',), 'blend')
        techniques[technique['id']] = technique
    require(techniques, 'Schema', 'techniques', 'nonempty authored techniques required')
    # A masked depth-only technique must name its own authored coverage function;
    # the opaque void depth program never stands in for masked coverage.
    opaque_depth = techniques.get('DefaultStaticOpaque.DepthOnly')
    masked_depth = techniques.get('DefaultStaticMasked.DepthOnly')
    require(opaque_depth is None or masked_depth is None or opaque_depth['fragment'] != masked_depth['fragment'],
            'Schema', 'DefaultStaticMasked.DepthOnly', 'masked coverage requires a separately authored depth function')
    used_programs = {t[s] for t in techniques.values() for s in ('vertex', 'fragment')}
    require(used_programs == programs.keys(), 'Schema', 'programs', 'every program must belong to a declared technique')
    counts = {'configurations': len(techniques), 'programs': len(programs),
              'artifacts': len(programs) * len(value['profiles']),
              'pipelines': sum(len(t['recipes']) for t in techniques.values()) * len(value['profiles'])}
    for key, count in counts.items():
        require(count <= value['budgets'][key], 'Capacity', key, f'{count} exceeds authored budget {value["budgets"][key]}')
    return schemas, programs, techniques, counts


def shader_source(program, schemas):
    """Generate declarations/bindings and entry wrapper; user source owns functions."""
    used = {program['input'], *(b['schema'] for b in program['bindings'])}
    if program['stage'] == 'vertex':
        used.add(program['output'])
    lines = ['// Generated RG1 declarations; edit the schema/build source.']
    for sid in sorted(used):
        schema = schemas[sid]
        lines += ['struct ' + sid, '{']
        for field in schema['fields']:
            prefix, suffix = '', ''
            if schema['role'] != 'Uniform':
                if field['semantic'] != 'SV_Position':
                    prefix = '[[vk::location(' + str(field['location']) + ')]] '
                suffix = ' : ' + field['semantic']
            lines.append('    ' + prefix + SHAPES[field['type']][0] + ' ' + field['id'] + suffix + ';')
        lines += ['};']
    for binding in program['bindings']:
        lines.append(f'[[vk::binding({binding["slot"]}, 0)]] ConstantBuffer<{binding["schema"]}> {binding["id"]};')
    if program['stage'] == 'fragment' and program['output'] == 'Depth':
        lines.append('void ' + program['function'] + '(' + program['input'] + ' value);')
    lines += ['#include "' + program['source'] + '"', '[shader("' + program['stage'] + '")]']
    if program['stage'] == 'vertex':
        result, suffix = program['output'], ''
    else:
        result, suffix = ('float4', ' : SV_Target0') if program['output'] == 'Color' else ('void', '')
    lines += [result + ' rgEntry(' + program['input'] + ' value)' + suffix, '{',
              ('    return ' if result != 'void' else '    ') + program['function'] + '(value);', '}']
    return '\n'.join(lines) + '\n'


def check_layout(program, schemas, result, raw):
    """Verify field identity/type/offset and declared read-only binding use."""
    declared = {b['id']: b for b in program['bindings']}
    parameters = raw.get('parameters', [])
    require(all(p.get('name') in declared for p in parameters), 'Layout', program['id'], 'source declares a resource outside generated bindings')
    layouts = []
    for entry in result['interface']['entries']:
        binding = declared.get(entry['name'])
        require(binding is not None and entry['binding'] == binding['slot'] and entry['kind'] == 'UniformBuffer',
                'Layout', program['id'], 'undeclared resource use/binding')
        schema = schemas[binding['schema']]
        expected = {f['id']: f['type'] for f in schema['fields']}
        require(entry.get('shapes') == expected and set(entry.get('offsets', {})) == expected.keys(),
                'Layout', program['id'], 'field identities/types differ from generated schema')
        occupied = []
        for field in schema['fields']:
            offset = entry['offsets'][field['id']]
            size = SHAPES[field['type']][1] * 4
            require(type(offset) is int and offset >= 0 and offset % 4 == 0 and offset + size <= entry['size'],
                    'Layout', program['id'], 'invalid reflected field extent/alignment')
            require(all(offset + size <= start or offset >= end for start, end in occupied), 'Layout', program['id'], 'overlapping fields')
            occupied.append((offset, offset + size))
        layouts.append({'binding': binding['id'], 'slot': binding['slot'], 'schema': schema['id'],
                        'size': entry['size'], 'fields': [{'id': f['id'], 'type': f['type'],
                                                        'offset': entry['offsets'][f['id']]} for f in schema['fields']]})
    expected_inputs = [{ 'location': f['location'], 'format': f['type'], 'name': f['id']} for f in schemas[program['input']]['fields']] if program['stage'] == 'vertex' else []
    require(sorted(result['interface']['inputs'], key=lambda x: x['location']) == sorted(expected_inputs, key=lambda x: x['location']), 'Layout', program['id'], 'vertex inputs differ from schema')
    return layouts


def cpp_header(schemas, compiled):
    """Generate logical CPU values and independent, output-preserving target packers."""
    lines = ['#pragma once', '#include <ludus/foundation/base/types.h>', '', 'namespace ludus::render_graph_generated', '{']
    for sid, schema in sorted(schemas.items()):
        lines += ['/// Logical schema values; target upload bytes must use the generated packer.', 'struct ' + sid, '{']
        for field in schema['fields']:
            width = SHAPES[field['type']][1]
            lines += ['    /// Schema field ' + field['id'] + '; native float32 values.',
                      '    foundation::float32 ' + field['id'] + (f'[{width}]' if width > 1 else '') + '{};']
        lines += ['};']
    layouts = {}
    for (pid, target), result in compiled.items():
        for layout in result['layouts']:
            label = pid + '_' + target + '_' + layout['binding']
            require(label not in layouts, 'Schema', label, 'generated packer name collision; rename the program or binding')
            layouts[label] = layout
    for label, layout in sorted(layouts.items()):
        lines += ['/// Pack native-endian target uniform bytes. Caller owns output; any thread.',
                  '/// Returns false on null/short output and preserves all bytes on failure.',
                  '/// Input/output storage must not overlap. Padding is zeroed; no allocation.',
                  f'inline bool Pack_{label}(const {layout["schema"]}& value, foundation::uint8* output, foundation::usize capacity) noexcept', '{',
                  f'    if (output == nullptr || capacity < {layout["size"]})', '    {', '        return false;', '    }',
                  f'    for (foundation::usize i = 0; i < {layout["size"]}; ++i)', '    {', '        output[i] = 0;', '    }']
        for field in layout['fields']:
            size = SHAPES[field['type']][1] * 4
            lines += ['    {', f'        const auto* bytes = reinterpret_cast<const foundation::uint8*>(&value.{field["id"]});',
                      f'        for (foundation::usize i = 0; i < {size}; ++i)', '        {', f'            output[{field["offset"]} + i] = bytes[i];', '        }', '    }']
        lines += ['    return true;', '}']
    lines += ['}']
    return '\n'.join(lines) + '\n'


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def tool_identity(helper, compiler, validator, cross):
    """Identity includes helper/locks, actual binaries and Slang shared libraries."""
    paths = {f'helper/{p}': helper / p for p in HELPERS}
    paths['generator/render_graph_build.py'] = Path(__file__)
    paths['generator/render_graph.py'] = Path(graph.__file__)
    paths['tool/slangc'] = Path(compiler).resolve()
    if validator:
        paths['tool/spirv-val'] = Path(validator).resolve()
    if cross:
        paths['tool/spirv-cross'] = Path(cross).resolve()
        paths['tool/spirv-cross.build.json'] = Path(str(Path(cross).resolve()) + '.build.json')
    # The pinned archive carries frontend/backend shared libraries beside bin or
    # in lib. Hash them too: executable version alone cannot establish freshness.
    binary = paths['tool/slangc']
    for folder in (binary.parent, binary.parent.parent / 'lib'):
        if folder.is_dir():
            for p in sorted(folder.iterdir()):
                if p.is_file() and ('.dylib' in p.name or '.so' in p.name or p.suffix == '.dll'):
                    paths['slang/' + folder.name + '/' + p.name] = p
    require(len(paths) <= 64, 'Capacity', 'toolchain', 'unexpected tool file count')
    try:
        return {key: file_hash(p) for key, p in paths.items()}, paths
    except OSError as error:
        fail('Tools', 'toolchain', str(error))


# compile_shader.py reports its failure on stderr as `Ludus shader build: <compiler
# output>`, so the first compiler line carries that prefix; later lines do not.
SHADER_HELPER_PREFIX = 'Ludus shader build: '
SHADER_DIAGNOSTIC = re.compile(
    r'^(?:' + re.escape(SHADER_HELPER_PREFIX) + r')?'
    r'(?P<file>[^\s()+][^()\n]*?)\((?P<line>\d+)(?:,\s*(?P<column>\d+))?\):\s*'
    r'(?P<severity>error|warning)\b[^:\n]*:\s*(?P<message>[^\n]*)$', re.M)
MAX_SHADER_DIAGNOSTICS = 32


def map_shader_diagnostics(output, program, target, closure):
    """Map compiler file(line) messages to captured authored sources (RG3).

    Captured function/include files map to their package-relative source and
    line. Generated wrapper lines map to the owning program record, because the
    wrapper is a read-only build product regenerated from the manifest. Unknown
    files keep only their message. Bounded to 32 records; no absolute paths.
    """
    diagnostics = []
    for match in SHADER_DIAGNOSTIC.finditer(output):
        if len(diagnostics) == MAX_SHADER_DIAGNOSTICS:
            break
        record = {'severity': match['severity'], 'message': match['message'][:512],
                  'program': program['id'], 'target': target}
        line = int(match['line'])
        if match['file'] in closure:
            record['location'] = {'source': match['file'], 'line': line}
        elif match['file'] == 'wrapper.slang':
            record['location'] = {'generated': 'wrapper.slang', 'program': program['id'], 'line': line}
        if 'location' in record and match['column'] is not None:
            record['location']['column'] = int(match['column'])
        diagnostics.append(record)
    return diagnostics


def checked_cache(path, expected):
    """A cache hit compares full identity and hashes every retained artifact."""
    try:
        meta = document(read(path / 'cache.json'), str(path))
        if type(meta) is not dict or meta.get('identity') != expected or type(meta.get('files')) is not dict or not meta['files'] or len(meta['files']) > 12:
            return None
        for relative, hash_value in meta['files'].items():
            if type(relative) is not str or type(hash_value) is not str or re.fullmatch(r'[a-f0-9]{64}', hash_value) is None:
                return None
            if relative in ('.', '..') or Path(relative).name != relative or (path / relative).is_symlink() or digest(read(path / relative, 2 * 1024 * 1024)) != hash_value:
                return None
        result = document(read(path / 'stage.json'), 'cached stage')
        if result != meta['result'] or not {'stage.json', 'reflection.json', 'wrapper.slang', result['artifact']} <= meta['files'].keys():
            return None
        return result
    except (ToolingError, KeyError, TypeError, OSError):
        return None


def atomic(path, data):
    """Same-directory replacement; publication pointer is written last."""
    descriptor, temp = tempfile.mkstemp(prefix='.rg1-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        Path(temp).unlink(missing_ok=True)


@contextmanager
def cache_writes(tool_paths, tool_hashes):
    """Discard this job's cache writes if its compiler/helper snapshot changed.

    A stale/cancelled job must not leave artifacts under a tool identity that did
    not actually produce them. Source snapshots are compiled from private copies;
    external tools are verified at publication and again during failure cleanup.
    """
    written = []
    try:
        yield written
    finally:
        try:
            changed = any(file_hash(path) != tool_hashes[key] for key, path in tool_paths.items())
        except OSError:
            changed = True
        if changed:
            for path in written:
                shutil.rmtree(path, ignore_errors=True)


def checkpoint(cancel):
    require(not (cancel and cancel()), 'Cancelled', 'build', 'cancelled; prior package retained')


def portable_stage_output(output, command):
    """Helper/compiler report of one stage without machine paths (RG3).

    compile_shader.py echoes each command it runs as `+ <tool> <arguments>`, with
    absolute tool and temporary paths. Those lines are dropped: the stage descriptor
    and build manifest already identify the tools and inputs, and the echoes would
    consume the bounded error message before the compiler's diagnostics. Remaining
    references name captured sources package-relative, stage outputs as `artifact/`,
    helper files as `helper/` and the compiler by its file name.
    """
    def argument(name):
        return Path(command[command.index(name) + 1])
    text = ''.join(line for line in output.splitlines(keepends=True) if not line.startswith('+ '))
    compiler = argument('--compiler')
    roots = ((argument('--source').parent, ''), (argument('--output'), 'artifact/'), (Path(command[1]).parent, 'helper/'))
    for path in (compiler, compiler.resolve()):
        text = text.replace(str(path), compiler.name)
    for root, label in roots:
        for path in (root, root.resolve()):
            text = text.replace(str(path) + '/', label)
    return text


def run_stage(command, cancel):
    """Poll cancellation; terminate/reap compiler process on cancellation/failure."""
    import signal
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while process.poll() is None:
                checkpoint(cancel)
                time.sleep(0.05)
            log.seek(0)
            output = log.read(64 * 1024).decode('utf-8', errors='replace')
            require(process.returncode == 0, 'Shader', 'shader helper', portable_stage_output(output, command))
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def compile_program(program, schemas, closure, snapshot, work, helper, tools, target, cancel, worker_socket=None, worker_identity=None):
    """Compile only captured sources; reject any untracked compiler dependency."""
    source = work / 'source'
    source.mkdir()
    for relative in closure:
        p = source / relative
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(snapshot.files[relative])
    wrapper = shader_source(program, schemas)
    (source / 'wrapper.slang').write_text(wrapper)
    output = work / 'artifact'
    command = [sys.executable, str(helper / 'compile_shader.py'), '--source', str(source / 'wrapper.slang'),
               '--output', str(output), '--name', 'rgEntry', '--compiler', str(tools['tool/slangc']),
               '--lock', str(helper / 'shader_toolchain.json'), '--graph-stage', program['stage'], '--graph-target', target]
    if worker_socket:
        command += ['--worker-socket', worker_socket, '--worker-identity', worker_identity]
    if 'tool/spirv-val' in tools:
        command += ['--validator', str(tools['tool/spirv-val'])]
    if target == 'glsl':
        command += ['--spirv-cross', str(tools['tool/spirv-cross']), '--spirv-cross-lock', str(helper / 'spirv_cross_toolchain.json')]
    try:
        run_stage(command, cancel)
    except ToolingError as error:
        if error.code == 'RenderGraphBuildShader':
            output = error.message.removeprefix('shader helper: ')
            error.diagnostics = map_shader_diagnostics(output, program, target, closure)
            located = [d['location'] for d in error.diagnostics if 'location' in d]
            error.location = located[0] if located else {'program': program['id']}
        raise
    result = document(read(output / 'stage.json'), 'stage result')
    raw = document(read(output / 'reflection.json'), 'target reflection')
    result['layouts'] = check_layout(program, schemas, result, raw)
    # Escape-aware depfile parsing supports spaces in package/build directories.
    text = (output / 'stage.d').read_text().replace('\\\n', ' ')
    dependencies = re.findall(r'(?:\\.|[^\s])+', text.split(': ', 1)[-1])
    allowed = {(source / p).resolve() for p in closure} | {(source / 'wrapper.slang').resolve()}
    for item in dependencies:
        path = Path(re.sub(r'\\(.)', r'\1', item)).resolve()
        require(path in allowed, 'Source', program['id'], 'compiler read an uncaptured dependency')
    # Readable provenance is separate from shader identities. No temporary or
    # machine paths enter immutable stage outputs/cache/package descriptors.
    (output / 'wrapper.slang').write_text(wrapper)
    result['source_map'] = {'wrapper': 'wrapper.slang', 'function': program['source'], 'schema_owner': 'build manifest'}
    (output / 'stage.json').write_bytes(encoded(result))
    (output / 'stage.d').unlink()
    if (output / 'intermediate.spv').exists():
        (output / 'intermediate.spv').unlink()
    return output, result


def build_package(manifest, output, *, helper, compiler, validator=None, spirv_cross=None, root=None, cancel=None, worker_socket=None):
    """Build every authored technique/profile and atomically publish current.json.

    Sources/tools must remain identical to their captured snapshot at publication.
    Bounded errors, cancellation, corrupt caches, and concurrent writer rejection
    preserve the prior package pointer. No download, device pipeline creation or
    GPU execution. Return an immutable package path and separate hit/miss counts.
    helper names the installed SDK shaders directory (or checkout cmake/shaders).
    Runs on macOS/Linux, with one nonblocking writer per output tree.
    """
    started = time.monotonic()
    manifest = Path(manifest).resolve()
    snapshot = Snapshot(root if root is not None else manifest.parent)
    require(manifest.is_relative_to(snapshot.root), 'Source', str(manifest), 'manifest outside root')
    manifest_label = manifest.relative_to(snapshot.root).as_posix()
    value = document(snapshot.load(manifest_label), manifest_label)
    schemas, programs, techniques, counts = validate_manifest(value)
    helper = Path(helper).resolve()
    tool_hashes, tool_paths = tool_identity(helper, compiler, validator, spirv_cross)
    worker_socket = worker_socket or os.environ.get('LUDUS_SHADER_WORKER_SOCKET')
    worker_identity = None
    if worker_socket:
        import importlib.util
        spec = importlib.util.spec_from_file_location('_ludus_native_worker', helper / 'native_worker.py')
        native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(native)
        try:
            descriptor = native.exchange(worker_socket, {'version': 1, 'identity': 0})
        except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
            fail('Tools', 'native worker', str(error))
        require(descriptor.get('compiler') == document(read(helper / 'shader_toolchain.json'), 'lock')['slang']['version'],
                'Tools', 'native worker', 'compiler version differs from SDK pin')
        require(descriptor.get('library_sha256') in tool_hashes.values(), 'Tools', 'native worker', 'loaded API library differs from the selected toolchain')
        inventory = ''.join(key.removeprefix('slang/') + ':' + value + '\n'
                            for key, value in sorted(tool_hashes.items()) if key.startswith('slang/'))
        require(descriptor.get('library_inventory_sha256') == digest(inventory.encode()),
                'Tools', 'native worker', 'loaded compiler/backend libraries differ from the selected toolchain')
        worker_identity = descriptor.get('implementation')
        require(isinstance(worker_identity, str) and len(worker_identity) == 64, 'Tools', 'native worker', 'missing implementation identity')
        # Native compilation is a distinct implementation and owns separate artifact keys.
        tool_hashes['native/implementation'] = worker_identity
    if any(t in value['profiles'] for t in ('spirv', 'glsl')):
        require(validator, 'Tools', 'profiles', 'SPIR-V/glsl requires --validator')
    if 'glsl' in value['profiles']:
        require(spirv_cross, 'Tools', 'profiles', 'glsl requires --spirv-cross')
    closures = {pid: snapshot.shader_closure(p['source']) for pid, p in programs.items()}
    # Build plans for every declared closed root configuration and preserve graph
    # imports in the same source snapshot used by shaders.
    graph_path = manifest.with_suffix('').with_suffix('.lurender.json')
    # The sibling graph is the explicit owner of this build manifest.
    require(graph_path.is_file(), 'Source', str(graph_path), 'expected sibling .lurender.json graph')
    library, graph_path = graph._library(graph_path, snapshot.root)
    for p, data in library.bytes.items():
        label = library.label(p)
        require(snapshot.load(label) == data, 'Stale', label, 'graph changed while capturing sources')
    plans = [graph.compile_graph(graph_path, root=snapshot.root, configuration=c['id'])
             for c in library.assets[graph_path]['configurations']]
    for plan in plans:
        require(all(digest(snapshot.files[d['source']]) == d['sha256'] for d in plan['dependencies']),
                'Stale', plan['configuration'], 'graph changed during plan compilation')
        require(set(plan['techniques']) <= techniques.keys(), 'Configuration', plan['configuration'], 'graph selects an undeclared technique')
    counts['root_configurations'] = len(plans)
    counts['graph_configurations'] = sum(len(a['configurations']) for a in library.assets.values())
    require(counts['graph_configurations'] <= value['budgets']['configurations'], 'Capacity', 'graph configurations', 'closure configuration budget exceeded')
    require(len(plans) <= value['budgets']['configurations'], 'Capacity', 'root configurations', 'root configuration budget exceeded')
    output = Path(output).resolve()
    require(not snapshot.root.is_relative_to(output), 'Source', str(output), 'output must not contain source root')
    output.mkdir(parents=True, exist_ok=True)
    with BuildTreeLock(output), cache_writes(tool_paths, tool_hashes) as written_cache:
        checkpoint(cancel)
        cache = output / 'cache'
        generations = output / 'generations'
        cache.mkdir(exist_ok=True); generations.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.rg1-', dir=output) as stage:
            stage = Path(stage)
            compiled, entries, hits, misses = {}, {}, 0, 0
            for pid, program in sorted(programs.items()):
                for target in sorted(value['profiles']):
                    checkpoint(cancel)
                    closure = closures[pid]
                    used = {program['input'], *(b['schema'] for b in program['bindings'])}
                    if program['stage'] == 'vertex':
                        used.add(program['output'])
                    key_data = {'version': VERSION, 'program': {k: v for k, v in program.items() if k != 'id'},
                                'schemas': [schemas[s] for s in sorted(used)], 'target': target,
                                'sources': {p: digest(snapshot.files[p]) for p in closure}, 'wrapper': shader_source(program, schemas),
                                'tools': {k: h for k, h in tool_hashes.items()
                                          if k != 'generator/render_graph.py'
                                          and not (target in ('metal', 'wgsl') and k == 'tool/spirv-val')
                                          and not (target != 'glsl' and ('spirv-cross' in k or k.endswith('spirv_cross_toolchain.json')))}}
                    key = identity(key_data)
                    saved = cache / key
                    result = checked_cache(saved, key_data)
                    if result is None:
                        work = stage / ('compile-' + pid + '-' + target)
                        work.mkdir()
                        artifacts, result = compile_program(program, schemas, closure, snapshot, work, helper, tool_paths, target, cancel, worker_socket, worker_identity)
                        files = {p.name: digest(read(p, 2 * 1024 * 1024)) for p in artifacts.iterdir() if p.is_file()}
                        (artifacts / 'cache.json').write_bytes(encoded({'identity': key_data, 'files': files, 'result': result}))
                        checkpoint(cancel)
                        if saved.exists():
                            shutil.rmtree(saved)
                        os.replace(artifacts, saved)
                        written_cache.append(saved)
                        misses += 1
                    else:
                        hits += 1
                    require(result['target'] == target and result['stage'] == program['stage'], 'Cache', key, 'cached stage identity mismatch')
                    compiled[(pid, target)] = result
                    entries[(pid, target)] = key
            recipes, routes = [], []
            for tid, technique in sorted(techniques.items()):
                for target in sorted(value['profiles']):
                    vertex, fragment = (compiled[(technique[s], target)] for s in ('vertex', 'fragment'))
                    provided = {x['location']: x['type'] for x in vertex['links']}
                    require(all(provided.get(x['location']) == x['type'] for x in fragment['links']), 'Layout', tid, 'vertex/fragment stage links mismatch')
                    for recipe in technique['recipes']:
                        record = {'technique': tid, 'profile': target, 'state': recipe,
                                  'vertex': entries[(technique['vertex'], target)], 'fragment': entries[(technique['fragment'], target)],
                                  'vertex_schema': schemas[programs[technique['vertex']]['input']],
                                  'bindings': {'vertex': vertex['layouts'], 'fragment': fragment['layouts']}}
                        record['canonical'] = {**{k: v for k, v in record.items() if k not in ('technique', 'state')},
                                               'state': {k: v for k, v in recipe.items() if k != 'id'}}
                        record['key'] = identity(record['canonical'])
                        recipes.append(record)
            for plan in plans:
                for operation in plan['passes']:
                    if operation['kind'] != 'Raster':
                        continue
                    technique = techniques[operation['technique']]
                    matches = [r for r in technique['recipes'] if r['samples'] == 1 and r['depth'] == 'Depth32Float' and
                               r['color'] == ('RGBA8Unorm' if operation['configuration'] == 'ColorAndDepth' else 'None')]
                    require(matches, 'Configuration', operation['id'], 'no admitted attachment recipe')
                    routes.append({'configuration': plan['configuration'], 'node': operation['id'], 'technique': operation['technique'],
                                   'recipe_ids': sorted(r['id'] for r in matches)})
            counts['unique_stage_programs'] = len(set(entries.values()))
            counts['unique_pipeline_recipes'] = len({r['key'] for r in recipes})
            package = stage / 'package'
            package.mkdir()
            (package / 'schemas.h').write_text(cpp_header(schemas, compiled))
            (package / 'schemas.json').write_bytes(encoded(value['schemas']))
            for relative, data in snapshot.files.items():
                saved_source = package / 'sources' / relative
                saved_source.parent.mkdir(parents=True, exist_ok=True)
                saved_source.write_bytes(data)
            for key in sorted(set(entries.values())):
                shutil.copytree(cache / key, package / 'programs' / key, ignore=shutil.ignore_patterns('cache.json'))
            normalized_plans = [{**{k: v for k, v in p.items() if k != 'dependencies'},
                                 'checks_pending': [c for c in p['checks_pending'] if c != 'ShaderBuild']} for p in plans]
            payload = {'version': VERSION, 'stage': 'RG1', 'execution': 'OfflinePackage', 'id': value['id'], 'plans': normalized_plans,
                       'counts': counts, 'budgets': value['budgets'], 'profiles': sorted(value['profiles']),
                       'programs': [{'id': pid, 'profile': target, 'key': key, 'descriptor': compiled[(pid, target)]}
                                    for (pid, target), key in sorted(entries.items())], 'recipes': recipes, 'routes': routes,
                       'schemas': value['schemas'], 'checks_pending': ['DeviceCapabilities', 'SceneCoverage', 'GpuExecution', 'PipelineReadiness']}
            # Manifest/graph source hashes are provenance, separate from semantic
            # generation identity so moving graph nodes does not rebuild shaders.
            payload['provenance'] = {p: digest(data) for p, data in sorted(snapshot.files.items())}
            payload['tools'] = tool_hashes
            payload['source_map'] = {'build_manifest': manifest_label, 'root_graph': library.label(graph_path),
                                     'programs': {pid: {'source': p['source'], 'function': p['function'],
                                                        'input_schema': p['input'], 'output_schema': p['output']}
                                                  for pid, p in sorted(programs.items())}}
            (package / 'package.json').write_bytes(encoded(payload))
            integrity = {p.relative_to(package).as_posix(): digest(read(p, 2 * 1024 * 1024))
                         for p in sorted(package.rglob('*')) if p.is_file()}
            require(len(integrity) <= MAX_PACKAGE_FILES and sum(p.stat().st_size for p in package.rglob('*') if p.is_file()) <= MAX_PACKAGE_BYTES,
                    'Capacity', 'package', 'package inventory/byte budget exceeded')
            generation_key = identity(integrity)
            (package / 'integrity.json').write_bytes(encoded(integrity))
            checkpoint(cancel)
            snapshot.verify()
            require(all(file_hash(p) == tool_hashes[k] for k, p in tool_paths.items()), 'Stale', 'toolchain', 'toolchain changed during build')
            generation = generations / generation_key
            if generation.exists():
                verify_package(generation)
            else:
                os.replace(package, generation)
            checkpoint(cancel)
            report = {'package': str(generation), 'generation': generation_key, 'cache_hits': hits, 'cache_misses': misses,
                      'elapsed_seconds': time.monotonic() - started, 'counts': counts}
            atomic(output / 'build-report.json', encoded(report))
            # The activation pointer is the sole last write. Failure to write
            # diagnostics cannot publish a partially successful build.
            checkpoint(cancel)
            snapshot.verify()
            require(all(file_hash(p) == tool_hashes[k] for k, p in tool_paths.items()), 'Stale', 'toolchain', 'toolchain changed before publication')
            atomic(output / 'current.json', encoded({'version': VERSION, 'generation': generation_key}))
            return report


def verify_package(path):
    """Verify a cooked generation's entire bounded file inventory, without tools."""
    path = Path(path).resolve()
    inventory = document(read(path / 'integrity.json'), 'package integrity')
    require(type(inventory) is dict and len(inventory) <= MAX_PACKAGE_FILES, 'Package', str(path), 'bounded inventory required')
    actual, total, visited = set(), 0, 0
    for artifact in path.rglob('*'):
        visited += 1
        require(visited <= MAX_PACKAGE_FILES * 2, 'Capacity', str(path), 'directory inventory budget exceeded')
        require(not artifact.is_symlink(), 'Package', str(artifact), 'symlink artifact is unsupported')
        if artifact.is_file():
            actual.add(artifact.relative_to(path).as_posix())
            total += artifact.stat().st_size
            require(len(actual) <= MAX_PACKAGE_FILES + 1 and total <= MAX_PACKAGE_BYTES + MAX_FILE,
                    'Capacity', str(path), 'package inventory/byte budget exceeded')
    require(actual == set(inventory) | {'integrity.json'}, 'Package', str(path), 'package inventory mismatch')
    for relative, hash_value in inventory.items():
        p = Path(relative)
        require(not p.is_absolute() and '..' not in p.parts and not (path / p).is_symlink() and (path / p).resolve().is_relative_to(path), 'Package', relative, 'invalid artifact path')
        require(digest(read(path / p, 2 * 1024 * 1024)) == hash_value, 'Package', relative, 'artifact digest mismatch')
    require(identity(inventory) == path.name, 'Package', str(path), 'generation identity mismatch')
    return document(read(path / 'package.json', 2 * 1024 * 1024), 'package descriptor')


def new_build_sources(path, *, profiles=('metal',), root=None, renderer_static=False):
    """Save an explicit build manifest and shader functions next to an RG0 root.

    Refuses any existing destination. This is a source template, not GPU execution
    or game/SDK setup. It declares both techniques even when the root selects one.
    Generated schemas and bindings are owned by the manifest, not handwritten
    shader structs. Returns the sibling .lushader.json path.
    """
    path = Path(path).resolve()
    source_root = Path(root).resolve() if root is not None else path.parent
    graph.compile_graph(path, root=source_root)
    require(path.is_relative_to(source_root), 'Source', str(path), 'graph is outside its package')
    require(path.name.endswith('.lurender.json'), 'Source', str(path), 'expected .lurender.json root')
    manifest = path.with_name(path.name.removesuffix('.lurender.json') + '.lushader.json')
    source_dir = path.name.removesuffix('.lurender.json') + '_shaders'
    source_label = (path.parent / source_dir).relative_to(source_root).as_posix()
    field = lambda fid, ty: {'id': fid, 'type': ty}
    vertex = {'id': 'RendererStatic', 'role': 'Vertex', 'fields': [
        {**field('position', 'Float3'), 'semantic': 'POSITION', 'location': 0}]}
    varying = {'id': 'RasterVarying', 'role': 'Varying', 'fields': [
        {**field('position', 'Float4'), 'semantic': 'SV_Position', 'location': 0}]}
    material = {'id': 'Material', 'role': 'Uniform', 'fields': [field('tint', 'Float3'), field('gain', 'Float32')]}
    programs = [
        {'id': 'Geometry', 'stage': 'vertex', 'source': source_label + '/geometry.slang', 'function': 'VSMain',
         'input': 'RendererStatic', 'output': 'RasterVarying', 'bindings': []},
        {'id': 'Color', 'stage': 'fragment', 'source': source_label + '/color.slang', 'function': 'PSMain',
         'input': 'RasterVarying', 'output': 'Color', 'bindings': [{'id': 'material', 'schema': 'Material', 'slot': 0}]},
        {'id': 'Depth', 'stage': 'fragment', 'source': source_label + '/depth.slang', 'function': 'DepthMain',
         'input': 'RasterVarying', 'output': 'Depth', 'bindings': []}]
    techniques = []
    for mode in graph.MODES:
        techniques.append({'id': 'DefaultStaticOpaque.' + mode, 'configuration': mode,
                           'vertex': 'Geometry', 'fragment': 'Color' if mode == 'ColorAndDepth' else 'Depth',
                           'recipes': [{'id': 'Default', 'color': 'RGBA8Unorm' if mode == 'ColorAndDepth' else 'None',
                                        'depth': 'Depth32Float', 'samples': 1, 'topology': 'TriangleList',
                                        'cull': 'Back', 'front': 'CounterClockwise', 'depth_compare': 'Less',
                                        'depth_write': True, 'blend': 'Opaque'}]})
    value = {'version': VERSION, 'id': 'DefaultRasterBuild', 'profiles': list(profiles),
             'schemas': [vertex, varying, material], 'programs': programs, 'techniques': techniques,
             'budgets': {'configurations': 8, 'programs': 8, 'artifacts': 32, 'pipelines': 16}}
    validate_manifest(value)
    sources = {'geometry.slang': 'RasterVarying VSMain(RendererStatic value)\n{\n    RasterVarying result;\n    result.position = float4(value.position, 1);\n    return result;\n}\n',
               'color.slang': 'float4 PSMain(RasterVarying value)\n{\n    return float4(material.tint * material.gain, 1);\n}\n',
               'depth.slang': 'void DepthMain(RasterVarying value)\n{\n    // Authored opaque coverage: no alpha discard or custom depth.\n}\n'}
    if renderer_static:
        fields = [{'id': 'PositionMesh', 'type': 'Float3', 'semantic': 'POSITION', 'location': 0}]
        fields += [{'id': 'MeshToClipRow' + str(i), 'type': 'Float4', 'semantic': 'TEXCOORD' + str(i), 'location': i + 1} for i in range(4)]
        fields.append({'id': 'Color', 'type': 'Float4', 'semantic': 'COLOR0', 'location': 5})
        vertex['fields'] = fields
        varying['fields'] = [{'id': 'PositionClip', 'type': 'Float4', 'semantic': 'SV_Position', 'location': 0},
                             {'id': 'Color', 'type': 'Float4', 'semantic': 'COLOR0', 'location': 0}]
        value['schemas'] = [vertex, varying]
        value['programs'][1]['bindings'] = []
        value['id'] = 'RendererStaticBuild'
        for technique in value['techniques']:
            technique['recipes'][0]['cull'] = 'None'
        sources['geometry.slang'] = ('RasterVarying VSMain(RendererStatic value)\n{\n'
            '    RasterVarying result;\n    float4 positionMesh = float4(value.PositionMesh, 1);\n'
            '    result.PositionClip = float4(dot(value.MeshToClipRow0, positionMesh), dot(value.MeshToClipRow1, positionMesh), '
            'dot(value.MeshToClipRow2, positionMesh), dot(value.MeshToClipRow3, positionMesh));\n'
            '    result.Color = value.Color;\n    return result;\n}\n')
        sources['color.slang'] = 'float4 PSMain(RasterVarying value)\n{\n    return value.Color;\n}\n'
        validate_manifest(value)
    require(not manifest.exists() and not (path.parent / source_dir).exists(), 'Source', str(manifest), 'build sources already exist')
    # Keep creation transactional: publish the owning manifest last.
    with tempfile.TemporaryDirectory(prefix='.rg1-source-', dir=path.parent) as temp:
        staged = Path(temp) / source_dir
        staged.mkdir()
        for name_value, text in sources.items():
            (staged / name_value).write_text(text)
        destination = path.parent / source_dir
        destination.mkdir()
        try:
            for source in staged.iterdir():
                with (destination / source.name).open('xb') as stream:
                    stream.write(source.read_bytes())
            with manifest.open('xb') as stream:
                stream.write(json.dumps(value, indent=2).encode() + b'\n')
        except BaseException:
            shutil.rmtree(destination)
            raise
    return manifest
