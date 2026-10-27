// Ray path capture fixture (render-graph-authoring.md#ray-path-inspection-proposed): a scene
// compute kernel whose queries have known results in the capture test scene. A camera closest
// query, then a visibility query toward +Z, through the occluder behind the first surface, and
// one toward -Z, back past the camera. A final preview visibility query checks that its
// capture variant retains the full hit record. Red encodes the accumulated sample.
static const float VISIBILITY_ENDPOINT_DISTANCE_WORLD = 2;
static const float SAMPLE_COLOR_SCALE = 0.125;

LightSample MakeEndpointSample(float3 positionWorld, float3 endpointWorld)
{
    LightSample light = (LightSample)0;
    light.LightIndex = SCENE_INVALID_INDEX;
    light.TriangleIndex = SCENE_INVALID_INDEX;
    light.PositionWorld = endpointWorld;
    light.DirectionWorld = normalize(endpointWorld - positionWorld);
    light.DistanceWorld = length(endpointWorld - positionWorld);
    light.SelectionPmf = 1;
    light.ConditionalPdf = 1;
    light.IsDiscrete = true;
    light.HasFiniteEndpoint = true;
    light.HasSample = true;
    return light;
}

float3 TraceRayPathFixture(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    float sampleColor = float(launch.SampleIndex + 1) * SAMPLE_COLOR_SCALE;
    SurfaceHit hit;
    if (TraceClosest(scene, queries, launch.CameraRay, hit) != SCENE_QUERY_SURFACE)
        return float3(0, 0, sampleColor);
    float3 forwardWorld = hit.PositionWorld + float3(0, 0, VISIBILITY_ENDPOINT_DISTANCE_WORLD);
    float3 backwardWorld = hit.PositionWorld - float3(0, 0, VISIBILITY_ENDPOINT_DISTANCE_WORLD);
    uint occluded = TestVisibility(scene, queries, hit, MakeEndpointSample(hit.PositionWorld, forwardWorld));
    uint unoccluded = TestVisibility(scene, queries, hit, MakeEndpointSample(hit.PositionWorld, backwardWorld));
    // Test-only access to the preview helper: capture must record this shadow too.
    float3 previewDirectionWorld = float3(0, 0, 1);
    float3 previewOriginWorld = OffsetPositionWorld(hit.PositionWorld, hit.TriangleIndex, previewDirectionWorld);
    VisibilityResult previewVisibility =
        TraceVisibility(previewOriginWorld, previewDirectionWorld, 0, VISIBILITY_ENDPOINT_DISTANCE_WORLD);
    bool isOccluded =
        occluded == SCENE_QUERY_SURFACE && previewVisibility.HasFailed == false && previewVisibility.IsOccluded == true;
    return float3(sampleColor, isOccluded == true ? 1 : 0, unoccluded == SCENE_QUERY_MISS ? 1 : 0);
}
