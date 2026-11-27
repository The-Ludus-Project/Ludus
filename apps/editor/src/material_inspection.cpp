#include "internal/material_inspection.h"

#include "internal/inspector_fields.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/runtime/resources/scene.h>

#include <algorithm>
#include <limits>
#include <string_view>

#include <QJsonArray>
#include <QSize>
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
/// Most texels a texture may have and still read as a constant when every texel is the same (4 × 4).
constexpr usize MAX_CONSTANT_TEXTURE_TEXELS = 16;
// Cooked surface-map byte offsets from the MaterialRecordV6 channel layout.
constexpr usize SURFACE_ROUGHNESS_CHANNEL = 2;
constexpr usize SURFACE_METALLIC_CHANNEL = format::TEXEL_BYTES - 1;

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
        case format::MaterialLobeOperation::OrenNayar:
            return QStringLiteral("Oren–Nayar diffuse (σ in radians)");
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
        case format::MaterialModel::MediumInterface:
            return QStringLiteral("Absorbing medium boundary");
        case format::MaterialModel::HairCard:
            return QStringLiteral("Hair card");
        case format::MaterialModel::Skin:
            return QStringLiteral("Skin");
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

/// Preview one linear multiplier as opaque gray. Sample only the thumbnail's texels.
/// Named packed-map previews are adapted from Alloy's texture inspector.
/// https://github.com/Josh015/Alloy/blob/1c46654ffe783650644bda82b4e06998e34d6cf0/Scripts/MaterialInspectors/Editor/FieldBased/Drawers/AlloyTextureDrawer.cs#L147-L224
QImage MakeSurfaceChannelThumbnail(const resources::SceneTexture& source, usize channel)
{
    if (source.IsLinear == false || source.Width == 0 || source.Height == 0 ||
        source.Width > format::MAX_TEXTURE_DIMENSION || source.Height > format::MAX_TEXTURE_DIMENSION ||
        source.Pixels.GetSize() != static_cast<usize>(source.Width) * source.Height * format::TEXEL_BYTES)
    {
        return {};
    }
    QSize extent(static_cast<int>(source.Width), static_cast<int>(source.Height));
    if (extent.width() > MATERIAL_THUMBNAIL_EXTENT || extent.height() > MATERIAL_THUMBNAIL_EXTENT)
    {
        extent.scale(MATERIAL_THUMBNAIL_EXTENT, MATERIAL_THUMBNAIL_EXTENT, Qt::KeepAspectRatio);
    }
    QImage thumbnail(std::max(1, extent.width()), std::max(1, extent.height()), QImage::Format_Grayscale8);
    if (thumbnail.isNull() == true)
    {
        return {};
    }
    const usize width = static_cast<usize>(thumbnail.width());
    const usize height = static_cast<usize>(thumbnail.height());
    for (usize y = 0; y < height; ++y)
    {
        foundation::uint8* row = thumbnail.scanLine(static_cast<int>(y));
        const usize sourceY = ((2 * y + 1) * source.Height) / (2 * height);
        for (usize x = 0; x < width; ++x)
        {
            const usize sourceX = ((2 * x + 1) * source.Width) / (2 * width);
            row[x] = source.Pixels[(sourceY * source.Width + sourceX) * format::TEXEL_BYTES + channel];
        }
    }
    return thumbnail;
}

/// A colour row: its text, the display colour that stands for it, and what its short label leaves out.
MaterialPropertyRow MakeColourRow(const QString& label, const foundation::math::Vector3& linear, const QString& detail)
{
    return {label,
            FormatRgb(linear),
            false,
            ConvertLinearToDisplayColour(static_cast<foundation::float64>(linear.X),
                                         static_cast<foundation::float64>(linear.Y),
                                         static_cast<foundation::float64>(linear.Z)),
            {},
            detail};
}

