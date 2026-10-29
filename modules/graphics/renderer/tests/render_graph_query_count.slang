#include "shade.slang"

// Test-only program of the [.ris-projected-solid-angle-benchmark] packages, staged beside a template's shade.slang in
// place of its BSDF-only program. It runs that template's ShadeMultipleImportance unchanged and returns the queries
// the invocation issued instead of its radiance: red is visibility queries (shadow rays), green surface queries (the
// BSDF ray). Each count is scaled by QUERY_COUNT_SCALE, which keeps two queries well inside ToneMap's resolved range,
// so the accumulated image's mean is the mean query count per pixel and sample.

static const float QUERY_COUNT_SCALE = 0.25f;

float3 CountMultipleImportanceQueries(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint visibilityQueries = queries.VisibilityQueries;
    uint surfaceQueries = queries.SurfaceQueries;
    ShadeMultipleImportance(scene, launch, queries);
    return float3(float(queries.VisibilityQueries - visibilityQueries),
                  float(queries.SurfaceQueries - surfaceQueries),
                  0) *
           QUERY_COUNT_SCALE;
}
