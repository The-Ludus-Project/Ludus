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
MAX_RAY_PASSES = 12
# Matches RenderGraphFormat::MAX_RAY_PROGRAMS; packages share callbacks across configurations.
MAX_RAY_PROGRAMS = 16
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


def supports_webgpu_records(profile):
    """WebGPU can run an ordered structured record with software traversal."""
    return (bool(profile['record_readers']) and not profile['native_ray_queries'] and
            not (profile['storage_texture'] or profile['named_texture_count']))


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
# Per-job failure summary (LUDUS_GRAPH_FAILURE_BINDING); one uint count per LUDUS_SCENE_FAILURE_* reason.
FAILURE_SLOT = 14
FAILURE_STRIDE = 4
# World-feature tables (renderer_gpu_preview_layout.h; docs/development/render-graph-authoring.md#world-features).
# Scene compute kernels bind each float4 table only when they call its scene function, so reflection keeping a slot
# records the graph's support. Native ray stages reserve slot 11 for their function table and define neither slot.
AERIAL_PERSPECTIVE_SLOT = 11
ATMOSPHERE_SKY_SLOT = 15
# Matches LUDUS_GRAPH_COLOR_INPUT_BINDING in renderer_gpu_preview_layout.h.
COLOR_INPUT_SLOT = 12
STORAGE_TEXTURE_SLOT = RAY_PATH_CAPTURE_SLOT
WORLD_TABLE_STRIDE = 16
WORLD_FEATURE_DEFINES = ('#define LUDUS_AERIAL_PERSPECTIVE_BINDING LUDUS_GRAPH_AERIAL_PERSPECTIVE_BINDING\n'
                         '#define LUDUS_ATMOSPHERE_SKY_BINDING LUDUS_GRAPH_ATMOSPHERE_SKY_BINDING\n')
