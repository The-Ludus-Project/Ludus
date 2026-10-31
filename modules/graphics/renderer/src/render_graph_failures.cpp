#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/scene_shader_layout.h>

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
        case RenderGraphFailureReason::Unspecified:
            return "A shader set queries.HasFailed without a reason. Call FailSceneQuery with a reason to name the "
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
    }
    return "An unrecognized failure reason; the renderer may be older than the graph.";
}
} // namespace ludus::graphics::renderer
