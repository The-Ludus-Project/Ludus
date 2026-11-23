"""LUSL program sources in render-graph builds.

A program whose `source` ends in `.lusl` is lowered once per build with `luslc`, the
command-line front of Ludus::ShaderLanguage, from the same captured snapshot the rest of the
build uses. The generated Slang module joins the snapshot at `<source>.slang`, and the program
then names it, so wrappers, closures, cache keys and diagnostics treat it like authored Slang.
Raster programs read their schema structs and bindings through a generated header,
`graph_schemas.luslh`, which the build writes for each LUSL source from the manifest (the
schema generator's LUSL output). Each lowering also reports its template specializations and
`groupshared` clears; the package's
optional `budgets.specializations` bounds the specializations of all its LUSL sources together.
See docs/modules/tooling/shader-language.md, "Render-graph programs".
"""
from __future__ import annotations

import os
import posixpath
import re
import subprocess
import tempfile
from pathlib import Path

from . import render_graph_buffers as buffers

LANGUAGE_SUFFIX = '.lusl'
GENERATED_SUFFIX = '.slang'
# SDK headers are staged where ray wrappers already stage engine helpers.
SDK_DIRECTORY = '_ludus_ray'
STYLES = ('strict', 'warn', 'off')
TIMEOUT_SECONDS = 120
# Paths LUSL names: `#include "x"` and `extern slang "x"`. The scan is lexical and conservative.
NAMED_PATH = re.compile(r'(?:#[ \t]*include|\bextern[ \t\r\n]+slang)[ \t\r\n]*"([^"\n]+)"')
HEADER_SUFFIX = '.luslh'
LANGUAGE_FILES = (LANGUAGE_SUFFIX, HEADER_SUFFIX)
# The generated schema header of raster programs, below its own include directory.
GRAPH_DIRECTORY = '_ludus_graph'
SCHEMA_HEADER = 'graph_schemas.luslh'
RESOURCES_HEADER = 'graph_resources.luslh'
RASTER_STAGES = ('vertex', 'fragment')
# Ray callbacks read their manifest bindings as structured buffers (render_graph_buffers.declarations).
RAY_STAGES = ('ray_generation', 'miss', 'closest_hit')
FAILURE_CODE_VARIABLE = '_luslFailureCode'
# Schema field types (render_graph_build SHAPES and STORAGE_SHAPES) as LUSL types.
LANGUAGE_TYPES = {'Float32': 'float32', 'Float2': 'float32x2', 'Float3': 'float32x3', 'Float4': 'float32x4',
                  **{prefix + suffix: spelling + ('' if suffix == '32' else 'x' + suffix)
                     for prefix, spelling in (('UInt', 'uint32'), ('Int', 'int32')) for suffix in ('32', '2', '3', '4')}}


def is_language_source(source):
    return source.endswith(LANGUAGE_SUFFIX)


def find_luslc(helper, luslc=None):
    """The luslc executable: the explicit one, $LUDUS_LUSLC, or the installed SDK's bin/luslc."""
    for candidate in (luslc, os.environ.get('LUDUS_LUSLC')):
        if candidate:
            return Path(candidate)
    # An installed SDK keeps its helpers in lib/cmake/Ludus/shaders and luslc in bin.
    helper = Path(helper).resolve()
    if len(helper.parents) >= 4:
        installed = helper.parents[3] / 'bin' / ('luslc.exe' if os.name == 'nt' else 'luslc')
        if installed.is_file():
            return installed
    return None


def _load(snapshot, helper, build, relative):
    """The bytes of a snapshot path, capturing it on first use; None when no such file exists."""
    if relative in snapshot.files:
        return snapshot.files[relative]
    if relative.startswith(SDK_DIRECTORY + '/'):
        name = relative[len(SDK_DIRECTORY) + 1:]
        path = Path(helper) / name
        if '/' in name or not path.is_file():
            return None
        data = build.read(path)
        snapshot.capture(relative, data, external=path)
        return data
    if relative.startswith('../') or relative == '..' or posixpath.isabs(relative):
        return None
    try:
        path = snapshot.path(relative)
    except build.ToolingError:
        return None
    return snapshot.load(relative) if path.is_file() else None