/// What a texture is, readably: "1024 × 512 · sRGB", "Constant white" for an importer's one-texel stand-in, "None"
/// for format::NO_TEXTURE or "Missing" for an index past the scene's textures. A cooked scene keeps no texture
/// names, so its size and encoding (with the thumbnail beside them) say which texture it is; the scene's own texture
/// number is in DescribeTextureDetail, not in a "Texture 3 · …" prefix that shortened to "Texture 3…48".
QString DescribeTexture(const resources::SceneDefinition& definition, uint32 texture)
{
    if (texture == format::NO_TEXTURE)
    {
        return QStringLiteral("None");
    }
    if (static_cast<usize>(texture) >= definition.Textures.GetSize())
    {
        return QStringLiteral("Missing");
    }
    const resources::SceneTexture& source = definition.Textures[texture];
    const QString encoding = source.IsLinear == true ? QStringLiteral("linear") : QStringLiteral("sRGB");
    // A tiny texture of one texel value is a constant, such as an importer's 4 × 4 white stand-in: name the constant.
    constexpr usize BYTES_PER_TEXEL = 4;
    const usize texelCount = static_cast<usize>(source.Width) * source.Height;
    if (source.IsLinear == false && texelCount > 0 && texelCount <= MAX_CONSTANT_TEXTURE_TEXELS &&
        source.Pixels.GetSize() == texelCount * BYTES_PER_TEXEL)
    {
        bool isUniform = true;
        for (usize i = BYTES_PER_TEXEL; i < source.Pixels.GetSize(); ++i)
        {
            isUniform = isUniform == true && source.Pixels[i] == source.Pixels[i % BYTES_PER_TEXEL];
        }
        if (isUniform == true)
        {
            const bool isWhite = source.Pixels[0] == std::numeric_limits<foundation::uint8>::max() &&
                                 source.Pixels[1] == std::numeric_limits<foundation::uint8>::max() &&
                                 source.Pixels[2] == std::numeric_limits<foundation::uint8>::max();
            const bool isBlack = source.Pixels[0] == 0 && source.Pixels[1] == 0 && source.Pixels[2] == 0;
            return isWhite == true   ? QStringLiteral("Constant white")
                   : isBlack == true ? QStringLiteral("Constant black")
                                     : QStringLiteral("Constant colour");
        }
    }
    return QStringLiteral("%1 × %2 · %3").arg(source.Width).arg(source.Height).arg(encoding);
}

/// The tooltip of a texture row: its role, then the scene's texture number, size and encoding.
QString DescribeTextureDetail(const QString& role, const resources::SceneDefinition& definition, uint32 texture)
{
    if (texture == format::NO_TEXTURE)
    {
        return QStringLiteral("%1: none").arg(role);
    }
    if (static_cast<usize>(texture) >= definition.Textures.GetSize())
    {
        return QStringLiteral("%1: texture %2 is missing from the cooked scene").arg(role).arg(texture);
    }
    const resources::SceneTexture& source = definition.Textures[texture];
    return QStringLiteral("%1: texture %2 of the cooked scene, %3 × %4, %5")
        .arg(role)
        .arg(texture)
        .arg(source.Width)
        .arg(source.Height)
        .arg(source.IsLinear == true ? QStringLiteral("linear data") : QStringLiteral("sRGB colour"));
}

/// A texture row: what the texture is, its thumbnail, and its role and number in the tooltip.
MaterialPropertyRow
MakeTextureRow(const QString& label, const QString& role, const resources::SceneDefinition& definition, uint32 texture)
{
    return {label,
            DescribeTexture(definition, texture),
            false,
            {},
            MakeThumbnail(definition, texture),
            DescribeTextureDetail(role, definition, texture)};
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

QJsonObject HairCardField::MakeNumberProperty() const
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QString::fromUtf8(Label)},
            {QStringLiteral("min"), static_cast<foundation::float64>(Minimum)},
            {QStringLiteral("max"), static_cast<foundation::float64>(Maximum)}};
}

QJsonObject MakeHairCardCutoffProperty()
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QStringLiteral("Mask cutoff")},
            {QStringLiteral("min"), 0},
            {QStringLiteral("max"), 1}};
}

