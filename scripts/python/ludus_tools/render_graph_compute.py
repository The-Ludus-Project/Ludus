"""Scene compute passes: kernels or shared ray callbacks using selected World traversal.

A ComputePipeline with a Scene input lowers to a member of the ordered ray sequence whose
callback set is one authored kernel or the shared ray-generation/miss/closest-hit set. It keeps the RayTracing pass kind, the numeric
Technique::RayTracing record and the persistent ray wrapper; only the offline plan names its
source node kind. See .kiro/specs/compute-ray-tracing/design.md, D1-D3.
"""
from . import render_graph_ray as ray

SOURCE_KIND = 'ComputePipeline'
# The persistent ray wrapper's fixed workgroup (LUDUS_GPU_PREVIEW_GROUP_WIDTH x 1 x 1).
GROUP_SIZE = ray.DEFAULT_WORKGROUP_SIZE
# C03: float3 Name(SceneContext scene, RayLaunch launch, inout RayQueryContext queries).
SIGNATURE = ('RayLaunch', 'Radiance')


def lower(node, where, graph):
    """Validate a scene compute node and return its RayTracing record and port mappings."""
    stages = [shader['stage'] for shader in node['shaders']]
    has_callbacks = any(stage in ray.STAGES for stage in stages)
    if has_callbacks:
        graph._require(len(stages) == len(ray.STAGES) and set(stages) == set(ray.STAGES),
                       'IncompletePipeline', where, 'scene compute callbacks require ray_generation, miss and closest_hit without a compute kernel',
                       field='shaders', expected=', '.join(ray.STAGES), actual=', '.join(stages),
                       repair='Assign the same three callback programs used by a ray pipeline, or use one compute kernel.')
    graph._require(has_callbacks or stages == [ray.COMPUTE_STAGE], 'IncompletePipeline', where,
                   'a scene compute pass requires exactly one compute program',
                   field='shaders', expected=ray.COMPUTE_STAGE, actual=', '.join(stages) or 'no stages',
                   repair='Assign one build-manifest compute program in Shaders.')
    group_size = node['state'].get('group_size', GROUP_SIZE)
    graph._require(group_size in ray.SUPPORTED_WORKGROUP_SIZES, 'Capability', where,
                   f'scene compute passes support {ray.SUPPORTED_WORKGROUP_SIZES}; '
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
