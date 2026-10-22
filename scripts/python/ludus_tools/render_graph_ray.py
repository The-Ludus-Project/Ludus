"""Hardware triangle ray pipelines using native queries and authored functions.

The renderer owns native acceleration structures and the packed shading tables. The graph owns
stage assignment, launch extent and publication. No CPU transport is selected.
"""
from copy import deepcopy
import re
from . import render_graph_buffers as buffers

TECHNIQUE = 'RendererTriangles.RayTracing'
STAGES = ('ray_generation', 'miss', 'closest_hit')
MAX_RAY_PASSES = 4
MAX_BUFFERS = 2
BUFFER_WORD_BYTES = 16
MAX_BUFFER_PIXEL_BYTES = 4096
MAX_BUFFER_BYTES = 256 * 1024 * 1024
PASS_INDEX_SHIFT = 8


def program_tuple(node):
    assignments = {shader['stage']: shader['program'] for shader in node['shaders']}
    return tuple(assignments[stage] for stage in STAGES)

# Thanks to Microsoft, DirectX Raytracing Functional Spec, “Intersection Attributes”:
# https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#intersection-attributes
# Triangle intersections supply float2 barycentrics. This native-query profile
# retains that contract, but uses shader-local ScenePayload rather than a DXR payload.
STATE = {'max_recursion': 1, 'payload_bytes': 0, 'attribute_bytes': 8}
MAX_LOCAL_PAYLOAD_BYTES = 4096
PAYLOAD_FIELD_ALIGNMENT_BYTES = 16
# One owner for build admission and the editor controls. Editor CMake reads this
# data through a module import at configure time; no Python interpreter enters the UI.
SCENE_QUERY_LIMITS = {
    'MIN_SURFACE_QUERIES': 1,
    'MAX_SURFACE_QUERIES': 8,
    'MIN_VISIBILITY_QUERIES': 0,
    'MAX_VISIBILITY_QUERIES': 16,
}

COMPUTE_ID = 'GraphRayCompute'
IMAGE_VERTEX_ID = 'GraphRayImageVertex'
IMAGE_INPUTS = [{'location': 0, 'format': 'Float2'}]
IMAGE_FRAGMENT_ID = 'GraphRayImageFragment'
# Files are installed by their owning renderer/math/RHI targets beside the helper.
HELPERS = ('scene_data.h', 'renderer_gpu_preview_layout.h', 'renderer_surface_functions.slang', 'recipes.json',
           'lighting_layout.h', 'math.slang', 'raster_bindings.slang', 'scene_shader.slang', 'scene_shader_layout.h', 'ray_shader.slang')


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
    types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {}
    for aspect, kind in (('scene', 'Scene'), ('view', 'View'), ('color', 'ColorTexture')):
        choices = [identity for identity, value in types.items() if value == kind]
        graph._require(len(choices) == 1, 'IncompletePipeline', where, f'exactly one {kind} input required',
                       field='inputs', expected=f'one {kind} input', actual=f'{len(choices)} inputs',
                       repair=f'Add or retain exactly one {kind} input in the pipeline editor.')
        mappings[aspect] = choices[0]
    storage = {name for name, kind in types.items() if kind == 'Buffer'}
    graph._require(len(storage) <= buffers.MAX_BUFFERS, 'Capacity', where, 'at most two Buffer inputs supported')
    mappings.update({'buffer:' + name: name for name in storage})
    unsupported = sorted(set(types) - set(mappings.values()))
    descriptions = ', '.join(f"'{identity}' ({types[identity]})" for identity in unsupported)
    graph._require(not unsupported, 'Capability', where,
                   f"Unsupported ray pipeline inputs: {descriptions}. This triangle ray profile accepts only "
                   'Scene, View, ColorTexture and up to two Buffers. Remove the extra input declarations and their wires; '
                   'if exposed by a subgraph, also remove the matching interface inputs and parent wires.',
                   port=unsupported[0] if unsupported else None, direction='input',
                   expected='Scene, View, ColorTexture', actual=descriptions,
                   repair='Remove unsupported pipeline inputs and their wires; also update exposed subgraph interfaces.')
    colors = [port for port in node['outputs'] if port['type'] == 'ColorTexture']
    graph._require(len(colors) == 1, 'Capability', where, 'triangle ray profile requires one color output')
    output = colors[0]
    graph._require(output['target'] == mappings['color'] and output['load'] == 'Clear' and output['store'] == 'Store',
                   'Capability', where, 'ray output must Clear/Store its ColorTexture target')
    outputs = {'color': output['id']}
    for port in node['outputs']:
        if port is output:
            continue
        graph._require(port['type'] == 'Buffer' and port['target'] in storage and
                       port['load'] == 'Load' and port['store'] == 'Store', 'Capability', where,
                       'writable structured buffers require Load/Store on a paired Buffer input')
        outputs['buffer:' + port['target']] = port['id']
    record = {'id': node['id'], 'kind': 'RayTracing', 'color_load': output['load'], 'color_store': output['store']}
    return record, mappings, outputs


