#pragma once
#include "transport.slang"

// Samples, reservoirs and their resampling for the ReSTIR GI template.
//
// Thanks to Yaobin Ouyang, Shiqiu Liu, Markus Kettunen, Matt Pharr and Jacopo Pantaleoni, "ReSTIR GI: Path
// Resampling for Real-Time Path Tracing", Computer Graphics Forum 40(8) (High-Performance Graphics 2021), pp. 17-29,
// https://doi.org/10.1111/cgf.14378 . Adopted from the paper:
// - the three image-sized buffers of section 4 (initial samples, temporal reservoirs, spatial reservoirs), each
//   sample identified by its visible point and its sample point together with the sample point's radiance;
// - weighted reservoir sampling and reservoir merging (their Algorithm 1, after Bitterli et al.);
// - the target function of Equation 10 (outgoing radiance) or, as a Graph setting, Equation 9;
// - the reconnection Jacobian of Equation 11 when a sample moves to another visible point;
// - Algorithm 4's spatial reuse with the geometric similarity test (25 degrees, 5% depth) and its unbiased count Z,
//   the M limits of section 5 (30 temporal, 500 spatial), its adaptive search radius (10% of the image, halved on a
//   rejected neighbour down to 3 pixels) and its neighbour counts for sparse and well-sampled spatial reservoirs.
// Departures, all documented in docs/development/render-graph-authoring.md#use-the-built-in-restir-gi-template:
// - Algorithm 4 writes p'_q = p_q / |J|, while the text divides the source PDF by |J|. The merge here multiplies
//   the resampling weight by |J_{q->r}| = dw_r / dw_q, which is what dividing the source PDF by it means.
// - The target function is zero where the sample point lies below the receiver's hemisphere, so a receiver only
//   selects samples that its own source distribution can generate.
// - There are no motion vectors: temporal history is the same pixel's reservoir, accepted when its visible point
//   passes the spatial similarity test and shifted to this frame's visible point with the Jacobian.
// - Spatial reuse merges the pixel's previous spatial reservoir, its own temporal reservoir, then neighbours'
//   temporal reservoirs. Reusing neighbours' spatial reservoirs (their Figure 10) would race with their writers.
// - Static scenes only: World edits start a new scene generation, which empties every reservoir, so the periodic
//   sample validation of section 4.3 is not needed and not implemented.
//
// Thanks to Benedikt Bitterli, Chris Wyman, Matt Pharr, Peter Shirley, Aaron Lefohn and Wojciech Jarosz,
// "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting", ACM TOG 39(4), 2020,
// https://doi.org/10.1145/3386569.3392481 , for the reservoir algorithms the paper builds on.

// Two uint4 words per pixel in the `samples` Buffer (32 bytes per pixel), written by the initial pass and read by
// the temporal pass of the same frame:
//   word 0: sample triangle (SCENE_INVALID_INDEX without a sample), asuint(barycentric.xy), asuint(source PDF);
//           a pixel whose initial pass failed stores SCENE_INVALID_INDEX, its failure reason, 0, SAMPLE_FAILED_WORD
//   word 1: asuint(L_o.rgb), the frame's SampleIndex
static const uint SAMPLE_RECORD_WORDS = 2;
static const uint SAMPLE_FAILED_WORD = 0xffffffffu;

// Four uint4 words per pixel in the `temporal` and `spatial` Buffers (64 bytes per pixel):
//   word 0: sample triangle, asuint(sample barycentric.xy), asuint(W)
//   word 1: asuint(L_o.rgb), M
//   word 2: visible-point triangle (SCENE_INVALID_INDEX for a camera miss), asuint(visible-point barycentric.xy),
//           the SampleIndex of the frame that wrote it
//   word 3: scene generation, packed extent (x | y << 16), layout version << 8 | failure, where failure is zero or one
//           more than the LUDUS_SCENE_FAILURE_* reason of a pass that failed
// Zeroed memory (a new or resized buffer) decodes as an empty reservoir of version 0, which no pass accepts.
static const uint RESERVOIR_RECORD_WORDS = 4;
static const uint RESERVOIR_LAYOUT_VERSION = 1;
static const uint RESERVOIR_FAILURE_MASK = 0xff;
static const uint MAXIMUM_EXTENT_PIXELS = 0xffff;

