"""Triangle ray pipelines using selectable traversal and shared authored functions.

The renderer owns native acceleration structures and the packed shading tables. The graph owns
stage assignment, launch extent and publication. No CPU transport is selected.
"""
from copy import deepcopy
from pathlib import Path
import re
from . import render_graph_buffers as buffers
from . import render_graph_ray_invocation as invocation
from . import render_graph_metal_ray as metal_ray

TECHNIQUE = 'RendererTriangles.RayTracing'
STAGES = ('ray_generation', 'miss', 'closest_hit')
MAX_RAY_PASSES = 4
DEFAULT_WORKGROUP_SIZE = [64, 1, 1]
SUPPORTED_WORKGROUP_SIZES = (DEFAULT_WORKGROUP_SIZE, [8, 8, 1])
MAX_BUFFERS = 2
BUFFER_WORD_BYTES = 16
MAX_BUFFER_PIXEL_BYTES = 4096
MAX_BUFFER_BYTES = 256 * 1024 * 1024
PASS_INDEX_SHIFT = 8


COMPUTE_STAGE = 'compute'
# A sequence made only of scene compute kernels has no hit or miss callback. These names
# fill the template positions; persistent_wrapper replaces each call with a pass-index
# dispatch that has no compute case, so a compute kernel's TraceAndShade latches HasFailed.
UNUSED_COMPUTE_CALLBACKS = {'miss': 'GraphComputeMiss', 'closest_hit': 'GraphComputeClosestHit'}


# Strategy tags are cooked into callback variants so the same programs can be used
# by different passes without sharing a traversal choice or changing their sources.
TRIANGLE_LOOP_TAG = '@TriangleLoop'
COMPUTE_BVH_TAG = '@ComputeBvh'
HARDWARE_PIPELINE_TAG = '@RayTracing'
CUSTOM_TRAVERSAL_TAG = '@Custom:'
# Runtime RayTraversal names, in pass-word code order (LUDUS_GRAPH_TRAVERSAL_* in
# renderer_gpu_preview_layout.h). Cooking and the linked kernel's per-pass table read this.
TRAVERSAL_CODES = ('NativeQuery', 'TriangleLoop', 'Custom', 'HardwarePipeline', 'ComputeBvh')
STRATEGY_MACROS = {'NativeQuery': 'LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY',
                   'TriangleLoop': 'LUDUS_GRAPH_TRAVERSAL_TRIANGLE_LOOP',
                   'Custom': 'LUDUS_GRAPH_TRAVERSAL_CUSTOM',
                   'ComputeBvh': 'LUDUS_GRAPH_TRAVERSAL_COMPUTE_BVH'}


def get_custom_traversal_source(variant):
    """Return the custom query source identity, if this variant owns one."""
    return variant[-1][len(CUSTOM_TRAVERSAL_TAG):] if variant[-1].startswith(CUSTOM_TRAVERSAL_TAG) else None


def get_traversal(variant):
    """Return the runtime RayTraversal name a cooked callback/strategy variant selects."""
    if variant[-1] == HARDWARE_PIPELINE_TAG:
        return 'HardwarePipeline'
    if get_custom_traversal_source(variant):
        return 'Custom'
    return {TRIANGLE_LOOP_TAG: 'TriangleLoop', COMPUTE_BVH_TAG: 'ComputeBvh'}.get(variant[-1], 'NativeQuery')


def callback_programs(variant):
    """Return only authored program IDs from a cooked callback/strategy variant."""
    return tuple(variant[:-1]) if variant[-1].startswith('@') else tuple(variant)


def program_tuple(node):
    """Return the callback IDs and optional software traversal tag for one pass."""
    assignments = {shader['stage']: shader['program'] for shader in node['shaders']}
    has_kernel = COMPUTE_STAGE in assignments
    variant = (assignments[COMPUTE_STAGE],) if has_kernel else tuple(assignments[stage] for stage in STAGES)
    if invocation.resolve_type(node) == invocation.HARDWARE_PIPELINE:
        return variant + (HARDWARE_PIPELINE_TAG,)
    traversal = invocation.resolve_traversal(node)
    if traversal == invocation.TRIANGLE_LOOP:
        variant += (TRIANGLE_LOOP_TAG,)
    elif traversal == invocation.COMPUTE_BVH:
        variant += (COMPUTE_BVH_TAG,)
    elif traversal == invocation.CUSTOM_QUERY:
        variant += (CUSTOM_TRAVERSAL_TAG + node['invocation']['traversal_source'],)
    return variant


def get_callback_variant(node):
    """Hardware stages and inline-query compute share callbacks and native query semantics.

    Execution type is recorded per pass and selects a distinct pipeline. It must
    not manufacture an extra callback set or require persistent buffers merely
    because two configurations select different native invocation modes.
    """
    variant = program_tuple(node)
    return variant[:-1] if variant[-1] == HARDWARE_PIPELINE_TAG else variant


def requires_native_queries(plan):
    """Whether any selected ray pass explicitly needs native ray traversal."""
    return requires_native_pipeline(plan) or any(operation['kind'] == 'RayTracing' and
               invocation.resolve_type(operation['pipeline']) == invocation.COMPUTE_PIPELINE and
               invocation.resolve_traversal(operation['pipeline']) == invocation.NATIVE_QUERY
               for operation in plan['passes'])


def is_compute_variant(variant):
    """Scene compute kernels occupy only the ray-generation position of the sequence."""
    return len(callback_programs(variant)) == 1


def requires_native_pipeline(plan):
    """Whether an authored ray pass requires native stage/function-table execution."""
    return any(operation['kind'] == 'RayTracing' and
               invocation.resolve_type(operation['pipeline']) == invocation.HARDWARE_PIPELINE
               for operation in plan['passes'])


def select_callbacks(programs, variant):
    """Map a variant to wrapper stage programs, filling a compute kernel's unused callbacks."""
    if not is_compute_variant(variant):
        return {stage: programs[identity] for stage, identity in zip(STAGES, callback_programs(variant))}
    kernel = programs[variant[0]]
    selected = {'ray_generation': kernel}
    for stage, function in UNUSED_COMPUTE_CALLBACKS.items():
        selected[stage] = dict(kernel, function=function, output='ScenePayload')
    return selected

