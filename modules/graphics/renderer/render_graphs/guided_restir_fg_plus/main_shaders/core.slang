#pragma once
// Thanks to Rene Kern, Felix Brull, Jonas Kastning and Thorsten Grosch, "Guided ReSTIR FG+:
// Photon Resampling for Large Scenes and Many Lights", EGSR 2026, sections 3-4,
// https://doi.org/10.2312/sr.20261001 . This reflective, static-scene subset adopts histogram
// guiding, photon final gathering, hybrid camera replay/reconnection, and caustic backprojection.
// The authoring guide records the departures from the full algorithm, including finite depth,
// fixed radius, one spatial neighbour, same-pixel temporal history and no transmission.
// These are independent Ludus implementations; no Falcor implementation code is included.
static const uint FG_DIRECTION_SIDE = 4;
static const uint FG_DIRECTION_CELLS = FG_DIRECTION_SIDE * FG_DIRECTION_SIDE;
static const uint FG_MAXIMUM_LIGHTS = 32768;
static const uint FG_MAXIMUM_PHOTON_PATHS = 65536;
static const uint FG_MAXIMUM_PHOTON_DEPTH = 4;
static const uint FG_MAXIMUM_PIXELS = 2097152;
static const uint FG_HEADER_WORDS = 4;
static const uint FG_LIGHT_WORDS = 1 + FG_DIRECTION_CELLS;
static const uint FG_HEAD_OFFSET = FG_HEADER_WORDS + FG_MAXIMUM_LIGHTS * FG_LIGHT_WORDS;
static const uint FG_PHOTON_OFFSET = FG_HEAD_OFFSET + FG_MAXIMUM_PIXELS;
static const uint FG_PHOTON_WORDS = 5;
static const uint FG_PHOTON_CAPACITY = FG_MAXIMUM_PHOTON_PATHS * FG_MAXIMUM_PHOTON_DEPTH;
static const uint FG_POOL_WORDS = FG_PHOTON_OFFSET + FG_PHOTON_CAPACITY * FG_PHOTON_WORDS;
// Photon UV identifies its guiding cell exactly; no separate serialized cell word.
static const uint FG_PATH_GATHER_WORD = FG_PHOTON_WORDS;
static const uint FG_PATH_RECEIVER_WORD = FG_PATH_GATHER_WORD + 1;
static const uint FG_PATH_RANDOM_WORD = FG_PATH_RECEIVER_WORD + 1;
static const uint FG_PATH_IDENTITY_WORD = FG_PATH_RANDOM_WORD + 1;
static const uint FG_PATH_VIEW_WORD = FG_PATH_IDENTITY_WORD + 1;
static const uint FG_PATH_THROUGHPUT_WORD = FG_PATH_VIEW_WORD + 1;
// One reserved word retains the published per-pixel Buffer stride.
static const uint FG_PATH_WORDS = 12;
static const uint FG_CAUSTIC_WORDS = 8;
static const uint FG_VERSION = 1;
static const uint FG_INVALID = 0xffffffffu;
static const uint FG_CAUSTIC_FLAG = 256;
static const uint FG_RADIANCE_FLAG = 512;
static const uint FG_DEPTH_MASK = 255;
static const uint FG_SOURCE_FIRST_DIMENSION = 8;
static const uint FG_CAMERA_FIRST_DIMENSION = 32;
static const uint FG_GATHER_FIRST_DIMENSION = 48;
static const uint FG_DIRECT_FIRST_DIMENSION = 64;
static const uint FG_RESAMPLE_FIRST_DIMENSION = 80;
static const uint FG_VERTEX_DIMENSIONS = 3;
static const float FG_MAXIMUM_DISTANCE_WORLD = 3.402823466e38f;
static const float FG_NORMAL_AGREEMENT = 0.9f;
static const float FG_PLANE_RADIUS_FRACTION = 0.1f;
static const float FG_GUIDING_DISCRETIZATION = 255;
// Two recorded reservoirs per pixel and at most FG_MAXIMUM_PIXELS pixels keep integer sums below 2^32.
static const uint FG_MAXIMUM_GUIDING_CONTRIBUTION = 255;
static const float3 FG_LUMINANCE = float3(0.2126f, 0.7152f, 0.0722f);