def _collect(snapshot, helper, build, source):
    """Every file LUSL may read for `source`, resolved as the preprocessor resolves them:
    beside the naming file, from the snapshot root, then below SDK_DIRECTORY."""
    files = {}
    pending = [source]
    while pending:
        relative = pending.pop()
        if relative in files:
            continue
        data = _load(snapshot, helper, build, relative)
        if data is None:
            continue
        files[relative] = data
        if not relative.endswith(LANGUAGE_FILES):
            continue
        directory = posixpath.dirname(relative)
        for name in NAMED_PATH.findall(data.decode('utf-8', 'replace')):
            for candidate in (posixpath.join(directory, name), name, posixpath.join(SDK_DIRECTORY, name)):
                pending.append(posixpath.normpath(candidate))
    return files


def binding_type(program, binding):
    """How the wrapper of `program` declares `binding`: a uniform struct for raster programs, a
    structured buffer of the binding's access for ray callbacks."""
    if program.get('stage') in RAY_STAGES:
        return ('StructuredBuffer<' if binding.get('access') == 'Read' else 'RWStructuredBuffer<') + binding['schema'] + '>'
    return binding['schema']


def schema_header(source, programs, schemas, build):
    """The LUSL declarations of the schemas and bindings that the raster and ray programs of
    `source` read, or None when no such program names it.

    The block names no file: the generated wrapper declares these structs and bindings before it
    includes the lowered module, as for Slang sources. Raster bindings are uniforms and ray
    bindings structured buffers; LUSL rejects writes to uniforms and to read-only buffers.
    """
    # After lowering, programs name the generated module and keep their authored path.
    users = [p for p in programs.values()
             if p.get('language_source', p['source']) == source and p.get('stage') in RASTER_STAGES + RAY_STAGES]
    if not users:
        return None
    used, bindings = set(), {}
    for program in users:
        if program['stage'] in RASTER_STAGES:
            used.update(sid for sid in (program['input'], program['output']) if sid in schemas)
        for binding in program['bindings']:
            used.add(binding['schema'])
            declared = binding_type(program, binding)
            previous = bindings.setdefault(binding['id'], declared)
            build.require(previous == declared, 'Schema', source,
                          f"programs of one LUSL source bind '{binding['id']}' as {previous} and {declared}; "
                          'give the bindings different ids or the programs different sources')
    if not used and not bindings:
        return None
    lines = ['// Generated by the render-graph build from the manifest schemas; do not edit.',
             '// docs/modules/tooling/shader-language.md#render-graph-programs', '#pragma once', '', 'extern slang', '{']
    for index, sid in enumerate(sorted(used)):
        schema = schemas[sid]
        if index > 0:
            lines.append('')
        lines += [f'    /// The {schema["role"]} schema {sid}.', f'    struct {sid}', '    {']
        for field in schema['fields']:
            build.require(field['type'] in LANGUAGE_TYPES, 'Schema', sid,
                          f"field {field['id']} has type {field['type']}, which LUSL programs cannot read yet")
            lines.append(f'        {LANGUAGE_TYPES[field["type"]]} {field["id"]};')
        lines.append('    };')
    if bindings:
        lines.append('')
    for binding_id, declared in sorted(bindings.items()):
        lines += [f'    /// Binding {binding_id}: {declared}.', f'    {declared} {binding_id};']
    lines += ['};', '']
    return '\n'.join(lines).encode('utf-8')


CONFIGURATION_NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]{0,62}\Z')
MIN_CONFIGURATION_INTEGER = -(1 << 31)
MAX_CONFIGURATION_INTEGER = (1 << 31) - 1


