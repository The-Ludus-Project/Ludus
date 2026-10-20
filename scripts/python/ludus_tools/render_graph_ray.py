"""Portable triangle ray pipelines lowered to GPU traversal and authored functions.

The renderer owns the packed BVH and its shader implementation. The graph owns
stage assignment, launch extent and publication. No CPU transport is selected.
"""
from copy import deepcopy

TECHNIQUE = 'RendererTriangles.RayTracing'
STAGES = ('ray_generation', 'miss', 'closest_hit')
STATE = {'max_recursion': 1, 'payload_bytes': 0, 'attribute_bytes': 8}
COMPUTE_ID = 'GraphRayCompute'
IMAGE_VERTEX_ID = 'GraphRayImageVertex'
IMAGE_FRAGMENT_ID = 'GraphRayImageFragment'
# Files are installed by their owning renderer/math/RHI targets beside the helper.
HELPERS = ('renderer_gpu_preview_layout.h', 'renderer_surface_functions.slang', 'recipes.json',
           'lighting_layout.h', 'math.slang', 'raster_bindings.slang')


def lower(node, where, graph):
    assignments = {shader['stage']: shader['program'] for shader in node['shaders']}
    graph._require(set(assignments) == set(STAGES) and len(node['shaders']) == len(STAGES),
                   'IncompletePipeline', where, 'triangle ray pipelines require one ray_generation, miss and closest_hit program')
    graph._require(node['state'] == STATE, 'Capability', where,
                   'triangle ray profile requires max_recursion=1, payload_bytes=0 and attribute_bytes=8')
    types = {port['id']: port['type'] for port in node['inputs']}
    mappings = {}
    for aspect, kind in (('scene', 'Scene'), ('view', 'View'), ('color', 'ColorTexture')):
        choices = [identity for identity, value in types.items() if value == kind]
        graph._require(len(choices) == 1, 'IncompletePipeline', where, f'exactly one {kind} input required')
        mappings[aspect] = choices[0]
    graph._require(len(types) == len(mappings) and len(node['outputs']) == 1, 'Capability', where,
                   'triangle ray profile accepts Scene, View and ColorTexture, and writes one color output')
    output = node['outputs'][0]
    graph._require(output['type'] == 'ColorTexture' and output['target'] == mappings['color'] and
                   output['load'] == 'Clear' and output['store'] == 'Store', 'Capability', where,
                   'ray output must Clear/Store its ColorTexture target')
    record = {'id': node['id'], 'kind': 'RayTracing', 'color_load': output['load'], 'color_store': output['store']}
    return record, mappings, {'color': output['id']}


def validate_program(program, build):
    build.require(program['input'] == {'ray_generation': 'RayLaunch', 'miss': 'Ray', 'closest_hit': 'RayHit'}[program['stage']]
                  and program['output'] == ('Ray' if program['stage'] == 'ray_generation' else 'Color')
                  and program['bindings'] == [], 'Schema', program['id'], 'ray function interface differs from triangle ray profile')


def expand_programs(programs, technique, snapshot, helper, build):
    """Capture the engine kernel and all three functions in one linked compute entry."""
    selected = {stage: programs[technique[stage]] for stage in STAGES}
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
struct Ray
{
    float3 OriginWorld;
    float MinDistanceWorld;
    float3 DirectionWorld;
    float MaxDistanceWorld;
};
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    Ray CameraRay;
};
struct RayHit
{
    Ray IncomingRay;
    float3 PositionWorld;
    float DistanceWorld;
    float3 NormalWorld;
    uint Primitive;
    float2 Barycentric;
    float4 Color;
};
'''
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
    result = {identity: deepcopy(program) for identity, program in programs.items() if program['stage'] not in STAGES}
    result[COMPUTE_ID] = {'id': COMPUTE_ID, 'stage': 'compute', 'source': selected['ray_generation']['source'],
                          'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': wrapper}
    for identity, stage, function in ((IMAGE_VERTEX_ID, 'vertex', image_recipe['vertex']), (IMAGE_FRAGMENT_ID, 'fragment', image_recipe['fragment'])):
        # Use the same renderer-owned image recipe; its interface is validated by the runtime.
        source = '#include "_ludus_ray/' + image_recipe['source'] + '"\n'
        suffix = 'VertexOutput rgEntry(VertexInput value) { return ' + function + '(value); }' if stage == 'vertex' else 'float4 rgEntry(VertexOutput value) : SV_Target0 { return ' + function + '(value); }'
        source += '[shader("' + stage + '")]\n' + suffix + '\n'
        result[identity] = {'id': identity, 'stage': stage, 'source': selected['ray_generation']['source'],
                            'function': function, 'input': '', 'output': '', 'bindings': [], 'ray_wrapper': source}
    build.require(not set(result) & {program['id'] for program in selected.values()}, 'Schema', 'ray programs', 'reserved linked program identity')
    build.require(snapshot.total <= build.MAX_BYTES and len(snapshot.files) <= build.MAX_FILES, 'Capacity', 'ray sources', 'source snapshot limits exceeded')
    return result


def check_interface(program, result, build):
    """Validate the frozen renderer ABI from independently reflected artifacts."""
    interface = result['interface']
    entries = sorted(interface['entries'], key=lambda value: value['binding'])
    if program['id'] == COMPUTE_ID:
        build.require(interface.get('workgroup') == [64, 1, 1] and interface['inputs'] == [] and
                      [(entry['binding'], entry['kind'], entry['size']) for entry in entries] ==
                      [(0, 'UniformBuffer', 224), (1, 'StorageRead', 16), (2, 'StorageReadWrite', 16),
                       (3, 'StorageReadWrite', 16), (4, 'StorageRead', 16)],
                      'ShaderInterface', program['id'], 'ray compute reflection differs from renderer BVH ABI')
    elif program['id'] == IMAGE_VERTEX_ID:
        build.require([(entry['binding'], entry['kind'], entry['size']) for entry in entries] == [(0, 'UniformBuffer', 16)]
                      and [(value['location'], value['format']) for value in interface['inputs']] == [(0, 'Float2'), (1, 'Float4')],
                      'ShaderInterface', program['id'], 'ray image vertex interface differs from renderer ABI')
    else:
        build.require(entries == [] and interface['inputs'] == [], 'ShaderInterface', program['id'], 'ray image fragment resources are unexpected')
