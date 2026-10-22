"""Cook verified RG1 packages into typed, numeric Renderer execution headers.

The neutral records belong to Ludus::RenderGraphFormat, not this tool or Renderer.
Only the static six-location profile is admitted, with opaque and explicitly
authored masked technique families and per-draw scalar/vector uniform bindings;
runtime scene/camera/uniform values remain separate. Unsupported resource topology
and recipes reject before writing the output. No shader configurations are synthesized.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct

from . import render_graph as graph
from . import render_graph_build as build
from . import render_graph_ray as ray
from . import render_graph_buffers as buffers

MAX_PASSES = 16
MAX_RESOURCES = 32
MAX_CAMERAS = 8
MAX_RAY_PASSES = 1
MAX_UNIFORM_VALUES = 64
MAX_UNIFORM_BINDINGS = 16
MAX_UNIFORM_FIELDS = 256
MAX_UNIFORM_BLOCK_BYTES = 1024
UNIFORM_SLOT_COUNT = 8
FORMAT_VERSION = 6
ABSENT = 0xFFFFFFFF
# Exact technique identities and their numeric program roles, in format order.
PROGRAMS = {'DefaultStaticOpaque.ColorAndDepth': 'ColorAndDepth', 'DefaultStaticOpaque.DepthOnly': 'DepthOnly',
            'DefaultStaticMasked.ColorAndDepth': 'MaskedColorAndDepth', 'DefaultStaticMasked.DepthOnly': 'MaskedDepthOnly', ray.TECHNIQUE: 'RayTracing'}
PROGRAM_CODES = {name: code for code, name in enumerate(PROGRAMS.values())}
PROGRAM_MEMBERS = {'ColorAndDepth': 'Color', 'DepthOnly': 'Depth', 'MaskedColorAndDepth': 'MaskedColor',
                   'MaskedDepthOnly': 'MaskedDepth', 'RayTracing': 'RayImage'}
# Preserve the five historical program IDs. Additional bounded raster pairs carry
# their coverage family in the low position of each four-role variant group.
LEGACY_PROGRAM_COUNT = len(PROGRAMS)
RASTER_FAMILY_COUNT = LEGACY_PROGRAM_COUNT - 1
MAX_AUTHORED_TECHNIQUES = build.LIMITS['configurations']
LEGACY_STAGE_ROLE_COUNT = 12
for variant in range(1, MAX_AUTHORED_TECHNIQUES):
    for family, base in enumerate(tuple(PROGRAM_MEMBERS)[:RASTER_FAMILY_COUNT]):
        name = base + 'Variant' + str(variant)
        code = LEGACY_PROGRAM_COUNT + (variant - 1) * RASTER_FAMILY_COUNT + family
        PROGRAM_CODES[name] = code
VISIBILITY = {'Vertex': 1, 'Fragment': 2, 'Both': 3}


def program_roles(package):
    """Assign deterministic bounded shader-pair slots within each coverage family."""
    result = dict(PROGRAMS)
    for identity, base in PROGRAMS.items():
        if base == 'RayTracing':
            continue
        variants = sorted({recipe['technique'] for recipe in package.get('recipes', [])
                           if recipe['technique'].startswith(identity + '.')})
        build.require(len(variants) < MAX_AUTHORED_TECHNIQUES, 'Capacity', identity,
                      'too many independent shader pairs for one raster family')
        result.update({technique: base + 'Variant' + str(index + 1) for index, technique in enumerate(variants)})
    return result


def program_member(program, stage):
    """C++ member expression of one cooked stage; declaration names stay separate."""
    code = PROGRAM_CODES[program]
    if code >= LEGACY_PROGRAM_COUNT:
        return 'AdditionalRasterStages[' + str((code - LEGACY_PROGRAM_COUNT) * 2 + (stage == 'fragment')) + ']'
    return 'RayCompute' if stage == 'compute' else PROGRAM_MEMBERS[program] + stage.title()


def resolve_generation(path):
    path = Path(path).resolve()
    if (path / 'current.json').is_file():
        pointer = build.document(build.read(path / 'current.json'), 'current generation')
        build.keys(pointer, ('version', 'generation'), 'current generation')
        build.require(pointer['version'] == 1 and type(pointer['generation']) is str and
                      len(pointer['generation']) == 64 and all(c in '0123456789abcdef' for c in pointer['generation']),
                      'Package', str(path), 'invalid generation pointer')
        path = path / 'generations' / pointer['generation']
    return path


def lower_plan(package, configuration):
    roles = program_roles(package)
    plans = [p for p in package['plans'] if p['configuration'] == configuration]
    build.require(len(plans) == 1, 'Configuration', configuration, 'one declared root configuration required')
    plan = plans[0]
    resources = sorted(name for name, value in plan['resources'].items() if value['type'] != 'Buffer')
    rasters = [p for p in plan['passes'] if p['kind'] in ('Raster', 'RayTracing')]
    cameras = sorted({p['view'] for p in rasters})
    build.require(sum(p['kind'] == 'RayTracing' for p in rasters) <= MAX_RAY_PASSES, 'Capacity', configuration,
                  'one triangle ray invocation is admitted per configuration')
    build.require(all(p['kind'] in ('Raster', 'RayTracing', 'Export', 'Present') for p in plan['passes']) and
                  all((p['kind'] == 'RayTracing' and p['technique'] == ray.TECHNIQUE) or (p['configuration'] in graph.MODES and p['material'] in graph.MATERIALS and
                      (p['technique'] == graph.MATERIALS[p['material']] + '.' + p['configuration'] or
                       p['technique'].startswith(graph.MATERIALS[p['material']] + '.' + p['configuration'] + '.')) and
                      p['technique'] in roles and p['vertex_layout'] == 'RendererStatic') for p in rasters),
                  'Configuration', configuration, 'unqualified operation/layout/material/technique')
    build.require(0 < len(rasters) <= MAX_PASSES and 0 < len(resources) <= MAX_RESOURCES and len(cameras) <= MAX_CAMERAS,
                  'Capacity', configuration, 'Renderer execution limits exceeded')
    build.require(len({p['scene'] for p in rasters}) == 1, 'Configuration', configuration, 'one World extraction per execution profile')
    backing = {r: r for r in resources}
    for operation in rasters:
        attachments = operation['attachments']
        if operation['kind'] == 'RayTracing':
            continue
        depth = attachments['depth']['resource']
        if 'color' in attachments:
            color = attachments['color']['resource']
            build.require(backing[depth] in (depth, color), 'Configuration', depth, 'independent/shared depth attachment topology unsupported')
            backing[depth] = color
    depth_owners = [backing[r] for r in resources if plan['resources'][r]['type'] == 'DepthTexture']
    surface_owners = {backing[r] for r in resources if plan['resources'][backing[r]]['presentation']}
    build.require(len(depth_owners) == len(set(depth_owners)) and len(surface_owners) <= 1,
                  'Configuration', configuration, 'distinct logical images cannot alias one private depth/surface')
    output_resources = {r['resource'] for r in plan['roots'] if r['kind'] == 'Export'}
    resource_records = []
    for resource in resources:
        value = plan['resources'][resource]
        build.require(value['format'] in ('RGBA8Unorm', 'Depth32Float') and value['samples'] == 1 and
                      not value['initialized'] and (not value['imported'] or value['presentation']),
                      'Configuration', resource, 'unqualified format/sample/import/history profile')
        parent = plan['resources'][backing[resource]]
        resource_records.append({'Width': value['extent'][0], 'Height': value['extent'][1],
                                 'Kind': 'Color' if value['type'] == 'ColorTexture' else 'Depth',
                                 'Backing': resources.index(backing[resource]), 'IsSurface': int(parent['presentation']),
                                 'IsExported': int(resource in output_resources),
                                 'ViewNumerator': value.get('view_scale', [0, 1])[0],
                                 'ViewDenominator': value.get('view_scale', [0, 1])[1]})
    pass_records = []
    color_defined = {r: False for r in resources}
    for operation in rasters:
        if operation['kind'] == 'RayTracing':
            if 'visibility' in operation:
                producer = rasters[rasters.index(operation) - 1] if rasters.index(operation) != 0 else {}
                visibility = operation['visibility']['resource']
                visibility_depth = operation['visibility_depth']['resource']
                color_output = producer.get('attachments', {}).get('color', {})
                depth_output = producer.get('attachments', {}).get('depth', {})
                build.require(producer.get('kind') == 'Raster' and producer.get('material') == 'DefaultOpaque' and
                              producer.get('view') == operation['view'] and
                              color_output.get('resource') == visibility and depth_output.get('resource') == visibility_depth and
                              color_output.get('output_version') == operation['visibility']['input_version'] and
                              depth_output.get('output_version') == operation['visibility_depth']['input_version'] and
                              all(item.get('load') == 'Clear' and item.get('store') == 'Store' for item in (color_output, depth_output)),
                              'Configuration', operation['id'],
                              'visibility/depth must come from the immediately preceding opaque Clear/Store raster using the same view')
            color = operation['attachments']['color']
            pass_records.append({'Program': 'RayTracing', 'Camera': cameras.index(operation['view']),
                'Color': resources.index(color['resource']), 'Depth': ABSENT, 'ColorLoad': 'Clear', 'ColorStore': 'Store',
                'DepthLoad': 'Discard', 'DepthStore': 'Discard', 'AllowsRepeatedCoverage': 0, 'SceneData': operation.get('scene_data', graph.scene_bundle.ALL_DATA),
                'Visibility': resources.index(operation['visibility']['resource']) if 'visibility' in operation else ABSENT,
                'VisibilityDepth': resources.index(operation['visibility_depth']['resource']) if 'visibility_depth' in operation else ABSENT})
            continue
        choices = [r for r in package['recipes'] if r['technique'] == operation['technique'] and
                   ('pipeline' not in operation or all(r['state'][key] == value
                    for key, value in operation['pipeline']['state'].items()))]
        build.require(choices and all(r['state']['color'] == ('RGBA8Unorm' if operation['configuration'] == 'ColorAndDepth' else 'None') and
                      r['state']['depth'] == 'Depth32Float' and r['state']['samples'] == 1 and r['state']['topology'] == 'TriangleList' and r['state']['cull'] == 'None' and r['state']['front'] == 'CounterClockwise' and
                      r['state']['depth_compare'] == 'Less' and r['state']['depth_write'] and r['state']['blend'] == 'Opaque'
                      for r in choices), 'Configuration', operation['id'], 'recipe differs from admitted static Renderer state')
        attachment = operation['attachments']
        depth = attachment['depth']['resource']
        color = attachment.get('color')
        physical = backing[depth]
        color_load = color['load'] if color else ('Load' if color_defined[physical] else 'Discard')
        color_store = color['store'] if color else ('Store' if color_defined[physical] else 'Discard')
        color_defined[physical] = color_store == 'Store' and (color_load == 'Clear' or color_load == 'Load' and color_defined[physical])
        pass_records.append({'Program': roles[operation['technique']], 'Camera': cameras.index(operation['view']),
                             'Color': resources.index(color['resource']) if color else ABSENT,
                             'Depth': resources.index(depth), 'ColorLoad': color_load, 'ColorStore': color_store,
                             'DepthLoad': attachment['depth']['load'], 'DepthStore': attachment['depth']['store'],
                             'AllowsRepeatedCoverage': 0, 'SceneData': operation.get('scene_data', graph.scene_bundle.ALL_DATA),
                             'Visibility': ABSENT, 'VisibilityDepth': ABSENT})
    # Distinct explicitly authored shader pairs represent deliberate passes (for
    # example GBuffer and beauty), rather than an accidental duplicate scene route.
    for operation, record in zip(rasters, pass_records):
        if operation['kind'] != 'Raster':
            continue
        peers = [peer for peer in rasters if peer['kind'] == 'Raster' and
                 peer['view'] == operation['view'] and peer['material'] == operation['material'] and
                 peer['configuration'] == operation['configuration']]
        if len(peers) > 1 and len({peer['technique'] for peer in peers}) == len(peers):
            record['AllowsRepeatedCoverage'] = 1
    for index, record in enumerate(pass_records):
        if record.get('Visibility', ABSENT) != ABSENT:
            record['AllowsRepeatedCoverage'] = 1
            pass_records[index - 1]['AllowsRepeatedCoverage'] = 1
    return plan, resource_records, pass_records, cameras


STATIC_INPUTS = [{'location': i, 'format': 'Float3' if i == 0 else 'Float4'} for i in range(6)]
VISIBILITY_INPUTS = STATIC_INPUTS + [{'location': i, 'format': 'Float2'} for i in (6, 7)]

def stage_inputs(item):
    return [{k: v for k, v in x.items() if k != 'name'} for x in item['descriptor']['interface']['inputs']]

TARGET_FIELDS = {'metal': 'Msl', 'spirv': 'Spirv', 'wgsl': 'Wgsl', 'glsl': 'GlslEs'}
# Serialized runtime generation (RG3); mirrors render_graph_format/runtime_package.h.
PACKAGE_MAGIC = 0x4752554C
PACKAGE_VERSION = 9
MAX_PASS_NAME_BYTES = 127
IDENTITY_BYTES = 32
MAX_RUNTIME_BYTES = 16 * 1024 * 1024
MAX_ENTRY_BYTES = 63
TARGET_CODES = {'metal': 0, 'spirv': 1, 'wgsl': 2, 'glsl': 3}
TARGET_NAMES = {'metal': 'Metal', 'spirv': 'Spirv', 'wgsl': 'Wgsl', 'glsl': 'GlslEs'}
FORMAT_CODES = {'Float32': 0, 'Float2': 1, 'Float3': 2, 'Float4': 3}


def role(program, stage):
    """Numeric StageRole: two consecutive roles (vertex, fragment) per program."""
    if program == 'RayTracing':
        return {'compute': 8, 'vertex': 9, 'fragment': 10}[stage]
    code = PROGRAM_CODES[program]
    return (code * 2 if code < LEGACY_PROGRAM_COUNT else
            LEGACY_STAGE_ROLE_COUNT + (code - LEGACY_PROGRAM_COUNT) * 2) + (stage == 'fragment')


def _lower_uniforms(package, stages):
    """Merge per-stage reflected layouts into neutral uniform tables.

    Each technique slot reads one schema; a slot declared by both stages must have
    the identical schema and target layout. Logical values are grouped by schema,
    in identifier order, so techniques sharing a schema share one value span.
    """
    schemas = {s['id']: s for s in package['schemas'] if s['role'] == 'Uniform'}
    targets = sorted({key[2] for key in stages}, key=TARGET_CODES.get)
    merged = {}
    for (program, stage, target), record in sorted(stages.items()):
        for layout in record['descriptor'].get('layouts', []):
            slot = layout['slot']
            build.require(type(slot) is int and 0 <= slot < UNIFORM_SLOT_COUNT and layout['schema'] in schemas,
                          'Configuration', record['id'], 'uniform binding outside the admitted profile')
            shape = {'size': layout['size'], 'fields': [(f['id'], f['type'], f['offset']) for f in layout['fields']]}
            entry = merged.setdefault((program, slot), {'schema': layout['schema'], 'stages': set(), 'layouts': {}})
            build.require(entry['schema'] == layout['schema'] and entry['layouts'].get(target, shape) == shape,
                          'Configuration', record['id'], 'one technique slot needs one schema and layout in every stage')
            entry['stages'].add(stage)
            entry['layouts'][target] = shape
    for (program, slot), entry in merged.items():
        build.require(set(entry['layouts']) == set(targets), 'Configuration', program,
                      'every carried target needs the binding layout')
    used = sorted({entry['schema'] for entry in merged.values()})
    offsets, count = {}, 0
    for schema in used:
        offsets[schema] = count
        count += sum(build.SHAPES[f['type']][1] for f in schemas[schema]['fields'])
    build.require(count <= MAX_UNIFORM_VALUES and len(merged) <= MAX_UNIFORM_BINDINGS, 'Capacity', 'uniforms',
                  'per-draw uniform values or bindings exceed the runtime profile')
    bindings, blocks, fields = [], [], []
    for index, ((program, slot), entry) in enumerate(sorted(merged.items(), key=lambda x: (PROGRAM_CODES[x[0][0]], x[0][1]))):
        schema = schemas[entry['schema']]
        values = sum(build.SHAPES[f['type']][1] for f in schema['fields'])
        visibility = 'Both' if len(entry['stages']) == 2 else ('Vertex' if 'vertex' in entry['stages'] else 'Fragment')
        bindings.append({'Program': program, 'Slot': slot, 'Stages': visibility,
                         'ValueOffset': offsets[entry['schema']], 'ValueCount': values, 'schema': entry['schema'],
                         'layouts': {t: entry['layouts'][t] for t in targets}})
        for target in targets:
            layout = entry['layouts'][target]
            build.require(type(layout['size']) is int and 0 < layout['size'] <= MAX_UNIFORM_BLOCK_BYTES,
                          'Configuration', program, 'uniform block size outside the runtime profile')
            blocks.append({'Binding': index, 'Target': target, 'Size': layout['size']})
            relative = 0
            build.require([f[0] for f in layout['fields']] == [f['id'] for f in schema['fields']],
                          'Configuration', program, 'uniform layout fields differ from schema order')
            for (fid, kind, offset), source in zip(layout['fields'], schema['fields']):
                components = build.SHAPES[kind][1]
                build.require(kind == source['type'] and type(offset) is int and offset % 4 == 0 and
                              offset + 4 * components <= layout['size'],
                              'Configuration', program, 'invalid uniform field extent')
                fields.append({'Binding': index, 'Target': target, 'ValueOffset': relative,
                               'Components': components, 'ByteOffset': offset})
                relative += components
    build.require(len(fields) <= MAX_UNIFORM_FIELDS, 'Capacity', 'uniforms', 'uniform field records exceed the runtime profile')
    interface = [{'schema': schemas[s], 'value_offset': offsets[s],
                  'bindings': [{'technique': b['Program'], 'slot': b['Slot'], 'stages': b['Stages'], 'layouts': b['layouts']}
                               for b in bindings if b['schema'] == s]} for s in used]
    return count, bindings, blocks, fields, interface


def _stage_bindings(program, stage, bindings):
    """RasterBinding requirements of one stage: its slots, with the largest carried block size."""
    result = []
    for binding in bindings:
        if binding['Program'] == program and (binding['Stages'] == 'Both' or binding['Stages'] == stage.title()):
            result.append({'slot': binding['Slot'], 'visibility': stage.title(),
                           'size': max(layout['size'] for layout in binding['layouts'].values())})
    return result


def _block_names(descriptor):
    """GLSL ES uniform block names by slot, from the verified emitted interface."""
    names = []
    for slot, name in sorted((int(k), v) for k, v in descriptor['interface'].get('blocks', {}).items()):
        build.require(type(name) is str and 0 < len(name) <= MAX_ENTRY_BYTES and name.isascii() and
                      (name[0].isalpha() or name[0] == '_') and all(c.isalnum() or c == '_' for c in name),
                      'Package', descriptor.get('entry', 'stage'), 'invalid GLSL ES uniform block name')
        names.append((slot, name))
    return names


def _runtime_contents(path, configuration):
    """Lower one verified static generation into numeric plan records and stage artifacts.

    Shared by the compiled-header and runtime-package emitters, so both carry the
    same plan, uniform tables, programs and CPU interface identity for a generation.
    """
    path = resolve_generation(path)
    package = build.verify_package(path)
    build.require(package['id'] == 'RendererStaticBuild', 'Configuration', package['id'],
                  'use an explicitly authored --renderer-static build manifest')
    plan, resources, passes, cameras = lower_plan(package, configuration)
    stages = {}
    roles = program_roles(package)
    vertex_schemas = []
    selected_techniques = set(plan['techniques'])
    if ray.TECHNIQUE in selected_techniques:
        selected_techniques.add('DefaultStaticOpaque.ColorAndDepth')
    for technique in sorted(selected_techniques):
        program_name = roles[technique]
        recipes = [r for r in package['recipes'] if r['technique'] == technique]
        for recipe in recipes:
            if technique != ray.TECHNIQUE:
                vertex_schemas.append(recipe['vertex_schema'])
            for stage in (('compute', 'vertex', 'fragment') if technique == ray.TECHNIQUE else ('vertex', 'fragment')):
                matches = [p for p in package['programs'] if p['key'] == recipe[stage] and p['profile'] == recipe['profile']]
                build.require(len(matches) == 1, 'Package', technique, 'stage identity is ambiguous')
                key = (program_name, stage, recipe['profile'])
                build.require(key not in stages or stages[key]['key'] == matches[0]['key'],
                              'Configuration', technique, 'multiple stage programs for one static role/profile')
                stages[key] = matches[0]
    for color, depth in (('ColorAndDepth', 'DepthOnly'), ('MaskedColorAndDepth', 'MaskedDepthOnly')):
        if any(k[0] == color for k in stages) and any(k[0] == depth for k in stages):
            build.require(all(stages[(color, 'vertex', t)]['key'] == stages[(depth, 'vertex', t)]['key']
                              for t in package['profiles']), 'Configuration', configuration, 'geometry programs must match exactly')
    if any(k[0] == 'MaskedDepthOnly' for k in stages) and any(k[0] == 'DepthOnly' for k in stages):
        build.require(all(stages[('MaskedDepthOnly', 'fragment', t)]['key'] != stages[('DepthOnly', 'fragment', t)]['key']
                          for t in package['profiles']), 'Configuration', configuration,
                      'masked coverage requires a separately authored depth function')
    build.require(vertex_schemas and all(v == vertex_schemas[0] for v in vertex_schemas), 'Configuration', configuration,
                  'selected techniques must share one vertex interface')
    camera_targets = []
    for camera in cameras:
        choices = [c for c in plan['cameras'] if c['id'] == camera and c['active']]
        build.require(len(choices) == 1, 'Configuration', camera, 'active camera target required')
        camera_targets.append(sorted(name for name, value in plan['resources'].items() if value['type'] != 'Buffer').index(choices[0]['target']))
    value_count, bindings, blocks, fields, uniform_interface = _lower_uniforms(package, stages)
    storage_buffers = buffers.lower_bindings(plan, stages, build)
    artifacts = {}
    for ordinal, (key, program) in enumerate(sorted(stages.items(), key=lambda x: (role(x[0][0], x[0][1]), TARGET_CODES[x[0][2]]))):
        program_name, stage, target = key
        descriptor = program['descriptor']
        is_ray = program_name == 'RayTracing'
        build.require(is_ray or (all(e['kind'] == 'UniformBuffer' for e in descriptor['interface']['entries']) and
                      len(descriptor['interface']['entries']) == len(descriptor.get('layouts', []))),
                      'Configuration', program['id'], 'runtime profile admits only verified uniform bindings')
        expected = ([{'location': 0, 'format': 'Float2'}, {'location': 1, 'format': 'Float4'}]
                    if is_ray and stage == 'vertex' else stage_inputs(program) if stage == 'vertex' else [])
        build.require(stage != 'vertex' or is_ray or expected in (STATIC_INPUTS, VISIBILITY_INPUTS),
                      'Configuration', program['id'], 'unsupported static/visibility vertex interface')
        build.require([{k: v for k, v in x.items() if k != 'name'} for x in descriptor['interface']['inputs']] == expected,
                      'Configuration', program['id'], 'static vertex/instance interface mismatch')
        artifact_name = Path(descriptor['artifact'])
        build.require(len(program['key']) == 64 and all(c in '0123456789abcdef' for c in program['key']) and
                      not artifact_name.is_absolute() and '..' not in artifact_name.parts,
                      'Package', 'PROGRAM_' + str(ordinal), 'invalid stage/artifact identity')
        artifact = build.read(path / 'programs' / program['key'] / artifact_name, 2 * 1024 * 1024)
        if target == 'spirv':
            build.require(len(artifact) % 4 == 0, 'Package', 'PROGRAM_' + str(ordinal), 'SPIR-V word size mismatch')
        else:
            artifact.decode('utf-8')
        entry = descriptor['entry']
        build.require(type(entry) is str and 0 < len(entry.encode()) <= MAX_ENTRY_BYTES and entry.isascii(),
                      'Package', program['id'], 'invalid stage entry name')
        names = _block_names(descriptor) if target == 'glsl' else []
        build.require(target != 'glsl' or [n[0] for n in names] == sorted(b['slot'] for b in _stage_bindings(program_name, stage, bindings)),
                      'Package', program['id'], 'GLSL ES uniform blocks differ from the stage bindings')
        artifacts[key] = {'ordinal': ordinal, 'descriptor': descriptor, 'bytes': artifact, 'entry': entry, 'blocks': names}
    # CPU-visible interface: the logical vertex/instance schema, fixed static inputs,
    # and the per-draw uniform schemas with every target layout that packs them.
    interface = {'version': 2, 'scene_shader': 'SceneShaderInputsV1', 'vertex_schema': vertex_schemas[0], 'inputs': [stage_inputs(record) for (name, stage, target), record in stages.items() if stage == 'vertex' and name != 'RayTracing'], 'uniform_schemas': uniform_interface}
    scene_versions = {r.get('scene_interface_version', 0) for r in package['recipes'] if r['technique'] == ray.TECHNIQUE}
    build.require(all(type(version) is int for version in scene_versions) and scene_versions <= {0, 1} and len(scene_versions) <= 1, 'Package', 'ray interface', 'inconsistent scene callback profile')
    has_visibility = any(p.get('Visibility', ABSENT) != ABSENT for p in passes)
    visibility_profiles = {r.get('visibility_buffer', False) for r in package['recipes'] if r['technique'] == ray.TECHNIQUE}
    has_ray = any(p['Program'] == 'RayTracing' for p in passes)
    build.require(not has_ray or visibility_profiles == {has_visibility}, 'Configuration', 'visibility',
                  'ray visibility inputs must match scene_interface.visibility_buffer')
    return {'visibility_buffer': has_visibility, 'scene_interface': scene_versions == {1}, 'path': path, 'package': package, 'plan': plan, 'resources': resources, 'passes': passes,
            'cameras': cameras, 'camera_targets': camera_targets, 'artifacts': artifacts,
            'uniform_values': value_count, 'bindings': bindings, 'blocks': blocks, 'fields': fields,
            'storage_buffers': storage_buffers, 'uniform_interface': uniform_interface,
            'interface_identity': build.identity(interface), 'generation': path.name}


def _uniform_constants(contents):
    """Named logical value offsets for host code, one per schema field."""
    lines, names = [], set()
    for entry in contents['uniform_interface']:
        offset = entry['value_offset']
        for field in entry['schema']['fields']:
            name = 'UNIFORM_VALUE_' + entry['schema']['id'].upper() + '_' + field['id'].upper()
            build.require(name not in names, 'Schema', name, 'generated uniform value name collision; rename the field')
            names.add(name)
            lines += [f'/// First logical float32 value of {entry["schema"]["id"]}.{field["id"]} '
                      f'({build.SHAPES[field["type"]][1]} components) in RenderGraphItem::Uniforms.',
                      f'inline constexpr foundation::usize {name} = {offset};']
            offset += build.SHAPES[field['type']][1]
    return lines


def _emit_runtime(path, destination, *, configuration='Default', name='DefaultGraph'):
    """Atomically emit a compiled header from a fully integrity-checked generation.

    The header has static artifact storage and neutral numeric plan records. It
    supports every admitted Metal/SPIR-V/WGSL/GLSL representation present in the
    package. Callers supply the installed renderer_composite recipe descriptions;
    no built-in source path/entry is guessed or duplicated. Existing destination
    is replaced only after the complete package and execution contract validate.
    """
    build.name(name, 'generated namespace')
    contents = _runtime_contents(path, configuration)
    plan, resources, passes, cameras = contents['plan'], contents['resources'], contents['passes'], contents['cameras']
    lines = ['#pragma once', '#include <ludus/foundation/base/compiler.h>',
             '#include <ludus/foundation/containers/static_array.hpp>',
             '#include <ludus/content/render_graph_format/runtime_package.h>',
             '#include <ludus/graphics/renderer/render_graph.hpp>', '', 'namespace ludus::render_graph_packages::' + name, '{',
             'namespace format = content::render_graph_format;', 'namespace renderer = graphics::renderer;', 'namespace rhi = graphics::rhi;',
             f'inline constexpr foundation::usize RESOURCE_COUNT = {len(resources)};',
             f'inline constexpr foundation::usize PASS_COUNT = {len(passes)};',
             f'inline constexpr foundation::usize CAMERA_COUNT = {len(cameras)};',
             '/// Logical float32 values each drawn RenderGraphItem supplies; zero when no technique binds uniforms.',
             f'inline constexpr foundation::usize UNIFORM_VALUE_COUNT = {contents["uniform_values"]};',
             f'inline constexpr foundation::usize UNIFORM_BINDING_COUNT = {len(contents["bindings"])};',
             f'inline constexpr foundation::usize UNIFORM_BLOCK_COUNT = {len(contents["blocks"])};',
             f'inline constexpr foundation::usize UNIFORM_FIELD_COUNT = {len(contents["fields"])};']
    pass_names = [p['id'] for p in plan['passes'] if p['kind'] in ('Raster', 'RayTracing')]
    for label in pass_names:
        build.require(0 < len(label.encode('utf-8')) <= MAX_PASS_NAME_BYTES and '\0' not in label,
                      'Capacity', label, 'pass debug name exceeds the runtime UTF-8 bound')
    lines += ['inline constexpr foundation::StaticArray<foundation::BorrowedView<const char>, PASS_COUNT> PASS_NAMES', '{', '    {']
    lines += ['        {' + json.dumps(label, ensure_ascii=True) + ', ' + str(len(label.encode('utf-8'))) + '},' for label in pass_names]
    lines += ['    },', '};']
    lines += [f'inline constexpr foundation::usize STORAGE_BUFFER_COUNT = {len(contents["storage_buffers"])};']
    lines += _uniform_constants(contents)
    kinds = {'Kind': 'Aspect', 'Program': 'Technique', 'ColorLoad': 'Load', 'DepthLoad': 'Load', 'ColorStore': 'Store',
             'DepthStore': 'Store', 'Stages': 'Visibility', 'Target': 'ShaderTarget'}
    tables = [('STORAGE_BUFFERS', contents['storage_buffers'], 'StorageBuffer', 'STORAGE_BUFFER_COUNT'), ('RESOURCES', resources, 'Resource', 'RESOURCE_COUNT'), ('PASSES', passes, 'Pass', 'PASS_COUNT'),
              ('UNIFORM_BINDINGS', contents['bindings'], 'UniformBinding', 'UNIFORM_BINDING_COUNT'),
              ('UNIFORM_BLOCKS', contents['blocks'], 'UniformBlock', 'UNIFORM_BLOCK_COUNT'),
              ('UNIFORM_FIELDS', contents['fields'], 'UniformField', 'UNIFORM_FIELD_COUNT')]
    for label, values, ty, count in tables:
        if not values:
            continue
        lines += [f'inline constexpr foundation::StaticArray<format::{ty}, {count}> {label}', '{', '    {']
        for value in values:
            cells = []
            for field, item in value.items():
                if field in ('schema', 'layouts'):
                    continue
                if field == 'Target':
                    cells.append('format::ShaderTarget::' + TARGET_NAMES[item])
                elif field == 'Program' and PROGRAM_CODES[item] >= LEGACY_PROGRAM_COUNT:
                    cells.append('static_cast<format::Technique>(' + str(PROGRAM_CODES[item]) + ')')
                elif isinstance(item, str):
                    cells.append('format::' + kinds[field] + '::' + item)
                else:
                    cells.append('format::ABSENT' if item == ABSENT else str(item))
            lines.append('        {' + ', '.join(cells) + '},')
        lines += ['    },', '};']
    lines += ['inline constexpr foundation::StaticArray<format::Camera, CAMERA_COUNT> CAMERAS', '{', '    {']
    lines += ['        {' + str(target) + '},' for target in contents['camera_targets']]
    view = lambda label, values: '{' + label + '.GetData(), ' + label + '.GetSize()}' if values else '{}'
    lines += ['    },', '};', '/// Return numeric plan and borrowed immutable record tables.',
              '[[nodiscard]] LUDUS_INLINE renderer::RenderGraphPlan MakePlan() noexcept', '{',
              '    return {{format::VERSION, RESOURCE_COUNT, PASS_COUNT, CAMERA_COUNT, UNIFORM_VALUE_COUNT,',
              '             UNIFORM_BINDING_COUNT, UNIFORM_BLOCK_COUNT, UNIFORM_FIELD_COUNT},',
              '        {RESOURCES.GetData(), RESOURCES.GetSize()}, {CAMERAS.GetData(), CAMERAS.GetSize()}, {PASSES.GetData(), PASSES.GetSize()},',
              '        ' + ', '.join(view(label, values) for label, values in
                                    [('UNIFORM_BINDINGS', contents['bindings']), ('UNIFORM_BLOCKS', contents['blocks']),
                                     ('UNIFORM_FIELDS', contents['fields'])]) + ', {PASS_NAMES.GetData(), PASS_NAMES.GetSize()}, ' + view('STORAGE_BUFFERS', contents['storage_buffers']) + '};', '}']
    identity = bytes.fromhex(contents['interface_identity'])
    lines += ['/// SHA-256 of the CPU-visible vertex/instance and uniform interface this host was compiled against (RG3).',
              '/// Runtime generations with another identity are rejected with RestartRequired.',
              'static_assert(renderer::SCENE_SHADER_INTERFACE_VERSION == 1);',
              'inline constexpr foundation::StaticArray<foundation::uint8, format::IDENTITY_BYTES> INTERFACE_IDENTITY', '{', '    {',
              '        ' + ', '.join(hex(b) for b in identity[:16]) + ',',
              '        ' + ', '.join(hex(b) for b in identity[16:]) + ',', '    },', '};']
    artifact_labels = {}
    for key, item in sorted(contents['artifacts'].items(), key=lambda x: x[1]['ordinal']):
        ordinal = item['ordinal']
        label = 'PROGRAM_' + str(ordinal)
        artifact_labels[key] = (label, item)
        artifact = item['bytes']
        if key[2] == 'spirv':
            words = struct.unpack('<' + 'I' * (len(artifact) // 4), artifact)
            lines += [f'inline constexpr foundation::usize {label}_WORD_COUNT = {len(words)};',
                      f'inline constexpr foundation::StaticArray<foundation::uint32, {label}_WORD_COUNT> {label}', '{', '    {']
            for i in range(0, len(words), 8):
                lines.append('        ' + ', '.join(hex(w) for w in words[i:i + 8]) + ',')
            lines += ['    },', '};']
        else:
            text = artifact.decode('utf-8')
            delimiter = 'RG' + str(ordinal)
            build.require(')' + delimiter + '"' not in text, 'Package', label, 'raw-string delimiter collision')
            if len(text.encode('utf-8')) > 65535:
                # Large linked ray programs exceed the portable C++ string-literal limit.
                # Character initializers preserve exact UTF-8 bytes without warning suppression.
                characters = ["'\\x%02x'" % byte for byte in text.encode('utf-8')] + ["'\\0'"]
                lines.append('inline constexpr char ' + label + '[] = {\n' +
                             '\n'.join(','.join(characters[start:start + 32]) + ','
                                       for start in range(0, len(characters), 32)) + '\n};')
            else:
                lines.append('inline constexpr char ' + label + '[] = R"' + delimiter + '(' + text + ')' + delimiter + '";')
    lines += ['/// Return cooked stage descriptions backed by immutable static storage.',
              '[[nodiscard]] LUDUS_INLINE renderer::RenderGraphPrograms MakePrograms(',
              '    const rhi::RasterShaderDescription& compositeVertex,',
              '    const rhi::RasterShaderDescription& compositeFragment) noexcept', '{', '    renderer::RenderGraphPrograms result;']
    if contents.get('scene_interface', False):
        lines.append('    result.HasSceneShaderInterface = true;')
    if contents.get('visibility_buffer', False):
        lines.append('    result.HasVisibilityBuffer = true;')
    for program_name in PROGRAM_CODES:
        for stage in (('compute', 'vertex', 'fragment') if program_name == 'RayTracing' else ('vertex', 'fragment')):
            member = program_member(program_name, stage)
            declaration = member if PROGRAM_CODES[program_name] < LEGACY_PROGRAM_COUNT else program_name + stage.title()
            keys = [key for key in artifact_labels if key[0] == program_name and key[1] == stage]
            if not keys:
                continue
            lines.append('    result.' + member + '.Artifact.Stage = rhi::ShaderStage::' + stage.title() + ';')
            if stage == 'vertex':
                input_values = [{'location': 0, 'format': 'Float2'}, {'location': 1, 'format': 'Float4'}] if program_name == 'RayTracing' else stage_inputs(contents['artifacts'][keys[0]])
                constant = declaration.upper() + '_INPUT_COUNT'
                lines += [f'    static constexpr foundation::usize {constant} = {len(input_values)};',
                          f'    static constexpr foundation::StaticArray<rhi::RasterShaderInput, {constant}> {declaration}Inputs', '{', '        {']
                lines += ['            {' + str(value['location']) + ', rhi::RasterVertexFormat::' + value['format'] + '},' for value in input_values]
                lines += ['        },', '    };', f'    result.{member}.Inputs = {declaration}Inputs.AsSpan();']
            if program_name == 'RayTracing':
                lines += ['    renderer::SetRenderGraphRayInterface(result.' + member + ', rhi::ShaderStage::' + stage.title() + ', result.HasVisibilityBuffer);']
            stage_bindings = _stage_bindings(program_name, stage, contents['bindings'])
            if program_name == 'RayTracing' and stage == 'compute':
                stage_bindings = [{'slot': entry['binding'], 'kind': entry['kind'], 'visibility': 'Compute', 'size': entry['size']}
                                  for entry in contents['artifacts'][keys[0]]['descriptor']['interface']['entries']]
            if stage_bindings:
                constant = declaration.upper() + '_BINDING_COUNT'
                lines += [f'    static constexpr foundation::usize {constant} = {len(stage_bindings)};',
                          f'    static constexpr foundation::StaticArray<rhi::RasterBinding, {constant}> {declaration}Bindings', '{', '        {']
                lines += ['            {' + str(b['slot']) + ', rhi::RasterBindingKind::' + b.get('kind', 'UniformBuffer') + ', rhi::RasterVisibility::' +
                          b['visibility'] + ', ' + str(b['size']) + '},' for b in stage_bindings]
                lines += ['        },', '    };', f'    result.{member}.Bindings = {declaration}Bindings.AsSpan();']
            for key in sorted(keys, key=lambda k: TARGET_CODES[k[2]]):
                label, item = artifact_labels[key]
                field = TARGET_FIELDS[key[2]]
                lines += [f'    result.{member}.Artifact.{field} = ' + (label + '.AsSpan();' if key[2] == 'spirv' else label + ';'),
                          f'    result.{member}.Artifact.{field}Entry = ' + json.dumps(item['entry']) + ';']
                for slot, block in item['blocks']:
                    lines.append(f'    result.{member}.UniformBlocks[{slot}] = ' + json.dumps(block) + ';')
    lines += ['    result.CompositeVertex = compositeVertex;', '    result.CompositeFragment = compositeFragment;', '    return result;', '}', '}']
    destination = Path(destination).absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    build.atomic(destination, ('\n'.join(lines) + '\n').encode())
    return {'header': str(destination), 'stage': 'RG2', 'configuration': configuration,
            'resources': len(resources), 'passes': len(passes), 'cameras': cameras,
            'uniform_values': contents['uniform_values'], 'uniform_bindings': len(contents['bindings']),
            'interface_identity': contents['interface_identity'],
            'scene': next(p['scene'] for p in plan['passes'] if p['kind'] in ('Raster', 'RayTracing'))}


def encode_runtime_package(contents):
    """Serialize lowered contents field by field in little-endian order.

    Layout (runtime_package.h): header of ten uint32 values (magic, version,
    resource/pass/camera counts, uniform value/binding/block/field counts and the
    stage count) plus interface and generation identities; resource, camera, pass,
    uniform binding, block and field records; then stage records with their static
    inputs, GLSL ES block names, entry name and artifact bytes, each byte string
    zero-padded to four bytes.
    """
    u32 = lambda *values: struct.pack('<' + 'I' * len(values), *values)
    aspects = {'Color': 0, 'Depth': 1}
    loads, stores = {'Clear': 0, 'Load': 1, 'Discard': 2}, {'Store': 0, 'Discard': 1}
    stages = sorted(contents['artifacts'].items(), key=lambda item: item[1]['ordinal'])
    data = bytearray(u32(PACKAGE_MAGIC, PACKAGE_VERSION, len(contents['resources']), len(contents['passes']),
                         len(contents['camera_targets']), contents['uniform_values'], len(contents['bindings']),
                         len(contents['blocks']), len(contents['fields']), len(stages)))
    data += bytes.fromhex(contents['interface_identity']) + bytes.fromhex(contents['generation'])
    for r in contents['resources']:
        data += u32(r['Width'], r['Height'], aspects[r['Kind']], r['Backing'], r['IsSurface'], r['IsExported'],
                    r.get('ViewNumerator', 0), r.get('ViewDenominator', 1))
    for target in contents['camera_targets']:
        data += u32(target)
    for p in contents['passes']:
        data += u32(PROGRAM_CODES[p['Program']], p['Camera'], p['Color'], p['Depth'], loads[p['ColorLoad']],
                    stores[p['ColorStore']], loads[p['DepthLoad']], stores[p['DepthStore']], p['AllowsRepeatedCoverage'], p.get('SceneData', graph.scene_bundle.ALL_DATA), p.get('Visibility', ABSENT), p.get('VisibilityDepth', ABSENT))
    for b in contents['bindings']:
        data += u32(PROGRAM_CODES[b['Program']], b['Slot'], VISIBILITY[b['Stages']], b['ValueOffset'], b['ValueCount'])
    for b in contents['blocks']:
        data += u32(b['Binding'], TARGET_CODES[b['Target']], b['Size'])
    for f in contents['fields']:
        data += u32(f['Binding'], TARGET_CODES[f['Target']], f['ValueOffset'], f['Components'], f['ByteOffset'])
    for (program_name, stage, target), item in stages:
        inputs = ([{'location': 0, 'format': 'Float2'}, {'location': 1, 'format': 'Float4'}]
                  if program_name == 'RayTracing' and stage == 'vertex' else stage_inputs(item) if stage == 'vertex' else [])
        entry = item['entry'].encode()
        stage_role = 11 if program_name == 'RayTracing' and stage == 'compute' and contents.get('scene_interface', False) else role(program_name, stage)
        data += u32(stage_role, TARGET_CODES[target], len(inputs), len(entry), len(item['bytes']),
                    len(item['blocks']))
        for value in inputs:
            data += u32(value['location'], FORMAT_CODES[value['format']])
        for slot, block in item['blocks']:
            encoded = block.encode()
            data += u32(slot, len(encoded)) + encoded + bytes(-len(encoded) % 4)
        for blob in (entry, item['bytes']):
            data += blob + bytes(-len(blob) % 4)
    storage = contents.get('storage_buffers', [])
    data += u32(len(storage))
    for buffer in storage:
        data += u32(buffer['Slot'], buffer['Stride'], buffer['Count'], buffer['IsReadOnly'])
    for operation in contents['plan']['passes']:
        if operation['kind'] not in ('Raster', 'RayTracing'):
            continue
        label = operation['id'].encode('utf-8')
        build.require(0 < len(label) <= MAX_PASS_NAME_BYTES and b'\0' not in label,
                      'Capacity', operation['id'], 'pass debug name exceeds the runtime UTF-8 bound')
        data += u32(len(label)) + label + bytes(-len(label) % 4)
    build.require(len(data) <= MAX_RUNTIME_BYTES, 'Capacity', 'runtime package', 'runtime generation exceeds 16 MiB')
    return bytes(data)


RESOURCE_NAMES_SUFFIX = '.resources.json'
RESOURCE_NAMES_VERSION = 1


def resource_names_path(runtime_package):
    """Sidecar beside a runtime package naming its plan resources for editor inspection."""
    runtime_package = Path(runtime_package)
    return runtime_package.with_name(runtime_package.name + RESOURCE_NAMES_SUFFIX)


def encode_resource_names(contents):
    """Name each plan resource in record order, for tools that inspect a generation.

    The runtime package carries numeric records only. Record i is the i-th authored
    resource in sorted expanded-ID order (`<graph id>/<node id>`, with subgraph
    instance IDs between them), matching lower_plan. The generation identity ties
    the names to exactly one package; runtime activation never reads this file.
    """
    plan = contents['plan']
    names = sorted(name for name, resource in plan['resources'].items() if resource['type'] != 'Buffer')
    entries = [{'name': name, 'type': plan['resources'][name]['type'], 'format': plan['resources'][name]['format'],
                'extent': list(plan['resources'][name]['extent'])} for name in names]
    # Expanded operation IDs let a canvas node (Raster, Subgraph, Export, Present) find what it wrote or read.
    operations = []
    for operation in plan['passes']:
        if operation['kind'] in ('Raster', 'RayTracing'):
            attachments = {aspect: {'resource': value['resource'], 'load': value['load'], 'store': value['store']}
                           for aspect, value in sorted(operation['attachments'].items())}
        else:
            attachments = {'texture': {'resource': operation['resource']}}
        operations.append({'id': operation['id'], 'kind': operation['kind'], 'source': operation['source'],
                           'attachments': attachments})
    cameras = [{'id': camera['id'], 'target': camera['target']} for camera in plan['cameras'] if camera['target']]
    document = {'version': RESOURCE_NAMES_VERSION, 'generation': contents['generation'], 'resources': entries,
                'operations': operations, 'cameras': cameras}
    return (json.dumps(document, indent=2, sort_keys=True) + '\n').encode()


def _cook_runtime(path, destination, *, configuration='Default'):
    contents = _runtime_contents(path, configuration)
    data = encode_runtime_package(contents)
    destination = Path(destination).absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Names first: a reader that sees the new package always finds its matching names.
    build.atomic(resource_names_path(destination), encode_resource_names(contents))
    build.atomic(destination, data)
    return {'runtime_package': str(destination), 'resource_names': str(resource_names_path(destination)),
            'stage': 'RG3', 'configuration': configuration,
            'generation': contents['generation'], 'interface_identity': contents['interface_identity'],
            'bytes': len(data), 'resources': len(contents['resources']), 'passes': len(contents['passes']),
            'uniform_values': contents['uniform_values'], 'uniform_bindings': len(contents['bindings']),
            'stages': len(contents['artifacts']), 'profiles': sorted({key[2] for key in contents['artifacts']})}


def cook_runtime(path, destination, *, configuration='Default'):
    """Write a runtime-loadable generation for atomic activation (RG3).

    Contains the same plan records, uniform tables, stage artifacts and interface
    identity as emit_runtime's header, without a source parser or shader compiler
    at runtime. The destination is replaced atomically only after full package
    verification.
    """
    try:
        return _cook_runtime(path, destination, configuration=configuration)
    except (KeyError, TypeError, ValueError, IndexError, UnicodeError, struct.error) as error:
        build.fail('Package', str(path), 'malformed execution metadata: ' + str(error))


def emit_runtime(path, destination, *, configuration='Default', name='DefaultGraph'):
    """Emit only a complete qualified plan; preserve destination on malformed input.

    See the shared numeric format and Renderer contract. No compiler/GPU calls.
    """
    try:
        return _emit_runtime(path, destination, configuration=configuration, name=name)
    except (KeyError, TypeError, ValueError, IndexError, UnicodeError, struct.error) as error:
        build.fail('Package', str(path), 'malformed execution metadata: ' + str(error))
