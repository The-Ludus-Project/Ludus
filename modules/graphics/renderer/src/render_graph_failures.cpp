#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/scene_shader_layout.h>

#include "../shaders/metropolis_layout.h"
#include "../shaders/renderer_gpu_preview_layout.h"

// Failure reasons a graph ray kernel counts (renderer_gpu_preview.slang, CountGraphFailure). The shader codes in
// scene_shader_layout.h and this enum are one contract; see docs/development/render-graph-authoring.md, "Explain
// failed pixels".
namespace ludus::graphics::renderer
{
namespace
{
using foundation::uint32;
[[nodiscard]] constexpr uint32 ToCode(RenderGraphFailureReason reason) noexcept
{
    return static_cast<uint32>(reason);
}
} // namespace
static_assert(ToCode(RenderGraphFailureReason::Unspecified) == LUDUS_SCENE_FAILURE_UNSPECIFIED);
static_assert(ToCode(RenderGraphFailureReason::InvalidRadiance) == LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
static_assert(ToCode(RenderGraphFailureReason::InterfaceVersion) == LUDUS_SCENE_FAILURE_INTERFACE_VERSION);
static_assert(ToCode(RenderGraphFailureReason::SurfaceBudget) == LUDUS_SCENE_FAILURE_SURFACE_BUDGET);
static_assert(ToCode(RenderGraphFailureReason::VisibilityBudget) == LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET);
static_assert(ToCode(RenderGraphFailureReason::LineBudget) == LUDUS_SCENE_FAILURE_LINE_BUDGET);
static_assert(ToCode(RenderGraphFailureReason::InvalidRay) == LUDUS_SCENE_FAILURE_INVALID_RAY);
static_assert(ToCode(RenderGraphFailureReason::Traversal) == LUDUS_SCENE_FAILURE_TRAVERSAL);
static_assert(ToCode(RenderGraphFailureReason::RandomExhausted) == LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
static_assert(ToCode(RenderGraphFailureReason::InvalidArgument) == LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
static_assert(ToCode(RenderGraphFailureReason::LightSample) == LUDUS_SCENE_FAILURE_LIGHT_SAMPLE);
static_assert(ToCode(RenderGraphFailureReason::StorageCapacity) == LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
static_assert(ToCode(RenderGraphFailureReason::StoredRecord) == LUDUS_SCENE_FAILURE_STORED_RECORD);
static_assert(ToCode(RenderGraphFailureReason::SceneData) == LUDUS_SCENE_FAILURE_SCENE_DATA);
static_assert(ToCode(RenderGraphFailureReason::InvalidWeight) == LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
static_assert(ToCode(RenderGraphFailureReason::BsdfOnlyLights) == LUDUS_SCENE_FAILURE_BSDF_ONLY_LIGHTS);
static_assert(ToCode(RenderGraphFailureReason::PrimaryMaterial) == LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL);
static_assert(RENDER_GRAPH_FAILURE_AUTHORED_FIRST == LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
static_assert(MAX_RENDER_GRAPH_FAILURE_REASONS == LUDUS_SCENE_FAILURE_REASON_COUNT);
static_assert(LUDUS_GRAPH_FAILURE_BYTES == MAX_RENDER_GRAPH_FAILURE_REASONS * sizeof(uint32));

const char* DescribeRenderGraphFailureReason(uint32 reason) noexcept
{
    if (reason >= MAX_RENDER_GRAPH_FAILURE_REASONS)
    {
        return "";
    }
    if (reason >= RENDER_GRAPH_FAILURE_AUTHORED_FIRST)
    {
        return "An authored shader reported its own failure reason with FailSceneQuery; see that shader's reason "
               "codes.";
    }
    switch (static_cast<RenderGraphFailureReason>(reason))
    {
        case RenderGraphFailureReason::BsdfOnlyLights:
            return "This BSDF-only path template cannot sample analytic lights. Select a light-sampling graph, or "
                   "replace the analytic lights with authored emissive meshes or an environment.";
        case RenderGraphFailureReason::PrimaryMaterial:
            return "This primary-sample template does not support coated-diffuse internal random walks. Select a "
                   "renderer supporting coatings, or explicitly author a diffuse or conductor material.";
        case RenderGraphFailureReason::Unspecified:
            return "A shader set queries.HasFailed without a reason. Building the graph lists each such line as a "
                   "RenderGraphBuildReasonlessFailure warning; call FailSceneQuery there with a reason to name the "
                   "failing check.";
        case RenderGraphFailureReason::InvalidRadiance:
            return "A sample returned non-finite or negative radiance, such as a division by a zero probability.";
        case RenderGraphFailureReason::InterfaceVersion:
            return "The graph's scene interface version does not match the renderer. Rebuild the graph.";
        case RenderGraphFailureReason::SurfaceBudget:
            return "A shader traced more surface rays than scene_interface.surface_queries allows. Raise the budget "
                   "or trace fewer rays per sample.";
        case RenderGraphFailureReason::VisibilityBudget:
            return "A shader traced more shadow rays than scene_interface.visibility_queries allows. Estimators that "
                   "test every emissive triangle need a budget that grows with the scene's lights; the "
                   "polygonal-light template needs two per triangle. Raise the budget, capture with the capture "
                   "profile, or use an estimator that samples a fixed number of lights.";
        case RenderGraphFailureReason::LineBudget:
            return "A shader made more line visibility queries than scene_interface.line_queries allows.";
        case RenderGraphFailureReason::InvalidRay:
            return "A shader traced a ray with a non-unit direction or an invalid distance range.";
        case RenderGraphFailureReason::Traversal:
            return "Ray traversal could not finish, for example because the scene's BVH is deeper than its stack.";
        case RenderGraphFailureReason::RandomExhausted:
            return "A shader drew more random numbers in one sample than the stream provides.";
        case RenderGraphFailureReason::InvalidArgument:
            return "A scene function received an out-of-range or degenerate input, such as an empty sampling range.";
        case RenderGraphFailureReason::LightSample:
            return "No light could be selected or sampled for a shadow ray.";
        case RenderGraphFailureReason::StorageCapacity:
            return "A graph storage buffer, such as hit records or reservoirs, has fewer entries than the pixels that "
                   "use it. Size it from the render size.";
        case RenderGraphFailureReason::StoredRecord:
            return "A pass read per-pixel data (a hit record, reservoir or visibility-buffer texel) that an earlier "
                   "pass did not write for this pixel, frame or scene. Check that its producer runs first.";
        case RenderGraphFailureReason::SceneData:
            return "A scene lookup returned an invalid triangle, surface or material, such as a degenerate triangle "
                   "or a zero shading normal.";
        case RenderGraphFailureReason::InvalidWeight:
            return "A sampling weight, probability or PDF was non-finite or negative, or zero where it must be "
                   "positive.";
    }
    return "An unrecognized failure reason; the renderer may be older than the graph.";
}
} // namespace ludus::graphics::renderer
