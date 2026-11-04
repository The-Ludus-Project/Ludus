#include "internal/material_inspection.h"

#include "internal/inspector_fields.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/runtime/resources/scene.h>

#include <algorithm>
#include <string_view>

namespace ludus::editor
{
namespace
{
namespace format = content::scene_format;
using foundation::float32;
using foundation::int32;
using foundation::uint32;
using foundation::usize;

/// Significant digits shown for a material scalar; enough to round-trip typical source values.
constexpr int32 DISPLAYED_DIGITS = 6;

QString FormatScalar(float32 value)
{
    return QString::number(static_cast<foundation::float64>(value), 'g', DISPLAYED_DIGITS);
}
QString FormatRgb(float32 red, float32 green, float32 blue)
{
    return QStringLiteral("%1, %2, %3").arg(FormatScalar(red), FormatScalar(green), FormatScalar(blue));
}
QString FormatRgb(const foundation::math::Vector3& value)
{
    return FormatRgb(value.X, value.Y, value.Z);
}
QString FormatRgb(const foundation::StaticArray<float32, format::MATERIAL_RGB_CHANNELS>& value)
{
    return FormatRgb(value[0], value[1], value[2]);
}
QString GetModelName(format::MaterialModel model)
{
    switch (model)
    {
        case format::MaterialModel::MetallicRoughness:
            return QStringLiteral("Metallic-roughness");
        case format::MaterialModel::Diffuse:
            return QStringLiteral("Diffuse");
        case format::MaterialModel::Conductor:
            return QStringLiteral("Conductor");
        case format::MaterialModel::CoatedDiffuse:
            return QStringLiteral("Coated diffuse");
        case format::MaterialModel::Dielectric:
            return QStringLiteral("Dielectric");
        case format::MaterialModel::ThinDielectric:
            return QStringLiteral("Thin dielectric");
    }
    return QStringLiteral("Unknown (%1)").arg(static_cast<uint32>(model));
}
/// A small copy of a texture's first mip for a row; null when the texture is missing or malformed.
QImage MakeThumbnail(const resources::SceneDefinition& definition, uint32 texture)
{
    if (texture == format::NO_TEXTURE || static_cast<usize>(texture) >= definition.Textures.GetSize())
    {
        return {};
    }
    const resources::SceneTexture& source = definition.Textures[texture];
    constexpr usize BYTES_PER_TEXEL = 4;
    if (source.Width == 0 || source.Height == 0 ||
        source.Pixels.GetSize() != static_cast<usize>(source.Width) * source.Height * BYTES_PER_TEXEL)
    {
        return {};
    }
    // A view over the decoded pixels; scaling copies, so the definition is never retained.
    const QImage view(source.Pixels.GetData(),
                      static_cast<int>(source.Width),
                      static_cast<int>(source.Height),
                      static_cast<qsizetype>(source.Width * BYTES_PER_TEXEL),
                      QImage::Format_RGBA8888);
    return view.scaled(MATERIAL_THUMBNAIL_EXTENT,
                       MATERIAL_THUMBNAIL_EXTENT,
                       Qt::KeepAspectRatio,
                       Qt::SmoothTransformation);
}

/// A colour row: its text, and the display colour that stands for it.
MaterialPropertyRow MakeColourRow(const QString& label, const foundation::math::Vector3& linear)
{
    return {label, FormatRgb(linear), ConvertLinearToDisplayColour(linear.X, linear.Y, linear.Z), {}};
}

/// "Texture 3 · 1024 × 512 · sRGB", or "None" for format::NO_TEXTURE.
QString DescribeTexture(const resources::SceneDefinition& definition, uint32 texture)
{
    if (texture == format::NO_TEXTURE)
    {
        return QStringLiteral("None");
    }
    if (static_cast<usize>(texture) >= definition.Textures.GetSize())
    {
        return QStringLiteral("Texture %1 (missing)").arg(texture);
    }
    const resources::SceneTexture& source = definition.Textures[texture];
    return QStringLiteral("Texture %1 · %2 × %3 · %4")
        .arg(texture)
        .arg(source.Width)
        .arg(source.Height)
        .arg(source.IsLinear == true ? QStringLiteral("linear") : QStringLiteral("sRGB"));
}
} // namespace

QString GetFallbackMaterialName(int materialIndex)
{
    return QStringLiteral("Material %1").arg(materialIndex);
}

bool DescribeSceneObjectMaterial(const RenderScene& scene,
                                 const QString& objectId,
                                 QVector<MaterialPropertyRow>& outRows)
{
    if (scene.Definition == nullptr)
    {
        return false;
    }
    const resources::SceneDefinition& definition = *scene.Definition;
    int32 surface = -1;
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == objectId)
        {
            surface = object.Surface;
            break;
        }
    }
    if (surface < 0 || static_cast<usize>(surface) >= definition.Materials.GetSize())
    {
        return false;
    }
    const resources::SceneMaterial& material = definition.Materials[static_cast<usize>(surface)];
    QVector<MaterialPropertyRow> rows;
    const std::string_view name = material.Name.View();
    rows.append({QStringLiteral("Name"),
                 name.empty() == true ? GetFallbackMaterialName(surface + 1)
                                      : QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))});
    rows.append({QStringLiteral("Model"), GetModelName(material.Scattering.Model)});
    // Legacy materials without source parameters reflect their texture unscaled, or black untextured;
    // the same resolution the World composition writes into merged scenes.
    const foundation::math::Vector3 reflectance = material.HasSourceMaterial == true ? material.Reflectance
                                                  : material.Texture == format::NO_TEXTURE
                                                      ? foundation::math::Vector3{}
                                                      : foundation::math::Vector3{1, 1, 1};
    rows.append(MakeColourRow(QStringLiteral("Reflectance (linear RGB)"), reflectance));
    rows.append({QStringLiteral("Base color map"),
                 DescribeTexture(definition, material.Texture),
                 {},
                 MakeThumbnail(definition, material.Texture)});
    if (material.Texture != format::NO_TEXTURE)
    {
        rows.append({QStringLiteral("Addressing"),
                     material.Repeat == true ? QStringLiteral("Repeat") : QStringLiteral("Clamp")});
    }
    rows.append({QStringLiteral("Metallic"), FormatScalar(material.Metallic)});
    rows.append({QStringLiteral("Roughness (perceptual)"), FormatScalar(material.Roughness)});
    const format::MaterialScattering& scattering = material.Scattering;
    if (scattering.Model == format::MaterialModel::Conductor)
    {
        rows.append({QStringLiteral("IOR η (RGB)"), FormatRgb(scattering.Eta)});
        rows.append({QStringLiteral("Extinction k (RGB)"), FormatRgb(scattering.K)});
        rows.append({QStringLiteral("GGX α (U, V)"),
                     QStringLiteral("%1, %2").arg(FormatScalar(scattering.AlphaU), FormatScalar(scattering.AlphaV))});
    }
    else if (scattering.Model == format::MaterialModel::CoatedDiffuse)
    {
        rows.append({QStringLiteral("Coating IOR"), FormatScalar(scattering.CoatingEta)});
        rows.append({QStringLiteral("Coating thickness"), FormatScalar(scattering.CoatingThickness)});
        rows.append({QStringLiteral("Coating GGX α (U, V)"),
                     QStringLiteral("%1, %2").arg(FormatScalar(scattering.AlphaU), FormatScalar(scattering.AlphaV))});
    }
    else if (scattering.Model == format::MaterialModel::Dielectric ||
             scattering.Model == format::MaterialModel::ThinDielectric)
    {
        rows.append({QStringLiteral("IOR η"), FormatScalar(scattering.Eta[0])});
    }
    rows.append({QStringLiteral("Surface map"),
                 DescribeTexture(definition, material.SurfaceTexture),
                 {},
                 MakeThumbnail(definition, material.SurfaceTexture)});
    if (material.SurfaceTexture != format::NO_TEXTURE)
    {
        rows.append({QStringLiteral("Normal scale"), FormatScalar(material.NormalScale)});
    }
    rows.append({QStringLiteral("Alpha"),
                 material.IsAlphaMasked == true
                     ? QStringLiteral("Masked below %1").arg(FormatScalar(material.AlphaCutoff))
                     : QStringLiteral("Opaque")});
    const bool isEmissive = material.Emission.X > 0 || material.Emission.Y > 0 || material.Emission.Z > 0;
    MaterialPropertyRow emission{QStringLiteral("Emission (linear RGB)"), FormatRgb(material.Emission), {}, {}};
    if (isEmissive == true)
    {
        // Emission is in nits and can exceed one; the swatch shows its hue at full brightness.
        const float32 brightest = std::max({material.Emission.X, material.Emission.Y, material.Emission.Z});
        emission.Swatch = ConvertLinearToDisplayColour(material.Emission.X / brightest,
                                                       material.Emission.Y / brightest,
                                                       material.Emission.Z / brightest);
    }
    rows.append(emission);
    if (isEmissive == true)
    {
        rows.append({QStringLiteral("Emitting sides"),
                     material.IsEmissionTwoSided == true ? QStringLiteral("Both") : QStringLiteral("Front")});
        if (material.HasEmissionPower == true)
        {
            rows.append({QStringLiteral("Emission power (lm)"), FormatScalar(material.EmissionPower)});
        }
        rows.append({QStringLiteral("Emission map"),
                     DescribeTexture(definition, material.EmissionTexture),
                     {},
                     MakeThumbnail(definition, material.EmissionTexture)});
    }
    outRows = rows;
    return true;
}
} // namespace ludus::editor