// GraphBuffer slots, in the order each pass lists its Buffer inputs.
static const bool FIRST_BUFFER = false;
static const bool SECOND_BUFFER = true;

static const uint SOURCE_UNIFORM_HEMISPHERE = 0;
static const uint SOURCE_COSINE_HEMISPHERE = 1;

// Section 4.2: normals within 25 degrees and normalized depths within 0.05, following Bitterli et al.
static const float SIMILARITY_NORMAL_COSINE = 0.906307787f;
static const float SIMILARITY_DEPTH_FRACTION = 0.05f;
// Section 5: the first search radius is 10% of the image resolution (here its larger dimension), halved after each
// rejected neighbour down to 3 pixels.
static const float SPATIAL_INITIAL_RADIUS_FRACTION = 0.1f;
static const float SPATIAL_MINIMUM_RADIUS_PIXELS = 3;
// Spatial reuse traces up to one visibility ray per neighbour for its merge and one for its count Z, plus two for the
// previous spatial reservoir: 2 * 7 + 2 = 16, the scene interface's visibility budget.
static const uint SPATIAL_NEIGHBOR_CAPACITY = 7;

// Candidates of one resampling, identified so that the count Z can skip the candidate that supplied the sample.
static const uint CANDIDATE_NONE = 0xffffffffu;
static const uint CANDIDATE_INITIAL = 0;
static const uint CANDIDATE_HISTORY = 1;
static const uint CANDIDATE_OWN_TEMPORAL = 2;
static const uint CANDIDATE_FIRST_NEIGHBOR = 3;

// Initial pass random dimensions: the source direction, then one VERTEX_DIMENSION_COUNT range per path vertex.
static const uint INITIAL_SOURCE_DIMENSION_FIRST = 0;
static const uint INITIAL_PATH_DIMENSION_FIRST = INITIAL_SOURCE_DIMENSION_FIRST + 2;
// Resampling passes: one reservoir coin per candidate, then the neighbour offsets (two per neighbour).
static const uint RESAMPLING_COIN_DIMENSION_FIRST = 0;
static const uint SPATIAL_OFFSET_DIMENSION_FIRST =
    RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_FIRST_NEIGHBOR + SPATIAL_NEIGHBOR_CAPACITY;
// Shade pass: one direct-lighting vertex.
static const uint SHADE_DIRECT_DIMENSION_FIRST = 0;

/// A sample point and the radiance it scatters toward the visible point that generated it (the paper's SAMPLE, whose
/// visible point is the reservoir's receiver). Positions and normals are rebuilt from the triangle.
struct GiSample
{
    uint TriangleIndex; ///< SCENE_INVALID_INDEX without a sample.
    float2 Barycentric;
    float3 RadianceOut; ///< L_o(x_s, w_o) in linear RGB.
};

/// An initial sample with its source PDF in solid angle at the visible point.
struct SampleRecord
{
    GiSample Sample;
    float SourcePdf;
    uint SampleIndex;
    bool HasFailed;
    uint FailureReason; ///< The initial pass's LUDUS_SCENE_FAILURE_* reason when HasFailed.
};

/// A reservoir of the paper's Algorithm 1 with its finalized weight W (Equation 7) and the visible point it belongs to.
struct Reservoir
{
    GiSample Sample;
    float Weight;
    uint CandidateCount;
    uint ReceiverTriangleIndex; ///< SCENE_INVALID_INDEX when the camera ray missed.
    float2 ReceiverBarycentric;
    uint SampleIndex;
    uint2 SceneGeneration;
    uint2 ExtentPixel;
    uint LayoutVersion;
    bool HasFailed;
    uint FailureReason; ///< The writing pass's LUDUS_SCENE_FAILURE_* reason when HasFailed.
};

/// A visible point: where a pixel's camera ray hits, with the interpolated normal around which it samples directions.
struct Receiver
{
    SurfaceHit Hit;
    float3 NormalWorld;
    bool IsValid;
};

/// The current visible point and what its target function needs.
struct TargetContext
{
    Receiver Point;
    ShadingPoint Shading;
};

uint GetPixelIndex(RayLaunch launch)
{
    return launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
}

uint GetBufferWords(bool isSecond)
{
    uint wordCount;
    uint stride;
    if (isSecond == true)
        GraphBuffer1.GetDimensions(wordCount, stride);
    else
        GraphBuffer0.GetDimensions(wordCount, stride);
    return wordCount;
}

