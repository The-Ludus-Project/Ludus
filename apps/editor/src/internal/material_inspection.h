#pragma once

#include <QString>
#include <QVector>

namespace ludus::editor
{
struct RenderScene;

/// One read-only row of the World inspector's Material group.
struct MaterialPropertyRow final
{
    QString Label; ///< Property name, with its unit or space where it has one.
    QString Value; ///< Displayed value; colours are comma-separated linear RGB.
};

/// Describe the decoded material a scene object of a loaded render scene uses, as the renderer
/// receives it: its source name, scattering model, the parameters common to every model, the
/// optical parameters of the conductor and coated-diffuse models, alpha, emission and texture maps.
/// Parameters of other scattering models are omitted. Reads the immutable definition only.
/// @param scene Presented viewport scene; only cooked scenes (a Definition) carry materials.
/// @param objectId Stable World/scene object ID.
/// @param outRows Replaced on success; preserved on failure.
/// @return False when the scene has no cooked definition, no object objectId, or the object's
/// material index is outside the definition.
[[nodiscard]] bool
DescribeSceneObjectMaterial(const RenderScene& scene, const QString& objectId, QVector<MaterialPropertyRow>& outRows);
} // namespace ludus::editor
