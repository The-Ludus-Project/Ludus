// Test-only kernel for slice_sampling_luminaire_estimator_tests.cpp: runs the slice-sampling-luminaire template's three
// proposals (render_graphs/slice_sampling_luminaire/main_shaders/slice_sampling.slang) at fixed receivers and returns
// per-invocation sums, so the host can check that slice sampling and its defensive mixture estimate the same
// irradiance as uniform panel-area sampling evaluated in the area measure, and that the seed weight W estimates the
// normalizer Z of Eq. (14). The receivers are unoccluded: the template's TestVisibility adds no factor for them.
// slice_sampling_luminaire_estimators_layout.h owns the ABI.
#include "math.slang"
#include "raster_bindings.slang"
#include "slice_sampling.slang"
#include "slice_sampling_luminaire_estimators_layout.h"

LUDUS_SHADER_BINDING(LUDUS_SLICE_SAMPLING_ESTIMATORS_RESULTS_BINDING) RWStructuredBuffer<uint4> Results;

static const uint2 ESTIMATOR_TEST_RANDOM_KEY = uint2(0x534c4943u, 0x45535431u);
static const uint RECEIVER_COUNT = LUDUS_SLICE_SAMPLING_ESTIMATORS_RECEIVER_COUNT;
// Receivers below the default panel (luminaire.slang): on the floor of the 2 x 2 x 2 test room where two tile
// footprints overlap (two roots per seed), where one footprint lies, between footprints where only Gaussian tails
// reach, on top of the room's block, on a tilted surface and on the left wall.
static const float3 RECEIVER_POSITIONS_WORLD[RECEIVER_COUNT] = {
    float3(0.1, -1, -0.2),
    float3(0.2, -1, 0.4),
    float3(-0.4, -1, 0.3),
    float3(0.07, -1, -0.05),
    float3(0.1, -0.3, -0.2),
    float3(0.3, -1, 0.2),
    float3(0.6, -1, -0.6),
    float3(-0.99, 0, 0.1),
};
static const float3 RECEIVER_NORMALS_WORLD[RECEIVER_COUNT] = {
    float3(0, 1, 0),
    float3(0, 1, 0),
    float3(0, 1, 0),
    float3(0, 1, 0),
    float3(0, 1, 0),
    float3(0.447214, 0.894427, 0),
    float3(0, 1, 0),
    float3(1, 0, 0),
};
static const float DEFENSIVE_FRACTION = 0.1;
// Replayed and sampled seed weights sum the same branch weights in the same order unless rounding moves a root across
// a tile edge; the relative tolerance only absorbs the quantile's and the solve's float rounding.
static const float REPLAY_RELATIVE_TOLERANCE = 0.0001;

/// Profile 0 is the template's default Graph settings. Profile 1 has no spread or twist: each tile's lobe is then
/// constant over the tile, the auxiliary map is piecewise constant with jumps, as in the draft's running example, and
/// the stronger steering makes the footprints of different tiles overlap more.
LuminaireProfile MakeTestProfile(uint profileIndex)
{
    LuminaireProfile profile;
    profile.RadianceScale = 6;
    profile.BeamWidth = 0.02;
    profile.BeamSteering = profileIndex == 0 ? 0.12 : 0.2;
    profile.BeamSpread = profileIndex == 0 ? 0.08 : 0;
    profile.BeamTwist = profileIndex == 0 ? 0.04 : 0;
    return profile;
}

float DrawUniform(uint invocation, uint sampleIndex, uint dimension)
{
    return SampleRandomFloat01(ESTIMATOR_TEST_RANDOM_KEY,
                               MakeRandomAddress(uint2(invocation, 0), sampleIndex, dimension));
}

/// Unoccluded irradiance contribution of one proposal at an upward-facing or tilted receiver.
float EstimateIrradiance(LuminaireLightSample light, float3 normalWorld)
{
    if (light.HasSample == false)
        return 0;
    float cosine = max(dot(normalWorld, light.DirectionWorld), 0);
    return dot(light.RadianceOverDensity, float3(1.0 / 3)) * cosine;
}

