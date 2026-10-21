// Test-only copies exercise failure boundaries without changing the live invocation.
bool ValidateSceneRandomLimits(SceneContext scene, RayLaunch launch, RayQueryContext queries)
{
    RayQueryContext uniformQueries = queries;
    uniformQueries.NextRandomDimension = RANDOM_MAX_DIMENSION;
    float last = SampleSceneRandom(uniformQueries);
    if (last != SampleSceneRandom(launch, RANDOM_MAX_DIMENSION) || uniformQueries.HasFailed == true)
        return false;
    float exhausted = SampleSceneRandom(uniformQueries);
    if (exhausted != 0 || uniformQueries.HasFailed == false ||
        uniformQueries.NextRandomDimension != RANDOM_MAX_DIMENSION + 1)
        return false;
    if (SampleSceneRandom(uniformQueries) != 0 || SelectLight(scene, uniformQueries).HasSample == true ||
        uniformQueries.NextRandomDimension != RANDOM_MAX_DIMENSION + 1)
        return false;
    if (GetLightCount(scene) > 0)
    {
        RayQueryContext selectionQueries = queries;
        selectionQueries.NextRandomDimension = RANDOM_MAX_DIMENSION;
        SelectedLight selected = SelectLight(scene, selectionQueries);
        SelectedLight expected = SelectLight(scene, launch, RANDOM_MAX_DIMENSION);
        if (selected.HasSample == false || selected.LightIndex != expected.LightIndex ||
            selectionQueries.HasFailed == true)
            return false;
        if (SelectLight(scene, selectionQueries).HasSample == true || selectionQueries.HasFailed == false ||
            selectionQueries.NextRandomDimension != RANDOM_MAX_DIMENSION + 1)
            return false;
    }
    return true;
}
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    if (ValidateSceneRandomLimits(scene, launch, queries) == false)
        queries.HasFailed = true;
    // Check automatic initialization against the independently addressed public API.
    float first = SampleSceneRandom(queries);
    if (first != SampleSceneRandom(launch, 0))
        queries.HasFailed = true;
    payload.ExpectedMissRandom = SampleSceneRandom(launch, 1);
    payload.ExpectedLightIndex = SelectLight(scene, launch, 1).LightIndex;
    uint shapeDimension = GetLightCount(scene) > 0 ? 2 : 1;
    payload.ExpectedRandom = float3(SampleSceneRandom(launch, shapeDimension),
                                    SampleSceneRandom(launch, shapeDimension + 1),
                                    SampleSceneRandom(launch, shapeDimension + 2));
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    return payload.Radiance;
}
