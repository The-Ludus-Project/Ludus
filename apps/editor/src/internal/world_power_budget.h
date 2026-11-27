#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/vector.hpp>

#include <QString>

class QWidget;

namespace ludus::editor
{
class RenderPreview;
struct RenderScene;

struct WorldPowerFlux final
{
    foundation::math::Vector3d Emitted{};
    foundation::math::Vector3d Absorbed{};
    foundation::math::Vector3d Escaped{};
    foundation::math::Vector3d Unresolved{};
};

struct WorldPowerBudget final
{
    QString Error;
    WorldPowerFlux Flux;
    foundation::float64 SamplingChange = 0;
};

/// Estimate the steady-state flux of a frozen World with untextured Lambertian surfaces, mesh emitters and
/// a black environment. SamplingChange is the largest absolute shift as a fraction of emission across the
/// absorbed, escaped and unresolved buckets and nonzero RGB channels when using a second visibility sample set.
[[nodiscard]] WorldPowerBudget MeasureWorldPower(const RenderScene& scene);

/// Build the native Reference Capture tab that snapshots and measures the viewport's loaded World.
[[nodiscard]] QWidget* CreateWorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull);
} // namespace ludus::editor
