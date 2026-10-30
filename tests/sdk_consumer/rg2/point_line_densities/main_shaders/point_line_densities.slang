// Scene compute kernel for the salesin-jarosz-lines template's MIS weights and line densities
// (point-line spec PL2). The renderer's test build stages the template's line_sampling.slang and
// multiple_importance.slang beside this file, so it checks the shipped sources. Each pixel writes one
// result as exact fixed-point bytes (no Accumulate or ToneMap):
// - rows 0-7, columns 0-63: |sum of the three strategies' weights - 1| * ERROR_SCALE for densities
//   p = DENSITIES[column % 8], q = DENSITIES[column / 8] and r = DENSITIES[row], or the sum itself
//   when all three are zero (no strategy proposes the sample);
// - row 8: single weights: column 0 a delta light that both line strategies propose with one
//   probability and BSDF sampling never proposes, ComputeMisWeight(p, p, 0) = 1/2; column 1 the only
//   strategy, ComputeMisWeight(p, 0, 0) = 1; column 2 a strategy that did not propose the sample,
//   ComputeMisWeight(0, p, p) = 0; column 3 three equal densities, 1/3;
// - row 9, columns 0-11: chords through the vertices of an area-uniform light point's triangle, seen
//   from the surface at the image's lower center. Case c = column / 2 takes vertex c / 2 (barycentric
//   (0, 0), (1, 0) or (0, 1)) and direction U for even c, V for odd c. Even columns write
//   DEGENERATE_CHORD when IsEmitterChordQueryable rejects the chord, else QUERYABLE_CHORD; odd columns
//   write POSITIVE_DENSITY when EvaluateLineDensity there is positive, else zero. A rejected chord
//   once latched a failure (the Sponza failure pixels, design section 5);
// - rows 16-63: trace the camera ray through RECEIVER_JITTER_PIXEL; for a surface that is not a mesh
//   emitter, draw an area-uniform light point (dimensions 2-5) and write
//   |EvaluateLineDensity / EvaluateLightPdfSolidAngle - 1| * ERROR_SCALE, for direction U in even
//   columns and V in odd ones. Without occluders the two densities are equal (design D6 step 6).
//   A primary miss or an emitter receiver writes SKIPPED. The jitter is off center: by the room's
//   symmetry, pixel-center rays land exactly on the seams where floor and walls meet, where the
//   receiver lies in an adjacent wall's plane and the line query clips that wall at grazing incidence
//   while a shadow ray does not. Seams have zero area, so jittered sampling never meets them.
// Errors are clamped below SKIPPED.
#include "line_sampling.slang"
#include "multiple_importance.slang"

static const uint DENSITY_COUNT = 8;
static const float DENSITIES[DENSITY_COUNT] = {0.0, 1.0e-20, 1.0e-6, 0.03125, 0.5, 1.0, 37.0, 1.0e12};
static const uint WEIGHT_ROW_COUNT = 8;
static const uint SINGLE_WEIGHT_ROW = 8;
static const uint DENSITY_FIRST_ROW = 16;
static const uint VERTEX_CHORD_ROW = 9;
static const uint VERTEX_COUNT = 3;
static const uint LINE_DIRECTION_COUNT = 2;
static const float QUERYABLE_CHORD = 0.25;
static const float DEGENERATE_CHORD = 0.5;
static const float POSITIVE_DENSITY = 0.75;
static const uint POINT_DIMENSION_FIRST = 2;
static const float DELTA_PROBABILITY = 0.25;
static const float2 RECEIVER_JITTER_PIXEL = float2(0.37, 0.61);
static const float ERROR_SCALE = 1000.0;
static const float LARGEST_ERROR = 0.999;
static const float SKIPPED = 1.0;
static const float FIXED_POINT_SCALE = 16777215.0;
static const float BYTE_SCALE = 255.0;

float3 EncodeFixed24(float value)
{
    uint bits = min(uint(clamp(value, 0.0, 1.0) * FIXED_POINT_SCALE + 0.5), uint(FIXED_POINT_SCALE));
    return float3(float((bits >> 16) & 255), float((bits >> 8) & 255), float(bits & 255)) / BYTE_SCALE;
}

float3 EncodeError(float error)
{
    return EncodeFixed24(isfinite(error) == true ? min(error * ERROR_SCALE, LARGEST_ERROR) : LARGEST_ERROR);
}

