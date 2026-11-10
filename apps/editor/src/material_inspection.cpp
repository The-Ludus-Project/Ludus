#include "internal/material_inspection.h"

#include "internal/inspector_fields.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/runtime/resources/scene.h>

#include <algorithm>
#include <string_view>

#include <QStringList>

namespace ludus::editor
{
namespace
{
namespace format = content::scene_format;
using foundation::float32;
using foundation::uint32;
using foundation::usize;

/// Significant digits shown for a material scalar; enough to round-trip typical source values.
constexpr foundation::int32 DISPLAYED_DIGITS = 6;

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
/// One closure-program lobe: its operation and the parameters it carries.
QString DescribeProgramLobe(const format::MaterialProgramLobe& lobe)
{
    switch (lobe.Operation)
    {
        case format::MaterialLobeOperation::Diffuse:
            return QStringLiteral("Diffuse");
        case format::MaterialLobeOperation::Conductor:
            return QStringLiteral("Conductor (η %1; k %2)").arg(FormatRgb(lobe.Eta), FormatRgb(lobe.K));
        case format::MaterialLobeOperation::CosinePower:
            return QStringLiteral("Cosine power %1").arg(FormatScalar(lobe.Power));
        case format::MaterialLobeOperation::None:
            break;
    }
    return QStringLiteral("Unknown");
}
/// The capabilities a pass requirement tests, in reading order.
QString DescribeCapabilities(format::MaterialCapabilityBits capabilities)
{
    struct Capability final
    {
        format::MaterialCapabilityBits Bit;
        const char* Label;
    };
    constexpr usize CAPABILITY_COUNT = 5;
    constexpr foundation::StaticArray<Capability, CAPABILITY_COUNT> CAPABILITIES{{
        {format::LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION, "continuous reflection"},
        {format::LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER, "fixed three-value sampler"},
        {format::LUDUS_MATERIAL_CAPABILITY_TRANSMISSION, "transmission"},
        {format::LUDUS_MATERIAL_CAPABILITY_INTERNAL_WALK, "internal walk"},
        {format::LUDUS_MATERIAL_CAPABILITY_DELTA, "delta events"},
    }};
    QStringList names;
    for (const Capability& capability : CAPABILITIES)
    {
        if ((capabilities & capability.Bit) != 0)
        {
            names.append(QString::fromLatin1(capability.Label));
        }
    }
    if (names.isEmpty() == true)
    {
        return QStringLiteral("None");
    }
    names.front()[0] = names.front()[0].toUpper();
    return names.join(QStringLiteral(", "));
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
        case format::MaterialModel::DiffuseConductorMix:
            return QStringLiteral("Diffuse / rough conductor Mix");
        case format::MaterialModel::ProjectBrdf:
            return QStringLiteral("Project BRDF");
        case format::MaterialModel::Program:
            return QStringLiteral("Closure program");
        case format::MaterialModel::Unresolved:
            return QStringLiteral("Unsupported imported material");
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
    return {label,
            FormatRgb(linear),
            false,
            ConvertLinearToDisplayColour(static_cast<foundation::float64>(linear.X),
                                         static_cast<foundation::float64>(linear.Y),
                                         static_cast<foundation::float64>(linear.Z)),
            {}};
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

QJsonObject MakeProjectBrdfRoughnessProperty()
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QStringLiteral("Project BRDF roughness")},
            {QStringLiteral("min"), format::MIN_AUTHORED_ROUGHNESS},
            {QStringLiteral("max"), 1}};
}

QJsonObject CoatingField::MakeNumberProperty() const
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QString::fromLatin1(Label)},
            {QStringLiteral("min"), static_cast<foundation::float64>(Minimum)},
            {QStringLiteral("max"), static_cast<foundation::float64>(Maximum)}};
}

const resources::SceneMaterial* GetSceneObjectMaterialOrNull(const RenderScene& scene, const QString& objectId)
{
    if (scene.Definition == nullptr)
    {
        return nullptr;
    }
    const resources::SceneDefinition& definition = *scene.Definition;
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == objectId)
        {
            return object.Surface >= 0 && static_cast<usize>(object.Surface) < definition.Materials.GetSize()
                       ? &definition.Materials[static_cast<usize>(object.Surface)]
                       : nullptr;
        }
    }
    return nullptr;
}

