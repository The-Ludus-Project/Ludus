"""Scene compute passes: ComputePipeline nodes that trace the World with native ray queries.

A ComputePipeline with a Scene input lowers to a member of the ordered ray sequence whose
callback set is one authored kernel. It keeps the RayTracing pass kind, the numeric
Technique::RayTracing record and the persistent ray wrapper; only the offline plan names its
source node kind. See .kiro/specs/compute-ray-tracing/design.md, D1-D3.
"""
from . import render_graph_ray as ray

SOURCE_KIND = 'ComputePipeline'
# The persistent ray wrapper's fixed workgroup (LUDUS_GPU_PREVIEW_GROUP_WIDTH x 1 x 1).
GROUP_SIZE = [64, 1, 1]
# C03: float3 Name(SceneContext scene, RayLaunch launch, inout RayQueryContext queries).
SIGNATURE = ('RayLaunch', 'Radiance')


def lower(node, where, graph):
    """Validate a scene compute node and return its RayTracing record and port mappings."""
    stages = [shader['stage'] for shader in node['shaders']]
    graph._require(stages == [ray.COMPUTE_STAGE], 'IncompletePipeline', where,
                   'a scene compute pass requires exactly one compute program',
                   field='shaders', expected=ray.COMPUTE_STAGE, actual=', '.join(stages) or 'no stages',
                   repair='Assign one build-manifest compute program in Shaders.')
    group_size = node['state'].get('group_size', GROUP_SIZE)
    graph._require(group_size == GROUP_SIZE, 'Capability', where,
                   f'scene compute passes run on the renderer\'s fixed {GROUP_SIZE} workgroup; '
                   f'the authored group_size {group_size} is not supported',
                   field='state.group_size', expected=GROUP_SIZE, actual=group_size,
                   repair='Clear group_size so Pipeline State is automatic (state: {}).')
    mappings, outputs, color = ray.lower_ports(node, where, graph, 'scene compute', 'scene compute')
    record = {'id': node['id'], 'kind': 'RayTracing', 'color_load': color['load'], 'color_store': color['store']}
    return record, mappings, outputs


def validate_program(program, build):
    """Admit only the C03 kernel signature; the generated wrapper owns every binding."""
    build.require((program['input'], program['output']) == SIGNATURE, 'Schema', program['id'],
                  'scene compute programs use input RayLaunch and output Radiance: '
                  'float3 Name(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)')
    build.require(program['bindings'] == [], 'Schema', program['id'],
                  'scene compute programs declare no bindings; the ray wrapper supplies scene resources and slots')
