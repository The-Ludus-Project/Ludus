// Test-only copies exercise failure boundaries without changing the live invocation.
bool ValidateSceneRandomLimits(SceneContext scene, RayLaunch launch, RayQueryContext queries)
{
    RayQueryContext uniformQueries = queries;
    uniformQueries.NextRandomDimension = RANDOM_MAX_DIMENSION;
    float last = SampleSceneRandom(uniformQueries);
    if (last != SampleUniformFloat01(launch, RANDOM_MAX_DIMENSION) || uniformQueries.HasFailed == true)
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
bool ValidateSceneWeightedSampling(RayLaunch launch, RayQueryContext queries)
{
    const uint DRAW_COUNT = 8;
    const uint WEIGHT_COUNT = 6;
    const float weights[WEIGHT_COUNT] = {0, 2, 0, 6, 0, 0};
    const float4 normalizedWeights = float4(0, 0.25f, 0, 0.75f);
    RayQueryContext arrayQueries = queries;
    RayQueryContext vectorQueries = queries;
    for (uint dimension = 0; dimension < DRAW_COUNT; ++dimension)
    {
        const uint expectedIndex = SampleUniformFloat01(launch, dimension) < 0.25f ? 1 : 3;
        if (SampleSceneWeightedIndex(arrayQueries, weights) != expectedIndex ||
            SampleSceneWeightedIndex(vectorQueries, normalizedWeights) != expectedIndex ||
            arrayQueries.HasFailed == true || vectorQueries.HasFailed == true ||
            arrayQueries.NextRandomDimension != dimension + 1 || vectorQueries.NextRandomDimension != dimension + 1)
            return false;
    }

    // Max+max overflows an unscaled sum. Smallest+smallest can flush on some shader backends.
    const float largest = asfloat(0x7f7fffffu);
    const float smallest = asfloat(1u);
    const uint expectedIndex = SampleUniformFloat01(launch, 0) < 0.5f ? 0 : 1;
    RayQueryContext largeQueries = queries;
    RayQueryContext smallQueries = queries;
    if (SampleSceneWeightedIndex(largeQueries, float2(largest, largest)) != expectedIndex ||
        SampleSceneWeightedIndex(smallQueries, float2(smallest, smallest)) != expectedIndex)
        return false;

    const uint INVALID_COUNT = 4;
    const float3 invalidWeights[INVALID_COUNT] = {
        float3(0),
        float3(1, -1, 0),
        float3(1, asfloat(0x7f800000u), 0),
        float3(1, asfloat(0x7fc00000u), 0),
    };
    for (uint index = 0; index < INVALID_COUNT; ++index)
    {
        RayQueryContext invalidQueries = queries;
        if (SampleSceneWeightedIndex(invalidQueries, invalidWeights[index]) != SCENE_INVALID_INDEX ||
            invalidQueries.HasFailed == false || invalidQueries.FailureReason != LUDUS_SCENE_FAILURE_INVALID_ARGUMENT ||
            invalidQueries.NextRandomDimension != 1)
            return false;
    }
    RayQueryContext lastQueries = queries;
    lastQueries.NextRandomDimension = RANDOM_MAX_DIMENSION;
    return SampleSceneWeightedIndex(lastQueries, float3(0, 1, 0)) == 1 &&
           SampleSceneWeightedIndex(lastQueries, float3(0, 1, 0)) == SCENE_INVALID_INDEX &&
           lastQueries.HasFailed == true && lastQueries.FailureReason == LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED;
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
        IncidentLightSample incident = SampleIncidentLight(scene,
                                                           lightIndex,
                                                           receiver,
                                                           launch,
                                                           isContinuous == true ? lastValidStart : exhaustedStart,
                                                           sampleQueries);
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
        if (incident.Measure != IncidentLightMeasure::SolidAngle || evaluatedIndex != lightIndex || evaluatedPdf <= 0 ||
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
// Sample above the fixture, where no triangle hit is needed to define the connection.
bool ValidateWorldPositionLightSampling(SceneContext scene, RayLaunch launch, RayQueryContext queries)
{
    const float3 positionWorld = float3(0, 0, 2);
    SurfaceHit shadingHit = (SurfaceHit)0;
    shadingHit.PositionWorld = positionWorld;
    RayQueryContext positionQueries = queries;
    RayQueryContext surfaceQueries = queries;
    SceneLightSample positionSample =
        SampleLight(scene, positionWorld, launch, 0, 1, LightSelectionPolicy::Uniform, positionQueries);
    SceneLightSample surfaceSample =
        SampleLight(scene, shadingHit, launch, 0, 1, LightSelectionPolicy::Uniform, surfaceQueries);
    if (positionQueries.HasFailed == true || surfaceQueries.HasFailed == true ||
        positionQueries.NextRandomDimension != queries.NextRandomDimension ||
        positionQueries.SurfaceQueries != queries.SurfaceQueries ||
        positionQueries.VisibilityQueries != queries.VisibilityQueries)
        return false;
    if (GetLightCount(scene) == 0)
        return positionSample.Selection.HasSample == false && positionSample.Incident.HasSample == false;
    if (positionSample.Selection.HasSample == false || surfaceSample.Selection.HasSample == false ||
        positionSample.Selection.LightIndex != surfaceSample.Selection.LightIndex ||
        positionSample.Selection.SelectionPmf != surfaceSample.Selection.SelectionPmf)
        return false;
    IncidentLightSample incident = positionSample.Incident;
    IncidentLightSample surfaceIncident = surfaceSample.Incident;
    if (incident.HasSample == false || surfaceIncident.HasSample == false ||
        incident.Measure != surfaceIncident.Measure ||
        incident.HasFiniteEndpoint != surfaceIncident.HasFiniteEndpoint ||
        incident.ConditionalProbability != surfaceIncident.ConditionalProbability ||
        any(incident.DirectionWorld != surfaceIncident.DirectionWorld) ||
        any(incident.IncidentCoefficient != surfaceIncident.IncidentCoefficient))
        return false;
    if (incident.HasFiniteEndpoint == false)
        return true;
    const float CONNECTION_RELATIVE_TOLERANCE = 0.0001;
    float3 offsetWorld = incident.PositionWorld - positionWorld;
    return incident.DistanceWorld > 0 && length(offsetWorld - incident.DirectionWorld * incident.DistanceWorld) <=
                                             CONNECTION_RELATIVE_TOLERANCE * incident.DistanceWorld;
}
bool ValidateWorldOriginVisibility(SceneContext scene, RayQueryContext queries)
{
    const float CONNECTION_DISTANCE_WORLD = 2;
    Ray ray;
    ray.OriginWorld = float3(0, 0, CONNECTION_DISTANCE_WORLD);
    ray.DirectionWorld = float3(0, 0, -1);
    ray.MinDistanceWorld = 0;
    ray.MaxDistanceWorld = asfloat(asuint(CONNECTION_DISTANCE_WORLD) - 1);
    RayQueryContext closestQueries = queries;
    SurfaceHit hit;
    SceneQuery expected = TraceClosest(scene, closestQueries, ray, hit);
    if (expected == SceneQuery::Failure)
        return false;

    IncidentLightSample incident = (IncidentLightSample)0;
    incident.TriangleIndex = SCENE_INVALID_INDEX;
    incident.PositionWorld = float3(0);
    incident.HasSample = true;
    incident.HasFiniteEndpoint = true;
    // The finite connection must be rebuilt from the origin, even when the sample points elsewhere.
    incident.DirectionWorld = -ray.DirectionWorld;
    RayQueryContext visibilityQueries = queries;
    visibilityQueries.MaximumVisibilityQueries = visibilityQueries.VisibilityQueries + 1;
    SceneQuery visibility = TestVisibility(scene, visibilityQueries, ray.OriginWorld, incident);
    if (visibility != expected || visibilityQueries.HasFailed == true ||
        visibilityQueries.VisibilityQueries != queries.VisibilityQueries + 1 ||
        visibilityQueries.SurfaceQueries != queries.SurfaceQueries)
        return false;
    SceneQuery exhausted = TestVisibility(scene, visibilityQueries, ray.OriginWorld, incident);
    return exhausted == SceneQuery::Failure && visibilityQueries.HasFailed == true &&
           visibilityQueries.FailureReason == LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET &&
           visibilityQueries.VisibilityQueries == queries.VisibilityQueries + 1;
}
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    if (ValidateSceneRandomLimits(scene, launch, queries) == false ||
        ValidateSceneIntegerSampling(launch, queries) == false ||
        ValidateSceneWeightedSampling(launch, queries) == false ||
        ValidateAddressedIncidentSampling(scene, launch, queries) == false ||
        ValidateWorldPositionLightSampling(scene, launch, queries) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    if (all(launch.PositionPixel == launch.ExtentPixel / 2) && ValidateWorldOriginVisibility(scene, queries) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    // Check automatic initialization against the independently addressed public API.
    float first = SampleSceneRandom(queries);
    if (first != SampleUniformFloat01(launch, 0))
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    payload.ExpectedMissRandom = SampleUniformFloat01(launch, 1);
    payload.ExpectedLightIndex = SelectLight(scene, launch, 1).LightIndex;
    uint shapeDimension = GetLightCount(scene) > 0 ? 2 : 1;
    payload.ExpectedRandom = float3(SampleUniformFloat01(launch, shapeDimension),
                                    SampleUniformFloat01(launch, shapeDimension + 1),
                                    SampleUniformFloat01(launch, shapeDimension + 2));
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
