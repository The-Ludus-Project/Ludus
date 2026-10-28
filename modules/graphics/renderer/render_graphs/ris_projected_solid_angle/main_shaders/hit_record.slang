#pragma once
// Per-pixel primary hit record: the primary-hits pass of this template writes it and its three
// shade passes read it.
// Thanks to Samuli Laine, Tero Karras and Timo Aila, "Megakernels Considered Harmful: Wavefront
// Path Tracing on GPUs", HPG 2013, https://doi.org/10.1145/2492045.2492060 : their stage split
// motivates generating hits and shading them in separate kernels. This template stops at two
// passes over every pixel; it has no ray queues or compaction.
// Thanks to Christopher A. Burns and Warren A. Hunt, "The Visibility Buffer: A Cache-Friendly
// Approach to Deferred Shading", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/04/ :
// the record stores hit identity (triangle and barycentrics), not shaded attributes.
//
// Two uint4 words per pixel in GraphBuffer0 (the 32-byte-per-pixel `hits` Buffer):
//   word 0: packed triangle + 1 (0 = miss or never written), asuint(DistanceWorld),
//           asuint(Barycentric.x), asuint(Barycentric.y)
//   word 1: asuint(jitterPixel.x), asuint(jitterPixel.y), primary SampleIndex, flags
//           (bit 0: front face; bit 1: the primary query failed)
// The triangle is the renderer's packed scene index, which is identical in every pass of one
// sequence. Zeroed memory (a new or resized buffer) decodes as "no hit", never as triangle 0.
// A shade pass rejects a record whose sample index differs from its own sample.
//
// Random dimensions: each pass draws from its own explicit, disjoint range through the
// RayLaunch address overloads, so neither depends on the other's automatic stream. The table is
// docs/architecture/many-area-light-sampling-reference-review.md, "Template: ris-projected-solid-angle".
// Dimensions 2 to 8 keep the veach-guibas-mis meanings, so analytic, environment and BSDF draws are
// paired across the two templates. Ranges are sized for the settings' maximums, so changing
// RIS_CANDIDATE_COUNT or RIS_NORMALIZATION_CANDIDATE_COUNT never renumbers a dimension.

static const uint HIT_RECORD_WORDS = 2;
static const uint HIT_RECORD_FLAG_FRONT_FACE = 1;
static const uint HIT_RECORD_FLAG_FAILED = 2;
// Primary pass: subpixel jitter x, y.
static const uint PRIMARY_RANDOM_DIMENSION_FIRST = 0;
static const uint PRIMARY_RANDOM_DIMENSION_COUNT = 2;
// Shade passes, all three with one layout. First the veach-guibas-mis range: light category
// selection, the three SampleIncidentLight shape coordinates, then the three SampleStandardBsdf
// coordinates (lobe, radius, angle).
static const uint SHADE_RANDOM_DIMENSION_FIRST = PRIMARY_RANDOM_DIMENSION_FIRST + PRIMARY_RANDOM_DIMENSION_COUNT;
static const uint SHADE_LIGHT_SELECTION_DIMENSION = SHADE_RANDOM_DIMENSION_FIRST;
static const uint SHADE_LIGHT_SHAPE_DIMENSION_FIRST = SHADE_LIGHT_SELECTION_DIMENSION + 1;
static const uint SHADE_BSDF_DIMENSION_FIRST = SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 3;
// Then the selected triangle's two light techniques: the diffuse projected-solid-angle direction
// omega_0 (9 and 10; 11 is reserved) and the LTC direction omega_1 (12 and 13; 14 and 15 are reserved).
static const uint SHADE_DIFFUSE_TECHNIQUE_DIMENSION_FIRST = SHADE_BSDF_DIMENSION_FIRST + 3;
static const uint SHADE_SPECULAR_TECHNIQUE_DIMENSION_FIRST = SHADE_DIFFUSE_TECHNIQUE_DIMENSION_FIRST + 3;
// Candidate i of the reservoir draws its power-CDF ticket at FIRST + 2i and its reservoir ticket at
// FIRST + 2i + 1, for i below RIS_MAX_CANDIDATE_COUNT (16 to 79).
static const uint RIS_MAX_CANDIDATE_COUNT = 32;
static const uint RIS_CANDIDATE_DIMENSION_FIRST = SHADE_SPECULAR_TECHNIQUE_DIMENSION_FIRST + 4;
static const uint RIS_DIMENSIONS_PER_CANDIDATE = 2;
// Normalization candidate k draws its power-CDF ticket at FIRST + k, for k below
// RIS_MAX_NORMALIZATION_CANDIDATE_COUNT (80 to 95).
static const uint RIS_MAX_NORMALIZATION_CANDIDATE_COUNT = 16;
static const uint RIS_NORMALIZATION_DIMENSION_FIRST =
    RIS_CANDIDATE_DIMENSION_FIRST + RIS_MAX_CANDIDATE_COUNT * RIS_DIMENSIONS_PER_CANDIDATE;