/// True when GraphBuffer0 holds firstWords and GraphBuffer1 holds secondWords (zero: unused) per pixel of the launch.
bool HasRecordCapacity(RayLaunch launch, uint firstWords, uint secondWords)
{
    if (launch.ExtentPixel.x == 0 || launch.ExtentPixel.y == 0 || launch.ExtentPixel.x > MAXIMUM_EXTENT_PIXELS ||
        launch.ExtentPixel.y > MAXIMUM_EXTENT_PIXELS)
        return false;
    bool hasFirst = launch.ExtentPixel.x <= GetBufferWords(FIRST_BUFFER) / firstWords / launch.ExtentPixel.y;
    bool hasSecond =
        secondWords == 0 || launch.ExtentPixel.x <= GetBufferWords(SECOND_BUFFER) / secondWords / launch.ExtentPixel.y;
    return hasFirst == true && hasSecond == true;
}

uint4 LoadWord(bool isSecond, uint address)
{
    return isSecond == true ? GraphBuffer1[address] : GraphBuffer0[address];
}

void StoreWord(bool isSecond, uint address, uint4 value)
{
    if (isSecond == true)
        GraphBuffer1[address] = value;
    else
        GraphBuffer0[address] = value;
}

/// The `samples` Buffer is always the first Buffer input of the passes that use it.
SampleRecord LoadSampleRecord(uint pixel)
{
    uint address = pixel * SAMPLE_RECORD_WORDS;
    uint4 sampleWord = GraphBuffer0[address];
    uint4 radianceWord = GraphBuffer0[address + 1];
    SampleRecord result = (SampleRecord)0;
    result.Sample.TriangleIndex = sampleWord.x;
    result.Sample.Barycentric = asfloat(sampleWord.yz);
    result.Sample.RadianceOut = asfloat(radianceWord.xyz);
    result.HasFailed = sampleWord.x == SCENE_INVALID_INDEX && sampleWord.w == SAMPLE_FAILED_WORD;
    result.FailureReason = result.HasFailed == true ? sampleWord.y : 0;
    result.SourcePdf = result.HasFailed == true ? 0 : asfloat(sampleWord.w);
    result.SampleIndex = radianceWord.w;
    return result;
}

void StoreSampleRecord(uint pixel, SampleRecord record)
{
    uint address = pixel * SAMPLE_RECORD_WORDS;
    bool hasSample = record.HasFailed == false && record.Sample.TriangleIndex != SCENE_INVALID_INDEX;
    if (record.HasFailed == true)
        GraphBuffer0[address] = uint4(SCENE_INVALID_INDEX, record.FailureReason, 0, SAMPLE_FAILED_WORD);
    else
        GraphBuffer0[address] = uint4(hasSample == true ? record.Sample.TriangleIndex : SCENE_INVALID_INDEX,
                                      asuint(record.Sample.Barycentric),
                                      hasSample == true ? asuint(record.SourcePdf) : 0);
    GraphBuffer0[address + 1] = uint4(asuint(record.Sample.RadianceOut), record.SampleIndex);
}

/// An empty reservoir of this frame, scene and extent, without a visible point.
Reservoir MakeEmptyReservoir(SceneContext scene, RayLaunch launch)
{
    Reservoir result = (Reservoir)0;
    result.Sample.TriangleIndex = SCENE_INVALID_INDEX;
    result.ReceiverTriangleIndex = SCENE_INVALID_INDEX;
    result.SampleIndex = launch.SampleIndex;
    result.SceneGeneration = scene.Generation;
    result.ExtentPixel = launch.ExtentPixel;
    result.LayoutVersion = RESERVOIR_LAYOUT_VERSION;
    return result;
}