def configuration_values(program, build):
    """Validate authored scalar values and serialize them in stable name order."""
    if 'configuration_values' not in program:
        return {}
    values = program['configuration_values']
    where = program['id']
    build.require(is_language_source(program['source']) and type(values) is dict, 'Configuration', where,
                  'configuration_values must be an object on a LUSL program')
    for name, value in values.items():
        build.require(type(name) is str and CONFIGURATION_NAME.fullmatch(name) is not None, 'Configuration', where,
                      f'invalid configuration macro name {name!r}; use a shader identifier of up to 63 characters')
        build.require(type(value) is bool or type(value) is int and
                      MIN_CONFIGURATION_INTEGER <= value <= MAX_CONFIGURATION_INTEGER, 'Configuration', where,
                      f'{name} must be a Boolean or a signed 32-bit integer')
    return {name: int(values[name]) for name in sorted(values)}


def configuration_arguments(configuration):
    """Build luslc arguments from normalized scalar values."""
    return [argument for name, value in configuration.items() for argument in ('--define', f'{name}={value}')]


def configuration_reads(snapshot, closure):
    """Collect the selected values read by the lowered modules linked into this stage."""
    reads = {}
    for relative in closure:
        if relative.endswith(GENERATED_SUFFIX):
            source = relative.removesuffix(GENERATED_SUFFIX)
            values = snapshot.language_reports.get(source, {}).get('configuration_reads')
            if values:
                reads[source] = values
    return reads


def _parse_report(text, build, source, configuration=None):
    """Read the configuration mask, specializations, groupshared clears and bounds checks.

    Graph program bindings come from the wrapper, so standalone layout records are skipped.
    """
    configuration = configuration or {}
    report = {'specializations': [], 'groupshared_clears': []}
    mask = None
    for line in text.splitlines():
        fields = line.split('\t')
        if fields[0] in ('binding', 'vertex-input'):
            continue
        if fields[0] == 'configuration-read-mask':
            build.require(mask is None and len(fields) == 2 and len(fields[1]) == len(configuration) and
                          set(fields[1]) <= {'0', '1'}, 'Tools', source,
                          'invalid luslc configuration read mask; use luslc from the matching Ludus SDK')
            mask = fields[1]
        elif fields[0] == 'specialization' and len(fields) == 4:
            report['specializations'].append({'template': fields[1], 'name': fields[2], 'generated': fields[3]})
        elif fields[0] == 'groupshared-clear' and len(fields) == 4 and fields[3].isdigit():
            report['groupshared_clears'].append({'entry': fields[1], 'variable': fields[2], 'bytes': int(fields[3])})
        elif fields[0] == 'bounds-check' and len(fields) == 3:
            path, separator, source_line = fields[1].rpartition(':')
            build.require(bool(separator and path and source_line.isdecimal() and fields[2].isdecimal()),
                          'Tools', source, 'luslc wrote a malformed bounds-check location')
            sites = report.setdefault('bounds_checks', [])
            code = int(fields[2])
            build.require(code == len(sites) + 1 and 0 < int(source_line) <= 0xffffffff,
                          'Tools', source, 'luslc wrote an invalid bounds-check code or line')
            sites.append({'code': code, 'source': path, 'line': int(source_line)})
        else:
            build.fail('Tools', source, f'luslc wrote an unknown report line: {line[:200]}')
    if configuration:
        build.require(mask is not None, 'Tools', source,
                      'luslc did not report configuration reads; use luslc from the matching Ludus SDK')
        report['configuration_reads'] = {name: value for (name, value), bit in zip(configuration.items(), mask)
                                         if bit == '1'}
    return report


