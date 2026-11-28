#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <QString>

class QWidget;

namespace ludus::editor
{
class RenderPreview;
struct RenderScene;

inline constexpr foundation::uint32 MAX_WORLD_POWER_PROBE_DENSITY = 4;

struct WorldPowerProbes final
{
    foundation::Array<graphics::renderer::ReferenceProbeRay> Rays;
    foundation::Array<foundation::math::Vector3d> Weights;
    foundation::math::Vector3d Emitted{};
    foundation::usize EscapeRayCount = 0;
    foundation::uint32 RowWidth = 0;
};

struct WorldPowerFlux final
{
    foundation::math::Vector3d Emitted{};
    foundation::math::Vector3d Absorbed{};
    foundation::math::Vector3d Escaped{};
};

/// Build a common set of radiance probes for both RT1 tracers. Escape samples an enclosing sphere;
/// absorption integrates each diffuse triangle's incident hemisphere on both sides.
[[nodiscard]] QString
BuildWorldPowerProbes(const RenderScene& scene, foundation::uint32 probeDensity, WorldPowerProbes& outProbes);

/// Weight the selected tracer's radiances by surface area, solid angle, and absorptance.
[[nodiscard]] bool IntegrateWorldPower(const WorldPowerProbes& probes,
                                       foundation::BorrowedView<const graphics::renderer::ReferencePixel> pixels,
                                       WorldPowerFlux& outFlux) noexcept;

/// Build the native Reference Capture tab that snapshots and measures the viewport's loaded World.
[[nodiscard]] QWidget* CreateWorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull);
} // namespace ludus::editor