Reservoir LoadReservoir(bool isSecond, uint pixel)
{
    uint address = pixel * RESERVOIR_RECORD_WORDS;
    uint4 sampleWord = LoadWord(isSecond, address);
    uint4 radianceWord = LoadWord(isSecond, address + 1);
    uint4 receiverWord = LoadWord(isSecond, address + 2);
    const uint IDENTITY_WORD = 3;
    uint4 identityWord = LoadWord(isSecond, address + IDENTITY_WORD);
    Reservoir result;
    result.Sample.TriangleIndex = sampleWord.x;
    result.Sample.Barycentric = asfloat(sampleWord.yz);
    result.Weight = asfloat(sampleWord.w);
    result.Sample.RadianceOut = asfloat(radianceWord.xyz);
    result.CandidateCount = radianceWord.w;
    result.ReceiverTriangleIndex = receiverWord.x;
    result.ReceiverBarycentric = asfloat(receiverWord.yz);
    result.SampleIndex = receiverWord.w;
    result.SceneGeneration = identityWord.xy;
    result.ExtentPixel = uint2(identityWord.z & MAXIMUM_EXTENT_PIXELS, identityWord.z >> 16);
    result.LayoutVersion = identityWord.w >> 8;
    const uint failure = identityWord.w & RESERVOIR_FAILURE_MASK;
    result.HasFailed = failure != 0;
    result.FailureReason = failure != 0 ? failure - 1 : 0;
    return result;
}

void StoreReservoir(bool isSecond, uint pixel, Reservoir reservoir)
{
    uint address = pixel * RESERVOIR_RECORD_WORDS;
    StoreWord(isSecond,
              address,
              uint4(reservoir.Sample.TriangleIndex, asuint(reservoir.Sample.Barycentric), asuint(reservoir.Weight)));
    StoreWord(isSecond, address + 1, uint4(asuint(reservoir.Sample.RadianceOut), reservoir.CandidateCount));
    StoreWord(isSecond,
              address + 2,
              uint4(reservoir.ReceiverTriangleIndex, asuint(reservoir.ReceiverBarycentric), reservoir.SampleIndex));
    const uint IDENTITY_WORD = 3;
    uint failure = reservoir.HasFailed == true ? reservoir.FailureReason + 1 : 0;
    StoreWord(isSecond,
              address + IDENTITY_WORD,
              uint4(reservoir.SceneGeneration,
                    (reservoir.ExtentPixel.x & MAXIMUM_EXTENT_PIXELS) | (reservoir.ExtentPixel.y << 16),
                    (reservoir.LayoutVersion << 8) | (failure & RESERVOIR_FAILURE_MASK)));
}

/// A reservoir that this scene, extent and layout can read, with a finite weight and an M within maxCandidates.
bool HasMatchingIdentity(Reservoir source, SceneContext scene, RayLaunch launch, uint maxCandidates)
{
    return source.LayoutVersion == RESERVOIR_LAYOUT_VERSION && source.HasFailed == false &&
           all(source.SceneGeneration == scene.Generation) == true &&
           all(source.ExtentPixel == launch.ExtentPixel) == true && source.CandidateCount <= maxCandidates &&
           isfinite(source.Weight) == true && source.Weight >= 0 && all(isfinite(source.Sample.RadianceOut)) == true &&
           any(source.Sample.RadianceOut < 0) == false;
}

/// A reservoir that this frame's earlier pass wrote for its pixel: same frame, not failed, readable.
bool IsWrittenThisFrame(Reservoir source, SceneContext scene, RayLaunch launch, uint maxCandidates)
{
    return source.SampleIndex == launch.SampleIndex && HasMatchingIdentity(source, scene, launch, maxCandidates);
}

Receiver MakeReceiver(SurfaceHit hit)
{
    Receiver result;
    result.Hit = hit;
    result.NormalWorld = NormalizeOrZero(hit.ShadingNormalWorld);
    result.IsValid = hit.IsValid == true && any(result.NormalWorld != 0) == true;
    return result;
}

/// Rebuild a stored visible point without tracing a ray.
Receiver LoadReceiver(SceneContext scene, uint triangleIndex, float2 barycentric)
{
    if (triangleIndex == SCENE_INVALID_INDEX)
        return (Receiver)0;
    return MakeReceiver(GetSurfaceHit(scene, triangleIndex, barycentric));
}

bool AreSameReceiver(Reservoir source, Receiver current)
{
    return source.ReceiverTriangleIndex == current.Hit.TriangleIndex &&
           all(source.ReceiverBarycentric == current.Hit.Barycentric) == true;
}

/// Camera-relative depth: the distance from the eye, or along the view direction for orthographic cameras.
float ComputeViewDepth(RayLaunch launch, float3 positionWorld)
{
    float3 offsetWorld = positionWorld - launch.CameraPositionWorld;
    return launch.IsOrthographic == true ? abs(dot(offsetWorld, launch.CameraRay.DirectionWorld)) : length(offsetWorld);
}

