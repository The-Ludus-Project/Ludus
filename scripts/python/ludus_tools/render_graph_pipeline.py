"""Version-2 pipeline node authoring and checked legacy raster conversion.

Pipeline records own interfaces, program references, state and invocation. This
module never creates device pipelines; the qualified RG1/RG2 profile must admit
all authored choices before execution. See the canonical render graph design.
"""
from __future__ import annotations

from copy import deepcopy

KINDS = ('GraphicsPipeline', 'ComputePipeline', 'RayTracingPipeline')
RESOURCE_TYPES = ('ColorTexture', 'DepthTexture', 'Buffer')
INPUT_TYPES = ('Scene', 'View', *RESOURCE_TYPES, 'AccelerationStructure',
               'Float32', 'Float2', 'Float3', 'Float4', 'UInt32')
MAX_PORTS = 8
MAX_SHADERS = 16
MAX_DIMENSION = 65535
STAGES = {
    'GraphicsPipeline': ('vertex', 'fragment', 'geometry', 'hull', 'domain'),
    'ComputePipeline': ('compute',),
    'RayTracingPipeline': ('ray_generation', 'miss', 'closest_hit', 'any_hit', 'intersection', 'callable'),
}
GRAPHICS_STATE = {
    'topology': ('TriangleList', 'TriangleStrip', 'LineList', 'PointList'),
    'cull': ('None', 'Back', 'Front'),
    'front': ('Clockwise', 'CounterClockwise'),
    'depth_compare': ('Never', 'Less', 'LessEqual', 'Equal', 'Greater', 'GreaterEqual', 'Always'),
    'blend': ('Opaque', 'Alpha', 'Additive'),
}


def new_node(kind, identity):
    """Return an independent empty-port draft. Callers validate kind and identity."""
    state = {'group_size': [1, 1, 1]}
    invocation = {'dimensions': [1, 1, 1]}
    if kind == 'GraphicsPipeline':
        state = {'topology': 'TriangleList', 'cull': 'None', 'front': 'CounterClockwise',
                 'depth_compare': 'Less', 'depth_write': True, 'blend': 'Opaque', 'samples': 1}
        invocation = {'mode': 'ColorAndDepth', 'vertex_layout': 'RendererStatic',
                      'material': 'DefaultOpaque', 'scene': 'scene', 'view': 'view'}
    elif kind == 'RayTracingPipeline':
        state = {}  # The selected ray profile supplies its limits unless explicitly overridden.
        invocation = {'dimensions': 'ColorTarget'}
    return {'id': identity, 'kind': kind, 'inputs': [], 'outputs': [], 'shaders': [],
            'state': state, 'invocation': invocation}


def _check_field(check, value, field, where, graph, *arguments):
    """Retain diagnostic codes/node locations while identifying the exact editable field."""
    try:
        check(value, *arguments, where)
    except graph.ToolingError as error:
        detail = error.message.removeprefix(where + ': ')
        graph._fail(error.code.removeprefix('RenderGraph'), where,
                    f"{field} has invalid value {repr(value)[:160]}: {detail}. "
                    f"Fix {field} in Properties or the source JSON.")