QJsonObject MakeDefaultHairCard()
{
    const format::HairCardParameters defaults;
    QJsonObject hair;
    for (const HairCardField& field : HAIR_CARD_FIELDS)
    {
        hair.insert(QString::fromLatin1(field.Key), static_cast<foundation::float64>(defaults.*field.Member));
    }
    return hair;
}

QJsonObject SkinDistanceField::MakeNumberProperty() const
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QString::fromUtf8(Label)},
            {QStringLiteral("min"), 0},
            {QStringLiteral("max"),
             static_cast<foundation::float64>(format::MAX_SKIN_SCATTERING_DISTANCE) * MILLIMETRES_PER_METRE}};
}

QJsonObject SkinSurfaceField::MakeNumberProperty() const
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QString::fromUtf8(Label)},
            {QStringLiteral("min"), IsRoughness() == true ? format::MIN_AUTHORED_ROUGHNESS : 0.0},
            {QStringLiteral("max"), 1}};
}

foundation::float64 GetShortestFloat32Decimal(float32 value)
{
    // Nine significant digits round-trip every float32.
    constexpr foundation::int32 FLOAT32_ROUND_TRIP_DIGITS = 9;
    for (foundation::int32 digits = 1; digits < FLOAT32_ROUND_TRIP_DIGITS; ++digits)
    {
        const foundation::float64 candidate =
            QString::number(static_cast<foundation::float64>(value), 'g', digits).toDouble();
        if (static_cast<float32>(candidate) == value)
        {
            return candidate;
        }
    }
    return static_cast<foundation::float64>(value);
}

