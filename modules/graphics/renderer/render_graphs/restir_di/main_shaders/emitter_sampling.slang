#pragma once
// Adapted from the user-supplied closest-hit shader. Retains its emitter/triangle proposal modes.
#define LUDUS_EMITTER_SAMPLING_MODE_UNIFORM (0)
#define LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED (1)
#ifndef LUDUS_EMITTER_SAMPLING_MODE
#    define LUDUS_EMITTER_SAMPLING_MODE LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED
#endif

// Set to 0 to omit orientation from the emitter-selection heuristic.
#ifndef LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED_ORIENTATION
#    define LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED_ORIENTATION (1)
#endif

#define LUDUS_TRIANGLE_SAMPLING_MODE_UNIFORM (0)
#define LUDUS_TRIANGLE_SAMPLING_MODE_AREA_WEIGHTED (1)
#ifndef LUDUS_TRIANGLE_SAMPLING_MODE
#    define LUDUS_TRIANGLE_SAMPLING_MODE LUDUS_TRIANGLE_SAMPLING_MODE_AREA_WEIGHTED
#endif

#if LUDUS_EMITTER_SAMPLING_MODE != LUDUS_EMITTER_SAMPLING_MODE_UNIFORM &&                                              \
    LUDUS_EMITTER_SAMPLING_MODE != LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED
#    error "Choose a valid emitter sampling mode"
#endif
#if LUDUS_TRIANGLE_SAMPLING_MODE != LUDUS_TRIANGLE_SAMPLING_MODE_UNIFORM &&                                            \
    LUDUS_TRIANGLE_SAMPLING_MODE != LUDUS_TRIANGLE_SAMPLING_MODE_AREA_WEIGHTED
#    error "Choose a valid triangle sampling mode"
#endif
#if LUDUS_EMITTER_SAMPLING_MODE == LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED
float ComputeEmitterWeight(float3 shadingPositionWorld,
                           float3 emitterCenterWorld,
                           float emitterRadiusWorld,
                           float minimumDistanceWorld)
{
    const float3 offsetWorld = emitterCenterWorld - shadingPositionWorld;
    const float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    const float minimumDistanceSquaredWorld = minimumDistanceWorld * minimumDistanceWorld;
    const float radiusSquaredWorld = emitterRadiusWorld * emitterRadiusWorld;
    return 1.0f / max(distanceSquaredWorld, max(radiusSquaredWorld, minimumDistanceSquaredWorld));
}

float ComputeSceneEmitterWeight(SceneContext scene, uint emitterIndex, SurfaceHit receiver)
{
    const float MINIMUM_DISTANCE_WORLD = 0.001f;
    const float TRIANGLE_VERTEX_COUNT = 3.0f;
#    if LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED_ORIENTATION
    const float MINIMUM_DISTANCE_SQUARED_WORLD = MINIMUM_DISTANCE_WORLD * MINIMUM_DISTANCE_WORLD;
    const float ANGULAR_WEIGHT_FLOOR = 0.001f;
    const float3 receiverNormalWorld = normalize(receiver.ShadingNormalWorld);
#    endif
    const uint triangleCount = uint(GetEmitter(scene, emitterIndex).y);
    float emitterWeight = 0.0f;
    for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        if (triangle.IsValid == false || triangle.AreaWorld <= 0.0f)
        {
            continue;
        }
        const float3 centerWorld =
            (triangle.Position0World + triangle.Position1World + triangle.Position2World) / TRIANGLE_VERTEX_COUNT;
        const float radiusWorld =
            max(length(triangle.Position0World - centerWorld),
                max(length(triangle.Position1World - centerWorld), length(triangle.Position2World - centerWorld)));
        float angularWeight = 1.0f;
#    if LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED_ORIENTATION
        const float3 toEmitterWorld = centerWorld - receiver.PositionWorld;
        const float distanceSquaredWorld = dot(toEmitterWorld, toEmitterWorld);
        if (distanceSquaredWorld > MINIMUM_DISTANCE_SQUARED_WORLD)
        {
            const float3 directionWorld = toEmitterWorld * rsqrt(distanceSquaredWorld);
            const float3 emitterNormalWorld = normalize(triangle.GeometricNormalWorld);
            const MaterialSample emitterMaterial = GetMaterial(scene, triangle.MaterialIndex);
            const float cosSurface = max(dot(receiverNormalWorld, directionWorld), 0.0f);
            const float emitterFacing = dot(emitterNormalWorld, -directionWorld);
            const float cosEmitter =
                emitterMaterial.IsEmissionTwoSided == true ? abs(emitterFacing) : max(emitterFacing, 0.0f);
            angularWeight = cosSurface * cosEmitter;
        }
        // A centroid approximation must not exclude contributing regions.
        angularWeight = max(angularWeight, ANGULAR_WEIGHT_FLOOR);
#    endif
        const float distanceWeight =
            ComputeEmitterWeight(receiver.PositionWorld, centerWorld, radiusWorld, MINIMUM_DISTANCE_WORLD);
        emitterWeight += triangle.AreaWorld * distanceWeight * angularWeight;
    }
    return emitterWeight;
}
#endif

// The total weight is retained so a BSDF hit can evaluate the same emitter PMF.
bool TrySelectEmitter(SceneContext scene,
                      SurfaceHit receiver,
                      inout RayQueryContext queries,
                      out uint outEmitterIndex,
                      out float outSelectionPmf,
                      out float outTotalEmitterWeight)
{
    outEmitterIndex = 0;
    outSelectionPmf = 0.0f;
    outTotalEmitterWeight = 0.0f;
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterCount == 0)
    {
        return false;
    }