def validate(node, where, graph):
    """Check bounded source shape, allowing incomplete saved drafts, without mutation."""
    graph._keys(node, ('id', 'kind', 'inputs', 'outputs', 'shaders', 'state', 'invocation'), where)
    _check_field(graph._identifier, node['id'], 'Node ID', where, graph)
    graph._require(node['id'] != 'input', 'Schema', where, 'input is reserved')
    graph._enum(node['kind'], KINDS, where)
    ports = {}
    targets = {}
    for direction in ('inputs', 'outputs'):
        _check_field(graph._list, node[direction], direction, where, graph, MAX_PORTS)
        identities = set()
        for port in node[direction]:
            fields = ('id', 'type', 'required') if direction == 'inputs' else (
                'id', 'type', 'target', 'format', 'load', 'store')
            _check_field(graph._keys, port, f'{direction} port record', where, graph, fields)
            _check_field(graph._identifier, port['id'], f'{direction} port ID', where, graph)
            graph._require(port['id'] not in identities, 'DuplicateIdentity', where,
                           f"{direction} port '{port['id']}' is declared more than once. "
                           'Rename or remove the duplicate port in Properties.')
            identities.add(port['id'])
            _check_field(graph._enum, port['type'], f"{direction} port '{port['id']}' Type",
                         where, graph, INPUT_TYPES if direction == 'inputs' else RESOURCE_TYPES)
            if direction == 'inputs':
                _check_field(graph._boolean, port['required'], f"Input '{port['id']}' Required", where, graph)
                ports[port['id']] = port['type']
            else:
                _check_field(graph._identifier, port['target'], f"Output '{port['id']}' paired input ID", where, graph)
                target_type = ports.get(port['target'])
                compatible = [identity for identity, kind in ports.items() if kind == port['type']]
                if target_type != port['type']:
                    problem = (f"input '{port['target']}' does not exist" if target_type is None else
                               f"input '{port['target']}' has type {target_type}")
                    suggestion = (f"Pair it with a {port['type']} input: {', '.join(compatible)}."
                                  if compatible else
                                  f"Add a writable {port['type']} resource in Properties and connect its input.")
                    graph._fail('TypeMismatch', where,
                                f"Output '{port['id']}' ({port['type']}) cannot update its resource: "
                                f"{problem}. {suggestion} In JSON, set this output's target to the paired input ID.")
                graph._require(port['target'] not in targets, 'MultipleWriters', where,
                               f"Outputs '{targets.get(port['target'])}' and '{port['id']}' both update input '{port['target']}'. "
                               'Give each writable resource a separate input, or remove the duplicate output. '
                               'Use separate pipeline nodes for sequential writes.')
                targets[port['target']] = port['id']
                formats = {'ColorTexture': ('RGBA8Unorm',), 'DepthTexture': ('Depth32Float',), 'Buffer': ('Bytes',)}
                graph._require(port['format'] in formats[port['type']], 'Schema', where,
                               f"Output '{port['id']}' ({port['type']}) has format '{port['format']}'. "
                               f"Set Format to {', '.join(formats[port['type']])} in Writable Resources.")
                graph._require(port['load'] in ('Clear', 'Load', 'Discard'), 'Schema', where,
                               f"Output '{port['id']}' has unknown Load operation '{port['load']}'. "
                               'Choose Clear, Load or Discard in Writable Resources.')
                graph._require(port['store'] in ('Store', 'Discard'), 'Schema', where,
                               f"Output '{port['id']}' has unknown Store operation '{port['store']}'. "
                               'Choose Store or Discard in Writable Resources.')
    _check_field(graph._list, node['shaders'], 'Shaders', where, graph, MAX_SHADERS)
    identities = set()
    stages = set()
    for shader in node['shaders']:
        _check_field(graph._keys, shader, 'Shader record', where, graph, ('id', 'stage', 'program'))
        _check_field(graph._identifier, shader['id'], 'Shader ID', where, graph)
        _check_field(graph._identifier, shader['program'], f"Shader '{shader['id']}' Program ID", where, graph)
        _check_field(graph._enum, shader['stage'], f"Shader '{shader['id']}' Stage", where, graph, STAGES[node['kind']])
        graph._require(shader['id'] not in identities, 'DuplicateIdentity', where,
                       f"Shader ID '{shader['id']}' is declared twice. Rename or remove the duplicate row in Shaders.")
        graph._require(node['kind'] == 'RayTracingPipeline' or shader['stage'] not in stages,
                       'ShaderInterface', where,
                       f"Shader stage '{shader['stage']}' is assigned more than once. Remove the duplicate stage row in Shaders.")
        identities.add(shader['id'])
        stages.add(shader['stage'])
    state, invocation = node['state'], node['invocation']
    if node['kind'] == 'GraphicsPipeline':
        _check_field(graph._keys, state, 'Pipeline State', where, graph, (*GRAPHICS_STATE, 'depth_write', 'samples'))
        for field, choices in GRAPHICS_STATE.items():
            _check_field(graph._enum, state[field], f'Pipeline State → {field}', where, graph, choices)
        _check_field(graph._boolean, state['depth_write'], 'Pipeline State → depth_write', where, graph)
        _check_field(graph._integer, state['samples'], 'Pipeline State → samples', where, graph, 1, 64)
        _check_field(graph._keys, invocation, 'Invocation', where, graph, ('mode', 'vertex_layout', 'material', 'scene', 'view'))
        _check_field(graph._enum, invocation['mode'], 'Invocation → mode', where, graph, graph.MODES)
        graph._identifier(invocation['vertex_layout'], where)
        graph._identifier(invocation['material'], where)
        for name in ('scene', 'view'):
            _check_field(graph._identifier, invocation[name], f'Invocation → {name}', where, graph)
    elif node['kind'] == 'ComputePipeline':
        _check_field(graph._keys, state, 'Pipeline State', where, graph, ('group_size',))
        _dimensions(state['group_size'], where, graph, 'Pipeline State → group_size')
        _check_field(graph._keys, invocation, 'Invocation', where, graph, ('dimensions',))
        _dimensions(invocation['dimensions'], where, graph, 'Invocation → dimensions')
    else:
        graph._keys(state, (), where, ('max_recursion', 'payload_bytes', 'attribute_bytes'))
        if 'max_recursion' in state:
            _check_field(graph._integer, state['max_recursion'], 'Pipeline State → max_recursion', where, graph, 1, 31)
        for field in ('payload_bytes', 'attribute_bytes'):
            if field in state:
                _check_field(graph._integer, state[field], f'Pipeline State → {field} (bytes)', where, graph, 0, 4096)
        _check_field(graph._keys, invocation, 'Invocation', where, graph, ('dimensions',))
        if invocation['dimensions'] != 'ColorTarget':
            _dimensions(invocation['dimensions'], where, graph, 'Invocation → dimensions')