QJsonObject MakeDefaultSkin(const resources::SceneMaterial* materialOrNull)
{
    // Human skin's normalized-diffusion distance in metres (see the declaration for its derivation).
    constexpr foundation::StaticArray<foundation::float64, format::MATERIAL_RGB_CHANNELS> HUMAN_SKIN_DISTANCE{
        {0.0015, 0.00094, 0.00057}};
    const bool isSkin = materialOrNull != nullptr && materialOrNull->Scattering.Model == format::MaterialModel::Skin;
    QJsonArray distance;
    for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        distance.append(isSkin == true ? static_cast<foundation::float64>(materialOrNull->Scattering.K[channel])
                                       : HUMAN_SKIN_DISTANCE[channel]);
    }
    return {{QString::fromLatin1(SKIN_DISTANCE_KEY), distance}};
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
    // A skin slot edits its base colour and roughness beside its scattering distance.
    const bool isSkin = material.Scattering.Model == format::MaterialModel::Skin;
    rows.append(MakeColourRow(QStringLiteral("Reflectance"), reflectance, QStringLiteral("Reflectance, linear RGB")));
    rows.last().IsSkinParameter = isSkin;
    rows.append(
        MakeTextureRow(QStringLiteral("Base colour"), QStringLiteral("Base colour map"), definition, material.Texture));
    if (material.Texture != format::NO_TEXTURE)
    {
        rows.append({QStringLiteral("Addressing"),
                     material.Repeat == true ? QStringLiteral("Repeat") : QStringLiteral("Clamp")});
    }
    const bool isMix = material.Scattering.Model == format::MaterialModel::DiffuseConductorMix;
    const bool isProgram = material.Scattering.Model == format::MaterialModel::Program;
    if (material.Scattering.Model != format::MaterialModel::ProjectBrdf)
    {
        MaterialPropertyRow metallic{isMix == true ? QStringLiteral("Conductor weight") : QStringLiteral("Metallic"),
                                     FormatScalar(material.Metallic)};
        MaterialPropertyRow roughness{QStringLiteral("Roughness"), FormatScalar(material.Roughness)};
        metallic.Detail = isProgram == true ? QStringLiteral("Metallic, an input of the closure program") : QString();
        roughness.Detail = isProgram == true ? QStringLiteral("Roughness, an input of the closure program")
                                             : QStringLiteral("Perceptual roughness, 0.045 to 1");
        rows.append(metallic);
        roughness.IsSkinParameter = isSkin;
        rows.append(roughness);
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
    else if (scattering.Model == format::MaterialModel::HairCard)
    {
        const format::HairCardParameters hair = format::GetHairCardParameters(scattering);
        rows.append({QStringLiteral("Strand direction (°)"), FormatScalar(hair.StrandDirectionDegrees)});
        rows.last().IsHairCardParameter = true;
        MaterialPropertyRow shifts{QStringLiteral("Shifts (°)"),
                                   QStringLiteral("%1 / %2").arg(FormatScalar(hair.PrimaryShiftDegrees),
                                                                 FormatScalar(hair.SecondaryShiftDegrees))};
        shifts.Detail = QStringLiteral("Highlight shifts in degrees, primary / secondary");
        MaterialPropertyRow weights{
            QStringLiteral("Highlights"),
            QStringLiteral("%1 / %2").arg(FormatScalar(hair.PrimaryWeight), FormatScalar(hair.SecondaryWeight))};
        weights.Detail = QStringLiteral("Highlight weights, primary / tinted secondary");
        shifts.IsHairCardParameter = true;
        weights.IsHairCardParameter = true;
        rows.append(shifts);
        rows.append(weights);
    }
    else if (scattering.Model == format::MaterialModel::Skin)
    {
        MaterialPropertyRow distance{QStringLiteral("Scattering (m)"), FormatRgb(scattering.K)};
        distance.Detail = QStringLiteral("Subsurface scattering distance in metres, linear RGB");
        distance.IsSkinParameter = true;
        rows.append(distance);
    }
    const uint32 surfaceTexture = material.SurfaceTexture;
    const QString surfaceMapLabel = QStringLiteral("Surface map");
    MaterialPropertyRow surfaceMap{surfaceMapLabel, DescribeTexture(definition, surfaceTexture)};
    surfaceMap.Detail = DescribeTextureDetail(surfaceMapLabel, definition, surfaceTexture);
    rows.append(surfaceMap);
    if (surfaceTexture != format::NO_TEXTURE && static_cast<usize>(surfaceTexture) < definition.Textures.GetSize())
    {
        rows.last().Detail +=
            QStringLiteral("\nR/G encode tangent-space normal X/Y. B multiplies roughness. A multiplies metallic.");
        const resources::SceneTexture& texture = definition.Textures[surfaceTexture];
        MaterialPropertyRow roughnessMap{QStringLiteral("Roughness map"), QStringLiteral("Blue (B)")};
        roughnessMap.Detail = QStringLiteral("Linear blue channel, multiplied by the material roughness");
        roughnessMap.Thumbnail = MakeSurfaceChannelThumbnail(texture, SURFACE_ROUGHNESS_CHANNEL);
        MaterialPropertyRow metallicMap{QStringLiteral("Metallic map"), QStringLiteral("Alpha (A)")};
        metallicMap.Detail = QStringLiteral("Linear alpha channel, multiplied by the material metallic factor");
        metallicMap.Thumbnail = MakeSurfaceChannelThumbnail(texture, SURFACE_METALLIC_CHANNEL);
        rows.append(roughnessMap);
        rows.append(metallicMap);
    }
    if (material.SurfaceTexture != format::NO_TEXTURE)
    {
        rows.append({QStringLiteral("Normal scale"), FormatScalar(material.NormalScale)});
    }
    rows.append({QStringLiteral("Alpha"),
                 material.IsAlphaMasked == true
                     ? QStringLiteral("Masked below %1").arg(FormatScalar(material.AlphaCutoff))
                     : QStringLiteral("Opaque")});
    const bool isEmissive = material.Emission.X > 0 || material.Emission.Y > 0 || material.Emission.Z > 0;
    MaterialPropertyRow emission{QStringLiteral("Emission"),
                                 FormatRgb(material.Emission),
                                 false,
                                 {},
                                 {},
                                 QStringLiteral("Emission, linear RGB in nits")};
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
        rows.append(MakeTextureRow(QStringLiteral("Emission map"),
                                   QStringLiteral("Emission map"),
                                   definition,
                                   material.EmissionTexture));
    }
    outRows = rows;
    return true;
}
} // namespace ludus::editor