# Matches MAX_SCENE_WORK_ITEMS in render_graph_format/plan.h.
MAX_SCENE_WORK_ITEMS = 16 * 1024 * 1024
MAX_LOCAL_PAYLOAD_BYTES = 4096
PAYLOAD_FIELD_ALIGNMENT_BYTES = 16
DEFAULT_PAYLOAD = '_ludus_ray/scene_payload.slang'
# One owner for build admission and the editor controls. Editor CMake reads this
# data through a module import at configure time; no Python interpreter enters the UI.
SCENE_QUERY_LIMITS = {
    'MIN_SURFACE_QUERIES': 1,
    # Eight path queries plus FollowSpecularChain's eight smooth-glass events (SCENE_SPECULAR_CHAIN_EVENTS).
    'MAX_SURFACE_QUERIES': 16,
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
IMAGE_VERTEX_ID = 'GraphRayImageVertex'
IMAGE_INPUTS = [{'location': 0, 'format': 'Float2'}]
IMAGE_FRAGMENT_ID = 'GraphRayImageFragment'
# Files are installed by their owning renderer/math/RHI targets beside the helper. These are the staged roots;
# stage_helpers adds every helper file they include, so a new engine include needs no entry here.
HELPERS = ('scene_data.h', 'renderer_gpu_preview_layout.h', 'renderer_surface_functions.slang', 'recipes.json',
           'native_metal_ray.slang', 'metal_ray_pipeline_layout.h',
           'ray_triangle_loop.slang', 'ray_line_visibility.slang', 'ray_bvh_nodes.slang', 'ray_bvh_traversal.slang', 'renderer_reference_layout.h', 'lighting_layout.h', 'math.slang', 'raster_bindings.slang', 'scene_shader.slang', 'scene_payload.slang', 'scene_shader_layout.h', 'ray_shader.slang', 'visibility_buffer.slang')
# Staged and included only for scene_interface.polygonal_light_sampling: FoundationMath's projected-solid-angle,
# LTC, weighted-reservoir and shading-normal primitives and the renderer's LTC table lookup, in include order, so
# packages that do not request them keep their snapshot and cache identity. The table itself is a renderer-bound buffer.
LIGHT_TRANSFORMER_HELPERS = ('renderer_light_transformer_query.slang',)

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
    record = {'id': node['id'], 'kind': 'RayTracing',
              'color_load': color['load'] if color else 'Discard',
              'color_store': color['store'] if color else 'Discard'}
    return record, mappings, outputs


def lower_ports(node, where, graph, noun, profile, *, is_buffer_dispatch=False, allows_color_input=False):
    """Check the shared C04 port contract of ray and scene compute passes.

    Returns the input mappings, the output mappings and the color output port. The
    noun ('ray', 'scene compute') and profile name only change diagnostic wording.
    """
    types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {}
    required = (('scene', 'Scene'),) if is_buffer_dispatch else (('scene', 'Scene'), ('view', 'View'))
    for aspect, kind in required:
        choices = [identity for identity, value in types.items() if value == kind]
        graph._require(len(choices) == 1, 'IncompletePipeline', where, f'exactly one {kind} input required',
                       field='inputs', expected=f'one {kind} input', actual=f'{len(choices)} inputs',
                       repair=f'Add or retain exactly one {kind} input in the pipeline editor.')
        mappings[aspect] = choices[0]
    storage = {name for name, kind in types.items() if kind == 'Buffer'}
    graph._require(len(storage) <= buffers.MAX_BUFFERS, 'Capacity', where, 'at most two Buffer inputs supported')
    mappings.update({'buffer:' + name: name for name in storage})
    textures = [port for port in node['outputs'] if port.get('access') == 'ShaderReadWrite']
    graph._require(len(textures) <= buffers.MAX_TEXTURES, 'Capacity', where,
                   f'at most {buffers.MAX_TEXTURES} named shader read/write textures per pass')
    texture_inputs = {port['target'] for port in textures}
    texture_outputs = {}
    for port in textures:
        graph._require(port['load'] == 'Load' and port['store'] == 'Store', 'Capability', where,
                       f"Shader read/write texture '{port['id']}' requires Load/Store.",
                       port=port['id'], direction='output', repair='Set the texture to Load/Store.')
        key = 'texture:' + port['target']
        mappings[key] = port['target']
        texture_outputs[key] = port['id']
    colors = [port for port in node['outputs'] if port['type'] == 'ColorTexture' and port not in textures]
    if is_buffer_dispatch:
        remaining_types = {kind for name, kind in types.items() if name not in texture_inputs}
        graph._require((storage or textures) and remaining_types <= {'Scene', 'Buffer'} and not colors,
                       'Capability', where, 'a work-item scene dispatch requires Scene and shader-bound resources',
                       repair='Remove View, sampled textures and automatic color ports. Connect a Buffer or ShaderReadWrite texture.')
        return mappings, {**lower_buffer_outputs(node, where, graph, storage, None), **texture_outputs}, None
    graph._require(len(colors) == 1 or not colors and textures, 'Capability', where,
                   f'{noun} pipelines require one color output or named shader read/write textures')
    if colors:
        mappings['color'] = colors[0]['target']
        graph._require(types.get(mappings['color']) == 'ColorTexture', 'Capability', where, f'{noun} output requires color input')
    else:
        graph._require(node['invocation']['dimensions'] != 'ColorTarget', 'Capability', where,
                       'a pass with only shader read/write outputs requires explicit launch dimensions',
                       field='invocation.dimensions', repair='Choose Explicit size in Invocation.')
    graph._require(not textures or not node['invocation'].get('storage_texture'), 'Capability', where,
                   'named textures use per-resource access; the legacy storage output cannot share this pass',
                   repair='Set the legacy color output to Store returned RGB.')
    color_reads = [identity for identity, kind in types.items() if kind == 'ColorTexture' and
                   identity != mappings.get('color') and identity not in texture_inputs]
    depth_reads = [identity for identity, kind in types.items() if kind == 'DepthTexture']
    if allows_color_input and not depth_reads:
        graph._require(len(color_reads) <= 1, 'Capability', where,
                       'scene compute supports one sampled color input per pass',
                       repair='Keep the ColorTexture you want to sample and remove the other read-only color inputs.')
        if color_reads:
            mappings['source_color'] = color_reads.pop()
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
    output = colors[0] if colors else None
    outputs = {**lower_buffer_outputs(node, where, graph, storage, output), **texture_outputs}
    if output is not None:
        load = 'Load' if node['invocation'].get('storage_texture', False) else 'Clear'
        graph._require(output['load'] == load and output['store'] == 'Store',
                       'Capability', where, f'{noun} output must {load}/Store its ColorTexture target',
                       repair=f'Set the color output to {load}/Store in Writable Resources.')
        outputs['color'] = output['id']
    graph._require(output is not None or 'visibility' not in mappings, 'Capability', where,
                   'visibility/depth reconstruction needs an automatic color output')
    return mappings, outputs, output


def lower_buffer_outputs(node, where, graph, storage, color):
    outputs = {}
    for port in node['outputs']:
        if port is color or port.get('access') == 'ShaderReadWrite':
            continue
        graph._require(port['type'] == 'Buffer' and port['target'] in storage and
                       port['load'] == 'Load' and port['store'] == 'Store', 'Capability', where,
                       'writable structured buffers require Load/Store on a paired Buffer input')
        outputs['buffer:' + port['target']] = port['id']
    return outputs


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


def reject_native_invocation(operation, ports, reason, build):
    """Point the build diagnostic at the authored node and input connector."""
    location = {'source': operation['source'], 'configuration': operation['asset_configuration'],
                'node': operation['pipeline']['id'], 'instance': operation['id'], 'field': 'invocation.type'}
    subject = operation['id']
    if ports:
        location.update(port=ports[0], direction='input')
        inputs = 'input' if len(ports) == 1 else 'inputs'
        subject += f' Buffer {inputs} ' + ', '.join(map(repr, ports))
    raise build.ToolingError('RenderGraphBuildCapability',
                             f'{subject}: Hardware RT pipeline invocation {reason}', location=location)


def validate_native_invocations(plans, profiles, technique, build):
    """Reject unavailable native execution before the shader wrappers are generated."""
    if 'metal' not in profiles:
        reason = ('requires a Metal package profile. Add Metal to the shader manifest profiles or select '
                  'Compute pipeline in Invocation settings.')
    elif 'scene_interface' not in technique:
        reason = ('requires scene_interface version 1 in the ray technique. Declare that interface '
                  'in the shader manifest.')
    else:
        return
    for plan in plans:
        for operation in plan['passes']:
            if operation['kind'] != 'RayTracing':
                continue
            pipeline = operation['pipeline']
            if invocation.resolve_type(pipeline) == invocation.HARDWARE_PIPELINE:
                ports = [port['id'] for port in pipeline['inputs'] if port['type'] == 'Buffer']
                reject_native_invocation(operation, ports, reason, build)


def validate_native_buffer_layouts(operations, programs, named_resources, snapshot, build):
    """Identify records declared inline in a shader outside the native stage bundle."""
    sources = {programs[identity]['source'] for variant in operations for identity in callback_programs(variant)}
    unavailable_sources = snapshot.inline_buffer_sources - sources
    if not unavailable_sources:
        return
    for variant, operation in operations.items():
        ports = [port['id'] for port in operation['pipeline']['inputs'] if port['type'] == 'Buffer']
        for field in named_resources.get(variant, ()):
            record = field['record']
            if record is not None and record['source'] in unavailable_sources:
                reject_native_invocation(
                    operation, [ports[field['slot']]],
                    f"cannot bind record '{record['type']}' declared in '{record['source']}' outside its native stages. "
                    'Move the record to a shared .slang or .luslh declaration and select it in Buffer sizing.',
                    build)


def group_profiles(plans, technique, build):
    """Group compatible callback sets, isolating lighting dependencies by configuration."""
    groups = {}
    isolate_lighting = (technique.get('scene_interface', {}).get('polygonal_light_sampling', False) and
                        any(requires_native_pipeline(plan) for plan in plans))
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
        has_color_input = any('source_color' in operation for operation in operations)
        has_storage_texture = any(operation.get('storage_texture', False) for operation in operations)
        named_texture_count = max(len(operation.get('storage_textures', ())) for operation in operations)
        build.require(not named_texture_count or not has_storage_texture, 'Capability', plan['configuration'],
                      'named shader textures and the legacy storage output need separate configurations')
        build.require(not has_color_input or not has_visibility, 'Capability', plan['configuration'],
                      'scene compute color inputs require an ordered scene sequence without visibility/depth')
        has_native_queries = requires_native_queries(plan) or requires_native_pipeline(plan)
        build.require(not (has_storage_texture or named_texture_count) or
                      (not has_visibility and technique.get('scene_interface')),
                      'Capability', plan['configuration'],
                      'storage texture dispatch requires scene materials and a configuration without visibility/depth')
        variants = {get_callback_variant(operation['pipeline']) for operation in operations}
        native_operations = {get_callback_variant(operation['pipeline']): operation for operation in operations
                             if invocation.resolve_type(operation['pipeline']) == invocation.HARDWARE_PIPELINE}
        native_uses = [use for operation in operations for use in operation.get('persistent_buffers', [])
                       if 'binding_slot' in use]
        uses_native_records = bool(native_uses)
        record_readers = set()
        if uses_native_records:
            build.require(len({use['resource'] for use in native_uses}) == 1 and
                          {use['output_version'] is None for use in native_uses} == {False, True},
                          'Capability', plan['configuration'],
                          'connect one native structured Buffer from a scene compute writer to a reader')
            has_hardware_record_reader = False
            for operation in operations:
                variant = get_callback_variant(operation['pipeline'])
                for use in operation.get('persistent_buffers', []):
                    if 'binding_slot' in use:
                        is_hardware = variant in native_operations
                        is_reader = use['output_version'] is None
                        build.require(not is_hardware or is_reader, 'Capability', operation['id'],
                                      'write the native structured Buffer in an ordered scene compute pass')
                        if is_reader and not is_hardware:
                            record_readers.add(variant)
                        has_hardware_record_reader |= is_reader and is_hardware
                    else:
                        build.require(use['slot'] != buffers.NATIVE_RECORD_SLOT, 'Capability', operation['id'],
                                      'the native structured Buffer occupies the second graph binding. '
                                      'Use one positional Buffer in this configuration.')
            build.require(not native_operations or has_hardware_record_reader, 'Capability', plan['configuration'],
                          'connect the native structured Buffer as a read-only input of a hardware ray pass '
                          'or use only compute invocations in this configuration')
        # With optional lighting helpers, share an artifact only between configurations
        # that use the same callbacks. Reflection can then omit the table per configuration.
        callbacks = ((tuple(sorted(variants)), tuple(sorted(native_operations)), plan['configuration'])
                     if uses_native_records else
                     (tuple(sorted(variants)), tuple(sorted(native_operations))) if isolate_lighting else ())
        group = groups.setdefault((has_visibility, has_native_queries, has_color_input, has_storage_texture, named_texture_count, callbacks),
                                  {'variants': set(), 'native_operations': {}, 'configurations': [],
                                   'persistent': False, 'structured_buffers': False})
        group['variants'].update(variants)
        group['native_operations'].update(native_operations)
        group['native_graph_records'] = uses_native_records
        group['record_readers'] = record_readers
        group['configurations'].append(plan['configuration'])
        group['structured_buffers'] |= any(operation.get('buffers') for operation in operations)
        group['scene_dispatch'] = group.get('scene_dispatch', False) or any('work_items' in operation for operation in operations)
        # GraphBuffer0/1 and GetGraphPassIndex exist only in the persistent wrapper, so a
        # single scene compute pass needs it as well (compute ray tracing design, section 3.3).
        has_shader_traversal = any(invocation.resolve_traversal(operation['pipeline']) in
                                   (invocation.TRIANGLE_LOOP, invocation.CUSTOM_QUERY, invocation.COMPUTE_BVH)
                                   for operation in operations)
        group['persistent'] |= bool(named_texture_count) or has_storage_texture or has_shader_traversal or len(operations) > 1 or any(
            resource['type'] == 'Buffer' and 'stride' not in resource
            for resource in plan['resources'].values()) or any(
                operation['pipeline']['kind'] == 'ComputePipeline' for operation in operations)
    if not groups:
        build.require(all(stage in technique for stage in STAGES), 'Configuration', TECHNIQUE,
                      'a ray technique without default callbacks requires a RayTracingPipeline or scene compute pass')
        # No visibility pass dispatches the default callbacks, so the kernel reads no visibility
        # inputs and reflection drops them, even when scene_interface.visibility_buffer permits them.
        groups[(False, True, False, False, 0, ())] = {
            'variants': {tuple(technique[stage] for stage in STAGES)}, 'native_operations': {}, 'configurations': [],
            'persistent': False, 'structured_buffers': False}
    result = []
    has_native_record_profile = any(group.get('native_graph_records', False) for group in groups.values())
    for (has_visibility, has_native_queries, has_color_input, has_storage_texture, named_texture_count, callbacks), group in sorted(groups.items()):
        build.require(not (group['persistent'] or len(group['variants']) > 1) or not group['structured_buffers'],
                      'Capability', 'ray storage',
                      'ordered ray passes use shared Buffer nodes; StructuredBuffer nodes remain private to one ray job')
        if has_visibility and any(key[:4] == (False, has_native_queries, False, False) for key in groups):
            compute_identity = VISIBILITY_COMPUTE_ID if has_native_queries else SOFTWARE_VISIBILITY_COMPUTE_ID
        else:
            compute_identity = COMPUTE_ID if has_native_queries else SOFTWARE_COMPUTE_ID
        if has_color_input:
            compute_identity += 'WithColorInput'
        if has_storage_texture:
            compute_identity += 'WithStorageTexture'
        if named_texture_count:
            compute_identity += f'WithNamedTextures{named_texture_count}'
        if callbacks:
            compute_identity += 'Configuration' + str(len(result))
        variants = sorted(group['variants'])
        native_slots = {variant: index for index, variant in enumerate(variants) if variant in group['native_operations']}
        result.append({'workgroup': workgroup, 'visibility_buffer': has_visibility, 'native_ray_queries': has_native_queries,
                       'native_slots': native_slots, 'native_operations': group['native_operations'],
                       'native_graph_records': group.get('native_graph_records', False),
                       'record_readers': sorted(group.get('record_readers', ())),
                       'configurations': sorted(group['configurations']) if callbacks or has_native_record_profile else [],
                       'scene_dispatch': group.get('scene_dispatch', False),
                       'variants': variants, 'color_input': has_color_input,
                       'storage_texture': has_storage_texture, 'named_texture_count': named_texture_count,
                       'persistent': group['persistent'] or len(group['variants']) > 1,
                       'compute': compute_identity})
    return result


def report_bounds_failures(wrapper, selected_sources, snapshot, *, namespaced):
    """Map each lowered module's private latch back to a LUSL line after invocation."""
    sources = sorted(selected_sources)
    cases = []
    for index, source in enumerate(sources):
        report = snapshot.language_reports.get(source.removesuffix('.slang'), {})
        if not report.get('bounds_checks'):
            continue
        variable = report['failure_variable']
        if namespaced:
            variable = f'GraphSource{index}::{variable}'
        cases.append(f'    switch ({variable}) {{\n')
        for site in report['bounds_checks']:
            cases.append(f'    case {site["code"]}u: return ReportGraphBoundsFailure({site["line"]}u);\n')
        cases.append('    default: break;\n    }\n')
    if not cases:
        return wrapper
    # Leave the renderer's forward declaration unchanged. The wrapper catches
    # failures after all callbacks, including legacy and ordered dispatch.
    wrapper = wrapper.replace('float3 EvaluateGraphRay(uint pixel)\n{',
                              'float3 EvaluateGraphRayUnchecked(uint pixel)\n{', 1)
    return ('#define LUDUS_GRAPH_BOUNDS_CHECKS 1\n' + wrapper +
            '\nfloat3 EvaluateGraphRay(uint pixel)\n{\n'
            '    float3 radiance = EvaluateGraphRayUnchecked(pixel);\n' +
            ''.join(cases) + '    return radiance;\n}\n')


def expand_programs(programs, technique, snapshot, helper, build, variants=None, persistent=False, schemas=None,
                    compute_identity=COMPUTE_ID, native_ray_queries=True, ray_path_capture=False,
                    native_slots=None, workgroup=None, scene_dispatch=False, named_resources=None, dispatch_slots=None):
    """Capture the engine kernel and all three functions in one linked compute entry.

    ray_path_capture links the diagnostic LUDUS_RAY_PATH_CAPTURE variant of the same
    source snapshot. The flag is part of the program, so it joins the stage cache identity.
    """
    native_ray_pipeline = native_slots is not None
    selected = select_callbacks(programs, variants[0]) if variants else {stage: programs[technique[stage]] for stage in STAGES}
    storage = buffers.merge_bindings(selected, build)
    build.require(not persistent or not storage, 'Capability', TECHNIQUE, 'ordered ray passes use Buffer storage; StructuredBuffer remains the single-job profile')
    storage_source = buffers.declarations(storage, schemas or {}, build)
    recipes = build.document(build.read(helper / 'recipes.json'), 'renderer recipes')
    compute_recipe = recipes['renderer_gpu_preview']
    image_recipe = recipes['renderer_gpu_preview_image']
    has_light_transformer = any(programs[identity].get('light_transformer', False)
                                for variant in (variants or []) for identity in callback_programs(variant))
    if has_light_transformer:
        technique = deepcopy(technique)
        build.require(technique.get('scene_interface') is not None, 'Capability', TECHNIQUE,
                      'Light Transformer requires scene interface version 1')
        technique['scene_interface']['light_transformer'] = True
    polygonal_light_helpers = POLYGONAL_LIGHT_HELPERS if technique.get('scene_interface', {}).get(
        'polygonal_light_sampling', False) else ()
    light_transformer_helpers = LIGHT_TRANSFORMER_HELPERS if technique.get('scene_interface', {}).get('light_transformer', False) else ()
    stage_helpers(snapshot, helper, (*HELPERS, *polygonal_light_helpers, *light_transformer_helpers, compute_recipe['source'], image_recipe['source']),
                  build)
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
    CameraRaySegment camera = MakeCameraRaySegment(ClipToEye, CameraPositionWorld.xyz, CameraPositionWorld.w != 0,
                                                   float2(positionUv.x * 2 - 1, 1 - positionUv.y * 2), 0);
    launch.CameraRay.OriginWorld = camera.OriginWorld;
    launch.CameraRay.DirectionWorld = camera.DirectionWorld;
    launch.CameraRay.MinDistanceWorld = camera.MinDistanceWorld;
    launch.CameraRay.MaxDistanceWorld = camera.MaxDistanceWorld;
    Ray ray = RAY_GENERATION(launch);
    if (any(isfinite(ray.OriginWorld) == false) || any(isfinite(ray.DirectionWorld) == false) ||
        isfinite(ray.MinDistanceWorld) == false || isfinite(ray.MaxDistanceWorld) == false ||
        ray.MinDistanceWorld < 0 || ray.MaxDistanceWorld < ray.MinDistanceWorld ||
        abs(dot(ray.DirectionWorld, ray.DirectionWorld) - 1) > RAY_DIRECTION_SQUARED_TOLERANCE)
    {
        graphFailureReason = LUDUS_SCENE_FAILURE_INVALID_RAY;
        return float3(-1);
    }
    Hit hit = Trace(ray.OriginWorld, ray.DirectionWorld, ray.MinDistanceWorld, ray.MaxDistanceWorld);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
    {
        graphFailureReason = LUDUS_SCENE_FAILURE_TRAVERSAL;
        return float3(-1);
    }
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
    named_resources = {variant: named_resources[variant] for variant in (variants or [])
                       if named_resources and variant in named_resources}
    named_record_views = buffers.collect_record_views(named_resources.values(), build)
    native_records = buffers.collect_native_records(named_resources.values(), build)
    needs_dispatch = persistent or native_ray_pipeline or bool(named_resources)
    if needs_dispatch:
        build.require(technique.get('scene_interface') is not None, 'Capability', TECHNIQUE,
                      'native ray stages, ordered ray passes and persistent buffers require the scene callback interface')
        wrapper = persistent_wrapper(programs, technique, variants, snapshot, build, compute_recipe, compatibility,
                                     native_ray_queries, native_slots, persistent, storage_source, named_resources,
                                     named_record_views, native_records, dispatch_slots)
    selected_sources = ({programs[identity]['source'] for variant in variants for identity in callback_programs(variant)}
                        if needs_dispatch else {program['source'] for program in selected.values()})
    wrapper = report_bounds_failures(wrapper, selected_sources, snapshot,
                                     namespaced=needs_dispatch)
    if scene_dispatch:
        wrapper = '#define LUDUS_GRAPH_BUFFER_DISPATCH 1\n' + wrapper
    workgroup = list(workgroup or DEFAULT_WORKGROUP_SIZE)
    build.require(workgroup in SUPPORTED_WORKGROUP_SIZES, 'Capability', 'ray workgroups', 'unsupported ray workgroup')
    wrapper = wrapper.replace('[numthreads(LUDUS_GPU_PREVIEW_GROUP_WIDTH, 1, 1)]',
                              '[numthreads(' + ', '.join(str(value) for value in workgroup) + ')]')
    if technique.get('scene_interface') is not None and not native_ray_pipeline:
        wrapper = WORLD_FEATURE_DEFINES + wrapper
    if not native_ray_queries:
        wrapper = '#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1\n' + wrapper
    if ray_path_capture:
        wrapper = '#define LUDUS_RAY_PATH_CAPTURE 1\n' + wrapper
    result = {identity: deepcopy(program) for identity, program in programs.items()
              if program['stage'] not in (*STAGES, COMPUTE_STAGE)}
    result[compute_identity] = {'id': compute_identity, 'stage': 'compute', 'source': selected['ray_generation']['source'],
                          'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': wrapper, 'ray_workgroup': workgroup, 'native_ray_queries': native_ray_queries, 'software_ray_queries': not native_ray_queries, 'native_ray_profile': True, 'persistent_ray_profile': persistent, 'storage_bindings': storage, 'storage_schemas': {binding['schema']: schemas[binding['schema']] for binding in storage}, 'visibility_buffer': technique.get('scene_interface', {}).get('visibility_buffer', False), 'ltc_table': technique.get('scene_interface', {}).get('polygonal_light_sampling', False)}
    if needs_dispatch and snapshot.inline_buffer_sources:
        result[compute_identity]['buffer_layout_namespaces'] = {
            source: f'GraphSource{index}' for index, source in enumerate(sorted(selected_sources))
            if source in snapshot.inline_buffer_sources}
    if named_record_views:
        result[compute_identity]['named_record_views'] = named_record_views
    if native_records:
        result[compute_identity]['native_graph_records'] = native_records
    if technique.get('scene_interface', {}).get('color_input', False):
        result[compute_identity]['color_input'] = True
    if technique.get('scene_interface', {}).get('storage_texture', False):
        result[compute_identity]['storage_texture'] = True
    if technique.get('scene_interface', {}).get('named_texture_count', 0):
        result[compute_identity]['named_texture_count'] = technique['scene_interface']['named_texture_count']
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


HELPER_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"/\\\n]+)"', re.M)


def stage_helpers(snapshot, helper, roots, build):
    """Stage engine ray helpers under _ludus_ray/ with every helper file they include.

    The closure follows quoted includes of plain file names that exist in the helper
    directory, in every preprocessor branch, so an engine shader that gains an include
    cannot leave the snapshot short of it. Names absent from the helper directory are
    left to the compiler, which reports them; authored sources are never followed here.
    Return the captured paths so standalone probes can stage the same closure.
    """
    pending = list(roots)
    staged = set()
    while pending:
        filename = pending.pop(0)
        data = build.read(helper / filename)
        relative = '_ludus_ray/' + filename
        if relative in snapshot.files:
            build.require(snapshot.external.get(relative) == helper / filename and snapshot.files[relative] == data,
                          'Source', relative, 'reserved engine ray helper path')
            # LUSL lowering may have captured a helper without its includes; stage those too.
            if relative in staged:
                continue
        else:
            snapshot.capture(relative, data, external=helper / filename)
        staged.add(relative)
        text = data.decode('utf-8', 'replace') if isinstance(data, bytes) else data
        pending += [name for name in HELPER_INCLUDE.findall(text)
                    if '_ludus_ray/' + name not in staged and (helper / name).is_file()]
    return staged


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


def check_interface(program, result, build, raw=None):
    """Validate the frozen renderer ABI from independently reflected artifacts."""
    interface = result['interface']
    entries = sorted(interface['entries'], key=lambda value: value['binding'])
    if program.get('native_ray_profile') and program['stage'] == 'compute':
        expected = [(0, 'UniformBuffer', PARAMETERS_BYTES), (1, 'StorageRead', 16), (2, 'StorageReadWrite', 16),
                    (3, 'StorageReadWrite', 16), (4, 'StorageRead', 16), (5, 'RayScene', 0)]
        if not program.get('native_ray_queries', True):
            expected = [entry for entry in expected if entry[1] != 'RayScene']
        if program.get('persistent_ray_profile'):
            layouts = program.get('native_graph_layouts', {})
            parameters = {item['name']: item['type'] for item in (raw or {}).get('parameters', [])}
            for slot in range(buffers.MAX_BUFFERS):
                binding = buffers.FIRST_SLOT + slot
                if binding not in layouts:
                    expected.append((binding, 'StorageReadWrite', BUFFER_WORD_BYTES))
                    continue
                (stride, shape), is_writable = layouts[binding]
                expected.append((binding, 'StorageReadWrite' if is_writable else 'StorageRead', stride))
                reflected = parameters.get(f'GraphBuffer{slot}', {}).get('resultType', {})
                build.require(buffers.storage_shape(reflected) == buffers.storage_shape(shape),
                              'ShaderInterface', program['id'],
                              f'native structured Buffer {shape.get("name", "")} has different field offsets '
                              'or array strides on the selected target')
        build.require(interface.get('workgroup') == program.get('ray_workgroup', DEFAULT_WORKGROUP_SIZE) and interface['inputs'] == [] and
                      [(entry['binding'], entry['kind'], entry['size']) for entry in entries if program.get('persistent_ray_profile') or entry['binding'] < buffers.FIRST_SLOT or entry['binding'] >= VISIBILITY_SLOT] ==
                      expected + ([(VISIBILITY_SLOT, 'Texture2D', 0), (VISIBILITY_SLOT + 1, 'TextureDepth2D', 0)] if program.get('visibility_buffer', False) else []) +
                      [(buffers.FIRST_TEXTURE_SLOT + index, 'StorageTexture2DReadWrite', 0)
                       for index in range(program.get('named_texture_count', 0))] +
                      ([(RAY_PATH_CAPTURE_SLOT, 'StorageReadWrite', RAY_PATH_CAPTURE_STRIDE)] if program.get('ray_path_capture', False) else []) +
                      ([(STORAGE_TEXTURE_SLOT, 'StorageTexture2DReadWrite', 0)] if program.get('storage_texture') else []) +
                      # World-feature tables appear exactly when a callback calls their scene function.
                      ([(AERIAL_PERSPECTIVE_SLOT, 'StorageRead', WORLD_TABLE_STRIDE)] if not program.get('native_ray_pipeline') and
                       any(entry['binding'] == AERIAL_PERSPECTIVE_SLOT for entry in entries) else []) +
                      ([(COLOR_INPUT_SLOT, 'Texture2D', 0)] if program.get('color_input', False) else []) +
                      # Reflection drops the table when no callback reads it; the renderer then binds nothing.
                      ([(LTC_TABLE_SLOT, 'StorageRead', LTC_TABLE_STRIDE)] if program.get('ltc_table', False) and
                       any(entry['binding'] == LTC_TABLE_SLOT for entry in entries) else []) +
                      # Every graph ray kernel counts its latched pixels by failure reason.
                      [(FAILURE_SLOT, 'StorageReadWrite', FAILURE_STRIDE)] +
                      ([(ATMOSPHERE_SKY_SLOT, 'StorageRead', WORLD_TABLE_STRIDE)] if not program.get('native_ray_pipeline') and
                       any(entry['binding'] == ATMOSPHERE_SKY_SLOT for entry in entries) else []),
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
VISIBILITY_SHADE_DECLARATION = ('SceneQuery LoadVisibilityAndShade(SceneContext scene, inout RayQueryContext queries, '
                                'RayLaunch launch, inout ScenePayload payload);\n')
VISIBILITY_SHADE = '''SceneQuery LoadVisibilityAndShade(SceneContext scene, inout RayQueryContext queries, RayLaunch launch, inout ScenePayload payload)
{
    SurfaceHit hit;
    Ray ray = launch.CameraRay;
    SceneQuery status = LoadVisibilitySurface(scene, launch, queries, hit);
    if (status == SceneQuery::Surface)
        RAY_CLOSEST_HIT(scene, hit, queries, payload);
    else if (status == SceneQuery::Miss)
        RAY_MISS(scene, ray, queries, payload);
    return queries.HasFailed ? SceneQuery::Failure : status;
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
    if interface.get('light_transformer', False):
        wrapper += '#include "_ludus_ray/renderer_light_transformer_query.slang"\n'
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
    wrapper += 'SceneQuery TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    for source in sorted({program['source'] for program in selected.values()} | set(compatibility['callbacks'])):
        wrapper += '#include "' + source + '"\n'
    callbacks, adapters = adapt_legacy_callbacks(selected)
    wrapper += adapters
    wrapper += '''SceneQuery TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload)
{
    SurfaceHit hit;
    SceneQuery status = TraceClosest(scene, queries, ray, hit);
    if (status == SceneQuery::Surface)
        RAY_CLOSEST_HIT(scene, hit, queries, payload);
    else if (status == SceneQuery::Miss)
        RAY_MISS(scene, ray, queries, payload);
    return queries.HasFailed ? SceneQuery::Failure : status;
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
    RayLaunch launch = (RayLaunch)0;
#if defined(LUDUS_GRAPH_BUFFER_DISPATCH)
    if ((Sampling.z & LUDUS_GPU_PREVIEW_SCENE_WORK_FLAG) != 0)
    {
        launch.WorkItemIndex = pixel;
        launch.WorkItemCount = Dimensions.w;
    }
    else
#endif
    {
        launch.PositionPixel = uint2(pixel % Dimensions.x, pixel / Dimensions.x);
        launch.ExtentPixel = Dimensions.xy;
        launch.CameraClipToWorld = MakeCameraClipToWorld(ClipToEye, CameraPositionWorld.xyz);
        launch.CameraClipToEye = ClipToEye;
        launch.CameraPositionWorld = CameraPositionWorld.xyz;
        launch.IsOrthographic = CameraPositionWorld.w != 0;
        launch.IsFullImageDispatch = all(Tile.xy == uint2(0)) && all(Tile.zw == Dimensions.xy);
        launch.SampleIndex = asuint(accumulation[pixel * 2].w);
        launch.CameraRay = MakeCameraRay(launch, float2(0.5));
    }
    launch.Seed = Sampling.xy;
    queries.RandomSeed = launch.Seed;
    queries.RandomPixel = pixel;
    queries.RandomSampleIndex = launch.SampleIndex;
    queries.NextRandomDimension = 0;
    float3 radiance = RAY_GENERATION(scene, launch, queries);
    if (queries.HasFailed == true)
    {
        // The renderer counts the latched pixel under the scene interface's recorded reason.
        graphFailureReason = queries.FailureReason;
        return float3(-1);
    }
    return radiance;
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
                       native_ray_queries=True, native_slots=None, has_persistent_buffers=True, storage_source="",
                       named_resources=None, named_record_views=None, native_records=None, dispatch_slots=None):
    """Namespace each unique authored source and dispatch its callbacks by pass index."""
    native_ray_pipeline = native_slots is not None
    interface = technique['scene_interface']
    sources = sorted({programs[identity]['source'] for variant in variants for identity in callback_programs(variant)})
    # A ray variant supplies the callback template; it is the first variant in ray-only sequences.
    template = next((variant for variant in variants if not is_compute_variant(variant)), variants[0])
    selected = {stage: dict(program, function=f'GraphSource{sources.index(program["source"])}::' + program['function'])
                for stage, program in select_callbacks(programs, template).items()}
    wrapper = scene_wrapper(selected, interface, snapshot, build, recipe, storage_source, compatibility=compatibility)
    prefix, body = wrapper.split('SceneQuery TraceAndShade(', 1)
    first_callbacks, first_adapters = adapt_legacy_callbacks(selected)
    body = body.replace(first_adapters, '', 1) if first_adapters else body
    if any(programs[identity]['output'] in ('Ray', 'Color') for variant in variants for identity in callback_programs(variant)):
        if '#include "_ludus_ray/ray_shader.slang"' not in prefix:
            prefix += '#include "_ludus_ray/ray_shader.slang"\n'
    strategies = [get_traversal(variant) for variant in variants]
    if any(strategy in ('TriangleLoop', 'Custom', 'ComputeBvh') for strategy in strategies):
        # Native stages run their own pipeline, so their passes keep the native-query code here.
        if dispatch_slots is None:
            choices = ', '.join(STRATEGY_MACROS.get(strategy, STRATEGY_MACROS['NativeQuery']) for strategy in strategies)
            traversal = (f'    const uint strategies[] = {{ {choices} }};\n'
                         f'    return index < {len(variants)} ? strategies[index] : LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY;\n')
        else:
            cases = ''.join(f'case {dispatch_slots[variant]}: '
                            f'return {STRATEGY_MACROS.get(strategy, STRATEGY_MACROS["NativeQuery"])}; '
                            for variant, strategy in zip(variants, strategies))
            traversal = f'    switch (index) {{ {cases}default: return LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY; }}\n'
        prefix = ('#define LUDUS_GRAPH_TRAVERSAL_STRATEGIES 1\n' +
                  ('#define LUDUS_GRAPH_COMPUTE_BVH_TRAVERSAL 1\n' if 'ComputeBvh' in strategies else '') +
                  'uint GetGraphTraversalStrategy();\n' + prefix)
        prefix += ('uint GetGraphTraversalStrategy() {\n'
                   '    uint index = (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK;\n'
                   + traversal + '}\n')
    if has_persistent_buffers or native_ray_pipeline:
        prefix = prefix.replace('#define LUDUS_SCENE_RAY_INTERFACE 1',
                                '#define LUDUS_SCENE_RAY_INTERFACE 1\n#define LUDUS_GRAPH_PERSISTENT_PIPELINE 1')
    if has_persistent_buffers:
        for slot in range(buffers.MAX_BUFFERS):
            if slot not in (native_records or {}):
                prefix += f'[[vk::binding({buffers.FIRST_SLOT + slot}, 0)]] RWStructuredBuffer<uint4> GraphBuffer{slot};\n'
    prefix += 'uint GetGraphPassIndex() { return (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK; }\n'
    if interface.get('color_input', False):
        prefix += '[[vk::binding(LUDUS_GRAPH_COLOR_INPUT_BINDING, 0)]] Texture2D<float4> GraphColorInput;\n'
    if interface.get('storage_texture', False):
        prefix += ('[format("rgba8")] LUDUS_SHADER_BINDING(LUDUS_GRAPH_STORAGE_TEXTURE_BINDING) '
                   'RWTexture2D<float4> GraphStorageTexture;\n')
    named_texture_count = interface.get('named_texture_count', 0)
    for index in range(named_texture_count):
        slot = buffers.FIRST_TEXTURE_SLOT + index
        prefix += (f'[format("rgba8")] LUDUS_SHADER_BINDING({slot}) '
                   f'RWTexture2D<float4> GraphWritableTexture{index};\n')
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
    prefix += 'SceneQuery TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    # Shared type/helper includes are global and captured before entry-source namespaces.
    # Includes are compiler-resolved from the same immutable source snapshot.
    dependencies = {path for source in sources for path in snapshot.shader_closure(source)} - set(sources)
    layout_namespaces = {source: f'GraphSource{index}' for index, source in enumerate(sources)
                         if source in snapshot.inline_buffer_sources}
    for record_source, _, _ in (named_record_views or {}).values():
        if record_source in snapshot.inline_buffer_sources:
            build.require(record_source in layout_namespaces, 'ShaderInterface', record_source,
                          'the Buffer declaration must be visible to each linked shader; '
                          'use a shared .luslh or .slang declaration across separate shader contexts')
        else:
            source = snapshot.buffer_layout_sources.get(record_source, record_source)
            dependencies.add(source)
    for record_source, _, _ in (native_records or {}).values():
        dependencies.add(snapshot.buffer_layout_sources.get(record_source, record_source))
    for dependency in sorted(dependencies - {interface.get('payload')} - set((compatibility or {}).get('sources', {}))):
        prefix += '#include "' + dependency + '"\n'
    for slot, (_, type_name, is_writable) in (native_records or {}).items():
        access = 'RW' if is_writable else ''
        prefix += f'[[vk::binding({buffers.FIRST_SLOT + slot}, 0)]] {access}StructuredBuffer<{type_name}> GraphBuffer{slot};\n'
    for view, (source, type_name, is_writable) in (named_record_views or {}).items():
        qualified_type = buffers.qualify_record_type(source, type_name, layout_namespaces)
        prefix += buffers.record_view_definition(view, qualified_type, is_writable)
    prefix += buffers.word_view_definitions((named_resources or {}).values())
    if any(field.get('texture_view') for fields in (named_resources or {}).values() for field in fields):
        prefix += buffers.texture_view_definition()
    source_contexts, source_aliases = {}, {}
    for variant, fields in (named_resources or {}).items():
        program = programs[variant[0]]
        function, source = program['function'], program['source']
        context = (f'struct {function}Resources {{\n' + ''.join(
            f'    {buffers.record_view_name(field)} {field["field"]};\n' for field in fields) + '};\n')
        source_contexts.setdefault(source, {})[function] = context
        for field in fields:
            record = field['record']
            if record and record['source'] != source and record['source'] in layout_namespaces:
                source_aliases.setdefault(source, set()).add((record['source'], record['type']))
    for index, source in enumerate(sources):
        aliases = ''.join(f'typedef {buffers.qualify_record_type(owner, type_name, layout_namespaces)} {type_name};\n'
                          for owner, type_name in sorted(source_aliases.get(source, ())))
        declarations = ''.join(source_contexts.get(source, {}).values())
        prefix += f'namespace GraphSource{index} {{\n{aliases}{declarations}#include "{source}"\n}}\n'
    variant_callbacks = []
    for index, variant in enumerate(variants):
        variant_selected = {stage: dict(programs[identity], function=f'GraphSource{sources.index(programs[identity]["source"])}::' + programs[identity]['function'])
                            for stage, identity in zip(STAGES, callback_programs(variant))}
        callbacks, adapters = adapt_legacy_callbacks(variant_selected, str(index))
        variant_callbacks.append(callbacks)
        prefix += adapters
    body = 'SceneQuery TraceAndShade(' + body
    # scene_wrapper's include rows are replaced by the namespaced union above.
    body = re.sub(r'^#include "[^"\n]+"\n', '', body, flags=re.M)
    def dispatch(stage, arguments, returns=False):
        result = 'switch (GetGraphPassIndex()) {\n'
        for index, variant in enumerate(variants):
            if stage not in variant_callbacks[index]:
                continue  # A scene compute pass has no hit or miss callback; default: latches HasFailed.
            function = variant_callbacks[index][stage]
            slot = native_slots[variant] if native_ray_pipeline else (
                dispatch_slots[variant] if dispatch_slots is not None else index)
            fields = (named_resources or {}).get(variant) if stage == 'ray_generation' else None
            if fields is not None:
                values = ', '.join(buffers.record_view_argument(field) for field in fields)
                result += (f'case {slot}: {{ {function}Resources resources = {{ {values} }}; '
                           f'return {function}({arguments}, resources); }}\n')
                continue
            result += f'case {slot}: ' + ('return ' if returns else '') + function + '(' + arguments + ');'
            result += ('\n' if returns else ' break;\n')
        return result + ('default: graphFailureReason = LUDUS_SCENE_FAILURE_INVALID_ARGUMENT; return float3(-1); }' if returns
                         else 'default: FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT); break; }')
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
        buffer_liveness = ('        GraphBuffer0[0] = GraphBuffer1[0];\n'
                           '        GraphBuffer1[0] = uint4(interfaceHit.Triangle, 0, 0, 0);\n')
        if native_records:
            buffer_liveness = '        GraphBuffer0[0] = uint4(interfaceHit.Triangle, 0, 0, 0);\n'
        body = body.replace('    SceneContext scene;\n',
                        '    if (Dimensions.z == 0) {\n'
                        f'        Hit interfaceHit = {interface_trace}(float3(0), float3(0, 0, 1), 0, 1, false);\n'
                        + buffer_liveness +
                        ('        colors[0] = GraphColorInput.Load(int3(0));\n' if interface.get('color_input', False) else '') +
                        ('        GraphStorageTexture[uint2(0)] = GraphStorageTexture[uint2(0)] + colors[0];\n'
                         if interface.get('storage_texture', False) else '') +
                        ''.join(f'        GraphWritableTexture{index}[uint2(0)] = '
                                f'GraphWritableTexture{index}[uint2(0)] + colors[0];\n'
                                for index in range(named_texture_count)) +
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
