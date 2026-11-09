#include "internal/world_lighting_coverage.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
QString FormatCount(int32 count, const QString& singular, const QString& plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

float64 ComputeSurfaceArea(const math::Aabb3& bounds)
{
    const float64 x = static_cast<float64>(bounds.Max.X) - static_cast<float64>(bounds.Min.X);
    const float64 y = static_cast<float64>(bounds.Max.Y) - static_cast<float64>(bounds.Min.Y);
    const float64 z = static_cast<float64>(bounds.Max.Z) - static_cast<float64>(bounds.Min.Z);
    return 2 * (x * y + y * z + z * x);
}
} // namespace

math::Vector3 ComputeWorldLightRotationDegrees(math::Vector3 directionWorld)
{
    const float32 y = math::Clamp(directionWorld.Y, -1.0F, 1.0F);
    const float32 horizontal = math::Sqrt(math::Max(1 - y * y, 0.0F));
    const float32 pitch = math::Atan2(y, horizontal);
    // A vertical direction has no heading; zero keeps the result finite.
    const float32 yaw = horizontal > 0 ? math::Atan2(-directionWorld.X, -directionWorld.Z) : 0;
    return {math::RadiansToDegrees(pitch), math::RadiansToDegrees(yaw), 0};
}

WorldSunPlacement FitWorldSun(const WorldLightingSummary& world)
{
    WorldSunPlacement sun;
    // Follow the source's sun only when it shines down; a light from below the horizon cannot light the ground.
    sun.RotationDegrees = world.HasSunDirection == true && world.SunDirectionWorld.Y < 0
                              ? ComputeWorldLightRotationDegrees(world.SunDirectionWorld)
                              : DEFAULT_WORLD_SUN_ROTATION_DEGREES;
    sun.IlluminanceLux = DEFAULT_WORLD_SUN_ILLUMINANCE_LUX;
    sun.PositionWorld = world.HasBounds == true ? math::Vector3{(world.BoundsWorld.Min.X + world.BoundsWorld.Max.X) / 2,
                                                                world.BoundsWorld.Max.Y,
                                                                (world.BoundsWorld.Min.Z + world.BoundsWorld.Max.Z) / 2}
                                                : math::Vector3{0, 2, 0};
    return sun;
}

WorldLightingCoverage DescribeWorldLighting(const WorldLightingSummary& world)
{
    WorldLightingCoverage coverage;
    if (world.HasBounds == false || world.LightCount > 0 || world.HasEnvironment == true || world.LuminaireCount > 0)
    {
        return coverage;
    }
    const float64 area = ComputeSurfaceArea(world.BoundsWorld);
    const float64 illuminance = area > 0 ? world.EmittedLumens / area : 0;
    if (illuminance >= MIN_WORLD_MEAN_ILLUMINANCE_LUX)
    {
        return coverage;
    }
    QStringList causes{QStringLiteral("no enabled light or environment")};
    if (world.DisabledLightCount > 0)
    {
        causes.append(FormatCount(world.DisabledLightCount,
                                  QStringLiteral("imported light is disabled"),
                                  QStringLiteral("imported lights are disabled")) +
                      QStringLiteral(" (zero intensity)"));
    }
    if (world.EmissiveObjectCount > 0)
    {
        causes.append(QStringLiteral("emitting surfaces give about %1 lux on average (%2)")
                          .arg(illuminance, 0, 'g', 2)
                          .arg(FormatCount(world.EmissiveObjectCount,
                                           QStringLiteral("emissive object"),
                                           QStringLiteral("emissive objects"))));
    }
    coverage.Warning =
        QStringLiteral("This World renders almost black: %1. ").arg(causes.join(QStringLiteral("; "))) +
        (world.CanRepair == true
             ? QStringLiteral("Add a sun from Add World Preset, or reimport the scene with Choose environment.")
             : QStringLiteral("Add a directional light entity, or reimport the scene with Choose environment."));
    if (world.CanRepair == false)
    {
        return coverage;
    }
    coverage.Repair = WorldLightingRepair::AddSun;
    coverage.Sun = FitWorldSun(world);
    return coverage;
}
} // namespace ludus::editor
