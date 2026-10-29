#pragma once
// Per-pixel primary hit record: the first shade pass of this template (clamped optimal MIS) traces the camera ray,
// writes the record and shades it; the three comparison passes read the record, so all four outputs shade the same
// surface point.
// Thanks to Samuli Laine, Tero Karras and Timo Aila, "Megakernels Considered Harmful: Wavefront
// Path Tracing on GPUs", HPG 2013, https://doi.org/10.1145/2492045.2492060 : their stage split
// motivates generating hits once and shading them in separate kernels. This template has no ray
// queues or compaction.
// Thanks to Christopher A. Burns and Warren A. Hunt, "The Visibility Buffer: A Cache-Friendly
// Approach to Deferred Shading", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/04/ :
// the record stores hit identity (triangle and barycentrics), not shaded attributes. Peters' renderer is also a
// deferred renderer with a visibility buffer (Peters 2021, section 6.1).
//
// Two uint4 words per pixel in GraphBuffer0 (the 32-byte-per-pixel `hits` Buffer):
//   word 0: packed triangle + 1 (0 = miss or never written), asuint(DistanceWorld),
//           asuint(Barycentric.x), asuint(Barycentric.y)
//   word 1: asuint(jitterPixel.x), asuint(jitterPixel.y), primary SampleIndex, flags
//           (bit 0: front face; bit 1: the primary query failed)
// The triangle is the renderer's packed scene index, which is identical in every pass of one
// sequence. Zeroed memory (a new or resized buffer) decodes as "no hit", never as triangle 0.
// A comparison pass rejects a record whose sample index differs from its own sample.
//
// Random dimensions: every pass draws from explicit addresses through the RayLaunch overloads, never from the
// automatic stream, so the four outputs see the same random values. The table is
// docs/development/render-graph-authoring.md, "Compare BRDF importance sampling for polygonal lights":
// 0 and 1 jitter the camera ray. Emissive triangle k, in GetEmitter/GetEmitterTriangle order, draws its projected
// solid angle sample omega_0 at 2 + 4k and 3 + 4k and its LTC sample omega_1 at 4 + 4k and 5 + 4k, for k below
// MAX_POLYGONAL_LIGHT_COUNT. The single-technique outputs read only their technique's pair, so they share their
// samples with both MIS outputs.

static const uint HIT_RECORD_WORDS = 2;
static const uint HIT_RECORD_FLAG_FRONT_FACE = 1;
static const uint HIT_RECORD_FLAG_FAILED = 2;
// Camera ray: subpixel jitter x, y.
static const uint PRIMARY_RANDOM_DIMENSION_FIRST = 0;
static const uint PRIMARY_RANDOM_DIMENSION_COUNT = 2;
// Every pass shades each emissive triangle of the scene. Both MIS outputs trace two shadow rays per triangle, so the
// technique's visibility budget (16, the scene interface's maximum) bounds the scene to this many emissive triangles;
// a larger scene fails explicitly instead of dropping lights.
static const uint MAX_POLYGONAL_LIGHT_COUNT = 8;
static const uint POLYGONAL_LIGHT_DIMENSION_FIRST = PRIMARY_RANDOM_DIMENSION_FIRST + PRIMARY_RANDOM_DIMENSION_COUNT;
static const uint POLYGONAL_LIGHT_DIMENSIONS_PER_LIGHT = 4;
// Offsets within one light's four dimensions.
static const uint PROJECTED_SOLID_ANGLE_SAMPLE_DIMENSION_OFFSET = 0;
static const uint LTC_SAMPLE_DIMENSION_OFFSET = 2;
static const uint SHADE_RANDOM_DIMENSION_COUNT = MAX_POLYGONAL_LIGHT_COUNT * POLYGONAL_LIGHT_DIMENSIONS_PER_LIGHT;

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
/// frame's first pass did not write (another sample) or out-of-range fields; a miss is valid.
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

/// Trace this pixel's jittered camera ray and record what it hit, as the other templates' primary pass does. A failed
/// query is recorded too, so every comparison pass shows the error color for this sample. One surface query.
/// Returns the trace status.
uint TraceAndStoreHitRecord(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    HitRecord record = (HitRecord)0;
    record.JitterPixel = float2(SampleSceneRandom(launch, PRIMARY_RANDOM_DIMENSION_FIRST),
                                SampleSceneRandom(launch, PRIMARY_RANDOM_DIMENSION_FIRST + 1));
    record.SampleIndex = launch.SampleIndex;
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, MakeCameraRay(launch, record.JitterPixel), hit);
    record.HasFailed = status == SCENE_QUERY_FAILURE;
    record.IsHit = status == SCENE_QUERY_SURFACE;
    if (record.IsHit == true)
    {
        record.TriangleIndex = hit.TriangleIndex;
        record.DistanceWorld = hit.DistanceWorld;
        record.Barycentric = hit.Barycentric;
        record.IsFrontFace = hit.IsFrontFace;
    }
    StoreHitRecord(launch, record);
    return status;
}