static const uint SHADE_RANDOM_DIMENSION_COUNT =
    RIS_NORMALIZATION_DIMENSION_FIRST + RIS_MAX_NORMALIZATION_CANDIDATE_COUNT - SHADE_RANDOM_DIMENSION_FIRST;

struct HitRecord
{
    bool IsHit;
    uint TriangleIndex;
    float DistanceWorld;
    float2 Barycentric;
    float2 JitterPixel;
    uint SampleIndex;
    bool IsFrontFace;
    bool HasFailed;
};

/// True when GraphBuffer0 holds a record for every pixel of the launch.
bool HasHitRecordCapacity(RayLaunch launch)
{
    uint wordCount;
    uint stride;
    GraphBuffer0.GetDimensions(wordCount, stride);
    uint capacity = wordCount / HIT_RECORD_WORDS;
    return launch.ExtentPixel.x > 0 && launch.ExtentPixel.y > 0 &&
           launch.ExtentPixel.x <= capacity / launch.ExtentPixel.y;
}

uint GetHitRecordBase(RayLaunch launch)
{
    return (launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x) * HIT_RECORD_WORDS;
}

void StoreHitRecord(RayLaunch launch, HitRecord record)
{
    uint base = GetHitRecordBase(launch);
    GraphBuffer0[base] = uint4(record.IsHit == true ? record.TriangleIndex + 1 : 0,
                               asuint(record.DistanceWorld),
                               asuint(record.Barycentric.x),
                               asuint(record.Barycentric.y));
    GraphBuffer0[base + 1] = uint4(asuint(record.JitterPixel.x),
                                   asuint(record.JitterPixel.y),
                                   record.SampleIndex,
                                   (record.IsFrontFace == true ? HIT_RECORD_FLAG_FRONT_FACE : 0) |
                                       (record.HasFailed == true ? HIT_RECORD_FLAG_FAILED : 0));
}

/// Decode this pixel's record. Returns false for a failed primary query, a record that this
/// frame's primary pass did not write (another sample) or out-of-range fields; a miss is valid.
bool TryLoadHitRecord(SceneContext scene, RayLaunch launch, out HitRecord outRecord)
{
    outRecord = (HitRecord)0;
    uint base = GetHitRecordBase(launch);
    uint4 surfaceWord = GraphBuffer0[base];
    uint4 sampleWord = GraphBuffer0[base + 1];
    outRecord.IsHit = surfaceWord.x != 0;
    outRecord.TriangleIndex = outRecord.IsHit == true ? surfaceWord.x - 1 : SCENE_INVALID_INDEX;
    outRecord.DistanceWorld = asfloat(surfaceWord.y);
    outRecord.Barycentric = asfloat(surfaceWord.zw);
    outRecord.JitterPixel = asfloat(sampleWord.xy);
    outRecord.SampleIndex = sampleWord.z;
    outRecord.IsFrontFace = (sampleWord.w & HIT_RECORD_FLAG_FRONT_FACE) != 0;
    outRecord.HasFailed = sampleWord.w != (sampleWord.w & HIT_RECORD_FLAG_FRONT_FACE);
    if (outRecord.HasFailed == true || outRecord.SampleIndex != launch.SampleIndex ||
        all(isfinite(outRecord.JitterPixel)) == false || any(outRecord.JitterPixel < 0) == true ||
        any(outRecord.JitterPixel >= 1) == true)
        return false;
    if (outRecord.IsHit == false)
        return true;
    return outRecord.TriangleIndex < GetSceneInfo(scene).TriangleCount && isfinite(outRecord.DistanceWorld) == true &&
           outRecord.DistanceWorld >= 0;
}