# Thanks to Microsoft, DirectX Raytracing Functional Spec, “Intersection Attributes”:
# https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#intersection-attributes
# Triangle intersections supply float2 barycentrics. This native-query profile
# retains that contract, but uses shader-local ScenePayload rather than a DXR payload.
STATE = {'max_recursion': 1, 'payload_bytes': 0, 'attribute_bytes': 8}
VISIBILITY_SLOT = buffers.FIRST_SLOT + buffers.MAX_BUFFERS
# Ray uniform bytes (LUDUS_GPU_PREVIEW_PARAMETERS_BYTES), including the version 28 tile region.
PARAMETERS_BYTES = 240
# Diagnostic ray path capture buffer (LUDUS_RAY_PATH_CAPTURE_BINDING); uint32 words.
RAY_PATH_CAPTURE_SLOT = 10
RAY_PATH_CAPTURE_STRIDE = 4
# Renderer-owned LTC table (LUDUS_GRAPH_LTC_TABLE_BINDING); float4 entries. See docs/development/ltc-table.md.
LTC_TABLE_SLOT = 13
LTC_TABLE_STRIDE = 16
MAX_LOCAL_PAYLOAD_BYTES = 4096
PAYLOAD_FIELD_ALIGNMENT_BYTES = 16
DEFAULT_PAYLOAD = '_ludus_ray/scene_payload.slang'
# One owner for build admission and the editor controls. Editor CMake reads this
# data through a module import at configure time; no Python interpreter enters the UI.
SCENE_QUERY_LIMITS = {
    'MIN_SURFACE_QUERIES': 1,
    'MAX_SURFACE_QUERIES': 8,
    'MIN_VISIBILITY_QUERIES': 0,
    'MAX_VISIBILITY_QUERIES': 16,
    # A capture profile (scene_interface.capture_visibility_queries) may grant a frame capture's private
    # graph more shadow queries per invocation than interactive graphs; see
    # docs/architecture/render-graph-frame-capture.md#phase-3-capture-budget-decision.
    'MAX_CAPTURE_VISIBILITY_QUERIES': 256,
    'MIN_LINE_QUERIES': 0,
    'MAX_LINE_QUERIES': 8,
}
# scene_interface.line_traversal: how FindVisibleLineIntervals finds occluders in every pass, whatever
# the pass's ray traversal. The packed BVH is the default; the linear scan remains for comparison.
LINE_TRAVERSALS = ('ComputeBvh', 'TriangleLoop')
DEFAULT_LINE_TRAVERSAL = 'ComputeBvh'

COMPUTE_ID = 'GraphRayCompute'
VISIBILITY_COMPUTE_ID = 'GraphVisibilityRayCompute'
SOFTWARE_COMPUTE_ID = 'GraphSoftwareRayCompute'
SOFTWARE_VISIBILITY_COMPUTE_ID = 'GraphSoftwareVisibilityRayCompute'
COMPUTE_IDS = (COMPUTE_ID, VISIBILITY_COMPUTE_ID, SOFTWARE_COMPUTE_ID, SOFTWARE_VISIBILITY_COMPUTE_ID)
IMAGE_VERTEX_ID = 'GraphRayImageVertex'
IMAGE_INPUTS = [{'location': 0, 'format': 'Float2'}]
IMAGE_FRAGMENT_ID = 'GraphRayImageFragment'
# Files are installed by their owning renderer/math/RHI targets beside the helper.
HELPERS = ('scene_data.h', 'renderer_gpu_preview_layout.h', 'renderer_surface_functions.slang', 'material_scattering.slang', 'material.h', 'recipes.json',
           'native_metal_ray.slang', 'metal_ray_pipeline_layout.h',
           'ray_triangle_loop.slang', 'ray_line_visibility.slang', 'ray_bvh_nodes.slang', 'ray_bvh_traversal.slang', 'renderer_reference_layout.h', 'lighting_layout.h', 'math.slang', 'raster_bindings.slang', 'scene_shader.slang', 'scene_payload.slang', 'scene_shader_layout.h', 'ray_shader.slang', 'visibility_buffer.slang')
# Staged and included only for scene_interface.polygonal_light_sampling: FoundationMath's projected-solid-angle,
# LTC, weighted-reservoir and shading-normal primitives and the renderer's LTC table lookup, in include order, so
# packages that do not request them keep their snapshot and cache identity. The table itself is a renderer-bound buffer.
POLYGONAL_LIGHT_HELPERS = ('projected_solid_angle.slang', 'linearly_transformed_cosine.slang', 'weighted_reservoir.slang',
                           'shading_normal.slang', 'ltc_table.slang', 'ltc_table_layout.h')
POLYGONAL_LIGHT_INCLUDES = ('projected_solid_angle.slang', 'linearly_transformed_cosine.slang', 'weighted_reservoir.slang',
                            'shading_normal.slang', 'ltc_table.slang')


def resolve_state(state):
    """Resolve validated optional overrides without changing authored source."""
    return {**STATE, **state}


def lower(node, where, graph):
    assignments = {shader['stage']: shader['program'] for shader in node['shaders']}
    graph._require(set(assignments) == set(STAGES) and len(node['shaders']) == len(STAGES),
                   'IncompletePipeline', where, 'triangle ray pipelines require one ray_generation, miss and closest_hit program',
                   field='shaders', expected=', '.join(STAGES), actual=', '.join(sorted(assignments)) or 'no stages',
                   repair='Add the missing stages in Shaders and assign a build-manifest program to each.')
    graph._require(resolve_state(node['state']) == STATE, 'Capability', where,
                   'triangle ray profile derives recursion=1, native payload=0 bytes and triangle barycentrics=8 bytes. '
                   'Use automatic Pipeline State (state: {}) or matching advanced overrides. '
                   'Customize ScenePayload in the scene interface; custom intersection attributes require an unsupported procedural profile',
                   field='state', expected=STATE, actual=resolve_state(node['state']),
                   repair='Reset advanced ray state overrides to automatic.')
    mappings, outputs, color = lower_ports(node, where, graph, 'ray', 'triangle ray')
    record = {'id': node['id'], 'kind': 'RayTracing', 'color_load': color['load'], 'color_store': color['store']}
    return record, mappings, outputs