def validate_program(program, build):
    legacy = {'ray_generation': ('RayLaunch', 'Ray'), 'miss': ('Ray', 'Color'),
              'closest_hit': ('RayHit', 'Color')}
    scene = {'ray_generation': ('RayLaunch', 'Radiance'), 'miss': ('Ray', 'ScenePayload'),
             'closest_hit': ('SurfaceHit', 'ScenePayload')}
    signature = (program['input'], program['output'])
    build.require(signature in (legacy[program['stage']], scene[program['stage']]),
                  'Schema', program['id'], 'ray function interface differs from declared profile')


def expand_programs(programs, technique, snapshot, helper, build, variants=None, persistent=False, schemas=None):
    """Capture the engine kernel and all three functions in one linked compute entry."""
    selected = {stage: programs[identity] for stage, identity in zip(STAGES, variants[0])} if variants else {stage: programs[technique[stage]] for stage in STAGES}
    storage = buffers.merge_bindings(selected, build)
    build.require(not persistent or not storage, 'Capability', TECHNIQUE, 'ordered ray passes use Buffer storage; StructuredBuffer remains the single-job profile')
    storage_source = buffers.declarations(storage, schemas or {}, build)
    recipes = build.document(build.read(helper / 'recipes.json'), 'renderer recipes')
    compute_recipe = recipes['renderer_gpu_preview']
    image_recipe = recipes['renderer_gpu_preview_image']
    for filename in (*HELPERS, compute_recipe['source'], image_recipe['source']):
        data = build.read(helper / filename)
        relative = '_ludus_ray/' + filename
        build.require(relative not in snapshot.files, 'Source', relative, 'reserved engine ray helper path')
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
    if technique.get('scene_interface') is not None:
        wrapper = scene_wrapper(selected, technique['scene_interface'], snapshot, build, compute_recipe, storage_source)
    if persistent:
        build.require(technique.get('scene_interface') is not None, 'Capability', TECHNIQUE,
                      'ordered ray passes and persistent buffers require the scene callback interface')
        wrapper = persistent_wrapper(programs, technique, variants, snapshot, build, compute_recipe)
    result = {identity: deepcopy(program) for identity, program in programs.items() if program['stage'] not in STAGES}
    result[COMPUTE_ID] = {'id': COMPUTE_ID, 'stage': 'compute', 'source': selected['ray_generation']['source'],
                          'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': wrapper, 'native_ray_queries': True, 'native_ray_profile': True, 'persistent_ray_profile': persistent, 'storage_bindings': storage, 'storage_schemas': {binding['schema']: schemas[binding['schema']] for binding in storage}}
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
    if program['id'] == COMPUTE_ID:
        expected = [(0, 'UniformBuffer', 224), (1, 'StorageRead', 16), (2, 'StorageReadWrite', 16),
                    (3, 'StorageReadWrite', 16), (4, 'StorageRead', 16), (5, 'RayScene', 0)]
        if program.get('persistent_ray_profile'):
            expected += [(6, 'StorageReadWrite', 16), (7, 'StorageReadWrite', 16)]
        build.require(interface.get('workgroup') == [64, 1, 1] and interface['inputs'] == [] and
                      [(entry['binding'], entry['kind'], entry['size']) for entry in entries if program.get('persistent_ray_profile') or entry['binding'] < buffers.FIRST_SLOT] ==
                      expected,
                      'ShaderInterface', program['id'], 'ray query reflection differs from the native renderer ABI')
        declared = program.get('storage_bindings', [])
        extra = [entry for entry in entries if entry['binding'] >= buffers.FIRST_SLOT and not program.get('persistent_ray_profile')]
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


def scene_wrapper(selected, interface, snapshot, build, recipe, storage_source=''):
    """Link ordinary callbacks; the user raygen is the only sample/integrator driver."""
    expected = {'ray_generation': ('RayLaunch', 'Radiance'), 'miss': ('Ray', 'ScenePayload'),
                'closest_hit': ('SurfaceHit', 'ScenePayload')}
    for stage, program in selected.items():
        build.require((program['input'], program['output']) == expected[stage],
                      'ShaderInterface', program['id'], 'scene ray profile requires its complete callback interface')
    snapshot.shader_closure(interface['payload'])
    validate_payload(snapshot.files[interface['payload']], interface.get('local_payload_bytes'), build)
    wrapper = '''float3 EvaluateGraphRay(uint pixel);
#define LUDUS_RENDER_GRAPH_RAY_PIPELINE 1
#define LUDUS_SCENE_RAY_INTERFACE 1
#include "_ludus_ray/renderer_gpu_preview.slang"
#include "_ludus_ray/scene_shader.slang"
'''
    wrapper += storage_source
    wrapper += '#include "' + interface['payload'] + '"\n'
    wrapper += 'uint TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    for source in sorted({program['source'] for program in selected.values()}):
        wrapper += '#include "' + source + '"\n'
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
    RayLaunch launch;
    launch.PositionPixel = uint2(pixel % Dimensions.x, pixel / Dimensions.x);
    launch.ExtentPixel = Dimensions.xy;
    launch.CameraClipToWorld = ClipToWorld;
    launch.CameraPositionWorld = CameraPositionWorld.xyz;
    launch.IsOrthographic = CameraPositionWorld.w != 0;
    launch.SampleIndex = uint(accumulation[pixel * 2].w);
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
    wrapper = wrapper.replace('MAX_SURFACE_QUERIES', str(interface['surface_queries']))
    wrapper = wrapper.replace('MAX_VISIBILITY_QUERIES', str(interface['visibility_queries']))
    for token, stage in (('RAY_GENERATION', 'ray_generation'), ('RAY_MISS', 'miss'), ('RAY_CLOSEST_HIT', 'closest_hit')):
        wrapper = wrapper.replace(token, selected[stage]['function'])
    return wrapper.replace('_ludus_ray/renderer_gpu_preview.slang', '_ludus_ray/' + recipe['source'])


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


