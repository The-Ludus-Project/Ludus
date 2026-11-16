#pragma once

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <QColor>
#include <QImage>
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

[[nodiscard]] QJsonObject MakeProjectBrdfRoughnessProperty();

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

/// One editable strand control of a World hair-card slot, stored under Key in the slot's "hair_card" object.
struct HairCardField final
{
    const char* Key;
    const char* Label;
    foundation::float32 content::scene_format::HairCardParameters::* Member;
    foundation::float32 Minimum;
    foundation::float32 Maximum;

    /// Number property with this field's label and bounds, for WorldDocument::ParseValue.
    [[nodiscard]] QJsonObject MakeNumberProperty() const;
};

/// The strand controls of a World hair-card slot, in the order the inspector lists them. Bounds are the
/// scene format's admission limits.
inline const HairCardField HAIR_CARD_FIELDS[] = {
    {"strand_direction_degrees",
     "Strand direction (°)",
     &content::scene_format::HairCardParameters::StrandDirectionDegrees,
     -content::scene_format::MAX_HAIR_CARD_STRAND_DEGREES,
     content::scene_format::MAX_HAIR_CARD_STRAND_DEGREES},
    {"primary_shift_degrees",
     "Primary highlight shift (°)",
     &content::scene_format::HairCardParameters::PrimaryShiftDegrees,
     -content::scene_format::MAX_HAIR_CARD_SHIFT_DEGREES,
     content::scene_format::MAX_HAIR_CARD_SHIFT_DEGREES},
    {"secondary_shift_degrees",
     "Secondary highlight shift (°)",
     &content::scene_format::HairCardParameters::SecondaryShiftDegrees,
     -content::scene_format::MAX_HAIR_CARD_SHIFT_DEGREES,
     content::scene_format::MAX_HAIR_CARD_SHIFT_DEGREES},
    {"primary_weight", "Primary highlight weight", &content::scene_format::HairCardParameters::PrimaryWeight, 0, 1},
    {"secondary_weight",
     "Tinted secondary highlight weight",
     &content::scene_format::HairCardParameters::SecondaryWeight,
     0,
     1},
};

/// Key of a hair-card slot's optional mask cutoff override; only masked materials accept it.
inline constexpr const char* HAIR_CARD_CUTOFF_KEY = "alpha_cutoff";

/// Number property of the hair-card mask cutoff, in [0, 1].
[[nodiscard]] QJsonObject MakeHairCardCutoffProperty();

/// A "hair_card" slot object with every strand control at its scene-format default and no cutoff override.
[[nodiscard]] QJsonObject MakeDefaultHairCard();

/// Key of a World skin slot's scattering distance in its "skin" object: linear RGB metres, as in the scene source
/// format.
inline constexpr const char* SKIN_DISTANCE_KEY = "scattering_distance";
/// The inspector edits skin scattering distances in millimetres; slots store metres.
inline constexpr foundation::float64 MILLIMETRES_PER_METRE = 1000;

/// One channel of a World skin slot's scattering distance, edited in millimetres.
struct SkinDistanceField final
{
    const char* Key;   ///< Field key below "scene/skin/".
    const char* Label; ///< Short inspector label.
    foundation::usize Channel;

    /// Number property in millimetres, bounded by content::scene_format::MAX_SKIN_SCATTERING_DISTANCE.
    [[nodiscard]] QJsonObject MakeNumberProperty() const;
};

/// The red, green and blue scattering-distance fields of a World skin slot, in that order.
inline const foundation::StaticArray<SkinDistanceField, content::scene_format::MATERIAL_RGB_CHANNELS>
    SKIN_DISTANCE_FIELDS{{
        {"scattering_red_mm", "Scattering red (mm)", 0},
        {"scattering_green_mm", "Scattering green (mm)", 1},
        {"scattering_blue_mm", "Scattering blue (mm)", 2},
    }};

/// A "skin" slot object. A material that is already skin keeps its scattering distance; any other starts from human
/// skin's normalized-diffusion distance d, (1.50, 0.94, 0.57) mm. It comes from the measured skin1 coefficients of
/// Henrik Wann Jensen, Stephen R. Marschner, Marc Levoy and Pat Hanrahan, "A Practical Model for Subsurface Light
/// Transport", SIGGRAPH 2001, Table 2 (sigma_s' = 0.74, 0.88, 1.01 and sigma_a = 0.032, 0.17, 0.48 per mm), as the
/// mean free path l = 1 / sigma_t' over Per H. Christensen's scale s = 1.85 - A + 7 (0.8 - A)^3 with A =
/// sigma_s' / sigma_t' ("An Approximate Reflectance Profile for Efficient Subsurface Scattering", SIGGRAPH 2015
/// Talks, Eq. 2, DOI 10.1145/2775280.2792555). Skin1's diffuse mean free path, (3.67, 1.37, 0.68) mm, is a different
/// distance and blurs the profile about twice as wide.
[[nodiscard]] QJsonObject MakeDefaultSkin(const resources::SceneMaterial* materialOrNull);

/// Borrow the resolved material for as long as the scene definition is retained.
[[nodiscard]] const resources::SceneMaterial* GetSceneObjectMaterialOrNull(const RenderScene& scene,
                                                                           const QString& objectId);

/// One read-only row of the World inspector's Material group.
struct MaterialPropertyRow final
{
    QString Label;                   ///< Short property name; a short unit such as "(lm)" stays, the rest is Detail.
    QString Value;                   ///< Displayed value; colours are comma-separated linear RGB.
    bool IsCoatingParameter = false; ///< Editable separately in an imported coated-diffuse slot.
    /// Display colour of a colour row, shown as a swatch beside Value; invalid for other rows.
    QColor Swatch{};
    /// Small preview of a texture row, shown beside Value; null for other rows and for missing textures.
    QImage Thumbnail{};
    /// What the short Label leaves out (its colour space, unit or meaning) and, for a texture, which texture it is;
    /// shown as the row's tooltip. Empty when Label and Value say everything.
    QString Detail{};
    /// Editable separately in a World hair-card slot.
    bool IsHairCardParameter = false;
    /// Editable separately in a World skin slot.
    bool IsSkinParameter = false;
};

/// Edge of a texture thumbnail in pixels; the longer side of the texture is scaled to it.
inline constexpr int MATERIAL_THUMBNAIL_EXTENT = 48;

/// Name shown for a material the source scene does not name: "Material N", N counting from one in scene order.
[[nodiscard]] QString GetFallbackMaterialName(int materialIndex);

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