def lower_ports(node, where, graph, noun, profile):
    """Check the shared C04 port contract of ray and scene compute passes.

    Returns the input mappings, the output mappings and the color output port. The
    noun ('ray', 'scene compute') and profile name only change diagnostic wording.
    """
    types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {}
    for aspect, kind in (('scene', 'Scene'), ('view', 'View')):
        choices = [identity for identity, value in types.items() if value == kind]
        graph._require(len(choices) == 1, 'IncompletePipeline', where, f'exactly one {kind} input required',
                       field='inputs', expected=f'one {kind} input', actual=f'{len(choices)} inputs',
                       repair=f'Add or retain exactly one {kind} input in the pipeline editor.')
        mappings[aspect] = choices[0]
    storage = {name for name, kind in types.items() if kind == 'Buffer'}
    graph._require(len(storage) <= buffers.MAX_BUFFERS, 'Capacity', where, 'at most two Buffer inputs supported')
    mappings.update({'buffer:' + name: name for name in storage})
    colors = [port for port in node['outputs'] if port['type'] == 'ColorTexture']
    graph._require(len(colors) == 1, 'Capability', where, f'{noun} pipelines require one color output')
    mappings['color'] = colors[0]['target']
    graph._require(types.get(mappings['color']) == 'ColorTexture', 'Capability', where, f'{noun} output requires color input')
    color_reads = [identity for identity, kind in types.items() if kind == 'ColorTexture' and identity != mappings['color']]
    depth_reads = [identity for identity, kind in types.items() if kind == 'DepthTexture']
    graph._require((len(color_reads), len(depth_reads)) in ((0, 0), (1, 1)), 'Capability', where,
                   f'{noun} visibility input requires one read-only color and one depth texture; input declarations: ' +
                   ', '.join(f"'{identity}' ({types[identity]})" for identity in color_reads + depth_reads) +
                   '. Repair corresponding interface inputs and parent wires.')
    if color_reads:
        mappings['visibility'] = color_reads[0]
        mappings['visibility_depth'] = depth_reads[0]
    unsupported = sorted(set(types) - set(mappings.values()))
    descriptions = ', '.join(f"'{identity}' ({types[identity]})" for identity in unsupported)
    graph._require(not unsupported, 'Capability', where,
                   f"Unsupported {noun} pipeline inputs: {descriptions}. This {profile} profile accepts only "
                   'Scene, View, ColorTexture, paired visibility/depth textures and up to two Buffers. Remove the extra input declarations and their wires; '
                   'if exposed by a subgraph, also remove the matching interface inputs and parent wires.',
                   port=unsupported[0] if unsupported else None, direction='input',
                   expected='Scene, View, ColorTexture', actual=descriptions,
                   repair='Remove unsupported pipeline inputs and their wires; also update exposed subgraph interfaces.')
    colors = [port for port in node['outputs'] if port['type'] == 'ColorTexture']
    graph._require(len(colors) == 1, 'Capability', where, f'{profile} profile requires one color output')
    output = colors[0]
    graph._require(output['target'] == mappings['color'] and output['load'] == 'Clear' and output['store'] == 'Store',
                   'Capability', where, f'{noun} output must Clear/Store its ColorTexture target')
    outputs = {'color': output['id']}
    for port in node['outputs']:
        if port is output:
            continue
        graph._require(port['type'] == 'Buffer' and port['target'] in storage and
                       port['load'] == 'Load' and port['store'] == 'Store', 'Capability', where,
                       'writable structured buffers require Load/Store on a paired Buffer input')
        outputs['buffer:' + port['target']] = port['id']
    return mappings, outputs, output


def validate_program(program, build):
    legacy = {'ray_generation': ('RayLaunch', 'Ray'), 'miss': ('Ray', 'Color'),
              'closest_hit': ('RayHit', 'Color')}
    scene = {'ray_generation': ('RayLaunch', 'Radiance'), 'miss': ('Ray', 'ScenePayload'),
             'closest_hit': ('SurfaceHit', 'ScenePayload')}
    signature = (program['input'], program['output'])
    build.require(signature in (legacy[program['stage']], scene[program['stage']]),
                  'Schema', program['id'], 'ray function interface differs from declared profile')


def migrate_legacy_scene_sources(sources, snapshot):
    """Lift old editor bridge callbacks and shared types into the pipeline wrapper.

    Match only the complete generated bridge grammar. Preserve authored files and
    snapshot hashes; overrides apply solely to the compiler's private source tree.
    In particular a replacement callback may include the bridge that once included
    that callback, without being parsed twice or needing an authored include guard.
    """
    declaration = ('struct RayHit { Ray IncomingRay; float3 PositionWorld; float DistanceWorld; '
                   'float3 NormalWorld; uint Primitive; float2 Barycentric; float4 Color; };')
    expected = re.sub(r'\s+', '', '#pragma once\n' + declaration)
    overrides, callbacks = {}, set()
    dependencies = {path for source in sources for path in snapshot.shader_closure(source)}
    for path in sorted(dependencies):
        text = snapshot.files[path].decode('utf-8')
        if re.search(r'^// Compatibility inputs for the original authored callbacks\.\r?$', text, re.M) is None:
            continue
        code = re.sub(r'/\*.*?\*/|//[^\n]*', lambda match: re.sub(r'[^\n]', ' ', match[0]), text, flags=re.S)
        includes = list(re.finditer(r'^[ \t]*#include "([^"\n]+)"[ \t]*\r?$', code, re.M))
        if not includes:
            continue
        # Remove only include rows, retaining their newlines for diagnostic locations.
        shared = text
        for match in reversed(includes):
            shared = shared[:match.start()] + shared[match.end():]
        tokens = re.sub(r'/\*.*?\*/|//[^\n]*', '', shared, flags=re.S)
        if re.sub(r'\s+', '', tokens) != expected:
            continue
        # The wrapper supplies the renderer-owned RayHit once, including when
        # multiple old bridges or a mixture of legacy/scene callbacks is selected.
        shared = re.sub(r'\bstruct\s+RayHit\s*\{[^}]*\}\s*;',
                        lambda match: re.sub(r'[^\r\n]', ' ', match[0]), shared)
        overrides[path] = shared
        callbacks.update((Path(path).parent / match[1]).as_posix() for match in includes)
    return {'sources': overrides, 'callbacks': sorted(callbacks)}


def group_profiles(plans, technique, build):
    """Separate ordinary and visibility callback sets without changing their shared payload."""
    groups = {}
    explicit_workgroups = {tuple(operation['pipeline']['state']['group_size'])
                           for plan in plans for operation in plan['passes']
                           if operation['kind'] == 'RayTracing' and
                           'group_size' in operation['pipeline']['state']}
    build.require(len(explicit_workgroups) <= 1, 'Capability', 'ray workgroups',
                  'all scene compute passes in one shader bundle must use one workgroup shape')
    workgroup = list(next(iter(explicit_workgroups))) if explicit_workgroups else list(DEFAULT_WORKGROUP_SIZE)
    for plan in plans:
        operations = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        if not operations:
            continue
        visibility = {'visibility' in operation for operation in operations}
        build.require(len(visibility) == 1, 'Capability', plan['configuration'],
                      'ordinary and visibility ray passes require separate configurations')
        has_visibility = visibility.pop()
        build.require(not has_visibility or technique.get('scene_interface', {}).get('visibility_buffer', False),
                      'Configuration', plan['configuration'],
                      'visibility inputs require scene_interface.visibility_buffer')
        has_native_queries = requires_native_queries(plan) or requires_native_pipeline(plan)
        group = groups.setdefault((has_visibility, has_native_queries), {'variants': set(), 'persistent': False, 'structured_buffers': False})
        group['variants'].update(get_callback_variant(operation['pipeline']) for operation in operations)
        group['structured_buffers'] |= any(operation.get('buffers') for operation in operations)
        # GraphBuffer0/1 and GetGraphPassIndex exist only in the persistent wrapper, so a
        # single scene compute pass needs it as well (compute ray tracing design, section 3.3).
        has_shader_traversal = any(invocation.resolve_traversal(operation['pipeline']) in
                                   (invocation.TRIANGLE_LOOP, invocation.CUSTOM_QUERY, invocation.COMPUTE_BVH)
                                   for operation in operations)
        group['native_pipeline'] = group.get('native_pipeline', False) or requires_native_pipeline(plan)
        group['persistent'] |= has_shader_traversal or len(operations) > 1 or any(
            resource['type'] == 'Buffer' and 'stride' not in resource
            for resource in plan['resources'].values()) or any(
                operation['pipeline']['kind'] == 'ComputePipeline' for operation in operations)
    if not groups:
        build.require(all(stage in technique for stage in STAGES), 'Configuration', TECHNIQUE,
                      'a ray technique without default callbacks requires a RayTracingPipeline or scene compute pass')
        groups[(technique.get('scene_interface', {}).get('visibility_buffer', False), True)] = {
            'variants': {tuple(technique[stage] for stage in STAGES)}, 'persistent': False, 'structured_buffers': False}
    result = []
    for (has_visibility, has_native_queries), group in sorted(groups.items()):
        build.require(not (group['persistent'] or len(group['variants']) > 1) or not group['structured_buffers'],
                      'Capability', 'ray storage',
                      'ordered ray passes use shared Buffer nodes; StructuredBuffer nodes remain private to one ray job')
        result.append({'workgroup': workgroup, 'visibility_buffer': has_visibility, 'native_ray_queries': has_native_queries,
                       'native_pipeline': group.get('native_pipeline', False),
                       'variants': sorted(group['variants']),
                       'persistent': group['persistent'] or len(group['variants']) > 1,
                       'compute': ((VISIBILITY_COMPUTE_ID if has_visibility and (False, has_native_queries) in groups else COMPUTE_ID) if has_native_queries else
                                   (SOFTWARE_VISIBILITY_COMPUTE_ID if has_visibility and (False, has_native_queries) in groups else SOFTWARE_COMPUTE_ID))})
    return result


