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
        state = {'max_recursion': 1, 'payload_bytes': 0, 'attribute_bytes': 0}
    return {'id': identity, 'kind': kind, 'inputs': [], 'outputs': [], 'shaders': [],
            'state': state, 'invocation': invocation}


def validate(node, where, graph):
    """Check bounded source shape, allowing incomplete saved drafts, without mutation."""
    graph._keys(node, ('id', 'kind', 'inputs', 'outputs', 'shaders', 'state', 'invocation'), where)
    graph._identifier(node['id'], where)
    graph._require(node['id'] != 'input', 'Schema', where, 'input is reserved')
    graph._enum(node['kind'], KINDS, where)
    ports = {}
    for direction in ('inputs', 'outputs'):
        graph._list(node[direction], MAX_PORTS, where)
        identities = set()
        for port in node[direction]:
            fields = ('id', 'type', 'required') if direction == 'inputs' else (
                'id', 'type', 'target', 'format', 'load', 'store')
            graph._keys(port, fields, where)
            graph._identifier(port['id'], where)
            graph._require(port['id'] not in identities, 'DuplicateIdentity', where, port['id'])
            identities.add(port['id'])
            graph._enum(port['type'], INPUT_TYPES if direction == 'inputs' else RESOURCE_TYPES, where)
            if direction == 'inputs':
                graph._boolean(port['required'], where)
                ports[port['id']] = port['type']
            else:
                graph._identifier(port['target'], where)
                graph._require(ports.get(port['target']) == port['type'], 'TypeMismatch', where,
                               'output target must name a same-type input holding the previous resource version')
                formats = {'ColorTexture': ('RGBA8Unorm',), 'DepthTexture': ('Depth32Float',), 'Buffer': ('Bytes',)}
                graph._enum(port['format'], formats[port['type']], where)
                graph._enum(port['load'], ('Clear', 'Load', 'Discard'), where)
                graph._enum(port['store'], ('Store', 'Discard'), where)
    graph._list(node['shaders'], MAX_SHADERS, where)
    identities = set()
    stages = set()
    for shader in node['shaders']:
        graph._keys(shader, ('id', 'stage', 'program'), where)
        graph._identifier(shader['id'], where)
        graph._identifier(shader['program'], where)
        graph._enum(shader['stage'], STAGES[node['kind']], where)
        graph._require(shader['id'] not in identities, 'DuplicateIdentity', where, shader['id'])
        graph._require(node['kind'] == 'RayTracingPipeline' or shader['stage'] not in stages,
                       'ShaderInterface', where, 'only one program per graphics/compute stage')
        identities.add(shader['id'])
        stages.add(shader['stage'])
    state, invocation = node['state'], node['invocation']
    if node['kind'] == 'GraphicsPipeline':
        graph._keys(state, (*GRAPHICS_STATE, 'depth_write', 'samples'), where)
        for field, choices in GRAPHICS_STATE.items():
            graph._enum(state[field], choices, where)
        graph._boolean(state['depth_write'], where)
        graph._integer(state['samples'], 1, 64, where)
        graph._keys(invocation, ('mode', 'vertex_layout', 'material', 'scene', 'view'), where)
        graph._enum(invocation['mode'], graph.MODES, where)
        graph._identifier(invocation['vertex_layout'], where)
        graph._identifier(invocation['material'], where)
        for name in ('scene', 'view'):
            graph._identifier(invocation[name], where)
    elif node['kind'] == 'ComputePipeline':
        graph._keys(state, ('group_size',), where)
        _dimensions(state['group_size'], where, graph)
        graph._keys(invocation, ('dimensions',), where)
        _dimensions(invocation['dimensions'], where, graph)
    else:
        graph._keys(state, ('max_recursion', 'payload_bytes', 'attribute_bytes'), where)
        graph._integer(state['max_recursion'], 1, 31, where)
        for field in ('payload_bytes', 'attribute_bytes'):
            graph._integer(state[field], 0, 4096, where)
        graph._keys(invocation, ('dimensions',), where)
        _dimensions(invocation['dimensions'], where, graph)


def _dimensions(values, where, graph):
    graph._require(type(values) is list and len(values) == 3, 'Schema', where, 'three dimensions required')
    for value in values:
        graph._integer(value, 1, MAX_DIMENSION, where)


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
                   'graphics requires authored vertex and fragment programs in the current profile')
    graph._require(stages <= {'vertex', 'fragment'}, 'Capability', where,
                   'optional graphics stages are not admitted by the current graph build profile')
    input_types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {'scene': invocation['scene'], 'view': invocation['view']}
    for key, kind in (('scene', 'Scene'), ('view', 'View')):
        graph._require(input_types.get(mappings[key]) == kind, 'IncompletePipeline', where,
                       f'{key} invocation must name a declared {kind} input')
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