def persistent_wrapper(programs, technique, variants, snapshot, build, recipe):
    """Namespace each unique authored source and dispatch its callbacks by pass index."""
    interface = technique['scene_interface']
    sources = sorted({programs[identity]['source'] for variant in variants for identity in variant})
    selected = {stage: dict(programs[identity], function=f'GraphSource{sources.index(programs[identity]["source"])}::' + programs[identity]['function'])
                for stage, identity in zip(STAGES, variants[0])}
    wrapper = scene_wrapper(selected, interface, snapshot, build, recipe)
    prefix, body = wrapper.split('uint TraceAndShade(', 1)
    prefix = prefix.replace('#define LUDUS_SCENE_RAY_INTERFACE 1',
                            '#define LUDUS_SCENE_RAY_INTERFACE 1\n#define LUDUS_GRAPH_PERSISTENT_PIPELINE 1')
    prefix += '[[vk::binding(6, 0)]] RWStructuredBuffer<uint4> GraphBuffer0;\n'
    prefix += '[[vk::binding(7, 0)]] RWStructuredBuffer<uint4> GraphBuffer1;\n'
    prefix += 'uint GetGraphPassIndex() { return (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK; }\n'
    prefix += 'uint TraceAndShade(SceneContext scene, inout RayQueryContext queries, Ray ray, inout ScenePayload payload);\n'
    # Shared type/helper includes are global and captured before entry-source namespaces.
    # Includes are compiler-resolved from the same immutable source snapshot.
    dependencies = {path for source in sources for path in snapshot.shader_closure(source)} - set(sources)
    for dependency in sorted(dependencies - {interface['payload']}):
        prefix += '#include "' + dependency + '"\n'
    for index, source in enumerate(sources):
        prefix += f'namespace GraphSource{index} {{\n#include "{source}"\n}}\n'
    body = 'uint TraceAndShade(' + body
    # scene_wrapper's include rows are replaced by the namespaced union above.
    body = re.sub(r'^#include "[^"\n]+"\n', '', body, flags=re.M)
    def dispatch(stage, arguments, returns=False):
        result = 'switch (GetGraphPassIndex()) {\n'
        for index, variant in enumerate(variants):
            program = programs[variant[STAGES.index(stage)]]
            function = f'GraphSource{sources.index(program["source"])}::' + program['function']
            result += f'case {index}: ' + ('return ' if returns else '') + function + '(' + arguments + ');'
            result += ('\n' if returns else ' break;\n')
        return result + ('default: return float3(-1); }' if returns else 'default: queries.HasFailed = true; break; }')
    body = body.replace(selected['closest_hit']['function'] + '(scene, hit, queries, payload);',
                        '{ ' + dispatch('closest_hit', 'scene, hit, queries, payload') + ' }')
    body = body.replace(selected['miss']['function'] + '(scene, ray, queries, payload);',
                        '{ ' + dispatch('miss', 'scene, ray, queries, payload') + ' }')
    body = body.replace('float3 radiance = ' + selected['ray_generation']['function'] + '(scene, launch, queries);',
                        'float3 radiance = DispatchGraphRay(scene, launch, queries);')
    dispatch_function = ('float3 DispatchGraphRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) {\n' +
                         dispatch('ray_generation', 'scene, launch, queries', True) + '\n}\n')
    # Preserve both fixed slots under optimization even for callback sets without storage uses.
    body = body.replace('    SceneContext scene;\n',
                        '    if (Dimensions.z == 0) { GraphBuffer0[0] = GraphBuffer1[0]; GraphBuffer1[0] = uint4(0); }\n    SceneContext scene;\n')
    body = body.replace('uint(accumulation[pixel * 2].w)', 'asuint(accumulation[pixel * 2].w)')
    return prefix + dispatch_function + body