[shader("compute")][numthreads(LUDUS_SLICE_SAMPLING_ESTIMATORS_GROUP_SIZE, 1, 1)] void
SliceSamplingEstimatorsMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    uint caseIndex = invocation / LUDUS_SLICE_SAMPLING_ESTIMATORS_INVOCATIONS_PER_CASE;
    uint receiverIndex = caseIndex % RECEIVER_COUNT;
    LuminaireProfile profile = MakeTestProfile(caseIndex / RECEIVER_COUNT);
    LuminaireReceiver receiver = MakeLuminaireReceiver(RECEIVER_POSITIONS_WORLD[receiverIndex]);
    float3 normalWorld = RECEIVER_NORMALS_WORLD[receiverIndex];
    float defensiveSum = 0;
    float defensiveSquares = 0;
    float sliceSum = 0;
    float sliceSquares = 0;
    float areaSum = 0;
    float areaSquares = 0;
    float seedWeightSum = 0;
    float seedWeightSquares = 0;
    float normalizerSum = 0;
    float normalizerSquares = 0;
    float nullCount = 0;
    float replayMismatchCount = 0;
    float uniformAreaDensity = 0.25 * receiver.HeightPanel * receiver.HeightPanel;
    for (uint sampleIndex = 0; sampleIndex < LUDUS_SLICE_SAMPLING_ESTIMATORS_SAMPLES_PER_INVOCATION; ++sampleIndex)
    {
        float2 seedSample = float2(DrawUniform(invocation, sampleIndex, 0), DrawUniform(invocation, sampleIndex, 1));
        float branchSample = DrawUniform(invocation, sampleIndex, 2);
        float2 areaSample = float2(DrawUniform(invocation, sampleIndex, 3), DrawUniform(invocation, sampleIndex, 4));
        float componentSample = DrawUniform(invocation, sampleIndex, 5);
        // The mixture draws its own seeds, so it is independent of the two strategies alone.
        float2 mixtureSeedSample =
            float2(DrawUniform(invocation, sampleIndex, 6), DrawUniform(invocation, sampleIndex, 7));
        float mixtureBranchSample = DrawUniform(invocation, sampleIndex, 8);
        float2 mixtureAreaSample =
            float2(DrawUniform(invocation, sampleIndex, 9), DrawUniform(invocation, sampleIndex, 10));

        float defensive = EstimateIrradiance(SampleLuminaireDefensive(profile,
                                                                      receiver,
                                                                      DEFENSIVE_FRACTION,
                                                                      componentSample,
                                                                      mixtureSeedSample,
                                                                      mixtureBranchSample,
                                                                      mixtureAreaSample),
                                             normalWorld);
        defensiveSum += defensive;
        defensiveSquares += defensive * defensive;

        float seedWeight;
        float guideDensity;
        LuminaireLightSample slice =
            SampleLuminaireSlice(profile, receiver, seedSample, branchSample, seedWeight, guideDensity);
        float sliceEstimate = EstimateIrradiance(slice, normalWorld);
        sliceSum += sliceEstimate;
        sliceSquares += sliceEstimate * sliceEstimate;
        seedWeightSum += seedWeight;
        seedWeightSquares += seedWeight * seedWeight;
        if (slice.HasSample == false)
        {
            nullCount += 1;
        }
        else
        {
            // Replay (section 5): the slope's own seed must give back the seed weight it was sampled with.
            LuminaireTile tile = GetLuminaireTile(profile, slice.Tile);
            float2 standardSeed = ComputeStandardSeed(tile, slice.PositionPanelTangent, slice.SlopePanel);
            float replayed = ComputeSeedWeight(profile, receiver, standardSeed, slice.Tile);
            if (abs(replayed - seedWeight) > REPLAY_RELATIVE_TOLERANCE * seedWeight)
                replayMismatchCount += 1;
        }

        LuminaireLightSample area = SampleLuminaireArea(profile, receiver, areaSample);
        float areaEstimate = EstimateIrradiance(area, normalWorld);
        areaSum += areaEstimate;
        areaSquares += areaEstimate * areaEstimate;
        float normalizer = 0;
        if (area.HasSample == true)
        {
            LuminaireTile tile = GetLuminaireTile(profile, area.Tile);
            float2 standardSeed = ComputeStandardSeed(tile, area.PositionPanelTangent, area.SlopePanel);
            normalizer = tile.Strength * EvaluateEmissionDensity(tile, standardSeed) / uniformAreaDensity;
        }
        normalizerSum += normalizer;
        normalizerSquares += normalizer * normalizer;
    }
    uint base = invocation * LUDUS_SLICE_SAMPLING_ESTIMATORS_RECORDS_PER_INVOCATION;
    Results[base + LUDUS_SLICE_SAMPLING_ESTIMATORS_IRRADIANCE_RECORD] =
        asuint(float4(defensiveSum, defensiveSquares, sliceSum, sliceSquares));
    Results[base + LUDUS_SLICE_SAMPLING_ESTIMATORS_AREA_RECORD] =
        asuint(float4(areaSum, areaSquares, seedWeightSum, seedWeightSquares));
    Results[base + LUDUS_SLICE_SAMPLING_ESTIMATORS_NORMALIZER_RECORD] =
        asuint(float4(normalizerSum, normalizerSquares, nullCount, replayMismatchCount));
}