#if defined(LUDUS_DEBUG_LIGHT_SOURCE_INDEX)
    outEmitterIndex = uint(LUDUS_DEBUG_LIGHT_SOURCE_INDEX) % emitterCount;
    outSelectionPmf = 1.0f;
    outTotalEmitterWeight = 1.0f;
#elif LUDUS_EMITTER_SAMPLING_MODE == LUDUS_EMITTER_SAMPLING_MODE_UNIFORM
    outEmitterIndex = SampleSceneRandom(queries, 0u, emitterCount);
    outSelectionPmf = 1.0f / float(emitterCount);
    outTotalEmitterWeight = float(emitterCount);
#else
    float selectedWeight = 0.0f;
    // Thanks to Pharr, Jakob and Humphreys, PBRT 4e, Reservoir Sampling:
    // https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling
    // Adapted single-item weighted reservoir selection for emitter indices.
    for (uint i = 0; i < emitterCount; ++i)
    {
        const float weight = ComputeSceneEmitterWeight(scene, i, receiver);
        if (isfinite(weight) == false || weight < 0.0f)
        {
            queries.HasFailed = true;
            return false;
        }
        if (weight == 0.0f)
        {
            continue;
        }
        outTotalEmitterWeight += weight;
        if (isfinite(outTotalEmitterWeight) == false)
        {
            queries.HasFailed = true;
            return false;
        }
        const float randomValue = SampleSceneRandom(queries);
        if (queries.HasFailed == true)
        {
            return false;
        }
        if (randomValue < weight / outTotalEmitterWeight)
        {
            outEmitterIndex = i;
            selectedWeight = weight;
        }
    }
    if (outTotalEmitterWeight <= 0.0f || selectedWeight <= 0.0f)
    {
        return false;
    }
    outSelectionPmf = selectedWeight / outTotalEmitterWeight;
#endif
    return queries.HasFailed == false;
}

float EvaluateEmitterSelectionPmf(SceneContext scene, SurfaceHit receiver, uint emitterIndex, float totalEmitterWeight)
{
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterIndex >= emitterCount)
    {
        return 0.0f;
    }
#if defined(LUDUS_DEBUG_LIGHT_SOURCE_INDEX)
    return emitterIndex == uint(LUDUS_DEBUG_LIGHT_SOURCE_INDEX) % emitterCount ? 1.0f : 0.0f;
#elif LUDUS_EMITTER_SAMPLING_MODE == LUDUS_EMITTER_SAMPLING_MODE_UNIFORM
    return 1.0f / float(emitterCount);
#else
    return totalEmitterWeight > 0.0f ? ComputeSceneEmitterWeight(scene, emitterIndex, receiver) / totalEmitterWeight
                                     : 0.0f;
#endif
}

float EvaluateConditionalEmitterPdfArea(SceneContext scene, uint emitterIndex, uint triangleIndex)
{
    const float4 emitter = GetEmitter(scene, emitterIndex);
#if LUDUS_TRIANGLE_SAMPLING_MODE == LUDUS_TRIANGLE_SAMPLING_MODE_AREA_WEIGHTED
    return emitter.z > 0.0f ? 1.0f / emitter.z : 0.0f;
#else
    const TriangleData triangle = GetTriangle(scene, triangleIndex);
    return triangle.IsValid == true && triangle.AreaWorld > 0.0f && emitter.y > 0.0f
               ? 1.0f / (emitter.y * triangle.AreaWorld)
               : 0.0f;
#endif
}

bool TrySampleEmitterTriangle(SceneContext scene,
                              uint emitterIndex,
                              inout RayQueryContext queries,
                              out TriangleData outTriangle,
                              out float outConditionalPdfArea)
{
    outTriangle = (TriangleData)0;
    outConditionalPdfArea = 0.0f;
    const float4 emitter = GetEmitter(scene, emitterIndex);
    const uint triangleCount = uint(emitter.y);
    if (triangleCount == 0)
    {
        return false;
    }
#if LUDUS_TRIANGLE_SAMPLING_MODE == LUDUS_TRIANGLE_SAMPLING_MODE_UNIFORM
    const uint triangleIndex = SampleSceneRandom(queries, 0u, triangleCount);
    outTriangle = GetEmitterTriangle(scene, emitterIndex, triangleIndex);
#else
    const float totalAreaWorld = emitter.z;
    if (isfinite(totalAreaWorld) == false || totalAreaWorld <= 0.0f)
    {
        queries.HasFailed = true;
        return false;
    }
    const float targetAreaWorld = SampleSceneRandom(queries) * totalAreaWorld;
    float cumulativeAreaWorld = 0.0f;
    for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        if (triangle.IsValid == false)
        {
            queries.HasFailed = true;
            outTriangle = (TriangleData)0;
            return false;
        }
        if (triangle.AreaWorld <= 0.0f)
        {
            continue;
        }
        // Last positive-area triangle is the cumulative-rounding fallback.
        outTriangle = triangle;
        cumulativeAreaWorld += triangle.AreaWorld;
        if (targetAreaWorld < cumulativeAreaWorld)
        {
            break;
        }
    }
#endif
    if (queries.HasFailed == true || outTriangle.IsValid == false || outTriangle.AreaWorld <= 0.0f)
    {
        return false;
    }
    outConditionalPdfArea = EvaluateConditionalEmitterPdfArea(scene, emitterIndex, outTriangle.TriangleIndex);
    return isfinite(outConditionalPdfArea) == true && outConditionalPdfArea > 0.0f;
}
