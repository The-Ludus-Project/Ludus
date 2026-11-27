#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ludus::editor
{
class WorldDocument;

// A default World's absorbing medium entity is a box of bounded homogeneous absorber: its sizes and RGB
// absorption coefficient are component fields, and its Transform places and turns it. World extraction turns
// it into the closed boundary that RT1 reference captures transport (graphics::renderer::
// ReferenceMediumDescription); raster and render graph views do not draw it. One medium per World, as the
// renderer admits one per capture. See docs/development/editor-workspace.md#absorbing-media.

/// The default World's built-in absorbing medium component identifier.
[[nodiscard]] QString GetAbsorbingMediumComponentId();

/// Boundary triangles of one generated medium box: two per face.
inline constexpr foundation::usize ABSORBING_MEDIUM_BOX_TRIANGLES = 12;

/// Set the component definition's absorption bounds from content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD,
/// the limit the cooked format and RT1 admit, so Properties validates against the engine's own limit.
void ApplyAbsorbingMediumLimits(QJsonObject& inoutComponent);

/// Linear RGB absorption coefficient per world metre of schema-validated component values.
[[nodiscard]] foundation::math::Vector3d GetAbsorbingMediumAbsorption(const QJsonObject& values);

/// Check the component values and the Transform values of one entity: positive finite sizes, absorption in
/// [0, MAX_MEDIUM_ABSORPTION_PER_WORLD] and a box that stays within MAX_REFERENCE_MEDIUM_COORDINATE_WORLD of the
/// origin however it is turned. outError names the field to change; success leaves it unchanged.
[[nodiscard]] bool
ValidateAbsorbingMedium(const QJsonObject& values, const QJsonObject& transformValues, QString& outError);

/// Generate the closed boundary of the box in world space: boxToWorld maps the box frame (centred, axes along
/// the component's width, height and depth) to the World. Each of the 12 triangles has three corners, wound
/// counterclockwise seen from outside so its right-hand normal points out of the medium, and shared edges repeat
/// bit-identical corners, as ReferenceMediumDescription requires. A mirroring boxToWorld keeps that orientation.
/// Failure (invalid values, a degenerate map or a corner beyond MAX_REFERENCE_MEDIUM_COORDINATE_WORLD)
/// preserves outCornersWorld and sets outError.
[[nodiscard]] bool BuildAbsorbingMediumBoundary(const QJsonObject& values,
                                                const foundation::math::Affine3& boxToWorld,
                                                QVector<foundation::math::Vector3>& outCornersWorld,
                                                QString& outError);

/// Distance in metres over which one channel falls to 1/e: 1 / absorptionPerWorld.
/// @return False for zero absorption (vacuum), which never attenuates; outDistanceWorld is then unchanged.
[[nodiscard]] bool TryComputeAttenuationDistance(foundation::float64 absorptionPerWorld,
                                                 foundation::float64& outDistanceWorld) noexcept;

/// The Properties readout for an absorption coefficient: each channel's attenuation distance in metres, or
/// "vacuum (no attenuation)" for zero, and how much of each channel a path of the box's smallest extent keeps.
/// @param absorptionPerWorld Finite nonnegative RGB coefficients per metre.
/// @param smallestExtentWorld The shortest side of the box in world metres, after the Transform scale; zero
///        omits the transmittance sentence.
[[nodiscard]] QString DescribeAbsorbingMedium(const foundation::math::Vector3d& absorptionPerWorld,
                                              foundation::float64 smallestExtentWorld);
} // namespace ludus::editor