/// Row VERTEX_CHORD_ROW: one vertex chord case per column pair (see the file comment).
float3 ReadVertexChord(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint chordCase = launch.PositionPixel.x / 2;
    if (chordCase >= VERTEX_COUNT * LINE_DIRECTION_COUNT)
        return float3(0);
    RayLaunch receiverLaunch = launch;
    receiverLaunch.PositionPixel = uint2(launch.ExtentPixel.x / 2, launch.ExtentPixel.y * 3 / 4);
    SurfaceHit receiver;
    if (TraceClosest(scene, queries, MakeCameraRay(receiverLaunch, RECEIVER_JITTER_PIXEL), receiver) !=
            SCENE_QUERY_SURFACE ||
        receiver.EmitterIndex < GetEmitterCount(scene))
    {
        queries.HasFailed = true;
        return float3(0);
    }
    SelectedLight selected = SelectLight(scene, launch, POINT_DIMENSION_FIRST);
    float3 shapeSample = float3(SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 2),
                                SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 3));
    LightSample light = SampleIncidentLight(scene, selected, receiver, shapeSample);
    if (light.HasSample == false || light.TriangleIndex == SCENE_INVALID_INDEX)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    uint vertex = chordCase / LINE_DIRECTION_COUNT;
    uint direction = chordCase % LINE_DIRECTION_COUNT == 0 ? LINE_DIRECTION_U : LINE_DIRECTION_V;
    float2 barycentric = vertex == 0 ? float2(0, 0) : (vertex == 1 ? float2(1, 0) : float2(0, 1));
    TriangleData triangle = GetTriangle(scene, light.TriangleIndex);
    EmitterChord chord;
    if (TryMakeEmitterChord(triangle, barycentric, GetLineAlongWorld(triangle, direction), chord) == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    bool isQueryable = IsEmitterChordQueryable(chord);
    if (launch.PositionPixel.x % 2 == 0)
        return EncodeFixed24(isQueryable == true ? QUERYABLE_CHORD : DEGENERATE_CHORD);
    float lineDensity = EvaluateLineDensity(scene, receiver, light.TriangleIndex, barycentric, direction, queries);
    return EncodeFixed24(lineDensity > 0 ? POSITIVE_DENSITY : 0);
}

float3 ReadPointLineDensities(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint2 pixel = launch.PositionPixel;
    if (pixel.y < WEIGHT_ROW_COUNT)
    {
        if (pixel.x >= DENSITY_COUNT * DENSITY_COUNT)
            return float3(0);
        float p = DENSITIES[pixel.x % DENSITY_COUNT];
        float q = DENSITIES[pixel.x / DENSITY_COUNT];
        float r = DENSITIES[pixel.y];
        float sum = ComputeMisWeight(p, q, r) + ComputeMisWeight(q, p, r) + ComputeMisWeight(r, p, q);
        return EncodeError(p > 0 || q > 0 || r > 0 ? abs(sum - 1) : sum);
    }
    if (pixel.y == SINGLE_WEIGHT_ROW)
    {
        if (pixel.x == 0)
            return EncodeFixed24(ComputeMisWeight(DELTA_PROBABILITY, DELTA_PROBABILITY, 0));
        if (pixel.x == 1)
            return EncodeFixed24(ComputeMisWeight(DELTA_PROBABILITY, 0, 0));
        if (pixel.x == 2)
            return EncodeFixed24(ComputeMisWeight(0, DELTA_PROBABILITY, DELTA_PROBABILITY));
        if (pixel.x == 3)
            return EncodeFixed24(ComputeMisWeight(DELTA_PROBABILITY, DELTA_PROBABILITY, DELTA_PROBABILITY));
        return float3(0);
    }
    if (pixel.y == VERTEX_CHORD_ROW)
        return ReadVertexChord(scene, launch, queries);
    if (pixel.y < DENSITY_FIRST_ROW)
        return float3(0);
    SurfaceHit receiver;
    if (TraceClosest(scene, queries, MakeCameraRay(launch, RECEIVER_JITTER_PIXEL), receiver) != SCENE_QUERY_SURFACE ||
        receiver.EmitterIndex < GetEmitterCount(scene))
        return EncodeFixed24(SKIPPED);
    SelectedLight selected = SelectLight(scene, launch, POINT_DIMENSION_FIRST);
    float3 shapeSample = float3(SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 2),
                                SampleSceneRandom(launch, POINT_DIMENSION_FIRST + 3));
    LightSample light = SampleIncidentLight(scene, selected, receiver, shapeSample);
    if (light.HasSample == false || light.TriangleIndex == SCENE_INVALID_INDEX)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    float areaDensity = EvaluateLightPdfSolidAngle(scene,
                                                   receiver.PositionWorld,
                                                   GetSurfaceHit(scene, light.TriangleIndex, light.Barycentric));
    float lineDensity = EvaluateLineDensity(scene,
                                            receiver,
                                            light.TriangleIndex,
                                            light.Barycentric,
                                            pixel.x % 2 == 0 ? LINE_DIRECTION_U : LINE_DIRECTION_V,
                                            queries);
    if (areaDensity <= 0)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    return EncodeError(abs(lineDensity / areaDensity - 1));
}
