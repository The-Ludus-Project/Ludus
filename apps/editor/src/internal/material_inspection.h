#pragma once

#include <ludus/content/scene_format/material.h>

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ludus::resources
{
struct SceneMaterial;
}

namespace ludus::editor
{
struct RenderScene;

struct CoatingField final
{
    const char* Key;
    const char* Label;
    foundation::float32 content::scene_format::MaterialScattering::* Member;
    foundation::float32 Minimum;
    foundation::float32 Maximum;

    [[nodiscard]] QJsonObject MakeNumberProperty() const;
};

inline const CoatingField COATING_FIELDS[] = {
    {"alpha",
     "Coating GGX alpha",
     &content::scene_format::MaterialScattering::AlphaU,
     content::scene_format::LUDUS_COATING_MIN_ALPHA,
     1},
    {"eta",
     "Coating IOR",
     &content::scene_format::MaterialScattering::CoatingEta,
     1,
     content::scene_format::MAX_MATERIAL_OPTICAL_CONSTANT},
    {"thickness",
     "Coating optical thickness",
     &content::scene_format::MaterialScattering::CoatingThickness,
     0,
     content::scene_format::MAX_MATERIAL_OPTICAL_CONSTANT},
};

/// Borrow the resolved material for as long as the scene definition is retained.
[[nodiscard]] const resources::SceneMaterial* GetSceneObjectMaterialOrNull(const RenderScene& scene,
                                                                           const QString& objectId);

/// One read-only row of the World inspector's Material group.
struct MaterialPropertyRow final
{
    QString Label;                   ///< Property name, with its unit or space where it has one.
    QString Value;                   ///< Displayed value; colours are comma-separated linear RGB.
    bool IsCoatingParameter = false; ///< Editable separately in an imported coated-diffuse slot.
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
