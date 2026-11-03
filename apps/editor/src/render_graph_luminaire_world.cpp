#include "internal/render_graph_luminaire_coverage.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

// The scene-reading half of the luminaire check, apart from render_graph_luminaire_coverage.cpp so that targets
// without RenderScene can still compile the check and its repairs.
namespace ludus::editor
{
using namespace foundation;
RenderGraphLuminaireWorld
SummarizeRenderGraphLuminaireWorld(const RenderScene& scene, bool canRepair, const RenderGraphLuminaireView* viewOrNull)
{
    RenderGraphLuminaireWorld world;
    world.HasBounds = scene.TryGetBounds({}, world.BoundsWorld);
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Surface >= 0 && object.Surface < scene.Surfaces.size())
        {
            const math::Vector3 emission = scene.Surfaces[object.Surface].Emission;
            world.EmissiveObjectCount += emission.X > 0 || emission.Y > 0 || emission.Z > 0 ? 1 : 0;
        }
    }
    world.LightCount = static_cast<int32>(scene.AuthoredLights.Count);
    world.Luminaires = scene.Luminaires;
    world.CanRepair = canRepair;
    // Only a World that needs a repair pays for the brute-force rays.
    if (viewOrNull == nullptr || math::IsFinite(viewOrNull->EyeWorld) == false ||
        math::IsFinite(viewOrNull->ForwardWorld) == false || world.HasBounds == false ||
        DescribeRenderGraphLuminaireCoverage(world).Warning.isEmpty() == true)
    {
        return world;
    }
    world.HasAnchor = true;
    world.AnchorWorld = viewOrNull->EyeWorld;
    float64 viewDistance = 0;
    if (TryTraceRenderSceneRay(scene, {viewOrNull->EyeWorld, viewOrNull->ForwardWorld}, viewDistance) == true)
    {
        world.AnchorWorld = viewOrNull->EyeWorld + viewOrNull->ForwardWorld * (LUMINAIRE_ANCHOR_VIEW_FRACTION *
                                                                               static_cast<float32>(viewDistance));
    }
    float64 ceiling = 0;
    if (TryTraceRenderSceneRay(scene, {world.AnchorWorld, {0, 1, 0}}, ceiling) == true && ceiling > 0)
    {
        world.HasCeiling = true;
        world.CeilingHeightWorld = static_cast<float32>(ceiling);
    }
    return world;
}
} // namespace ludus::editor