/// Section 4.2's geometric similarity test between this pixel's visible point and another one.
bool AreReceiversSimilar(RayLaunch launch, Receiver current, Receiver other)
{
    if (current.IsValid == false || other.IsValid == false)
        return false;
    float currentDepth = ComputeViewDepth(launch, current.Hit.PositionWorld);
    float otherDepth = ComputeViewDepth(launch, other.Hit.PositionWorld);
    return dot(current.NormalWorld, other.NormalWorld) >= SIMILARITY_NORMAL_COSINE &&
           abs(otherDepth - currentDepth) <= SIMILARITY_DEPTH_FRACTION * currentDepth;
}

/// Orthonormal tangents of a unit normal.
/// Thanks to Tom Duff, James Burgess, Per Christensen, Christophe Hery, Andrew Kensler, Max Liani and Ryusuke
/// Villemin, "Building an Orthonormal Basis, Revisited", JCGT 6(1), 2017, https://jcgt.org/published/0006/01/01/ ,
/// whose branchless construction this is.
void MakeTangentFrame(float3 normalWorld, out float3 outTangentWorld, out float3 outBitangentWorld)
{
    float orientation = normalWorld.z >= 0 ? 1.0f : -1.0f;
    float a = -1.0f / (orientation + normalWorld.z);
    float b = normalWorld.x * normalWorld.y * a;
    outTangentWorld =
        float3(1.0f + orientation * normalWorld.x * normalWorld.x * a, orientation * b, -orientation * normalWorld.x);
    outBitangentWorld = float3(b, orientation + normalWorld.y * normalWorld.y * a, -normalWorld.y);
}

/// Section 4.1's source distribution at a visible point: the uniform or cosine-weighted hemisphere around its
/// interpolated normal (RESTIR_GI_SOURCE_DISTRIBUTION). Returns the solid-angle PDF; zero rejects the sample.
float SampleSourceDirection(Receiver receiver, float2 random, out float3 outDirectionWorld)
{
    float3 tangentWorld;
    float3 bitangentWorld;
    MakeTangentFrame(receiver.NormalWorld, tangentWorld, bitangentWorld);
    float cosTheta = RESTIR_GI_SOURCE_DISTRIBUTION == SOURCE_COSINE_HEMISPHERE ? sqrt(1 - random.x) : random.x;
    float sinTheta = sqrt(max(1 - cosTheta * cosTheta, 0));
    float phi = 2 * PI * random.y;
    outDirectionWorld = NormalizeOrZero(tangentWorld * (sinTheta * cos(phi)) + bitangentWorld * (sinTheta * sin(phi)) +
                                        receiver.NormalWorld * cosTheta);
    if (cosTheta <= 0 || any(outDirectionWorld != 0) == false)
        return 0;
    return RESTIR_GI_SOURCE_DISTRIBUTION == SOURCE_COSINE_HEMISPHERE ? cosTheta / PI : 1 / (2 * PI);
}

/// Whether the visible point's source distribution can propose the sample point: in its hemisphere and, for the
/// unbiased variant, visible from it. This is the test of Algorithm 4 line 18, which decides the count Z.
bool CanGenerateSample(SceneContext scene,
                       Receiver receiver,
                       SurfaceHit samplePoint,
                       bool shouldTestVisibility,
                       inout RayQueryContext queries)
{
    float3 offsetWorld = samplePoint.PositionWorld - receiver.Hit.PositionWorld;
    float distanceWorld = length(offsetWorld);
    if (receiver.IsValid == false || samplePoint.IsValid == false || distanceWorld <= 0 ||
        dot(receiver.NormalWorld, offsetWorld) <= 0)
        return false;
    if (shouldTestVisibility == false)
        return true;
    LightSample connection = (LightSample)0;
    connection.TriangleIndex = samplePoint.TriangleIndex;
    connection.Barycentric = samplePoint.Barycentric;
    connection.PositionWorld = samplePoint.PositionWorld;
    connection.DirectionWorld = offsetWorld / distanceWorld;
    connection.DistanceWorld = distanceWorld;
    connection.HasFiniteEndpoint = true;
    connection.HasSample = true;
    return TestVisibility(scene, queries, receiver.Hit, connection) == SCENE_QUERY_MISS;
}