def _lower(files, build, tool, source, style, specialization_budget, include_directories, configuration, bounds_checks):
    with tempfile.TemporaryDirectory(prefix='ludus-lusl-') as directory:
        root = Path(directory) / 'source'
        for relative, data in files.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        output = Path(directory) / 'generated.slang'
        report = Path(directory) / 'report.txt'
        command = [str(tool), 'lower', '--root', str(root), *configuration_arguments(configuration)]
        for include_directory in include_directories:
            command += ['--include-directory', include_directory]
        command += ['--style', style, '--output', str(output), '--report', str(report)]
        if specialization_budget is not None:
            command += ['--max-specializations', str(specialization_budget)]
        if bounds_checks:
            command.append('--bounds-checks')
        command.append(source)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            build.fail('Tools', source, f'luslc could not run: {error}')
        build.require(result.returncode == 0, 'Source', source,
                      result.stderr.strip() or f'luslc failed with exit code {result.returncode}')
        return output.read_bytes(), _parse_report(report.read_text(encoding='utf-8'), build, source, configuration)


def lower_buffer_headers(snapshot, sources, helper, build, *, luslc=None, style='strict', cancel=None):
    """Lower graph-owned LUSL layouts before shader resource interfaces are generated."""
    headers = sorted(source for source in sources if source.endswith(HEADER_SUFFIX))
    if not headers:
        return None
    tool = find_luslc(helper, luslc)
    build.require(tool is not None and tool.is_file(), 'Tools', headers[0],
                  'LUSL buffer layouts need luslc: use the installed SDK or set LUDUS_LUSLC')
    build.require(style in STYLES, 'Configuration', 'lusl_style', f'expected one of {", ".join(STYLES)}')
    for header in headers:
        build.checkpoint(cancel)
        source = header + LANGUAGE_SUFFIX
        generated = header + GENERATED_SUFFIX
        for path in (source, generated):
            build.require(path not in snapshot.files and not snapshot.path(path).exists(), 'Source', path,
                          'this name is reserved by the LUSL Buffer layout; rename the authored file')
        files = _collect(snapshot, helper, build, header)
        files[source] = f'#include "{posixpath.basename(header)}"\n'.encode()
        data, _ = _lower(files, build, tool, source, style, None, [SDK_DIRECTORY], {}, False)
        snapshot.capture(generated, b'#pragma once\n' + data, generated=True)
        snapshot.buffer_layout_sources[header] = generated
        closure = snapshot.shader_closure(generated)
        sdk_roots = [path.removeprefix(SDK_DIRECTORY + '/') for path in closure
                     if path.startswith(SDK_DIRECTORY + '/')]
        helpers = build.ray.stage_helpers(snapshot, helper, sdk_roots, build)
        snapshot.buffer_layout_dependencies[header] = sorted(set(closure) | helpers)
    return tool


def _record_declaration(shape, name, definitions, array_lengths, build):
    """Mirror the compiler's record shape for LUSL type checking, not storage sizing."""
    kind = shape['kind']
    if kind == 'struct':
        type_name = shape.get('name')
        build.name(type_name, 'shared Buffer record type')
        fields = [_record_declaration(field['type'], field['name'], definitions, array_lengths, build)
                  for field in shape['fields']]
        declaration = f"struct {type_name} {{ " + ' '.join(fields) + ' };'
        build.require(definitions.setdefault(type_name, declaration) == declaration, 'ShaderInterface', type_name,
                      'give distinct names to Buffer record types used in one LUSL program')
        return f'{type_name} {name};'
    if kind == 'array':
        length = shape['elementCount']
        constant = array_lengths.setdefault(length, f'LUDUS_GRAPH_REFLECTED_ARRAY_LENGTH_{length}')
        return _record_declaration(shape['elementType'], f'{name}[{constant}]', definitions, array_lengths, build)
    scalar = (shape['elementType'] if kind == 'vector' else shape).get('scalarType')
    build.require(kind in ('scalar', 'vector') and scalar in ('float32', 'int32', 'uint32'), 'ShaderInterface', name,
                  'Buffer record fields must use 32-bit scalars, vectors, structs or fixed arrays')
    spelling = scalar + (f'x{shape["elementCount"]}' if kind == 'vector' else '')
    return f'{spelling} {name};'