def _dimensions(values, where, graph, field):
    graph._require(type(values) is list and len(values) == 3, 'Schema', where,
                   f'{field} must contain exactly three dimensions [x, y, z]. Set all three values in Properties.')
    for axis, value in zip(('x', 'y', 'z'), values):
        _check_field(graph._integer, value, f'{field}.{axis}', where, graph, 1, MAX_DIMENSION)


def migrate_raster(node, technique):
    """Copy a legacy node using its explicit manifest technique and unique recipe.

    Callers must reject missing or ambiguous mappings. No source is changed; IDs,
    attachment endpoints and legacy load/store operations are preserved.
    """
    result = new_node('GraphicsPipeline', node['id'])
    result['invocation'].update({key: node[key] for key in ('mode', 'vertex_layout', 'material')})
    result['inputs'] = [{'id': 'scene', 'type': 'Scene', 'required': True},
                        {'id': 'view', 'type': 'View', 'required': True}]
    for aspect in ('depth', 'color') if node['mode'] == 'ColorAndDepth' else ('depth',):
        kind = 'ColorTexture' if aspect == 'color' else 'DepthTexture'
        result['inputs'].append({'id': aspect, 'type': kind, 'required': True})
        result['outputs'].append({'id': aspect, 'type': kind, 'target': aspect,
                                  'format': 'RGBA8Unorm' if aspect == 'color' else 'Depth32Float',
                                  'load': node[aspect + '_load'], 'store': node[aspect + '_store']})
    result['shaders'] = [{'id': stage, 'stage': stage, 'program': technique[stage]} for stage in ('vertex', 'fragment')]
    recipe = technique['recipes'][0]
    result['state'] = {key: deepcopy(recipe[key]) for key in result['state']}
    return result


def lower_graphics(node, where, graph):
    """Admit the existing raster execution contract without dropping authored settings.

    Returns a legacy-shaped lowering record and port mappings. Shader and recipe
    equality with the actual build manifest is checked independently by RG1.
    """
    invocation = node['invocation']
    graph._require(invocation['material'] in graph.MATERIALS and invocation['vertex_layout'] == 'RendererStatic',
                   'Capability', where, 'graphics invocation is outside the RendererStatic profile')
    stages = {shader['stage'] for shader in node['shaders']}
    graph._require({'vertex', 'fragment'} <= stages, 'IncompletePipeline', where,
                   f"GraphicsPipeline is missing shader stages: {', '.join(sorted({'vertex', 'fragment'} - stages))}. "
                   'Add these stages in Shaders and assign each a build-manifest program ID.')
    graph._require(stages <= {'vertex', 'fragment'}, 'Capability', where,
                   'optional graphics stages are not admitted by the current graph build profile')
    input_types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {'scene': invocation['scene'], 'view': invocation['view']}
    for key, kind in (('scene', 'Scene'), ('view', 'View')):
        graph._require(input_types.get(mappings[key]) == kind, 'IncompletePipeline', where,
                       f"Invocation '{key}' names '{mappings[key]}', which is not a declared {kind} input. "
                       f"Add a {kind} Read Input and select its ID in Invocation → {key}.")
    record = {'id': node['id'], 'kind': 'Raster', **{key: invocation[key] for key in ('mode', 'vertex_layout', 'material')}}
    outputs = {}
    for aspect in ('depth', 'color') if invocation['mode'] == 'ColorAndDepth' else ('depth',):
        kind = 'ColorTexture' if aspect == 'color' else 'DepthTexture'
        choices = [port for port in node['outputs'] if port['type'] == kind]
        graph._require(len(choices) == 1, 'Capability', where,
                       f'current graphics profile requires exactly one {kind} output')
        port = choices[0]
        mappings[aspect] = port['target']
        outputs[aspect] = port['id']
        record[aspect + '_load'] = port['load']
        record[aspect + '_store'] = port['store']
    graph._require(len(outputs) == len(node['outputs']) and set(mappings.values()) == set(input_types),
                   'Capability', where, 'additional resources require a separately qualified execution profile')
    return record, mappings, outputs


def migrate_document(document, manifest):
    """Return a version-2 copy with every legacy Raster explicitly converted.

    The supplied build manifest is fully validated. Multiple recipes require an
    author choice and reject without modifying either input. Imports remain
    unchanged; convert each referenced asset explicitly. This function does not
    write sources or synthesize shader programs.
    """
    from . import render_graph as graph
    from . import render_graph_build as build
    graph.canonical_asset(document)
    _, _, techniques, _ = build.validate_manifest(manifest)
    candidate = deepcopy(document)
    for configuration in candidate['configurations']:
        for index, node in enumerate(configuration['nodes']):
            if node['kind'] != 'Raster':
                continue
            identity = graph.MATERIALS[node['material']] + '.' + node['mode']
            technique = techniques.get(identity)
            graph._require(technique is not None and len(technique['recipes']) == 1,
                           'Migration', configuration['id'] + '/' + node['id'],
                           'conversion requires one explicitly declared technique with one recipe')
            configuration['nodes'][index] = migrate_raster(node, technique)
    candidate['version'] = 2
    graph.canonical_asset(candidate)
    return candidate