/// Equation 11, |J_{q->r}| = (|cos phi_2^r| / |cos phi_2^q|) * (|x_1^q - x_2^q|^2 / |x_1^r - x_2^q|^2): the ratio of
/// the destination's solid angle to the source's for the same sample point. Zero for a degenerate connection.
float ComputeReconnectionJacobian(float3 sourceReceiverWorld, float3 destinationReceiverWorld, SurfaceHit samplePoint)
{
    float3 normalWorld = NormalizeOrZero(samplePoint.GeometricNormalWorld);
    float3 sourceOffsetWorld = sourceReceiverWorld - samplePoint.PositionWorld;
    float3 destinationOffsetWorld = destinationReceiverWorld - samplePoint.PositionWorld;
    float sourceDistanceSquared = dot(sourceOffsetWorld, sourceOffsetWorld);
    float destinationDistanceSquared = dot(destinationOffsetWorld, destinationOffsetWorld);
    if (sourceDistanceSquared <= 0 || destinationDistanceSquared <= 0)
        return 0;
    float sourceCosine = abs(dot(normalWorld, sourceOffsetWorld)) * rsqrt(sourceDistanceSquared);
    float destinationCosine = abs(dot(normalWorld, destinationOffsetWorld)) * rsqrt(destinationDistanceSquared);
    if (sourceCosine <= 0)
        return 0;
    float jacobian = (destinationCosine / sourceCosine) * (sourceDistanceSquared / destinationDistanceSquared);
    return isfinite(jacobian) == true ? jacobian : 0;
}