def _record_array_declarations(shape, is_writable):
    declarations = []
    parameters = 'in uint32 recordIndex, in uint32 elementIndex'
    for field in buffers.record_array_fields(shape):
        name = field['name']
        element = field['type']['elementType']['name']
        declarations.append(f'{element} Load{name}({parameters});')
        if is_writable:
            declarations.append(f'void Store{name}({parameters}, in {element} element);')
    return ' ' + ' '.join(declarations) if declarations else ''


def prepare_resource_headers(snapshot, programs, contracts, compiler, cancel, build):
    """Stage each LUSL module's generated interface before lowering or source assistance."""
    contexts = {}
    for program in programs.values():
        source = program['source']
        if is_language_source(source) and buffers.requests_named_resources(program, snapshot):
            contexts.setdefault(source, {})[program['function']] = []
    for variant, fields in contracts.items():
        program = programs[variant[0]]
        if is_language_source(program['source']):
            contexts.setdefault(program['source'], {})[program['function']] = fields
    if not contexts:
        return
    # The same reflection results are reused to size storage and emit executable accessors.
    with tempfile.TemporaryDirectory(prefix='ludus-buffer-interface-') as directory:
        for source, functions in contexts.items():
            records, views, resources = {}, {}, []
            array_lengths = {}
            for function, fields in functions.items():
                members = []
                for field in fields:
                    view = buffers.record_view_name(field)
                    if field.get('texture'):
                        views[view] = (f'struct {view} {{ uint32x2 GetDimensions(); '
                                       'float32x4 Load(in uint32x2 positionPixel); '
                                       'void Store(in uint32x2 positionPixel, in float32x4 value); };')
                        members.append(f'{view} {field["field"]};')
                        continue
                    record = field['record']
                    array_access = ''
                    if record:
                        key = (record['source'], record['type'])
                        if key not in snapshot.buffer_record_layouts:
                            build.require(compiler, 'Tools', record['source'],
                                          'select the Slang compiler to resolve the connected Buffer layout')
                            stage = Path(directory) / str(len(snapshot.buffer_record_layouts))
                            snapshot.buffer_record_layouts[key] = buffers.reflect_record(
                                *key, snapshot, stage, compiler, cancel, build)
                        _, shape = snapshot.buffer_record_layouts[key]
                        _record_declaration(shape, field['field'], records, array_lengths, build)
                        element = record['type']
                        array_access = _record_array_declarations(shape, field['writable'])
                    else:
                        element = 'uint32x4'
                    setter = f' void Store(in uint32 index, in {element} record);' if field['writable'] else ''
                    byte_access = ''
                    if record:
                        byte_access = ' uint32 GetByteSize(); uint32 LoadWord(in uint32 byteOffset);'
                        if field['writable']:
                            byte_access += ' void StoreWord(in uint32 byteOffset, in uint32 word);'
                    views[view] = (f'struct {view} {{ uint32 GetCount(); {element} Load(in uint32 index);'
                                   f'{setter}{byte_access}{array_access} }};')
                    members.append(f'{view} {field["field"]};')
                resources.append(f'struct {function}Resources {{ ' +
                                 ' '.join(members) + ' };')
            constants = [f'static const uint32 {name} = {length};' for length, name in sorted(array_lengths.items())]
            lines = ['#pragma once', *constants, 'extern slang', '{', *records.values(), *views.values(), *resources,
                     '};', '']
            snapshot.language_resource_headers[source] = '\n'.join(lines).encode()


def stage_generated_headers(snapshot, files, include_directories, source, programs, schemas, build):
    """Give lowering and the language server one generated header set."""
    headers = {SCHEMA_HEADER: schema_header(source, programs, schemas, build),
               RESOURCES_HEADER: snapshot.language_resource_headers.get(source)}
    if not any(headers.values()):
        return
    include_directories.append(GRAPH_DIRECTORY)
    for name, header in headers.items():
        if header is not None:
            files[posixpath.join(GRAPH_DIRECTORY, name)] = header