bool DescribeSceneObjectMaterial(const RenderScene& scene,
                                 const QString& objectId,
                                 QVector<MaterialPropertyRow>& outRows)
{
    foundation::int32 surface = 0;
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == objectId)
        {
            surface = object.Surface;
        }
    }
    const resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, objectId);
    if (materialOrNull == nullptr)
    {
        return false;
    }
    const resources::SceneDefinition& definition = *scene.Definition;
    const resources::SceneMaterial& material = *materialOrNull;
    QVector<MaterialPropertyRow> rows;
    const std::string_view name = material.Name.View();
    rows.append({QStringLiteral("Name"),
                 name.empty() == true ? GetFallbackMaterialName(surface + 1)
                                      : QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))});
    rows.append({QStringLiteral("Model"), GetModelName(material.Scattering.Model)});
    if (material.Scattering.Model == format::MaterialModel::Unresolved)
    {
        outRows = foundation::Move(rows);
        return true;
    }
    // Legacy materials without source parameters reflect their texture unscaled, or black untextured;
    // the same resolution the World composition writes into merged scenes.
    const foundation::math::Vector3 reflectance = material.HasSourceMaterial == true ? material.Reflectance
                                                  : material.Texture == format::NO_TEXTURE
                                                      ? foundation::math::Vector3{}
                                                      : foundation::math::Vector3{1, 1, 1};
    rows.append(MakeColourRow(QStringLiteral("Reflectance (linear RGB)"), reflectance));
    rows.append({QStringLiteral("Base color map"),
                 DescribeTexture(definition, material.Texture),
                 false,
                 {},
                 MakeThumbnail(definition, material.Texture)});
    if (material.Texture != format::NO_TEXTURE)
    {
        rows.append({QStringLiteral("Addressing"),
                     material.Repeat == true ? QStringLiteral("Repeat") : QStringLiteral("Clamp")});
    }
    const bool isMix = material.Scattering.Model == format::MaterialModel::DiffuseConductorMix;
    const bool isProgram = material.Scattering.Model == format::MaterialModel::Program;
    if (material.Scattering.Model != format::MaterialModel::ProjectBrdf)
    {
        rows.append({isMix == true       ? QStringLiteral("Conductor weight")
                     : isProgram == true ? QStringLiteral("Metallic (program input)")
                                         : QStringLiteral("Metallic"),
                     FormatScalar(material.Metallic)});
        rows.append(
            {isProgram == true ? QStringLiteral("Roughness (program input)") : QStringLiteral("Roughness (perceptual)"),
             FormatScalar(material.Roughness)});
    }
    const format::MaterialScattering& scattering = material.Scattering;
    if (isProgram == true)
    {
        const format::MaterialProgram& program = material.Program;
        for (usize lobe = 0; lobe < program.LobeCount; ++lobe)
        {
            rows.append({QStringLiteral("Lobe %1").arg(lobe + 1), DescribeProgramLobe(program.Lobes[lobe])});
        }
        rows.append({QStringLiteral("Weights and roughness"),
                     QStringLiteral("%1 instructions over %2 constants")
                         .arg(program.InstructionCount)
                         .arg(program.ConstantCount)});
        rows.append(
            {QStringLiteral("Capabilities"), DescribeCapabilities(format::GetMaterialProgramCapabilities(program))});
        rows.append({QStringLiteral("Raster approximation"),
                     QStringLiteral("Metallic-roughness from the reflectance, metallic and roughness inputs")});
    }
    else if (scattering.Model == format::MaterialModel::ProjectBrdf)
    {
        rows.append({QStringLiteral("Lobe weights"), FormatRgb(scattering.Eta)});
        rows.append({QStringLiteral("Lobe powers"), FormatRgb(scattering.K)});
        rows.append({QStringLiteral("Fixed-dimension reflection"),
                     scattering.CoatingThickness == 1 ? QStringLiteral("Declared") : QStringLiteral("Undeclared")});
    }
    else if (format::HasRoughConductorLobe(scattering.Model) == true)
    {
        rows.append({QStringLiteral("IOR η (RGB)"), FormatRgb(scattering.Eta)});
        rows.append({QStringLiteral("Extinction k (RGB)"), FormatRgb(scattering.K)});
        rows.append({QStringLiteral("GGX α (U, V)"),
                     QStringLiteral("%1, %2").arg(FormatScalar(scattering.AlphaU), FormatScalar(scattering.AlphaV))});
    }
    else if (scattering.Model == format::MaterialModel::CoatedDiffuse)
    {
        rows.append({QStringLiteral("Coating IOR"), FormatScalar(scattering.CoatingEta), true});
        rows.append({QStringLiteral("Coating thickness"), FormatScalar(scattering.CoatingThickness), true});
        rows.append({QStringLiteral("Coating GGX α (U, V)"),
                     QStringLiteral("%1, %2").arg(FormatScalar(scattering.AlphaU), FormatScalar(scattering.AlphaV)),
                     true});
    }
    else if (scattering.Model == format::MaterialModel::Dielectric ||
             scattering.Model == format::MaterialModel::ThinDielectric)
    {
        rows.append({QStringLiteral("IOR η"), FormatScalar(scattering.Eta[0])});
    }
    rows.append({QStringLiteral("Surface map"),
                 DescribeTexture(definition, material.SurfaceTexture),
                 false,
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
    MaterialPropertyRow emission{QStringLiteral("Emission (linear RGB)"), FormatRgb(material.Emission), false, {}, {}};
    if (isEmissive == true)
    {
        // Emission is in nits and can exceed one; the swatch shows its hue at full brightness.
        const float32 brightest = std::max({material.Emission.X, material.Emission.Y, material.Emission.Z});
        emission.Swatch =
            ConvertLinearToDisplayColour(static_cast<foundation::float64>(material.Emission.X / brightest),
                                         static_cast<foundation::float64>(material.Emission.Y / brightest),
                                         static_cast<foundation::float64>(material.Emission.Z / brightest));
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
                     false,
                     {},
                     MakeThumbnail(definition, material.EmissionTexture)});
    }
    outRows = rows;
    return true;
}
} // namespace ludus::editor
