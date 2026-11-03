#include "internal/render_scene.h"
#include "internal/world_lighting_coverage.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/runtime/resources/scene.h>

#include <QVector>

// The scene-reading half of the World lighting check, apart from world_lighting_coverage.cpp so that targets
// without RenderScene can still compile the check and its repair.
namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Rec. 709 luminance weights, the importer's photometric proxy (best-effort-scene-import.md).
constexpr float64 LUMINANCE_RED = 0.2126;
constexpr float64 LUMINANCE_GREEN = 0.7152;
constexpr float64 LUMINANCE_BLUE = 0.0722;
/// Indices per triangle of a triangle list.
constexpr usize TRIANGLE_CORNERS = 3;

float64 ComputeLuminance(math::Vector3 rgb)
{
    return LUMINANCE_RED * static_cast<float64>(rgb.X) + LUMINANCE_GREEN * static_cast<float64>(rgb.Y) +
           LUMINANCE_BLUE * static_cast<float64>(rgb.Z);
}

/// World-space area of one placed mesh.
float64 ComputePlacedArea(const RenderScene& scene, const PreviewObject& object)
{
    if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size() ||
        (scene.Definition != nullptr && scene.Definition->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size())))
    {
        return 0;
    }
    const BorrowedView<const math::Vector3> positions = GetRenderSceneMeshPositions(scene, object.Mesh);
    const BorrowedView<const uint32> indices = GetRenderSceneMeshIndices(scene, object.Mesh);
    float64 area = 0;
    for (usize i = 0; i + 2 < indices.GetSize(); i += TRIANGLE_CORNERS)
    {
        if (indices[i] >= positions.GetSize() || indices[i + 1] >= positions.GetSize() ||
            indices[i + 2] >= positions.GetSize())
        {
            continue;
        }
        const math::Vector3 a = math::TransformPoint(object.Transform, positions[indices[i]]);
        const math::Vector3 b = math::TransformPoint(object.Transform, positions[indices[i + 1]]);
        const math::Vector3 c = math::TransformPoint(object.Transform, positions[indices[i + 2]]);
        area += 0.5 * static_cast<float64>(math::Length(math::Cross(b - a, c - a)));
    }
    return area;
}
} // namespace

WorldLightingSummary SummarizeWorldLighting(const RenderScene& scene, bool canRepair)
{
    WorldLightingSummary world;
    // An unlit preview shows surfaces without lighting, so it cannot render dark.
    world.HasBounds = scene.Lit == true && scene.TryGetBounds({}, world.BoundsWorld) == true;
    world.LightCount = static_cast<int32>(scene.AuthoredLights.Count);
    world.LuminaireCount = static_cast<int32>(scene.Luminaires.Count);
    world.CanRepair = canRepair;
    if (scene.Definition != nullptr)
    {
        const resources::SceneDefinition& definition = *scene.Definition;
        const math::Vector3 environment = definition.Environment.Rgb;
        world.HasEnvironment = environment.X > 0 || environment.Y > 0 || environment.Z > 0;
        for (const resources::SceneLight& light : definition.Lights)
        {
            if (light.IsEnabled == true)
            {
                continue;
            }
            ++world.DisabledLightCount;
            math::Vector3 direction;
            if (world.HasSunDirection == false && light.Kind == resources::SceneLightKind::Directional &&
                math::TryNormalize(light.DirectionWorld, direction) == math::MathStatus::Success)
            {
                world.HasSunDirection = true;
                world.SunDirectionWorld = direction;
            }
        }
    }
    // A power-authored emitter's EmissionPower is the total of every placement of its material, so count it once.
    QVector<bool> isPowerCounted(scene.Surfaces.size(), false);
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Surface < 0 || object.Surface >= scene.Surfaces.size())
        {
            continue;
        }
        const PreviewSurface& surface = scene.Surfaces[object.Surface];
        const math::Vector3 emission = surface.Emission;
        if ((emission.X > 0 || emission.Y > 0 || emission.Z > 0) == false &&
            (surface.HasEmissionPower == true && surface.EmissionPower > 0) == false)
        {
            continue;
        }
        ++world.EmissiveObjectCount;
        if (surface.HasEmissionPower == true)
        {
            if (isPowerCounted[object.Surface] == false)
            {
                isPowerCounted[object.Surface] = true;
                world.EmittedLumens += static_cast<float64>(surface.EmissionPower);
            }
            continue;
        }
        const math::Vector3 mean{emission.X * surface.EmissionMapMean.X,
                                 emission.Y * surface.EmissionMapMean.Y,
                                 emission.Z * surface.EmissionMapMean.Z};
        const float64 sides = surface.IsEmissionTwoSided == true ? 2 : 1;
        world.EmittedLumens += math::kPi * ComputeLuminance(mean) * sides * ComputePlacedArea(scene, object);
    }
    return world;
}
} // namespace ludus::editor