float ComputeFgLuminance(float3 value)
{
    return dot(value, FG_LUMINANCE);
}
uint GetFgPixel(RayLaunch launch)
{
    return launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
}
uint GetFgPixelCount(RayLaunch launch)
{
    return launch.ExtentPixel.x * launch.ExtentPixel.y;
}
uint GetFgLightAddress(uint light)
{
    return FG_HEADER_WORDS + light * FG_LIGHT_WORDS;
}
uint GetFgDirectionCell(float2 directionUv)
{
    uint2 cell = min(uint2(directionUv * float(FG_DIRECTION_SIDE)), uint2(FG_DIRECTION_SIDE - 1));
    return cell.y * FG_DIRECTION_SIDE + cell.x;
}
float SampleFgRandom(uint2 seed, uint path, uint frame, uint dimension)
{
    return SampleRandomFloat01(seed, MakeRandomAddress(uint2(path, 0), frame, dimension));
}
float SampleFgRandom(RayLaunch launch, uint dimension)
{
    return SampleFgRandom(launch.Seed, GetFgPixel(launch), launch.SampleIndex, dimension);
}
float2 SampleFgRandomPair(uint2 seed, uint path, uint frame, uint dimension)
{
    return float2(SampleFgRandom(seed, path, frame, dimension), SampleFgRandom(seed, path, frame, dimension + 1));
}
float3 SampleFgRandomTriple(uint2 seed, uint path, uint frame, uint dimension)
{
    return float3(SampleFgRandomPair(seed, path, frame, dimension), SampleFgRandom(seed, path, frame, dimension + 2));
}
// A deterministic partition of each light's photons. The leftover paths sample the whole square.
// The flux uses the *mixture* cell density (including those leftovers), avoiding double counting.
uint ComputeFgDirectionCount(float probability, uint lightPaths)
{
    return lightPaths > 2 * FG_DIRECTION_CELLS ? uint(floor(1 + probability * float(lightPaths - FG_DIRECTION_CELLS)))
                                               : 0;
}
float ComputeFgFluxScale(uint cellPaths, uint uniformPaths)
{
    return 1 / float(FG_DIRECTION_CELLS * cellPaths + uniformPaths);
}
float ComputeFgReconnectJacobian(float3 oldOffsetWorld, float3 newOffsetWorld, float3 normalWorld)
{
    float oldSquared = dot(oldOffsetWorld, oldOffsetWorld);
    float newSquared = dot(newOffsetWorld, newOffsetWorld);
    if (oldSquared <= 0 || newSquared <= 0)
        return 0;
    float oldCosine = abs(dot(normalWorld, oldOffsetWorld)) * rsqrt(oldSquared);
    float newCosine = abs(dot(normalWorld, newOffsetWorld)) * rsqrt(newSquared);
    return oldCosine > 0 ? newCosine * oldSquared / (oldCosine * newSquared) : 0;
}
float ComputeFgPairWeight(float confidence, float otherConfidence, float target, float otherTarget)
{
    float denominator = confidence * target + otherConfidence * otherTarget;
    return denominator > 0 ? confidence * target / denominator : 0;
}
float ComputeFgPowerWeight(float first, float second)
{
    float largest = max(first, second);
    if (largest <= 0)
        return 0;
    float a = first / largest;
    float b = second / largest;
    return a * a / (a * a + b * b);
}

uint ComputeFgBucket(int3 cellWorld, uint bucketCount)
{
    static const uint HASH_X = 73856093;
    static const uint HASH_Y = 19349663;
    static const uint HASH_Z = 83492791;
    uint3 coordinate = asuint(cellWorld);
    return (coordinate.x * HASH_X ^ coordinate.y * HASH_Y ^ coordinate.z * HASH_Z) % bucketCount;
}

float3 EvaluateFgPhotonDensity(float3 bsdfValue, float3 flux)
{
    return bsdfValue * flux / (PI * FG_GATHER_RADIUS_WORLD * FG_GATHER_RADIUS_WORLD);
}