def lower_sources(snapshot, programs, helper, build, *, luslc=None, style='strict', specialization_budget=None,
                  schemas=None, bounds_checks=False):
    """Lower every `.lusl` program source once and point its programs at the generated module.

    Each program keeps its authored path as `language_source`. `specialization_budget` is the
    manifest's budgets.specializations, or None for the engine limit per source. `schemas` are the
    manifest schemas, from which each source with raster programs gets its `graph_schemas.luslh`
    below `_ludus_graph`. Enable `bounds_checks` to collect and report runtime check sites
    for ray programs. Returns the luslc that ran and the report of each lowered source; the
    tool is None and the reports empty when no program is written in LUSL.
    """
    configurations = snapshot.language_configurations
    for program in programs.values():
        values = configuration_values(program, build)
        source = program['source']
        if is_language_source(source):
            previous = configurations.setdefault(source, values)
            build.require(values == previous, 'Configuration', source,
                          'programs sharing a LUSL source must select the same configuration_values; '
                          'use a separate entry file for a different selection')
    sources = sorted(configurations)
    if not sources:
        return None, {}
    build.require(style in STYLES, 'Configuration', 'lusl_style',
                  f"LUSL style '{style}' is not one of {', '.join(STYLES)}")
    tool = find_luslc(helper, luslc)
    build.require(tool is not None and tool.is_file(), 'Tools', sources[0],
                  'LUSL program sources need luslc: build with an SDK that installs bin/luslc, '
                  'or set LUDUS_LUSLC to its path')
    reports = {}
    for index, source in enumerate(sources):
        generated = source + GENERATED_SUFFIX
        build.require(generated not in snapshot.files, 'Source', generated,
                      'the generated module of ' + source + ' would replace an authored file; rename one of them')
        files = _collect(snapshot, helper, build, source)
        include_directories = [SDK_DIRECTORY]
        stage_generated_headers(snapshot, files, include_directories, source, programs, schemas or {}, build)
        # Ray invocations have a diagnostic buffer; raster stages have no failure channel.
        checks = bounds_checks and any(p['source'] == source and p.get('stage') in (*RAY_STAGES, 'compute')
                                      for p in programs.values())
        data, reports[source] = _lower(files, build, tool, source, style, specialization_budget,
                                       include_directories, configurations[source], checks)
        if reports[source].get('bounds_checks'):
            # Several lowered modules can be included in one global Slang scope.
            # Rename only compiler-reserved instrumentation identifiers in that module.
            names = sorted(set(re.findall(rb'\b(?:_luslFailureCode|_luslCheckIndex[0-9]+)\b', data)))
            prefixes = []
            for name in names:
                renamed = name + b'GraphModule' + str(index).encode('ascii')
                prefixes.append(b'#define ' + name + b' ' + renamed + b'\n')
                if name == FAILURE_CODE_VARIABLE.encode('ascii'):
                    reports[source]['failure_variable'] = renamed.decode('ascii')
            data = b''.join(prefixes) + data + b'\n' + b''.join(b'#undef ' + name + b'\n' for name in names)
        snapshot.capture(generated, data, generated=True)
    if specialization_budget is not None:
        total = sum(len(report['specializations']) for report in reports.values())
        largest = max(sources, key=lambda source: len(reports[source]['specializations']))
        build.require(total <= specialization_budget, 'Capacity', 'budgets.specializations',
                      f'the LUSL sources need {total} template specializations together, over budgets.specializations '
                      f'{specialization_budget}; increase budgets.specializations to at least {total}, or constrain '
                      f'templates by interfaces only so they lower to one Slang generic ({largest} has the most)')
    for program in programs.values():
        if is_language_source(program['source']):
            program.pop('configuration_values', None)
            program['language_source'] = program['source']
            program['source'] = program['source'] + GENERATED_SUFFIX
    return tool, reports