def expand_programs(programs, technique, snapshot, helper, build, variants=None, persistent=False, schemas=None,
                    compute_identity=COMPUTE_ID, native_ray_queries=True, ray_path_capture=False,
                    native_ray_pipeline=False, workgroup=None):
    """Capture the engine kernel and all three functions in one linked compute entry.

    ray_path_capture links the diagnostic LUDUS_RAY_PATH_CAPTURE variant of the same
    source snapshot. The flag is part of the program, so it joins the stage cache identity.
    """
    selected = select_callbacks(programs, variants[0]) if variants else {stage: programs[technique[stage]] for stage in STAGES}
    storage = buffers.merge_bindings(selected, build)
    build.require(not persistent or not storage, 'Capability', TECHNIQUE, 'ordered ray passes use Buffer storage; StructuredBuffer remains the single-job profile')
    storage_source = buffers.declarations(storage, schemas or {}, build)
    recipes = build.document(build.read(helper / 'recipes.json'), 'renderer recipes')
    compute_recipe = recipes['renderer_gpu_preview']
    image_recipe = recipes['renderer_gpu_preview_image']
    polygonal_light_helpers = POLYGONAL_LIGHT_HELPERS if technique.get('scene_interface', {}).get(
        'polygonal_light_sampling', False) else ()
    for filename in (*HELPERS, *polygonal_light_helpers, compute_recipe['source'], image_recipe['source']):
        data = build.read(helper / filename)
        relative = '_ludus_ray/' + filename
        if relative in snapshot.files:
            build.require(snapshot.external.get(relative) == helper / filename and snapshot.files[relative] == data,
                          'Source', relative, 'reserved engine ray helper path')
            continue
        snapshot.files[relative] = data
        snapshot.external[relative] = helper / filename
        snapshot.total += len(data)
    wrapper = '''float3 EvaluateGraphRay(uint pixel);
#define LUDUS_RENDER_GRAPH_RAY_PIPELINE 1
#include "_ludus_ray/renderer_gpu_preview.slang"
static const float RAY_DIRECTION_SQUARED_TOLERANCE = 0.00001;
#include "_ludus_ray/ray_shader.slang"
'''
    wrapper += storage_source
    for source in sorted({program['source'] for program in selected.values()}):
        wrapper += '#include "' + source + '"\n'
    wrapper += '''float3 EvaluateGraphRay(uint pixel)
{
    RayLaunch launch;
    launch.PositionPixel = uint2(pixel % Dimensions.x, pixel / Dimensions.x);
    launch.ExtentPixel = Dimensions.xy;
    float2 positionUv = (float2(launch.PositionPixel) + 0.5) / float2(launch.ExtentPixel);
    float4 nearPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 0, 1));
    float4 farPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 1, 1));
    nearPositionWorld /= nearPositionWorld.w;
    farPositionWorld /= farPositionWorld.w;
    launch.CameraRay.OriginWorld = CameraPositionWorld.w != 0 ? nearPositionWorld.xyz : CameraPositionWorld.xyz;
    launch.CameraRay.DirectionWorld = NormalizeOrZero(farPositionWorld.xyz - launch.CameraRay.OriginWorld);
    launch.CameraRay.MinDistanceWorld = CameraPositionWorld.w != 0 ? 0 : dot(nearPositionWorld.xyz - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    launch.CameraRay.MaxDistanceWorld = dot(farPositionWorld.xyz - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    Ray ray = RAY_GENERATION(launch);
    if (any(isfinite(ray.OriginWorld) == false) || any(isfinite(ray.DirectionWorld) == false) ||
        isfinite(ray.MinDistanceWorld) == false || isfinite(ray.MaxDistanceWorld) == false ||
        ray.MinDistanceWorld < 0 || ray.MaxDistanceWorld < ray.MinDistanceWorld ||
        abs(dot(ray.DirectionWorld, ray.DirectionWorld) - 1) > RAY_DIRECTION_SQUARED_TOLERANCE)
        return float3(-1);
    Hit hit = Trace(ray.OriginWorld, ray.DirectionWorld, ray.MinDistanceWorld, ray.MaxDistanceWorld);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
        return float3(-1);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE)
        return RAY_MISS(ray);
    RayHit input;
    input.IncomingRay = ray;
    input.PositionWorld = PositionWorld(hit.Triangle, hit.Barycentric);
    input.DistanceWorld = hit.DistanceWorld;
    input.NormalWorld = SceneValue(hit.Triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + LUDUS_GPU_PREVIEW_GEOMETRIC_NORMAL_WORD).xyz;
    input.Primitive = hit.Triangle;
    input.Barycentric = hit.Barycentric;
    input.Color = Surface(hit.Triangle, 0);
    return RAY_CLOSEST_HIT(input);
}
[numthreads(LUDUS_GPU_PREVIEW_GROUP_WIDTH, 1, 1)] void rgEntry(uint3 positionPixel : SV_DispatchThreadID)
{
    ComputeMain(positionPixel);
}
'''
    for token, stage in (('RAY_GENERATION', 'ray_generation'), ('RAY_MISS', 'miss'), ('RAY_CLOSEST_HIT', 'closest_hit')):
        wrapper = wrapper.replace(token, selected[stage]['function'])
    wrapper = wrapper.replace('_ludus_ray/renderer_gpu_preview.slang', '_ludus_ray/' + compute_recipe['source'])
    wrapper = wrapper.replace('ComputeMain(positionPixel)', compute_recipe['compute'] + '(positionPixel)')
    compatibility = {'sources': {}, 'callbacks': []}
    if technique.get('scene_interface') is not None:
        sources = {programs[identity]['source'] for variant in variants for identity in callback_programs(variant)} if variants else {
            program['source'] for program in selected.values()}
        compatibility = migrate_legacy_scene_sources(sources, snapshot)
        wrapper = scene_wrapper(selected, technique['scene_interface'], snapshot, build, compute_recipe, storage_source,
                                compatibility)
    build.require(not native_ray_pipeline or not technique.get('scene_interface', {}).get('polygonal_light_sampling', False),
                  'Capability', TECHNIQUE, 'native ray stages cannot bind the LTC table of polygonal_light_sampling; '
                  'use a scene compute pass')
    if persistent or native_ray_pipeline:
        build.require(technique.get('scene_interface') is not None, 'Capability', TECHNIQUE,
                      'native ray stages, ordered ray passes and persistent buffers require the scene callback interface')
        wrapper = persistent_wrapper(programs, technique, variants, snapshot, build, compute_recipe, compatibility,
                                     native_ray_queries, native_ray_pipeline, persistent, storage_source)
    workgroup = list(workgroup or DEFAULT_WORKGROUP_SIZE)
    build.require(workgroup in SUPPORTED_WORKGROUP_SIZES, 'Capability', 'ray workgroups', 'unsupported ray workgroup')
    wrapper = wrapper.replace('[numthreads(LUDUS_GPU_PREVIEW_GROUP_WIDTH, 1, 1)]',
                              '[numthreads(' + ', '.join(str(value) for value in workgroup) + ')]')
    if not native_ray_queries:
        wrapper = '#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1\n' + wrapper
    if ray_path_capture:
        wrapper = '#define LUDUS_RAY_PATH_CAPTURE 1\n' + wrapper
    result = {identity: deepcopy(program) for identity, program in programs.items()
              if program['stage'] not in (*STAGES, COMPUTE_STAGE)}
    result[compute_identity] = {'id': compute_identity, 'stage': 'compute', 'source': selected['ray_generation']['source'],
                          'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': wrapper, 'ray_workgroup': workgroup, 'native_ray_queries': native_ray_queries, 'software_ray_queries': not native_ray_queries, 'native_ray_profile': True, 'persistent_ray_profile': persistent, 'storage_bindings': storage, 'storage_schemas': {binding['schema']: schemas[binding['schema']] for binding in storage}, 'visibility_buffer': technique.get('scene_interface', {}).get('visibility_buffer', False), 'ltc_table': technique.get('scene_interface', {}).get('polygonal_light_sampling', False)}
    if compatibility['sources']:
        result[compute_identity]['source_overrides'] = compatibility['sources']
    if native_ray_pipeline:
        result[compute_identity]['native_ray_pipeline'] = True
    if ray_path_capture:
        result[compute_identity]['ray_path_capture'] = True
    for identity, stage, function in ((IMAGE_VERTEX_ID, 'vertex', image_recipe['vertex']), (IMAGE_FRAGMENT_ID, 'fragment', image_recipe['fragment'])):
        # Use the same renderer-owned image recipe; its interface is validated by the runtime.
        source = '#include "_ludus_ray/' + image_recipe['source'] + '"\n'
        suffix = 'VertexOutput rgEntry(VertexInput value) { return ' + function + '(value); }' if stage == 'vertex' else 'float4 rgEntry(VertexOutput value) : SV_Target0 { return ' + function + '(value); }'
        source += '[shader("' + stage + '")]\n' + suffix + '\n'
        result[identity] = {'id': identity, 'stage': stage, 'source': selected['ray_generation']['source'],
                            'function': function, 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': source, 'native_ray_profile': True, 'fragment_storage': stage == 'fragment'}
    build.require(not set(result) & {program['id'] for program in selected.values()}, 'Schema', 'ray programs', 'reserved linked program identity')
    build.require(snapshot.total <= build.MAX_BYTES and len(snapshot.files) <= build.MAX_FILES, 'Capacity', 'ray sources', 'source snapshot limits exceeded')
    return result


def capture_program_closure(program, snapshot, build):
    """Capture literal roots in our generated wrapper and their bounded includes.

    The image recipe has no dependency on authored ray callbacks. Engine helpers
    remain conservative: shared C++/Slang headers contain native-only includes
    outside the authored include grammar. All captured engine helpers participate
    in each ray key; authored roots use Snapshot's normal validated closure.
    """
    roots = re.findall(r'^\s*#include "([^"\n]+)"', program['ray_wrapper'], re.M)
    build.require(roots, 'Source', program['id'], 'generated ray wrapper has no source roots')
    engine = set(snapshot.external)
    authored = {path for root in roots if root not in engine for path in snapshot.shader_closure(root)}
    return sorted(engine | authored)


def check_interface(program, result, build):
    """Validate the frozen renderer ABI from independently reflected artifacts."""
    interface = result['interface']
    entries = sorted(interface['entries'], key=lambda value: value['binding'])
    if program['id'] in COMPUTE_IDS or program.get('native_ray_pipeline'):
        expected = [(0, 'UniformBuffer', PARAMETERS_BYTES), (1, 'StorageRead', 16), (2, 'StorageReadWrite', 16),
                    (3, 'StorageReadWrite', 16), (4, 'StorageRead', 16), (5, 'RayScene', 0)]
        if not program.get('native_ray_queries', True):
            expected = [entry for entry in expected if entry[1] != 'RayScene']
        if program.get('persistent_ray_profile'):
            expected += [(6, 'StorageReadWrite', 16), (7, 'StorageReadWrite', 16)]
        build.require(interface.get('workgroup') == program.get('ray_workgroup', DEFAULT_WORKGROUP_SIZE) and interface['inputs'] == [] and
                      [(entry['binding'], entry['kind'], entry['size']) for entry in entries if program.get('persistent_ray_profile') or entry['binding'] < buffers.FIRST_SLOT or entry['binding'] >= VISIBILITY_SLOT] ==
                      expected + ([(VISIBILITY_SLOT, 'Texture2D', 0), (VISIBILITY_SLOT + 1, 'TextureDepth2D', 0)] if program.get('visibility_buffer', False) else []) +
                      ([(RAY_PATH_CAPTURE_SLOT, 'StorageReadWrite', RAY_PATH_CAPTURE_STRIDE)] if program.get('ray_path_capture', False) else []) +
                      # Reflection drops the table when no callback reads it; the renderer then binds nothing.
                      ([(LTC_TABLE_SLOT, 'StorageRead', LTC_TABLE_STRIDE)] if program.get('ltc_table', False) and
                       any(entry['binding'] == LTC_TABLE_SLOT for entry in entries) else []),
                      'ShaderInterface', program['id'], 'ray compute reflection differs from the selected renderer ABI')
        declared = program.get('storage_bindings', [])
        extra = [entry for entry in entries if buffers.FIRST_SLOT <= entry['binding'] < VISIBILITY_SLOT and not program.get('persistent_ray_profile')]
        build.require(len(extra) == len(declared) and all(
            entry['binding'] == binding['slot'] and entry['name'] == binding['id'] and
            entry['kind'] == ('StorageRead' if binding['access'] == 'Read' else 'StorageReadWrite') and
            0 < entry['size'] <= buffers.MAX_STRIDE for entry, binding in zip(extra, declared)),
            'ShaderInterface', program['id'], 'structured buffer reflection differs from declared ray bindings')
        for entry, binding in zip(extra, declared):
            schema = program['storage_schemas'][binding['schema']]
            expected = [{'name': field['id'], 'scalar': 'uint32' if field['type'].startswith('UInt') else
                         'int32' if field['type'].startswith('Int') else 'float32',
                         'width': build.STORAGE_SHAPES[field['type']][1]} for field in schema['fields']]
            reflected = [{key: field[key] for key in ('name', 'scalar', 'width')} for field in entry.get('fields', [])]
            build.require(reflected == expected, 'ShaderInterface', binding['id'],
                          'structured buffer fields differ from the declared Storage schema')
    elif program['id'] == IMAGE_VERTEX_ID:
        build.require(entries == [] and
                      [{key: value for key, value in item.items() if key != 'name'} for item in interface['inputs']] == IMAGE_INPUTS,
                      'ShaderInterface', program['id'], 'ray image vertex interface differs from renderer ABI')
    else:
        build.require([(entry['binding'], entry['kind'], entry['size']) for entry in entries] ==
                      [(0, 'UniformBuffer', 16), (1, 'StorageRead', 16)] and interface['inputs'] == [],
                      'ShaderInterface', program['id'], 'ray image fragment resources differ from renderer ABI')


# The visibility counterpart of TraceAndShade: the raster first hit replaces the camera-ray query and
# runs the same per-pass closest-hit or miss callback. The callback calls keep TraceAndShade's
# spelling so persistent_wrapper's pass dispatch rewrites both functions.
VISIBILITY_SHADE_DECLARATION = ('uint LoadVisibilityAndShade(SceneContext scene, inout RayQueryContext queries, '
                                'RayLaunch launch, inout ScenePayload payload);\n')
VISIBILITY_SHADE = '''uint LoadVisibilityAndShade(SceneContext scene, inout RayQueryContext queries, RayLaunch launch, inout ScenePayload payload)
{
    SurfaceHit hit;
    Ray ray = launch.CameraRay;
    uint status = LoadVisibilitySurface(scene, launch, queries, hit);
    if (status == SCENE_QUERY_SURFACE)
        RAY_CLOSEST_HIT(scene, hit, queries, payload);
    else if (status == SCENE_QUERY_MISS)
        RAY_MISS(scene, ray, queries, payload);
    return queries.HasFailed ? SCENE_QUERY_FAILURE : status;
}
'''


def scene_wrapper(selected, interface, snapshot, build, recipe, storage_source='', compatibility=None):
    """Link ordinary callbacks; the user raygen is the only sample/integrator driver."""
    payload = interface.get('payload', DEFAULT_PAYLOAD)
    if 'payload' in interface:
        snapshot.shader_closure(payload)
    validate_payload(snapshot.files[payload], interface.get('local_payload_bytes'), build)
    wrapper = '''float3 EvaluateGraphRay(uint pixel);
#define LUDUS_RENDER_GRAPH_RAY_PIPELINE 1
#define LUDUS_SCENE_RAY_INTERFACE 1
#include "_ludus_ray/renderer_gpu_preview.slang"
#include "_ludus_ray/scene_shader.slang"
'''
    if interface.get('line_traversal', DEFAULT_LINE_TRAVERSAL) == 'TriangleLoop':
        # Read by ray_line_visibility.slang; the default BVH walk adds nothing, keeping wrapper bytes.
        wrapper = '#define LUDUS_SCENE_LINE_TRIANGLE_LOOP 1\n' + wrapper
    wrapper += storage_source
    compatibility = compatibility or {'sources': {}, 'callbacks': []}
    if interface.get('visibility_buffer', False):
        wrapper += '#define LUDUS_RENDER_GRAPH_VISIBILITY_INPUTS 1\n#include "_ludus_ray/visibility_buffer.slang"\n'
    if interface.get('polygonal_light_sampling', False):
        # ltc_table.slang reads the renderer's LTC table at this slot; the renderer binds it when reflection keeps it.
        wrapper += '#define LUDUS_LTC_TABLE_BINDING LUDUS_GRAPH_LTC_TABLE_BINDING\n'
        wrapper += ''.join('#include "_ludus_ray/' + source + '"\n' for source in POLYGONAL_LIGHT_INCLUDES)
    if compatibility['sources'] or any(program['output'] in ('Ray', 'Color') for program in selected.values()):
        wrapper += '#include "_ludus_ray/ray_shader.slang"\n'
    wrapper += '#include "' + payload + '"\n'
    for source in sorted(compatibility['sources']):
        wrapper += '#include "' + source + '"\n'
    if interface.get('visibility_buffer', False):
        # Declared before the split point so persistent wrappers keep it ahead of namespaced sources.
        wrapper += VISIBILITY_SHADE_DECLARATION
    wrapper += 'uint TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    for source in sorted({program['source'] for program in selected.values()} | set(compatibility['callbacks'])):
        wrapper += '#include "' + source + '"\n'
    callbacks, adapters = adapt_legacy_callbacks(selected)
    wrapper += adapters
    wrapper += '''uint TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload)
{
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, ray, hit);
    if (status == SCENE_QUERY_SURFACE)
        RAY_CLOSEST_HIT(scene, hit, queries, payload);
    else if (status == SCENE_QUERY_MISS)
        RAY_MISS(scene, ray, queries, payload);
    return queries.HasFailed ? SCENE_QUERY_FAILURE : status;
}
float3 EvaluateGraphRay(uint pixel)
{
    SceneContext scene;
    scene.Version = SCENE_INTERFACE_VERSION;
    scene.Generation = uint2(asuint(BoundsMinimumWorld.w), asuint(BoundsMaximumWorld.w));
    RayQueryContext queries = (RayQueryContext)0;
    queries.MaximumSurfaceQueries = MAX_SURFACE_QUERIES;
    queries.MaximumVisibilityQueries = MAX_VISIBILITY_QUERIES;
    queries.MaximumLineQueries = MAX_LINE_QUERIES;
    RayLaunch launch;
    launch.PositionPixel = uint2(pixel % Dimensions.x, pixel / Dimensions.x);
    launch.ExtentPixel = Dimensions.xy;
    launch.CameraClipToWorld = ClipToWorld;
    launch.CameraPositionWorld = CameraPositionWorld.xyz;
    launch.IsOrthographic = CameraPositionWorld.w != 0;
    launch.SampleIndex = asuint(accumulation[pixel * 2].w);
    launch.Seed = Sampling.xy;
    queries.RandomSeed = launch.Seed;
    queries.RandomPixel = pixel;
    queries.RandomSampleIndex = launch.SampleIndex;
    queries.NextRandomDimension = 0;
    float2 positionUv = (float2(launch.PositionPixel) + 0.5) / float2(launch.ExtentPixel);
    float4 nearPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 0, 1));
    float4 farPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 1, 1));
    nearPositionWorld /= nearPositionWorld.w;
    farPositionWorld /= farPositionWorld.w;
    launch.CameraRay.OriginWorld = CameraPositionWorld.w != 0 ? nearPositionWorld.xyz : CameraPositionWorld.xyz;
    launch.CameraRay.DirectionWorld = NormalizeOrZero(farPositionWorld.xyz - launch.CameraRay.OriginWorld);
    launch.CameraRay.MinDistanceWorld = CameraPositionWorld.w != 0 ? 0 : dot(nearPositionWorld.xyz - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    launch.CameraRay.MaxDistanceWorld = dot(farPositionWorld.xyz - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    float3 radiance = RAY_GENERATION(scene, launch, queries);
    return queries.HasFailed ? float3(-1) : radiance;
}
[numthreads(LUDUS_GPU_PREVIEW_GROUP_WIDTH, 1, 1)] void rgEntry(uint3 positionPixel : SV_DispatchThreadID)
{
    ComputeMain(positionPixel);
}
'''
    if interface.get('visibility_buffer', False):
        wrapper = wrapper.replace('float3 EvaluateGraphRay(uint pixel)\n{', VISIBILITY_SHADE + 'float3 EvaluateGraphRay(uint pixel)\n{', 1)
    wrapper = wrapper.replace('MAX_SURFACE_QUERIES', str(interface['surface_queries']))
    wrapper = wrapper.replace('MAX_VISIBILITY_QUERIES', str(interface['visibility_queries']))
    # An absent or zero line budget keeps the zero-initialized maximum and omits the assignment, so
    # packages without line queries keep their wrapper bytes.
    line_queries = interface.get('line_queries', 0)
    wrapper = (wrapper.replace('MAX_LINE_QUERIES', str(line_queries)) if line_queries else
               wrapper.replace('    queries.MaximumLineQueries = MAX_LINE_QUERIES;\n', '', 1))
    for token, stage in (('RAY_GENERATION', 'ray_generation'), ('RAY_MISS', 'miss'), ('RAY_CLOSEST_HIT', 'closest_hit')):
        wrapper = wrapper.replace(token, callbacks[stage])
    return wrapper.replace('_ludus_ray/renderer_gpu_preview.slang', '_ludus_ray/' + recipe['source'])


def adapt_legacy_callbacks(selected, suffix=''):
    """Keep compatibility types and calls in generated build output, never project helpers."""
    callbacks = {stage: program['function'] for stage, program in selected.items()}
    adapters = ''
    for stage, program in selected.items():
        if program['output'] not in ('Ray', 'Color'):
            continue
        function = program['function']
        callback = {'ray_generation': 'GraphLegacyGenerate', 'miss': 'GraphLegacyMiss',
                    'closest_hit': 'GraphLegacyClosest'}[stage] + suffix
        callbacks[stage] = callback
        if stage == 'ray_generation':
            adapters += f'''float3 {callback}(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, {function}(launch), payload);
    return payload.Radiance;
}}
'''
        elif stage == 'miss':
            adapters += f'''void {callback}(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{{
    payload.Radiance = {function}(ray);
}}
'''
        else:
            adapters += f'''void {callback}(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{{
    RayHit input;
    input.IncomingRay = hit.IncomingRay;
    input.PositionWorld = hit.PositionWorld;
    input.DistanceWorld = hit.DistanceWorld;
    input.NormalWorld = hit.GeometricNormalWorld;
    input.Primitive = hit.TriangleIndex;
    input.Barycentric = hit.Barycentric;
    input.Color = Surface(hit.TriangleIndex, 0);
    payload.Radiance = {function}(input);
}}
'''
    return callbacks, adapters


def validate_payload(data, budget, build):
    """Admit a flat plain-value payload with conservative 16-byte field alignment.

    These are shader-local values, not a serialized native ray payload. Restricting
    the source grammar prevents resources, pointers, hidden includes and unbounded arrays.
    """
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', data.decode('utf-8'), flags=re.S)
    source = re.sub(r'^\s*#\s*pragma\s+once\s*$', '', source, flags=re.M).strip()
    match = re.fullmatch(r'struct\s+ScenePayload\s*\{([^{}]*)\}\s*;', source, re.S)
    build.require(match is not None, 'ShaderInterface', 'ScenePayload',
                  'payload source must define only one flat struct ScenePayload with plain fields')
    # Slang owns the actual local layout. This accounting is only an admission cap,
    # not an allocation size or a native payload ABI. Omission needs no byte tuning.
    if budget is None:
        budget = MAX_LOCAL_PAYLOAD_BYTES
    size = 0
    fields = set()
    sizes = {'Ray': 64, 'RayLaunch': 512, 'SurfaceHit': 512, 'MaterialSample': 128}
    for declaration in match[1].split(';'):
        if not declaration.strip():
            continue
        field = re.fullmatch(r'\s*(float(?:[234](?:x[234])?)?|uint[234]?|int[234]?|bool|Ray|RayLaunch|SurfaceHit|MaterialSample)'
                             r'\s+([A-Za-z][A-Za-z0-9_]*)(?:\s*\[\s*([1-9][0-9]{0,2})\s*\])?\s*', declaration)
        build.require(field is not None, 'ShaderInterface', 'ScenePayload', 'unsupported payload field or unbounded array')
        kind, name, count = field.groups()
        build.require(name not in fields, 'ShaderInterface', name, 'duplicate payload field')
        fields.add(name)
        field_size = sizes.get(kind, PAYLOAD_FIELD_ALIGNMENT_BYTES * max(int(kind[-1]), int(kind[-3])) if 'x' in kind else PAYLOAD_FIELD_ALIGNMENT_BYTES)
        size += field_size * (int(count) if count else 1)
        build.require(size <= budget, 'Capacity', 'ScenePayload', f'conservative shader-local payload requires {size} bytes, exceeding the {budget}-byte admission limit; '
                      'reduce ScenePayload or raise the explicit local_payload_bytes cap (maximum '
                      f'{MAX_LOCAL_PAYLOAD_BYTES})')
    build.require(fields, 'ShaderInterface', 'ScenePayload', 'empty payload is not supported')


def persistent_wrapper(programs, technique, variants, snapshot, build, recipe, compatibility=None,
                       native_ray_queries=True, native_ray_pipeline=False, has_persistent_buffers=True, storage_source=""):
    """Namespace each unique authored source and dispatch its callbacks by pass index."""
    interface = technique['scene_interface']
    sources = sorted({programs[identity]['source'] for variant in variants for identity in callback_programs(variant)})
    # A ray variant supplies the callback template; it is the first variant in ray-only sequences.
    template = next((variant for variant in variants if not is_compute_variant(variant)), variants[0])
    selected = {stage: dict(program, function=f'GraphSource{sources.index(program["source"])}::' + program['function'])
                for stage, program in select_callbacks(programs, template).items()}
    wrapper = scene_wrapper(selected, interface, snapshot, build, recipe, storage_source, compatibility=compatibility)
    prefix, body = wrapper.split('uint TraceAndShade(', 1)
    first_callbacks, first_adapters = adapt_legacy_callbacks(selected)
    body = body.replace(first_adapters, '', 1) if first_adapters else body
    if any(programs[identity]['output'] in ('Ray', 'Color') for variant in variants for identity in callback_programs(variant)):
        if '#include "_ludus_ray/ray_shader.slang"' not in prefix:
            prefix += '#include "_ludus_ray/ray_shader.slang"\n'
    strategies = [get_traversal(variant) for variant in variants]
    if any(strategy in ('TriangleLoop', 'Custom', 'ComputeBvh') for strategy in strategies):
        # Native stages run their own pipeline, so their passes keep the native-query code here.
        choices = ', '.join(STRATEGY_MACROS.get(strategy, STRATEGY_MACROS['NativeQuery']) for strategy in strategies)
        prefix = ('#define LUDUS_GRAPH_TRAVERSAL_STRATEGIES 1\n' +
                  ('#define LUDUS_GRAPH_COMPUTE_BVH_TRAVERSAL 1\n' if 'ComputeBvh' in strategies else '') +
                  'uint GetGraphTraversalStrategy();\n' + prefix)
        prefix += ('uint GetGraphTraversalStrategy() {\n'
                   f'    const uint strategies[] = {{ {choices} }};\n'
                   '    uint index = (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK;\n'
                   f'    return index < {len(variants)} ? strategies[index] : LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY;\n}}\n')
    prefix = prefix.replace('#define LUDUS_SCENE_RAY_INTERFACE 1',
                            '#define LUDUS_SCENE_RAY_INTERFACE 1\n#define LUDUS_GRAPH_PERSISTENT_PIPELINE 1')
    if has_persistent_buffers:
        prefix += '[[vk::binding(6, 0)]] RWStructuredBuffer<uint4> GraphBuffer0;\n'
        prefix += '[[vk::binding(7, 0)]] RWStructuredBuffer<uint4> GraphBuffer1;\n'
    prefix += 'uint GetGraphPassIndex() { return (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK; }\n'
    custom_sources = sorted({get_custom_traversal_source(variant) for variant in variants
                             if get_custom_traversal_source(variant)})
    if custom_sources:
        prefix = '#define LUDUS_GRAPH_CUSTOM_RAY_TRAVERSAL 1\n' + prefix
        for index, source in enumerate(custom_sources):
            snapshot.shader_closure(source)
            prefix += f'namespace GraphTraversal{index} {{\n#include "{source}"\n}}\n'
        prefix += ('Hit TraceCustomWorld(float3 originWorld, float3 directionWorld, float nearDistanceWorld, '
                   'float farDistanceWorld, bool isVisibility) {\nswitch (GetGraphPassIndex()) {\n')
        for index, variant in enumerate(variants):
            source = get_custom_traversal_source(variant)
            if source:
                prefix += (f'case {index}: return GraphTraversal{custom_sources.index(source)}::TraceWorld('
                           'originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);\n')
        prefix += ('default: Hit hit = (Hit)0; hit.Triangle = LUDUS_GPU_PREVIEW_FAILED_TRIANGLE; '
                   'return hit; }\n}\n')
    prefix += 'uint TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    # Shared type/helper includes are global and captured before entry-source namespaces.
    # Includes are compiler-resolved from the same immutable source snapshot.
    dependencies = {path for source in sources for path in snapshot.shader_closure(source)} - set(sources)
    for dependency in sorted(dependencies - {interface.get('payload')} - set((compatibility or {}).get('sources', {}))):
        prefix += '#include "' + dependency + '"\n'
    for index, source in enumerate(sources):
        prefix += f'namespace GraphSource{index} {{\n#include "{source}"\n}}\n'
    variant_callbacks = []
    for index, variant in enumerate(variants):
        variant_selected = {stage: dict(programs[identity], function=f'GraphSource{sources.index(programs[identity]["source"])}::' + programs[identity]['function'])
                            for stage, identity in zip(STAGES, callback_programs(variant))}
        callbacks, adapters = adapt_legacy_callbacks(variant_selected, str(index))
        variant_callbacks.append(callbacks)
        prefix += adapters
    body = 'uint TraceAndShade(' + body
    # scene_wrapper's include rows are replaced by the namespaced union above.
    body = re.sub(r'^#include "[^"\n]+"\n', '', body, flags=re.M)
    def dispatch(stage, arguments, returns=False):
        result = 'switch (GetGraphPassIndex()) {\n'
        for index, variant in enumerate(variants):
            if stage not in variant_callbacks[index]:
                continue  # A scene compute pass has no hit or miss callback; default: latches HasFailed.
            function = variant_callbacks[index][stage]
            result += f'case {index}: ' + ('return ' if returns else '') + function + '(' + arguments + ');'
            result += ('\n' if returns else ' break;\n')
        return result + ('default: return float3(-1); }' if returns else 'default: queries.HasFailed = true; break; }')
    body = body.replace(first_callbacks['closest_hit'] + '(scene, hit, queries, payload);',
                        '{ ' + dispatch('closest_hit', 'scene, hit, queries, payload') + ' }')
    body = body.replace(first_callbacks['miss'] + '(scene, ray, queries, payload);',
                        '{ ' + dispatch('miss', 'scene, ray, queries, payload') + ' }')
    body = body.replace('float3 radiance = ' + first_callbacks['ray_generation'] + '(scene, launch, queries);',
                        'float3 radiance = DispatchGraphRay(scene, launch, queries);')
    dispatch_function = ('float3 DispatchGraphRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) {\n' +
                         dispatch('ray_generation', 'scene, launch, queries', True) + '\n}\n')
    # Preserve the frozen native scene/buffer ABI even when the authored kernel only
    # reads triangles. Graph jobs set MaxDepth (Dimensions.z) to 1 in
    # RenderGraphRayPass::Initialize; this ABI liveness branch never runs for them.
    interface_trace = 'TraceNative' if native_ray_queries else 'TraceTriangleLoop'
    if has_persistent_buffers:
        body = body.replace('    SceneContext scene;\n',
                        '    if (Dimensions.z == 0) {\n'
                        f'        Hit interfaceHit = {interface_trace}(float3(0), float3(0, 0, 1), 0, 1, false);\n'
                        '        GraphBuffer0[0] = GraphBuffer1[0];\n'
                        '        GraphBuffer1[0] = uint4(interfaceHit.Triangle, 0, 0, 0);\n'
                        '    }\n    SceneContext scene;\n')
    elif native_ray_pipeline:
        body = body.replace('    SceneContext scene;\n',
                            '    if (Dimensions.z == 0) {\n'
                            '        Hit interfaceHit = TraceNative(float3(0), float3(0, 0, 1), 0, 1, false);\n'
                            '        colors[0] = float4(interfaceHit.DistanceWorld);\n'
                            '    }\n    SceneContext scene;\n')
    if native_ray_pipeline:
        return metal_ray.make_wrapper(prefix, dispatch_function, body,
                                      dispatch('closest_hit', 'scene, hit, queries, payload'),
                                      dispatch('miss', 'scene, ray, queries, payload'))
    return prefix + dispatch_function + body
