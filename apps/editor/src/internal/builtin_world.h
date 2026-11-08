#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
class WorldDocument;
/// True only for the editor-owned default schema; project schemas keep their own contracts.
[[nodiscard]] bool IsBuiltInWorld(const WorldDocument& world);
/// Read a bundled default-World asset. Failure preserves outBytes.
[[nodiscard]] bool ReadBuiltInWorldAsset(const QString& name, QByteArray& outBytes, QString& outError);
/// Euler X rotation, in degrees, that turns a light's or luminaire's -Z emission straight down.
inline constexpr foundation::float32 WORLD_LUMINAIRE_DOWNWARD_DEGREES = -90;
/// Half extent of the Luminaire preset in metres: a 0.7 m panel.
inline constexpr foundation::float32 WORLD_LUMINAIRE_DEFAULT_HALF_EXTENT =
    graphics::renderer::DEFAULT_PANEL_LUMINAIRE_HALF_EXTENT;
/// Transform and size of a new default-schema luminaire entity.
struct WorldLuminairePlacement final
{
    foundation::math::Vector3 PositionWorld{};   ///< Panel center, the transform's position in metres.
    foundation::math::Vector3 RotationDegrees{}; ///< The transform's Euler rotation; the panel emits along its -Z.
    foundation::float32 HalfExtentWorld = WORLD_LUMINAIRE_DEFAULT_HALF_EXTENT; ///< The luminaire's halfExtent.
};
/// Create a luminaire entity (transform and luminaire components, schema defaults otherwise) in a default World as
/// one validated, undoable history transaction. Failure preserves the document and outId. Editor thread only.
[[nodiscard]] bool CreateWorldLuminaire(WorldDocument& inoutWorld,
                                        const foundation::identity::EntropySource& entropy,
                                        const WorldLuminairePlacement& placement,
                                        QString& outId,
                                        QString& outError);
/// Transform and illuminance of a new default-schema sun: a directional light entity.
struct WorldSunPlacement final
{
    /// The transform's position in metres. A directional light ignores it; it only places the viewport gizmo.
    foundation::math::Vector3 PositionWorld{};
    /// The transform's Euler rotation in degrees; the light shines along its -Z.
    foundation::math::Vector3 RotationDegrees{};
    /// The light's intensity: illuminance in lux on a surface facing the sun.
    foundation::float32 IlluminanceLux = 0;
};
/// Create a directional light entity named "Sun" with the Static Sun role in a
/// default World as one validated, undoable history transaction. Failure preserves the document and outId. Editor
/// thread only.
[[nodiscard]] bool CreateWorldSun(WorldDocument& inoutWorld,
                                  const foundation::identity::EntropySource& entropy,
                                  const WorldSunPlacement& placement,
                                  QString& outId,
                                  QString& outError);
/// Create one bundled geometry, light or luminaire preset as one validated history transaction.
/// Failure preserves the document and outId. Must run on the editor thread.
[[nodiscard]] bool CreateWorldObject(WorldDocument& inoutWorld,
                                     const foundation::identity::EntropySource& entropy,
                                     const QString& preset,
                                     QString& outId,
                                     QString& outError);
} // namespace ludus::editor