/// Target function p_q at this pixel's visible point (section 4.2): the luminance of L_o (Equation 10) or, with
/// IS_RESTIR_GI_TARGET_SCATTERED, of L_o f cos (Equation 9). Zero below the visible point's hemisphere.
float EvaluateTarget(TargetContext target, GiSample sample, SurfaceHit samplePoint)
{
    if (sample.TriangleIndex == SCENE_INVALID_INDEX || samplePoint.IsValid == false)
        return 0;
    float3 offsetWorld = samplePoint.PositionWorld - target.Point.Hit.PositionWorld;
    float distanceSquared = dot(offsetWorld, offsetWorld);
    if (distanceSquared <= 0 || dot(target.Point.NormalWorld, offsetWorld) <= 0)
        return 0;
    if (IS_RESTIR_GI_TARGET_SCATTERED == false)
        return max(ComputeLuminance(sample.RadianceOut), 0);
    float3 directionWorld = offsetWorld * rsqrt(distanceSquared);
    float cosSurface = dot(target.Shading.ShadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(target.Shading.Material,
                         target.Shading.ViewDirectionWorld,
                         directionWorld,
                         bsdfValue,
                         bsdfDensity);
    return cosSurface > 0 ? max(ComputeLuminance(sample.RadianceOut * bsdfValue * cosSurface), 0) : 0;
}

/// Algorithm 1's UPDATE with a candidate that represents candidateCount samples. Zero-weight candidates still count.
void StreamCandidate(GiSample sample,
                     float weight,
                     uint candidateCount,
                     uint candidate,
                     float randomValue,
                     inout Reservoir inoutReservoir,
                     inout float inoutWeightSum,
                     inout uint inoutSelectedCandidate,
                     inout RayQueryContext queries)
{
    inoutReservoir.CandidateCount += candidateCount;
    if (isfinite(weight) == false || weight < 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return;
    }
    if (weight == 0)
        return;
    inoutWeightSum += weight;
    if (isfinite(inoutWeightSum) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return;
    }
    if (randomValue < weight / inoutWeightSum)
    {
        inoutReservoir.Sample = sample;
        inoutSelectedCandidate = candidate;
    }
}

/// Algorithm 1's MERGE of a reservoir from another visible point: its resampling weight p_q(z) |J| W M, zero when the
/// shifted sample has no target here or, for the unbiased variant, is occluded from this visible point.
float ComputeShiftedWeight(SceneContext scene,
                           TargetContext target,
                           Receiver sourceReceiver,
                           Reservoir source,
                           bool isSameReceiver,
                           inout RayQueryContext queries)
{
    if (source.Sample.TriangleIndex == SCENE_INVALID_INDEX || source.CandidateCount == 0 || source.Weight <= 0)
        return 0;
    SurfaceHit samplePoint = GetSurfaceHit(scene, source.Sample.TriangleIndex, source.Sample.Barycentric);
    float targetValue = EvaluateTarget(target, source.Sample, samplePoint);
    if (targetValue <= 0)
        return 0;
    float jacobian = isSameReceiver == true ? 1
                                            : ComputeReconnectionJacobian(sourceReceiver.Hit.PositionWorld,
                                                                          target.Point.Hit.PositionWorld,
                                                                          samplePoint);
    if (jacobian <= 0)
        return 0;
    // Algorithm 4 lines 12-13. A sample from this very visible point was visible when it was traced or merged.
    if (isSameReceiver == false && IS_RESTIR_GI_UNBIASED == true &&
        CanGenerateSample(scene, target.Point, samplePoint, true, queries) == false)
        return 0;
    return targetValue * jacobian * source.Weight * float(source.CandidateCount);
}

/// Equation 7 with the count Z of Algorithm 4: W = w / (Z p_q(z)). Zero without a sample or a positive target.
void FinalizeReservoir(SceneContext scene,
                       TargetContext target,
                       float weightSum,
                       uint normalizationCount,
                       uint maxCandidates,
                       inout Reservoir inoutReservoir,
                       inout RayQueryContext queries)
{
    float targetValue = 0;
    if (inoutReservoir.Sample.TriangleIndex != SCENE_INVALID_INDEX)
    {
        SurfaceHit samplePoint =
            GetSurfaceHit(scene, inoutReservoir.Sample.TriangleIndex, inoutReservoir.Sample.Barycentric);
        targetValue = EvaluateTarget(target, inoutReservoir.Sample, samplePoint);
    }
    if (normalizationCount == 0 || targetValue <= 0 || weightSum <= 0)
    {
        inoutReservoir.Sample.TriangleIndex = SCENE_INVALID_INDEX;
        inoutReservoir.Weight = 0;
    }
    else
    {
        inoutReservoir.Weight = weightSum / (float(normalizationCount) * targetValue);
    }
    // Section 5 limits M after normalization, so W keeps its finalized scale.
    inoutReservoir.CandidateCount = min(inoutReservoir.CandidateCount, maxCandidates);
    if (isfinite(inoutReservoir.Weight) == false || inoutReservoir.Weight < 0)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
}

/// Trace this pixel's camera ray to its visible point. Returns false for a miss (no failure) or a failed query.
bool TryTraceReceiver(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, out Receiver outReceiver)
{
    outReceiver = (Receiver)0;
    SurfaceHit hit;
    if (TraceClosest(scene, queries, launch.CameraRay, hit) != SCENE_QUERY_SURFACE)
        return false;
    outReceiver = MakeReceiver(hit);
    if (outReceiver.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    return true;
}

/// The receiver's material, which the scattered target function (Equation 9) evaluates.
bool TryMakeTargetContext(SceneContext scene,
                          Receiver receiver,
                          inout RayQueryContext queries,
                          out TargetContext outTarget)
{
    outTarget = (TargetContext)0;
    outTarget.Point = receiver;
    if (IS_RESTIR_GI_TARGET_SCATTERED == false)
        return true;
    return TryMakeShadingPoint(scene,
                               receiver.Hit,
                               -receiver.Hit.IncomingRay.DirectionWorld,
                               queries,
                               outTarget.Shading);
}

/// Latch the failure that an earlier pass of this frame recorded for this pixel under that pass's own reason, or a
/// stored-record failure when no earlier pass wrote this pixel's record for this frame.
void FailFromStoredRecord(RayLaunch launch,
                          uint sampleIndex,
                          bool hasFailed,
                          uint failureReason,
                          inout RayQueryContext queries)
{
    FailSceneQuery(queries,
                   sampleIndex == launch.SampleIndex && hasFailed == true ? failureReason
                                                                          : LUDUS_SCENE_FAILURE_STORED_RECORD);
}

/// Write this pixel's reservoir, marking it failed when the pass latched a failure, so later passes report it.
void FinishReservoir(bool isSecond, RayLaunch launch, Reservoir reservoir, RayQueryContext queries)
{
    reservoir.HasFailed = queries.HasFailed;
    reservoir.FailureReason = queries.FailureReason;
    if (reservoir.HasFailed == true)
    {
        reservoir.Sample.TriangleIndex = SCENE_INVALID_INDEX;
        reservoir.Weight = 0;
        reservoir.CandidateCount = 0;
    }
    StoreReservoir(isSecond, GetPixelIndex(launch), reservoir);
}
