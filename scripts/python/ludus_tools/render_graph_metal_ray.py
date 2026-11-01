"""Native Metal stage dispatch, keeping authored Slang callbacks shared with compute."""


def make_wrapper(prefix, dispatch_function, body, hit_dispatch, miss_dispatch):
    """Generate three visible stage functions and the shared query/payload boundary."""
    packet = '''
struct NativeRayStagePacket
{
    SceneContext Scene;
    RayLaunch Launch;
    Ray RayWorld;
    SurfaceHit Hit;
    RayQueryContext Queries;
    ScenePayload Payload;
    float3 Radiance;
};
__target_intrinsic(metal, "LudusDispatchNativeStage($0, $1, $2, LUDUS_NATIVE_CONTEXT)")
void InvokeNativeRayStage(StructuredBuffer<NativeRayFunctionTableToken> functions,
                          inout NativeRayStagePacket packet, uint stage);
'''
    stages = '''[noinline]
void LudusNativeRayGeneration(inout NativeRayStagePacket packet)
{
    packet.Radiance = DispatchGraphRay(packet.Scene, packet.Launch, packet.Queries);
    if (Dimensions.z == 0) FailSceneQuery(packet.Queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
}
'''
    for name, dispatch in (('LudusNativeClosestHit', hit_dispatch), ('LudusNativeMiss', miss_dispatch)):
        stages += f'''[noinline]
void {name}(inout NativeRayStagePacket packet)
{{
    SceneContext scene = packet.Scene;
    SurfaceHit hit = packet.Hit;
    Ray ray = packet.RayWorld;
    RayQueryContext queries = packet.Queries;
    ScenePayload payload = packet.Payload;
    {dispatch}
    packet.Queries = queries;
    packet.Payload = payload;
    if (Dimensions.z == 0) FailSceneQuery(packet.Queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
}}
'''
    for name, arguments, assignments, index in (
        ('ShadeNativeClosest', 'SurfaceHit hit', 'packet.Hit = hit;', 1),
        ('ShadeNativeMiss', 'Ray ray', 'packet.RayWorld = ray;', 2)):
        stages += f'''void {name}(SceneContext scene, {arguments}, inout RayQueryContext queries,
                               inout ScenePayload payload)
{{
    NativeRayStagePacket packet = (NativeRayStagePacket)0;
    packet.Scene = scene;
    {assignments}
    packet.Queries = queries;
    packet.Payload = payload;
    InvokeNativeRayStage(NativeRayFunctions, packet, {index});
    queries = packet.Queries;
    payload = packet.Payload;
}}
'''
    stages += '''float3 GenerateNativeRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    NativeRayStagePacket packet = (NativeRayStagePacket)0;
    packet.Scene = scene;
    packet.Launch = launch;
    packet.Queries = queries;
    // Graph jobs use positive depth. Retain native stage entry definitions for
    // post-Slang opaque-resource legalization and independent driver linking.
    if (Dimensions.z == 0)
    {
        LudusNativeRayGeneration(packet);
        LudusNativeClosestHit(packet);
        LudusNativeMiss(packet);
    }
    InvokeNativeRayStage(NativeRayFunctions, packet, 0);
    queries = packet.Queries;
    return packet.Radiance;
}
'''
    body = body.replace('{ ' + hit_dispatch + ' }', '{ ShadeNativeClosest(scene, hit, queries, payload); }')
    body = body.replace('{ ' + miss_dispatch + ' }', '{ ShadeNativeMiss(scene, ray, queries, payload); }')
    body = body.replace('float3 radiance = DispatchGraphRay(scene, launch, queries);',
                        'float3 radiance = GenerateNativeRay(scene, launch, queries);')
    return '#define LUDUS_NATIVE_METAL_RAY_PIPELINE 1\n' + prefix + packet + dispatch_function + stages + body
