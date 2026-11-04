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
// Test-only copies compare range endpoints and stream advancement with FoundationMath.
bool ValidateSceneIntegerSampling(RayLaunch launch, RayQueryContext queries)
{
    const uint RANGE_COUNT = 8;
    const uint DRAW_COUNT = 8;
    const uint2 ranges[RANGE_COUNT] = {
        uint2(0, 1),
        uint2(5, 6),
        uint2(0, 7),
        uint2(17, 1031),
        uint2(0, 0x80000001),
        uint2(0xfffffffe, 0xffffffff),
        uint2(0xffffff00, 0xffffffff),
        uint2(0, 0xffffffff),
    };
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    for (uint rangeIndex = 0; rangeIndex < RANGE_COUNT; ++rangeIndex)
    {
        RayQueryContext rangeQueries = queries;
        uint minimumInclusive = ranges[rangeIndex].x;
        uint maximumExclusive = ranges[rangeIndex].y;
        for (uint dimension = 0; dimension < DRAW_COUNT; ++dimension)
        {
            uint expectedOffset;
            if (TrySampleRandomBounded(launch.Seed,
                                       PixelRandomAddress(pixel, launch.SampleIndex, dimension),
                                       maximumExclusive - minimumInclusive,
                                       expectedOffset) == false)
                return false;
            uint value = SampleSceneRandom(rangeQueries, minimumInclusive, maximumExclusive);
            if (value != minimumInclusive + expectedOffset || value < minimumInclusive || value >= maximumExclusive ||
                rangeQueries.HasFailed == true || rangeQueries.NextRandomDimension != dimension + 1)
                return false;
        }
    }
    const uint INVALID_RANGE_COUNT = 2;
    const uint2 invalidRanges[INVALID_RANGE_COUNT] = {uint2(5, 5), uint2(6, 5)};
    for (uint rangeIndex = 0; rangeIndex < INVALID_RANGE_COUNT; ++rangeIndex)
    {
        RayQueryContext invalidQueries = queries;
        if (SampleSceneRandom(invalidQueries, invalidRanges[rangeIndex].x, invalidRanges[rangeIndex].y) != 0 ||
            invalidQueries.HasFailed == false || invalidQueries.NextRandomDimension != 0)
            return false;
        if (SampleSceneRandom(invalidQueries, 0, 1) != 0 || invalidQueries.NextRandomDimension != 0)
            return false;
    }
    RayQueryContext lastQueries = queries;
    lastQueries.NextRandomDimension = RANDOM_MAX_DIMENSION;
    if (SampleSceneRandom(lastQueries, 5, 6) != 5 || lastQueries.HasFailed == true ||
        lastQueries.NextRandomDimension != RANDOM_MAX_DIMENSION + 1)
        return false;
    if (SampleSceneRandom(lastQueries, 5, 6) != 0 || lastQueries.HasFailed == false ||
        lastQueries.NextRandomDimension != RANDOM_MAX_DIMENSION + 1)
        return false;
    return true;
}
// The mixed-light fixture binds directional, point, spot, mesh and environment sources.
bool ValidateAddressedIncidentSampling(SceneContext scene, RayLaunch launch, RayQueryContext queries)
{
    SurfaceHit receiver = (SurfaceHit)0;
    // The fixture's geometry and punctual sources are below this virtual receiver.
    receiver.PositionWorld = float3(0, 0, 2);
    const uint lastValidStart = RANDOM_MAX_DIMENSION - (SCENE_INCIDENT_LIGHT_DIMENSION_COUNT - 1);
    const uint exhaustedStart = RANDOM_MAX_DIMENSION + 1;
    const float PDF_RELATIVE_TOLERANCE = 0.0001;
    const uint lightCount = GetLightCount(scene);
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        LightInfo source = GetLightInfo(scene, lightIndex);
        const bool isContinuous = source.Kind == SceneLightKind::Mesh || source.Kind == SceneLightKind::Environment;
        RayQueryContext sampleQueries = queries;
        IncidentLightSample incident = SampleIncidentLight(
            scene, lightIndex, receiver, launch, isContinuous == true ? lastValidStart : exhaustedStart, sampleQueries);
        if (incident.HasSample == false || sampleQueries.HasFailed == true ||
            sampleQueries.NextRandomDimension != queries.NextRandomDimension)
            return false;
        if (isContinuous == false)
        {
            if (incident.Measure != IncidentLightMeasure::Discrete || incident.ConditionalProbability != 1)
                return false;
            continue;
        }

        float evaluatedPdf;
        uint evaluatedIndex;
        if (source.Kind == SceneLightKind::Mesh)
        {
            SurfaceHit endpoint = GetSurfaceHit(scene, incident.TriangleIndex, incident.Barycentric);
            evaluatedPdf = EvaluateIncidentLightPdfSolidAngle(scene, receiver.PositionWorld, endpoint);
            evaluatedIndex = GetSurfaceLightIndex(scene, endpoint);
        }
        else
        {
            evaluatedPdf = EvaluateIncidentEnvironmentPdfSolidAngle(scene);
            evaluatedIndex = GetEnvironmentLightIndex(scene);
        }
        if (incident.Measure != IncidentLightMeasure::SolidAngle || evaluatedIndex != lightIndex ||
            evaluatedPdf <= 0 ||
            abs(evaluatedPdf - incident.ConditionalProbability) > PDF_RELATIVE_TOLERANCE * evaluatedPdf)
            return false;

        IncidentLightSample exhausted =
            SampleIncidentLight(scene, lightIndex, receiver, launch, lastValidStart + 1, sampleQueries);
        if (exhausted.HasSample == true || sampleQueries.HasFailed == false ||
            sampleQueries.FailureReason != LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED ||
            sampleQueries.NextRandomDimension != queries.NextRandomDimension)
            return false;
    }
    IncidentLightSample absent = SampleIncidentLight(scene, lightCount, receiver, launch, exhaustedStart, queries);
    return absent.HasSample == false && queries.HasFailed == false;
}
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    if (ValidateSceneRandomLimits(scene, launch, queries) == false ||
        ValidateSceneIntegerSampling(launch, queries) == false ||
        ValidateAddressedIncidentSampling(scene, launch, queries) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    // Check automatic initialization against the independently addressed public API.
    float first = SampleSceneRandom(queries);
    if (first != SampleSceneRandom(launch, 0))
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    payload.ExpectedMissRandom = SampleSceneRandom(launch, 1);
    payload.ExpectedLightIndex = SelectLight(scene, launch, 1).LightIndex;
    uint shapeDimension = GetLightCount(scene) > 0 ? 2 : 1;
    payload.ExpectedRandom = float3(SampleSceneRandom(launch, shapeDimension),
                                    SampleSceneRandom(launch, shapeDimension + 1),
                                    SampleSceneRandom(launch, shapeDimension + 2));
    uint expectedIntegerOffset;
    const uint INTEGER_RANGE_MINIMUM = 5;
    const uint INTEGER_RANGE_WIDTH = 7;
    bool hasIntegerSample = TrySampleRandomBounded(
        launch.Seed,
        PixelRandomAddress(launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x,
                           launch.SampleIndex,
                           shapeDimension + 3),
        INTEGER_RANGE_WIDTH,
        expectedIntegerOffset);
    if (hasIntegerSample == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    payload.ExpectedInteger = INTEGER_RANGE_MINIMUM + expectedIntegerOffset;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    return payload.Radiance;
}
