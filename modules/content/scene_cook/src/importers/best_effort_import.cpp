// Best-effort scene import: a front-end (pbrt-v4, glTF 2.0/GLB or Wavefront
// OBJ/MTL) builds SceneIR -> budget plan -> LUSCN v6 bundle and a deterministic
// import report. See docs/development/best-effort-scene-import.md.

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_format/luscn.h>

#include "../native_files.h"
#include "../python_compat.h"
#include "authored_lighting.h"
#include "budget_planner.h"
#include "front_end_support.h"
#include "gltf/gltf_reader.h"
#include "image/image_reader.h"
#include "obj/obj_reader.h"
#include "pbrt/pbrt_parser.h"
#include "source_resolver.h"
#include "tangent_generation.h"
#include "transform_utilities.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::content::scene_cook
{
namespace
{
using foundation::Array;
using foundation::String;
using foundation::StringStatus;
using namespace importers;
namespace format = content::scene_format;
namespace math = foundation::math;

/// Largest image file read for a texture.
constexpr usize MAX_IMAGE_FILE_BYTES = MAX_SOURCE_IMAGE_BYTES;
/// Side of the solid-colour swatch cooked for untextured slots.
constexpr uint32 SWATCH_SIZE = 4;
/// Texel channel holding coverage (straight alpha) in decoded RGBA8 images.
constexpr usize ALPHA_CHANNEL = 3;
/// Index of the bitangent sign in an IR tangent (xyz, w).
constexpr usize TANGENT_HANDEDNESS = IR_TANGENT_COMPONENT_COUNT - 1;
/// float32 components of one cooked vertex: a LUSCN v6 VertexRecordV6.
constexpr usize VERTEX_FLOATS = sizeof(format::VertexRecordV6) / sizeof(float32);
static_assert(VERTEX_FLOATS == format::VERTEX_COMPONENTS + format::TANGENT_COMPONENTS);

bool FailWith(CookError& outError, Status status, std::string_view message) noexcept
{
    outError.Result = status;
    (void)outError.Message.Set(message.substr(0, MAX_MESSAGE_BYTES));
    return false;
}

[[nodiscard]] uint8 EncodeChannel(float64 linear) noexcept
{
    const float32 clamped = static_cast<float32>(math::Clamp(linear, 0.0, 1.0));
    return static_cast<uint8>(math::Floor(static_cast<float64>(math::EncodeSrgb(clamped)) * 255.0 + 0.5));
}

[[nodiscard]] float64 DecodeChannel(uint8 value, TextureEncoding encoding, float64 gamma) noexcept
{
    const float64 unit = static_cast<float64>(value) / 255.0;
    switch (encoding)
    {
        case TextureEncoding::Linear:
            return unit;
        case TextureEncoding::Gamma:
            return math::Pow(unit, gamma);
        case TextureEncoding::Srgb:
            break;
    }
    return static_cast<float64>(math::DecodeSrgb(static_cast<float32>(unit)));
}

/// Halve an RGBA8 image with a 2x2 box filter (odd edges clamp).
[[nodiscard]] bool HalveImage(DecodedImage& inoutImage) noexcept
{
    const uint32 width = inoutImage.Width > 1 ? inoutImage.Width / 2 : 1;
    const uint32 height = inoutImage.Height > 1 ? inoutImage.Height / 2 : 1;
    Array<uint8> texels;
    if (texels.TryResize(usize{width} * height * 4) == false)
    {
        return false;
    }
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            for (usize k = 0; k < 4; ++k)
            {
                uint32 sum = 0;
                for (uint32 dy = 0; dy < 2; ++dy)
                {
                    for (uint32 dx = 0; dx < 2; ++dx)
                    {
                        const uint32 sx = math::Min(x * 2 + dx, inoutImage.Width - 1);
                        const uint32 sy = math::Min(y * 2 + dy, inoutImage.Height - 1);
                        sum += inoutImage.Texels[(usize{sy} * inoutImage.Width + sx) * 4 + k];
                    }
                }
                texels[(usize{y} * width + x) * 4 + k] = static_cast<uint8>((sum + 2) / 4);
            }
        }
    }
    inoutImage.Texels = static_cast<Array<uint8>&&>(texels);
    inoutImage.Width = width;
    inoutImage.Height = height;
    return true;
}

[[nodiscard]] bool MakeSwatch(const float64 (&linear)[3], DecodedImage& outImage) noexcept
{
    outImage.Width = SWATCH_SIZE;
    outImage.Height = SWATCH_SIZE;
    if (outImage.Texels.TryResize(usize{SWATCH_SIZE} * SWATCH_SIZE * 4) == false)
    {
        return false;
    }
    for (usize i = 0; i < usize{SWATCH_SIZE} * SWATCH_SIZE; ++i)
    {
        for (usize k = 0; k < 3; ++k)
        {
            outImage.Texels[i * 4 + k] = EncodeChannel(linear[k]);
        }
        outImage.Texels[i * 4 + 3] = 255;
    }
    return true;
}

/// Apply encoding, inversion and the colour multiplier; output is sRGB-encoded.
void ConvertTexels(const IrTexture& texture, DecodedImage& inoutImage) noexcept
{
    const bool isIdentity = texture.Encoding == TextureEncoding::Srgb && texture.IsInverted == false &&
                            texture.Scale[0] == 1.0 && texture.Scale[1] == 1.0 && texture.Scale[2] == 1.0;
    if (isIdentity == true || inoutImage.WasHighDynamicRange == true)
    {
        return;
    }
    uint8 lookUp[3][256];
    for (usize k = 0; k < 3; ++k)
    {
        for (usize value = 0; value < 256; ++value)
        {
            float64 linear = DecodeChannel(static_cast<uint8>(value), texture.Encoding, texture.Gamma);
            if (texture.IsInverted == true)
            {
                linear = 1.0 - linear;
            }
            lookUp[k][value] = EncodeChannel(linear * texture.Scale[k]);
        }
    }
    for (usize i = 0; i < inoutImage.Texels.GetSize(); i += 4)
    {
        for (usize k = 0; k < 3; ++k)
        {
            inoutImage.Texels[i + k] = lookUp[k][inoutImage.Texels[i + k]];
        }
    }
}

/// One cooked texture slot.
struct CookedTexture final
{
    DecodedImage Image;
    /// IR texture or IR_NONE for a swatch.
    uint32 Source = IR_NONE;
    uint32 SourceWidth = 0;
    uint32 SourceHeight = 0;
    bool IsFallback = false;
    /// Linear data (a LUSCN v6 surface map) rather than sRGB colour.
    bool IsLinear = false;
    /// IR texture whose image file was read (even if decoding failed), or IR_NONE.
    uint32 ReadSource = IR_NONE;
    /// SHA-256 of the image bytes read for ReadSource.
    Digest ReadDigest;
};

/// One cooked world-space mesh.
struct CookedMesh final
{
    /// VERTEX_FLOATS per vertex: world position, normal, uv and tangent (xyz, handedness).
    Array<float32> Vertices;
    Array<uint32> Indices;
};

/// True when the texture names image bytes (a file or an embedded image).
[[nodiscard]] bool HasImage(const IrTexture& texture) noexcept
{
    return texture.File.IsEmpty() == false || texture.EmbeddedImage != IR_NONE;
}

/// Report name of an image texture: its root-relative file or embedded label.
[[nodiscard]] std::string_view GetImageName(const SceneIr& scene, const IrTexture& texture) noexcept
{
    return texture.EmbeddedImage != IR_NONE ? scene.EmbeddedImages[texture.EmbeddedImage].Label.GetView()
                                            : texture.File.GetView();
}

/// Encoded bytes of an image texture: borrowed from the IR for embedded images,
/// otherwise read from root into inoutStorage. outIsFileRead reports a file read.
[[nodiscard]] Status LoadImageBytes(const SceneIr& scene,
                                    std::string_view root,
                                    const IrTexture& texture,
                                    Array<uint8>& inoutStorage,
                                    std::span<const uint8>& outBytes,
                                    std::string_view& outDecoderName,
                                    bool& outIsFileRead) noexcept
{
    outIsFileRead = false;
    if (texture.EmbeddedImage != IR_NONE)
    {
        const IrEmbeddedImage& image = scene.EmbeddedImages[texture.EmbeddedImage];
        outBytes = image.Bytes.AsSpan();
        outDecoderName = image.DecoderName.GetView();
        return Status::Ok;
    }
    String path;
    const std::string_view parts[] = {root, std::string_view("/"), texture.File.GetView()};
    if (path.TryAppendMany(parts) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    const Status read = detail::ReadFileBounded(path.GetView(), MAX_IMAGE_FILE_BYTES, inoutStorage);
    if (read == Status::Ok)
    {
        outIsFileRead = true;
        outBytes = inoutStorage.AsSpan();
        outDecoderName = texture.File.GetView();
    }
    return read;
}

/// Read and decode the image of a data texture (no colour conversion).
/// @return Ok, Invalid with outMessage set, or OutOfMemory.
[[nodiscard]] Status DecodeDataTexture(const SceneIr& scene,
                                       std::string_view root,
                                       const IrTexture& texture,
                                       DecodedImage& outImage,
                                       std::string_view& outMessage) noexcept
{
    Array<uint8> storage;
    std::span<const uint8> bytes;
    std::string_view decoderName;
    bool isFileRead = false;
    const Status loaded = LoadImageBytes(scene, root, texture, storage, bytes, decoderName, isFileRead);
    if (loaded != Status::Ok)
    {
        outMessage = "cannot read the image file";
        return loaded == Status::OutOfMemory ? loaded : Status::Invalid;
    }
    const ImageResult decoded = DecodeImage(decoderName, bytes, outImage, outMessage);
    if (decoded != ImageResult::Ok)
    {
        return decoded == ImageResult::OutOfMemory ? Status::OutOfMemory : Status::Invalid;
    }
    return Status::Ok;
}

/// Mean of one channel of a data texture in [0, 1] (alpha for channel 3).
[[nodiscard]] Status AverageTextureChannel(const SceneIr& scene,
                                           std::string_view root,
                                           const IrTexture& texture,
                                           uint8 channel,
                                           float64& outMean,
                                           std::string_view& outMessage) noexcept
{
    DecodedImage image;
    const Status decoded = DecodeDataTexture(scene, root, texture, image, outMessage);
    if (decoded != Status::Ok)
    {
        return decoded;
    }
    const usize component = channel < DECODED_IMAGE_CHANNEL_COUNT ? channel : 0;
    uint64 sum = 0;
    for (usize i = component; i < image.Texels.GetSize(); i += DECODED_IMAGE_CHANNEL_COUNT)
    {
        sum += image.Texels[i];
    }
    const usize count = image.Texels.GetSize() / DECODED_IMAGE_CHANNEL_COUNT;
    outMean =
        count != 0 ? static_cast<float64>(sum) / (DECODED_IMAGE_CHANNEL_MAXIMUM * static_cast<float64>(count)) : 1.0;
    return Status::Ok;
}

/// Fold the metallic/roughness data textures of a slot's representative material into
/// its scalar terms, for a slot whose surface map exceeds the texture count.
[[nodiscard]] Status AverageScalarTextures(const SceneIr& scene,
                                           std::string_view root,
                                           PlannedMaterial& inoutPlanned,
                                           DiagnosticLog& inoutLog) noexcept
{
    constexpr usize SCALAR_TERMS = 2;
    PlannedMaterial& planned = inoutPlanned;
    {
        const IrMaterial& material = scene.Materials[planned.Representative];
        const foundation::StaticArray<uint32, SCALAR_TERMS> textures{material.MetallicTexture,
                                                                     material.RoughnessTexture};
        const foundation::StaticArray<uint8, SCALAR_TERMS> channels{material.MetallicChannel,
                                                                    material.RoughnessChannel};
        for (usize term = 0; term < SCALAR_TERMS; ++term)
        {
            if (textures[term] == IR_NONE || HasImage(scene.Textures[textures[term]]) == false)
            {
                continue;
            }
            float64 mean = 1.0;
            std::string_view message;
            const Status averaged =
                AverageTextureChannel(scene, root, scene.Textures[textures[term]], channels[term], mean, message);
            if (averaged == Status::OutOfMemory)
            {
                return averaged;
            }
            const std::string_view name = GetImageName(scene, scene.Textures[textures[term]]);
            if (averaged != Status::Ok)
            {
                (void)inoutLog.Add(DiagnosticSeverity::Warning, codes::TEXTURE_UNREADABLE, name, 0, message);
                continue;
            }
            if (term == 0)
            {
                planned.Metallic = math::Clamp(planned.Metallic * mean, 0.0, 1.0);
            }
            else
            {
                planned.Roughness =
                    math::Clamp(planned.Roughness * mean, static_cast<float64>(format::MIN_ROUGHNESS), 1.0);
            }
            (void)inoutLog.Add(DiagnosticSeverity::Info,
                               codes::TEXTURE_APPROXIMATED,
                               name,
                               0,
                               "metallic/roughness texture averaged into the scalar term (texture count exhausted)");
        }
    }
    return Status::Ok;
}

/// A decoded data texture sampled nearest at another image's resolution.
struct SampledImage final
{
    DecodedImage Image;
    usize Channel = 0;
    bool IsPresent = false;
};

/// Decode a slot material's data texture for repacking; an unreadable image is reported
/// and left absent, so its multiplier stays one and the scalar term applies alone.
[[nodiscard]] Status DecodeSurfaceSource(const SceneIr& scene,
                                         std::string_view root,
                                         uint32 texture,
                                         usize channel,
                                         DiagnosticLog& inoutLog,
                                         SampledImage& outSource) noexcept
{
    if (texture == IR_NONE || HasImage(scene.Textures[texture]) == false)
    {
        return Status::Ok;
    }
    std::string_view message;
    const Status decoded = DecodeDataTexture(scene, root, scene.Textures[texture], outSource.Image, message);
    if (decoded == Status::OutOfMemory)
    {
        return decoded;
    }
    if (decoded != Status::Ok)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Warning,
                           codes::TEXTURE_UNREADABLE,
                           GetImageName(scene, scene.Textures[texture]),
                           0,
                           message);
        return Status::Ok;
    }
    outSource.Channel = math::Min(channel, ALPHA_CHANNEL);
    outSource.IsPresent = true;
    return Status::Ok;
}

/// Channel of a source texel at an output texel, sampled nearest; absent sources read fallback.
[[nodiscard]] uint8 SampleChannel(const SampledImage& source,
                                  usize channel,
                                  uint32 x,
                                  uint32 y,
                                  uint32 width,
                                  uint32 height,
                                  uint8 fallback) noexcept
{
    if (source.IsPresent == false)
    {
        return fallback;
    }
    const usize sourceX = usize{x} * source.Image.Width / width;
    const usize sourceY = usize{y} * source.Image.Height / height;
    return source.Image.Texels[(sourceY * source.Image.Width + sourceX) * DECODED_IMAGE_CHANNEL_COUNT + channel];
}

/// Repack a slot's tangent-space normal map (RG) and roughness (B) and metallic (A)
/// texture channels into one linear LUSCN v6 surface map at the normal map's resolution
/// (otherwise the larger data texture's), sampling the others nearest. The scalar
/// metallic/roughness terms stay as the multipliers they are in glTF and MTL. Without a
/// free texture slot the roughness/metallic textures are averaged into the scalars.
[[nodiscard]] Status CookSurfaceMap(const SceneIr& scene,
                                    std::string_view root,
                                    PlannedMaterial& inoutPlanned,
                                    DiagnosticLog& inoutLog,
                                    Array<CookedTexture>& inoutTextures) noexcept
{
    if (inoutPlanned.Representative == IR_NONE)
    {
        return Status::Ok;
    }
    const IrMaterial& material = scene.Materials[inoutPlanned.Representative];
    const uint32 normalTexture =
        material.NormalTextureKind == IrNormalTextureKind::TangentSpace ? material.NormalTexture : IR_NONE;
    const bool hasSource =
        (normalTexture != IR_NONE && HasImage(scene.Textures[normalTexture]) == true) ||
        (material.RoughnessTexture != IR_NONE && HasImage(scene.Textures[material.RoughnessTexture]) == true) ||
        (material.MetallicTexture != IR_NONE && HasImage(scene.Textures[material.MetallicTexture]) == true);
    if (hasSource == false)
    {
        return Status::Ok;
    }
    if (inoutTextures.GetSize() >= format::IMPORT_LIMITS.Textures)
    {
        if (normalTexture != IR_NONE)
        {
            (void)inoutLog.Add(DiagnosticSeverity::Warning,
                               codes::NORMALMAP_DROPPED,
                               GetImageName(scene, scene.Textures[normalTexture]),
                               0,
                               "normal map dropped: the bundle's texture count is exhausted");
        }
        return AverageScalarTextures(scene, root, inoutPlanned, inoutLog);
    }
    SampledImage normal;
    SampledImage roughness;
    SampledImage metallic;
    Status status = DecodeSurfaceSource(scene, root, normalTexture, 0, inoutLog, normal);
    if (status == Status::Ok)
    {
        status =
            DecodeSurfaceSource(scene, root, material.RoughnessTexture, material.RoughnessChannel, inoutLog, roughness);
    }
    if (status == Status::Ok)
    {
        status =
            DecodeSurfaceSource(scene, root, material.MetallicTexture, material.MetallicChannel, inoutLog, metallic);
    }
    if (status != Status::Ok ||
        (normal.IsPresent == false && roughness.IsPresent == false && metallic.IsPresent == false))
    {
        return status;
    }
    // The normal map sets the resolution; otherwise the larger roughness/metallic image.
    const SampledImage& reference =
        normal.IsPresent == true ? normal
        : roughness.IsPresent == true &&
                (metallic.IsPresent == false || usize{roughness.Image.Width} * roughness.Image.Height >=
                                                    usize{metallic.Image.Width} * metallic.Image.Height)
            ? roughness
            : metallic;
    CookedTexture* cookedOrNull = inoutTextures.TryAddInPlace();
    if (cookedOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    DecodedImage& image = cookedOrNull->Image;
    image.Width = reference.Image.Width;
    image.Height = reference.Image.Height;
    if (image.Texels.TryResize(usize{image.Width} * image.Height * DECODED_IMAGE_CHANNEL_COUNT) == false)
    {
        return Status::OutOfMemory;
    }
    // A flat tangent normal encodes as 0.5 (128); absent multipliers are one (255).
    constexpr uint8 FLAT_NORMAL_CODE = 128;
    constexpr uint8 UNIT_MULTIPLIER_CODE = 255;
    constexpr usize ROUGHNESS_CHANNEL = 2;
    for (uint32 y = 0; y < image.Height; ++y)
    {
        for (uint32 x = 0; x < image.Width; ++x)
        {
            uint8* texel = image.Texels.GetData() + (usize{y} * image.Width + x) * DECODED_IMAGE_CHANNEL_COUNT;
            texel[0] = SampleChannel(normal, 0, x, y, image.Width, image.Height, FLAT_NORMAL_CODE);
            texel[1] = SampleChannel(normal, 1, x, y, image.Width, image.Height, FLAT_NORMAL_CODE);
            texel[ROUGHNESS_CHANNEL] =
                SampleChannel(roughness, roughness.Channel, x, y, image.Width, image.Height, UNIT_MULTIPLIER_CODE);
            texel[ALPHA_CHANNEL] =
                SampleChannel(metallic, metallic.Channel, x, y, image.Width, image.Height, UNIT_MULTIPLIER_CODE);
        }
    }
    cookedOrNull->Source = normal.IsPresent == true      ? normalTexture
                           : roughness.IsPresent == true ? material.RoughnessTexture
                                                         : material.MetallicTexture;
    cookedOrNull->SourceWidth = image.Width;
    cookedOrNull->SourceHeight = image.Height;
    cookedOrNull->IsLinear = true;
    inoutPlanned.CookedSurfaceTexture = static_cast<uint32>(inoutTextures.GetSize() - 1);
    inoutPlanned.NormalScale =
        normal.IsPresent == true
            ? math::Clamp(material.NormalScale, 0.0, static_cast<float64>(format::MAX_NORMAL_SCALE))
            : 0.0;
    return Status::Ok;
}

/// Cook an emissive slot's emission colour texture (glTF emissiveTexture, MTL map_Ke)
/// as an sRGB LUSCN v6 emission map multiplying its emission.
[[nodiscard]] Status CookEmissionMap(const SceneIr& scene,
                                     std::string_view root,
                                     PlannedMaterial& inoutPlanned,
                                     DiagnosticLog& inoutLog,
                                     Array<CookedTexture>& inoutTextures) noexcept
{
    if (inoutPlanned.Representative == IR_NONE || inoutPlanned.IsEmissive == false)
    {
        return Status::Ok;
    }
    const uint32 source = scene.Materials[inoutPlanned.Representative].EmissionTexture;
    if (source == IR_NONE || HasImage(scene.Textures[source]) == false)
    {
        return Status::Ok;
    }
    const IrTexture& texture = scene.Textures[source];
    if (inoutTextures.GetSize() >= format::IMPORT_LIMITS.Textures)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Warning,
                           codes::EMISSION_APPROXIMATED,
                           GetImageName(scene, texture),
                           0,
                           "emission map dropped: the bundle's texture count is exhausted");
        return Status::Ok;
    }
    DecodedImage image;
    std::string_view message;
    const Status decoded = DecodeDataTexture(scene, root, texture, image, message);
    if (decoded == Status::OutOfMemory)
    {
        return decoded;
    }
    if (decoded != Status::Ok)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Warning,
                           codes::TEXTURE_UNREADABLE,
                           GetImageName(scene, texture),
                           0,
                           message);
        return Status::Ok;
    }
    ConvertTexels(texture, image);
    CookedTexture* cookedOrNull = inoutTextures.TryAddInPlace();
    if (cookedOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    cookedOrNull->Image = static_cast<DecodedImage&&>(image);
    cookedOrNull->Source = source;
    cookedOrNull->SourceWidth = cookedOrNull->Image.Width;
    cookedOrNull->SourceHeight = cookedOrNull->Image.Height;
    inoutPlanned.CookedEmissionTexture = static_cast<uint32>(inoutTextures.GetSize() - 1);
    return Status::Ok;
}

/// True when the image has a texel whose channel is below its maximum.
[[nodiscard]] bool HasPartialChannel(const DecodedImage& image, usize channel) noexcept
{
    for (usize i = channel; i < image.Texels.GetSize(); i += DECODED_IMAGE_CHANNEL_COUNT)
    {
        if (static_cast<float64>(image.Texels[i]) < DECODED_IMAGE_CHANNEL_MAXIMUM)
        {
            return true;
        }
    }
    return false;
}

/// Replace a swatch by a solid image of the given size and the swatch's first texel.
[[nodiscard]] bool ResizeSwatch(uint32 width, uint32 height, DecodedImage& inoutImage) noexcept
{
    foundation::StaticArray<uint8, DECODED_IMAGE_CHANNEL_COUNT> texel{};
    for (usize k = 0; k < DECODED_IMAGE_CHANNEL_COUNT; ++k)
    {
        texel[k] = inoutImage.Texels[k];
    }
    Array<uint8> texels;
    if (texels.TryResize(usize{width} * height * DECODED_IMAGE_CHANNEL_COUNT) == false)
    {
        return false;
    }
    for (usize i = 0; i < texels.GetSize(); ++i)
    {
        texels[i] = texel[i % DECODED_IMAGE_CHANNEL_COUNT];
    }
    inoutImage.Texels = static_cast<Array<uint8>&&>(texels);
    inoutImage.Width = width;
    inoutImage.Height = height;
    return true;
}

/// Write a masked slot's coverage into its cooked texture's alpha channel (LUSCN v6
/// ALPHA_MASKED): the base texture's own alpha (glTF) or the opacity map's channel
/// (MTL map_d, sampled nearest at the base resolution), times the constant opacity.
/// A mask that keeps every texel at its cutoff is cooked opaque; an unreadable
/// opacity map leaves the slot opaque with a warning.
[[nodiscard]] Status ApplyAlphaMask(const SceneIr& scene,
                                    std::string_view root,
                                    PlannedMaterial& inoutPlanned,
                                    CookedTexture& inoutTexture,
                                    DiagnosticLog& inoutLog) noexcept
{
    if (inoutPlanned.IsAlphaMasked == false || inoutPlanned.Representative == IR_NONE)
    {
        return Status::Ok;
    }
    const IrMaterial& material = scene.Materials[inoutPlanned.Representative];
    DecodedImage& image = inoutTexture.Image;
    if (material.OpacityTexture != IR_NONE && HasImage(scene.Textures[material.OpacityTexture]) == true)
    {
        const IrTexture& texture = scene.Textures[material.OpacityTexture];
        DecodedImage opacity;
        std::string_view message;
        const Status decoded = DecodeDataTexture(scene, root, texture, opacity, message);
        if (decoded == Status::OutOfMemory)
        {
            return decoded;
        }
        if (decoded != Status::Ok)
        {
            (void)inoutLog.Add(DiagnosticSeverity::Warning,
                               codes::TEXTURE_UNREADABLE,
                               GetImageName(scene, texture),
                               0,
                               message);
            inoutPlanned.IsAlphaMasked = false;
            inoutPlanned.AlphaCutoff = 0.0;
            return Status::Ok;
        }
        usize channel = material.OpacityChannel;
        if (channel == IR_DETECT_OPACITY_CHANNEL)
        {
            channel = HasPartialChannel(opacity, ALPHA_CHANNEL) == true ? ALPHA_CHANNEL : 0;
        }
        channel = math::Min(channel, ALPHA_CHANNEL);
        // An untextured slot takes the opacity map's resolution so the mask survives.
        if (inoutTexture.Source == IR_NONE && ResizeSwatch(opacity.Width, opacity.Height, image) == false)
        {
            return Status::OutOfMemory;
        }
        for (uint32 y = 0; y < image.Height; ++y)
        {
            const usize sourceY = usize{y} * opacity.Height / image.Height;
            for (uint32 x = 0; x < image.Width; ++x)
            {
                const usize sourceX = usize{x} * opacity.Width / image.Width;
                image.Texels[(usize{y} * image.Width + x) * DECODED_IMAGE_CHANNEL_COUNT + ALPHA_CHANNEL] =
                    opacity.Texels[(sourceY * opacity.Width + sourceX) * DECODED_IMAGE_CHANNEL_COUNT + channel];
            }
        }
    }
    bool isClipped = false;
    for (usize i = ALPHA_CHANNEL; i < image.Texels.GetSize(); i += DECODED_IMAGE_CHANNEL_COUNT)
    {
        const float64 alpha = static_cast<float64>(image.Texels[i]) * math::Clamp(material.Opacity, 0.0, 1.0);
        image.Texels[i] = static_cast<uint8>(math::Floor(alpha + .5));
        isClipped = isClipped == true ||
                    static_cast<float64>(image.Texels[i]) < inoutPlanned.AlphaCutoff * DECODED_IMAGE_CHANNEL_MAXIMUM;
    }
    if (isClipped == false)
    {
        inoutPlanned.IsAlphaMasked = false;
        inoutPlanned.AlphaCutoff = 0.0;
    }
    return Status::Ok;
}

/// Decode or synthesise the texture of every slot, then downscale to the byte budget.
[[nodiscard]] Status CookTextures(const SceneIr& scene,
                                  std::string_view root,
                                  CookPlan& inoutPlan,
                                  usize textureBudget,
                                  DiagnosticLog& inoutLog,
                                  Array<CookedTexture>& outTextures) noexcept
{
    outTextures.Clear();
    for (PlannedMaterial& planned : inoutPlan.Materials)
    {
        CookedTexture* cooked = outTextures.TryAddInPlace();
        if (cooked == nullptr)
        {
            return Status::OutOfMemory;
        }
        bool isDecoded = false;
        if (planned.Texture != IR_NONE && HasImage(scene.Textures[planned.Texture]) == true)
        {
            const IrTexture& texture = scene.Textures[planned.Texture];
            Array<uint8> storage;
            std::span<const uint8> bytes;
            std::string_view decoderName;
            bool isFileRead = false;
            std::string_view message = "cannot read the image file";
            const Status read = LoadImageBytes(scene, root, texture, storage, bytes, decoderName, isFileRead);
            if (read == Status::OutOfMemory)
            {
                return read;
            }
            ImageResult decoded = ImageResult::Invalid;
            if (read == Status::Ok)
            {
                if (isFileRead == true)
                {
                    cooked->ReadSource = planned.Texture;
                    cooked->ReadDigest = Hash(bytes);
                }
                decoded = DecodeImage(decoderName, bytes, cooked->Image, message);
            }
            if (decoded == ImageResult::OutOfMemory)
            {
                return Status::OutOfMemory;
            }
            if (decoded == ImageResult::Ok)
            {
                ConvertTexels(texture, cooked->Image);
                cooked->Source = planned.Texture;
                isDecoded = true;
            }
            else
            {
                const String* file =
                    texture.SourceFile < scene.SourceFiles.GetSize() ? &scene.SourceFiles[texture.SourceFile] : nullptr;
                (void)inoutLog.Add(DiagnosticSeverity::Warning,
                                   codes::TEXTURE_UNREADABLE,
                                   file != nullptr ? file->GetView() : std::string_view(),
                                   texture.Line,
                                   message);
                planned.BaseColour[0] = planned.BaseColour[1] = planned.BaseColour[2] = .5;
                for (usize k = 0; k < 3; ++k)
                {
                    planned.BaseColour[k] *= texture.Scale[k];
                }
                cooked->IsFallback = true;
            }
        }
        else if (planned.Texture != IR_NONE)
        {
            // Constant-valued texture: its value is the colour.
            for (usize k = 0; k < 3; ++k)
            {
                planned.BaseColour[k] = scene.Textures[planned.Texture].Scale[k];
            }
        }
        if (isDecoded == false)
        {
            if (MakeSwatch(planned.BaseColour, cooked->Image) == false)
            {
                return Status::OutOfMemory;
            }
            planned.IsTextureRepeated = false;
        }
        const Status masked = ApplyAlphaMask(scene, root, planned, *cooked, inoutLog);
        if (masked != Status::Ok)
        {
            return masked;
        }
        cooked->SourceWidth = cooked->Image.Width;
        cooked->SourceHeight = cooked->Image.Height;
    }
    // Surface and emission maps follow the base textures, slot by slot, while the
    // bundle's texture count allows.
    for (PlannedMaterial& planned : inoutPlan.Materials)
    {
        Status status = CookSurfaceMap(scene, root, planned, inoutLog, outTextures);
        if (status == Status::Ok)
        {
            status = CookEmissionMap(scene, root, planned, inoutLog, outTextures);
        }
        if (status != Status::Ok)
        {
            return status;
        }
    }
    // Halve the largest texture until each fits the per-texture limits and all fit the budget.
    while (true)
    {
        usize total = 0;
        usize largest = 0;
        usize largestBytes = 0;
        bool hasOversize = false;
        for (usize i = 0; i < outTextures.GetSize(); ++i)
        {
            const DecodedImage& image = outTextures[i].Image;
            const usize bytes = image.Texels.GetSize();
            total += bytes;
            const bool isOversize = bytes > format::MAX_TEXTURE_BYTES || image.Width > format::MAX_TEXTURE_DIMENSION ||
                                    image.Height > format::MAX_TEXTURE_DIMENSION;
            if ((isOversize == true && hasOversize == false) || (isOversize == hasOversize && bytes > largestBytes))
            {
                largest = i;
                largestBytes = bytes;
            }
            hasOversize = hasOversize == true || isOversize == true;
        }
        if (hasOversize == false && total <= textureBudget)
        {
            break;
        }
        DecodedImage& image = outTextures[largest].Image;
        if (image.Width == 1 && image.Height == 1)
        {
            break;
        }
        if (HalveImage(image) == false)
        {
            return Status::OutOfMemory;
        }
    }
    for (const CookedTexture& cooked : outTextures)
    {
        if (cooked.Image.Width != cooked.SourceWidth && cooked.Source != IR_NONE)
        {
            (void)inoutLog.Add(DiagnosticSeverity::Info,
                               codes::TEXTURE_DOWNSCALED,
                               {},
                               0,
                               GetImageName(scene, scene.Textures[cooked.Source]));
        }
    }
    return Status::Ok;
}

/// World-space LUSCN v6 tangent of an IR tangent (xyz, bitangent sign w): the direction
/// transforms with the linear part, is orthogonalized against the unit world normal and
/// normalized; the sign follows the transform's handedness. A tangent without a
/// perpendicular part (degenerate, non-finite or parallel to the normal) becomes all
/// zero, which consumers read as absent.
[[nodiscard]] math::Vector4 TransformTangent(const math::Matrix4d& transform,
                                             std::span<const float32> tangent,
                                             const math::Vector3d& normalWorld,
                                             float32 handedness) noexcept
{
    const math::Vector3d direction{static_cast<float64>(tangent[0]),
                                   static_cast<float64>(tangent[1]),
                                   static_cast<float64>(tangent[2])};
    const math::Vector3d world = TransformDirection(transform, direction);
    const math::Vector3d perpendicular = world - normalWorld * math::Dot(normalWorld, world);
    const float64 length = math::Length(perpendicular);
    if (math::IsFinite(length) == false || length <= static_cast<float64>(format::MIN_TANGENT_PERPENDICULAR) ||
        tangent[TANGENT_HANDEDNESS] == 0.0F)
    {
        return {};
    }
    const math::Vector3d unit = perpendicular * (1.0 / length);
    const math::Vector4 result{static_cast<float32>(unit.X),
                               static_cast<float32>(unit.Y),
                               static_cast<float32>(unit.Z),
                               (tangent[TANGENT_HANDEDNESS] < 0.0F ? -1.0F : 1.0F) * handedness};
    // Recheck after float32 rounding against the rounded normal, as the decoder does.
    const math::Vector3 normal{static_cast<float32>(normalWorld.X),
                               static_cast<float32>(normalWorld.Y),
                               static_cast<float32>(normalWorld.Z)};
    const math::Vector3 axis{result.X, result.Y, result.Z};
    math::Vector3 unitNormal;
    return math::TryNormalize(normal, unitNormal) == math::MathStatus::Success &&
                   math::Length(axis - unitNormal * math::Dot(unitNormal, axis)) > format::MIN_TANGENT_PERPENDICULAR
               ? result
               : math::Vector4{};
}

/// Bake every kept placement of each slot into a world-space mesh.
[[nodiscard]] Status BakeMeshes(const SceneIr& scene,
                                const CookPlan& plan,
                                Array<CookedMesh>& outMeshes,
                                const Cancellation* cancelOrNull) noexcept
{
    outMeshes.Clear();
    Array<float64> computedNormals;
    for (const PlannedMaterial& planned : plan.Materials)
    {
        CookedMesh* cooked = outMeshes.TryAddInPlace();
        if (cooked == nullptr ||
            cooked->Vertices.TryEnsureCapacity(usize{planned.VertexCount} * VERTEX_FLOATS) == false ||
            cooked->Indices.TryEnsureCapacity(planned.IndexCount) == false)
        {
            return Status::OutOfMemory;
        }
        for (const uint32 placementIndex : planned.Placements)
        {
            if (cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
                cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true)
            {
                return Status::Cancelled;
            }
            const Placement& placement = plan.Placements[placementIndex];
            const IrShape& shape = scene.Shapes[placement.Shape];
            const IrMesh& mesh = scene.Meshes[shape.Mesh];
            const usize vertexCount = mesh.GetVertexCount();
            const bool hasNormals = mesh.Normals.GetSize() == vertexCount * 3;
            const bool hasUvs = mesh.Uvs.GetSize() == vertexCount * 2;
            if (hasNormals == false)
            {
                // Area-weighted vertex normals in object space.
                if (computedNormals.TryResize(vertexCount * 3) == false)
                {
                    return Status::OutOfMemory;
                }
                for (float64& value : computedNormals)
                {
                    value = 0.0;
                }
                for (usize i = 0; i + 2 < mesh.Indices.GetSize(); i += 3)
                {
                    const uint32 corners[3] = {mesh.Indices[i], mesh.Indices[i + 1], mesh.Indices[i + 2]};
                    math::Vector3d points[3];
                    for (usize c = 0; c < 3; ++c)
                    {
                        const float32* p = mesh.Positions.GetData() + usize{corners[c]} * 3;
                        points[c] = math::Vector3d{static_cast<float64>(p[0]),
                                                   static_cast<float64>(p[1]),
                                                   static_cast<float64>(p[2])};
                    }
                    const math::Vector3d face = math::Cross(points[1] - points[0], points[2] - points[0]);
                    for (const uint32 corner : corners)
                    {
                        computedNormals[usize{corner} * 3] += face.X;
                        computedNormals[usize{corner} * 3 + 1] += face.Y;
                        computedNormals[usize{corner} * 3 + 2] += face.Z;
                    }
                }
            }
            math::Matrix4d inverse = math::Matrix4d::Identity();
            (void)TryInvertAffine(placement.Transform, inverse);
            const uint32 base = static_cast<uint32>(cooked->Vertices.GetSize() / VERTEX_FLOATS);
            const bool hasTangents = mesh.Tangents.GetSize() == vertexCount * IR_TANGENT_COMPONENT_COUNT;
            // Mirroring transforms reverse the bitangent, as the renderer does for object transforms.
            const float32 handedness = ComputeLinearDeterminant(placement.Transform) < 0.0 ? -1.0F : 1.0F;
            for (usize v = 0; v < vertexCount; ++v)
            {
                const float32* p = mesh.Positions.GetData() + v * 3;
                const math::Vector3d world = TransformPoint(
                    placement.Transform,
                    math::Vector3d{static_cast<float64>(p[0]), static_cast<float64>(p[1]), static_cast<float64>(p[2])});
                math::Vector3d normal = hasNormals == true
                                            ? math::Vector3d{static_cast<float64>(mesh.Normals[v * 3]),
                                                             static_cast<float64>(mesh.Normals[v * 3 + 1]),
                                                             static_cast<float64>(mesh.Normals[v * 3 + 2])}
                                            : math::Vector3d{computedNormals[v * 3],
                                                             computedNormals[v * 3 + 1],
                                                             computedNormals[v * 3 + 2]};
                // Normals transform by the inverse transpose of the linear part.
                const math::Vector3d transformed{
                    inverse.Elements[0] * normal.X + inverse.Elements[1] * normal.Y + inverse.Elements[2] * normal.Z,
                    inverse.Elements[4] * normal.X + inverse.Elements[5] * normal.Y + inverse.Elements[6] * normal.Z,
                    inverse.Elements[8] * normal.X + inverse.Elements[9] * normal.Y + inverse.Elements[10] * normal.Z};
                const float64 length = math::Length(transformed);
                normal = length > 0.0 && math::IsFinite(length) == true ? transformed * (1.0 / length)
                                                                        : math::Vector3d{0.0, 1.0, 0.0};
                const math::Vector4 tangent =
                    hasTangents == true ? TransformTangent(placement.Transform,
                                                           {mesh.Tangents.GetData() + v * IR_TANGENT_COMPONENT_COUNT,
                                                            IR_TANGENT_COMPONENT_COUNT},
                                                           normal,
                                                           handedness)
                                        : math::Vector4{};
                const float32 values[VERTEX_FLOATS] = {
                    static_cast<float32>(world.X),
                    static_cast<float32>(world.Y),
                    static_cast<float32>(world.Z),
                    static_cast<float32>(normal.X),
                    static_cast<float32>(normal.Y),
                    static_cast<float32>(normal.Z),
                    hasUvs == true ? mesh.Uvs[v * 2] : 0.0F,
                    hasUvs == true ? mesh.Uvs[v * 2 + 1] : 0.0F,
                    tangent.X,
                    tangent.Y,
                    tangent.Z,
                    tangent.W,
                };
                if (cooked->Vertices.TryAddRange(std::span<const float32>(values, VERTEX_FLOATS)) == false)
                {
                    return Status::OutOfMemory;
                }
            }
            const bool isWindingFlipped =
                shape.IsOrientationReversed != (ComputeLinearDeterminant(placement.Transform) < 0.0);
            for (usize i = 0; i + 2 < mesh.Indices.GetSize(); i += 3)
            {
                const uint32 a = base + mesh.Indices[i];
                uint32 b = base + mesh.Indices[i + 1];
                uint32 c = base + mesh.Indices[i + 2];
                if (isWindingFlipped == true)
                {
                    const uint32 swapped = b;
                    b = c;
                    c = swapped;
                }
                if (cooked->Indices.TryAdd(a) == false || cooked->Indices.TryAdd(b) == false ||
                    cooked->Indices.TryAdd(c) == false)
                {
                    return Status::OutOfMemory;
                }
            }
        }
    }
    return Status::Ok;
}

/// Camera from the IR, or framed on the cooked bounds when absent or degenerate.
void ResolveCamera(const SceneIr& scene,
                   const Array<CookedMesh>& meshes,
                   IrCamera& outCamera,
                   bool& outIsFramed) noexcept
{
    outCamera = scene.Camera;
    outIsFramed = false;
    const math::Vector3d forward = outCamera.Target - outCamera.Eye;
    const float64 forwardLength = math::Length(forward);
    const float64 upLength = math::Length(outCamera.Up);
    const bool isUsable = outCamera.IsPresent == true && forwardLength > 0.0 && upLength > 0.0 &&
                          math::Length(math::Cross(forward, outCamera.Up)) > 1e-9 * forwardLength * upLength;
    if (isUsable == true)
    {
        return;
    }
    outIsFramed = true;
    math::Vector3d low{1e300, 1e300, 1e300};
    math::Vector3d high{-1e300, -1e300, -1e300};
    for (const CookedMesh& mesh : meshes)
    {
        for (usize i = 0; i < mesh.Vertices.GetSize(); i += VERTEX_FLOATS)
        {
            const float64 p[3] = {static_cast<float64>(mesh.Vertices[i]),
                                  static_cast<float64>(mesh.Vertices[i + 1]),
                                  static_cast<float64>(mesh.Vertices[i + 2])};
            low = math::Vector3d{math::Min(low.X, p[0]), math::Min(low.Y, p[1]), math::Min(low.Z, p[2])};
            high = math::Vector3d{math::Max(high.X, p[0]), math::Max(high.Y, p[1]), math::Max(high.Z, p[2])};
        }
    }
    const math::Vector3d centre = (low + high) * .5;
    const float64 radius = math::Max(math::Length(high - low) * .5, 1e-3);
    outCamera.IsPresent = true;
    outCamera.Target = centre;
    outCamera.Eye = centre - math::Vector3d{0.0, 0.0, radius * 2.5};
    outCamera.Up = math::Vector3d{0.0, 1.0, 0.0};
    outCamera.VerticalFov = math::DegreesToRadians(45.0);
}

void PackBundle(const IrCamera& camera,
                const CookPlan& plan,
                const Array<CookedTexture>& textures,
                const Array<CookedMesh>& meshes,
                const CookedLighting& lighting,
                detail::ByteWriter& inoutBlob) noexcept
{
    inoutBlob.Append(format::MAGIC);
    inoutBlob.AppendUint32(format::VERSION_6);
    const uint32 count = static_cast<uint32>(plan.Materials.GetSize());
    inoutBlob.AppendUint32(count); // Meshes.
    inoutBlob.AppendUint32(count); // Materials.
    inoutBlob.AppendUint32(static_cast<uint32>(textures.GetSize()));
    inoutBlob.AppendUint32(count); // Objects.
    for (const math::Vector3d* vector : {&camera.Eye, &camera.Target, &camera.Up})
    {
        inoutBlob.AppendFloat32(vector->X);
        inoutBlob.AppendFloat32(vector->Y);
        inoutBlob.AppendFloat32(vector->Z);
    }
    inoutBlob.AppendFloat32(camera.VerticalFov);
    for (const CookedTexture& texture : textures)
    {
        inoutBlob.AppendUint32(texture.Image.Width);
        inoutBlob.AppendUint32(texture.Image.Height);
        inoutBlob.AppendUint32(static_cast<uint32>(texture.Image.Texels.GetSize()));
        inoutBlob.AppendBytes(texture.Image.Texels.AsSpan());
    }
    for (usize slot = 0; slot < plan.Materials.GetSize(); ++slot)
    {
        const PlannedMaterial& planned = plan.Materials[slot];
        inoutBlob.AppendUint32(static_cast<uint32>(slot));
        inoutBlob.AppendUint32(planned.IsTextureRepeated == true ? format::ADDRESS_REPEAT : format::ADDRESS_CLAMP);
        inoutBlob.AppendFloat32(planned.Metallic);
        inoutBlob.AppendFloat32(planned.Roughness);
        for (const float64 channel : planned.Emission)
        {
            inoutBlob.AppendFloat32(channel);
        }
        const uint32 flags = (planned.IsEmissionTwoSided == true ? format::EMISSION_TWO_SIDED : 0U) |
                             (planned.HasEmissionPower == true ? format::EMISSION_HAS_POWER : 0U);
        inoutBlob.AppendUint32(flags);
        inoutBlob.AppendUint64(planned.IsEmissive == true ? static_cast<uint64>(slot) + 1 : 0);
        inoutBlob.AppendFloat32(planned.EmissionPowerLumens);
        inoutBlob.AppendUint32(planned.IsAlphaMasked == true ? format::ALPHA_MASKED : format::ALPHA_OPAQUE);
        inoutBlob.AppendFloat32(planned.IsAlphaMasked == true ? planned.AlphaCutoff : 0.0);
        inoutBlob.AppendUint32(planned.CookedSurfaceTexture == IR_NONE ? format::NO_TEXTURE
                                                                       : planned.CookedSurfaceTexture);
        inoutBlob.AppendUint32(planned.CookedEmissionTexture == IR_NONE ? format::NO_TEXTURE
                                                                        : planned.CookedEmissionTexture);
        inoutBlob.AppendFloat32(planned.CookedSurfaceTexture == IR_NONE ? 0.0 : planned.NormalScale);
    }
    for (const CookedMesh& mesh : meshes)
    {
        inoutBlob.AppendUint32(static_cast<uint32>(mesh.Vertices.GetSize() / VERTEX_FLOATS));
        inoutBlob.AppendUint32(static_cast<uint32>(mesh.Indices.GetSize()));
        for (const float32 component : mesh.Vertices)
        {
            inoutBlob.AppendFloat32(static_cast<float64>(component));
        }
        for (const uint32 index : mesh.Indices)
        {
            inoutBlob.AppendUint32(index);
        }
    }
    for (usize slot = 0; slot < plan.Materials.GetSize(); ++slot)
    {
        inoutBlob.AppendUint32(static_cast<uint32>(slot));
        inoutBlob.AppendUint32(static_cast<uint32>(slot));
        static constexpr float64 IDENTITY[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        for (const float64 value : IDENTITY)
        {
            inoutBlob.AppendFloat32(value);
        }
        inoutBlob.AppendUint64(static_cast<uint64>(slot) + 1);
    }
    inoutBlob.AppendUint32(format::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
    inoutBlob.AppendUint32(static_cast<uint32>(lighting.Lights.GetSize()));
    inoutBlob.AppendUint32(lighting.Environment.Width);
    inoutBlob.AppendUint32(lighting.Environment.Height);
    for (const float32 value : lighting.EnvironmentRgb)
    {
        inoutBlob.AppendFloat32(static_cast<float64>(value));
    }
    for (const format::LightRecord& light : lighting.Lights)
    {
        inoutBlob.AppendUint64(light.SourceId);
        inoutBlob.AppendUint32(light.Kind);
        inoutBlob.AppendUint32(light.Flags);
        for (const float32 value : light.IntensityRgb)
        {
            inoutBlob.AppendFloat32(static_cast<float64>(value));
        }
        for (const float32 value : light.PositionWorld)
        {
            inoutBlob.AppendFloat32(static_cast<float64>(value));
        }
        for (const float32 value : light.DirectionWorld)
        {
            inoutBlob.AppendFloat32(static_cast<float64>(value));
        }
        inoutBlob.AppendFloat32(static_cast<float64>(light.Range));
        inoutBlob.AppendFloat32(static_cast<float64>(light.InnerCos));
        inoutBlob.AppendFloat32(static_cast<float64>(light.OuterCos));
    }
    for (const float32 value : lighting.Environment.Rgb)
    {
        inoutBlob.AppendFloat32(static_cast<float64>(value));
    }
}

[[nodiscard]] std::string_view GetSeverityName(DiagnosticSeverity severity) noexcept
{
    switch (severity)
    {
        case DiagnosticSeverity::Info:
            return "info";
        case DiagnosticSeverity::Warning:
            return "warning";
        case DiagnosticSeverity::Error:
            break;
    }
    return "error";
}

struct ReportFacts final
{
    std::string_view Profile;
    std::string_view EntryName;
    bool IsDryRun = false;
    bool IsCameraFramed = false;
    const IrCamera* CameraOrNull = nullptr;
    const Array<CookedTexture>* TexturesOrNull = nullptr;
    const Array<CookedMesh>* MeshesOrNull = nullptr;
    const CookedLighting* LightingOrNull = nullptr;
};

/// Deterministic report: fixed key order, root-relative paths, Python-repr floats.
void WriteReport(const SceneIr& scene,
                 const CookPlan& plan,
                 const DiagnosticLog& log,
                 const ImportSummary& summary,
                 const ReportFacts& facts,
                 Array<uint8>& outReport) noexcept
{
    outReport.Clear();
    detail::ByteWriter writer(outReport);
    detail::PythonJsonWriter json(writer);
    const auto writeCount = [&](std::string_view key, uint64 value) noexcept {
        json.WriteKey(key);
        json.WriteInteger(static_cast<int64>(value));
    };
    json.BeginObject();
    writeCount("schema", IMPORT_REPORT_SCHEMA);
    json.WriteKey("importer");
    json.WriteString(facts.Profile);
    json.WriteKey("entry");
    json.WriteString(facts.EntryName);
    json.WriteKey("dry_run");
    json.WriteBoolean(facts.IsDryRun);
    json.WriteKey("status");
    json.WriteString(summary.Errors == 0 ? "ok" : "failed");

    json.WriteKey("source");
    json.BeginObject();
    writeCount("files", summary.SourceFiles);
    writeCount("shapes", scene.Shapes.GetSize());
    writeCount("meshes", scene.Meshes.GetSize());
    writeCount("objects", scene.Objects.GetSize());
    writeCount("instances", scene.Instances.GetSize());
    writeCount("placements", summary.SourcePlacements);
    writeCount("vertices", summary.SourceVertices);
    writeCount("materials", summary.SourceMaterials);
    writeCount("textures", summary.SourceTextures);
    json.WriteKey("lights");
    json.BeginObject();
    writeCount("area", scene.Lights.Area);
    writeCount("infinite", scene.Lights.Infinite);
    writeCount("point", scene.Lights.Point);
    writeCount("spot", scene.Lights.Spot);
    writeCount("distant", scene.Lights.Distant);
    writeCount("other", scene.Lights.Other);
    json.EndObject();
    json.WriteKey("files_read");
    json.BeginArray();
    for (const String& file : scene.SourceFiles)
    {
        json.WriteString(file.GetView());
    }
    json.EndArray();
    json.EndObject();

    json.WriteKey("cooked");
    json.BeginObject();
    writeCount("meshes", summary.CookedMeshes);
    writeCount("materials", summary.CookedMaterials);
    writeCount("textures", summary.CookedTextures);
    writeCount("objects", summary.CookedObjects);
    writeCount("vertices", summary.CookedVertices);
    writeCount("bundle_bytes", summary.BundleBytes);
    writeCount("scene_format_version", format::VERSION_6);
    writeCount("analytic_lights", facts.LightingOrNull != nullptr ? facts.LightingOrNull->Lights.GetSize() : 0);
    writeCount("dropped_placements", plan.DroppedPlacements);
    writeCount("dropped_vertices", plan.DroppedVertices);
    json.EndObject();

    json.WriteKey("camera");
    json.BeginObject();
    json.WriteKey("source");
    json.WriteString(facts.IsCameraFramed == true ? "framed" : "scene");
    json.WriteKey("vertical_fov_degrees");
    json.WriteFloat(
        static_cast<float64>(static_cast<float32>(math::RadiansToDegrees(facts.CameraOrNull->VerticalFov))));
    json.EndObject();

    json.WriteKey("environment");
    if (scene.Environment.IsPresent == true)
    {
        json.BeginObject();
        json.WriteKey("fallback");
        json.WriteString("Rec709 photometric proxy; PBRT spectral/equal-area conversion");
        json.WriteKey("radiance");
        json.BeginArray();
        for (const float64 channel : scene.Environment.Radiance)
        {
            json.WriteFloat(static_cast<float64>(static_cast<float32>(channel)));
        }
        json.EndArray();
        json.WriteKey("file");
        json.WriteString(scene.Environment.File.GetView());
        json.WriteKey("pixels_deferred");
        json.WriteBoolean(facts.IsDryRun == true && scene.Environment.File.IsEmpty() == false);
        if (facts.LightingOrNull != nullptr)
        {
            writeCount("width", facts.LightingOrNull->Environment.Width);
            writeCount("height", facts.LightingOrNull->Environment.Height);
            json.WriteKey("cooked_radiance_multiplier");
            json.BeginArray();
            for (const float32 channel : facts.LightingOrNull->EnvironmentRgb)
            {
                json.WriteFloat(static_cast<float64>(channel));
            }
            json.EndArray();
        }
        json.EndObject();
    }
    else
    {
        json.WriteNull();
    }

    json.WriteKey("materials");
    json.BeginArray();
    for (usize slot = 0; slot < plan.Materials.GetSize(); ++slot)
    {
        const PlannedMaterial& planned = plan.Materials[slot];
        json.BeginObject();
        writeCount("slot", slot);
        json.WriteKey("emissive");
        json.WriteBoolean(planned.IsEmissive);
        json.WriteKey("sources");
        json.BeginArray();
        for (const uint32 source : planned.Sources)
        {
            if (source == IR_NONE)
            {
                json.WriteString("(default)");
            }
            else
            {
                const IrMaterial& material = scene.Materials[source];
                json.WriteString(material.Name.IsEmpty() == false ? material.Name.GetView()
                                                                  : material.SourceType.GetView());
            }
        }
        json.EndArray();
        json.WriteKey("texture");
        if (planned.Texture != IR_NONE && HasImage(scene.Textures[planned.Texture]) == true)
        {
            json.WriteString(GetImageName(scene, scene.Textures[planned.Texture]));
        }
        else
        {
            json.WriteString("(swatch)");
        }
        json.WriteKey("metallic");
        json.WriteFloat(static_cast<float64>(static_cast<float32>(planned.Metallic)));
        json.WriteKey("roughness");
        json.WriteFloat(static_cast<float64>(static_cast<float32>(planned.Roughness)));
        json.WriteKey("emission");
        json.BeginArray();
        for (const float64 channel : planned.Emission)
        {
            json.WriteFloat(static_cast<float64>(static_cast<float32>(channel)));
        }
        json.EndArray();
        json.WriteKey("emission_two_sided");
        json.WriteBoolean(planned.IsEmissionTwoSided);
        json.WriteKey("emission_power_lumens");
        if (planned.HasEmissionPower == true)
        {
            json.WriteFloat(planned.EmissionPowerLumens);
        }
        else
        {
            json.WriteNull();
        }
        json.WriteKey("emitter_sources");
        json.BeginArray();
        for (const uint32 index : planned.EmitterSources)
        {
            const IrShape& source = scene.Shapes[index];
            json.BeginObject();
            writeCount("shape", index);
            writeCount("area_light", source.AreaLight);
            writeCount("line", source.Line);
            json.WriteKey("file");
            json.WriteString(source.SourceFile < scene.SourceFiles.GetSize()
                                 ? scene.SourceFiles[source.SourceFile].GetView()
                                 : std::string_view());
            json.EndObject();
        }
        json.EndArray();
        writeCount("placements", planned.Placements.GetSize());
        writeCount("vertices", planned.VertexCount);
        writeCount("indices", planned.IndexCount);
        if (facts.TexturesOrNull != nullptr && slot < facts.TexturesOrNull->GetSize())
        {
            const CookedTexture& texture = (*facts.TexturesOrNull)[slot];
            writeCount("texture_width", texture.Image.Width);
            writeCount("texture_height", texture.Image.Height);
            writeCount("texture_source_width", texture.SourceWidth);
            writeCount("texture_source_height", texture.SourceHeight);
        }
        json.EndObject();
    }
    json.EndArray();

    json.WriteKey("diagnostics");
    json.BeginArray();
    for (const Diagnostic& entry : log.GetEntries())
    {
        json.BeginObject();
        json.WriteKey("code");
        json.WriteString(entry.Code.GetView());
        json.WriteKey("severity");
        json.WriteString(GetSeverityName(entry.Severity));
        json.WriteKey("file");
        json.WriteString(entry.File.GetView());
        writeCount("line", entry.Line);
        writeCount("count", entry.Count);
        json.WriteKey("message");
        json.WriteString(entry.Message.GetView());
        json.EndObject();
    }
    json.EndArray();
    writeCount("warnings", summary.Warnings);
    writeCount("errors", summary.Errors);
    json.EndObject();
    writer.Append('\n');
}

void CountDiagnostics(const DiagnosticLog& log, ImportSummary& inoutSummary) noexcept
{
    inoutSummary.Warnings = 0;
    inoutSummary.Errors = 0;
    for (const Diagnostic& entry : log.GetEntries())
    {
        if (entry.Severity == DiagnosticSeverity::Warning)
        {
            inoutSummary.Warnings += entry.Count;
        }
        else if (entry.Severity == DiagnosticSeverity::Error)
        {
            inoutSummary.Errors += entry.Count;
        }
    }
}

[[nodiscard]] std::string_view DescribeStatus(Status status) noexcept
{
    switch (status)
    {
        case Status::NotFound:
            return "entry file not found";
        case Status::Cancelled:
            return "import cancelled";
        case Status::OutOfMemory:
            return "out of memory";
        case Status::Invalid:
            return "the scene could not be imported; see the import report";
        default:
            break;
    }
    return "cannot read the entry file";
}

/// Append path and the digest of its bytes to the result unless already listed.
[[nodiscard]] bool AddInput(std::string_view path, const Digest& digest, BestEffortResult& inoutResult) noexcept
{
    for (const String& input : inoutResult.Inputs)
    {
        if (input.GetView() == path)
        {
            return true;
        }
    }
    String copy;
    return copy.TryAssign(path) == StringStatus::Ok && inoutResult.Inputs.TryAdd(static_cast<String&&>(copy)) == true &&
           inoutResult.InputDigests.TryAdd(digest) == true;
}

/// Every file the import consulted with the digest of the exact bytes it read:
/// sources, PLY files, existing images and the licence. Images the budget never
/// read are hashed here, since their bytes did not reach the bundle.
[[nodiscard]] Status CollectInputs(const SceneIr& scene,
                                   std::string_view root,
                                   const Array<CookedTexture>& textures,
                                   const CookedLighting& lighting,
                                   const Digest* licenseDigestOrNull,
                                   std::string_view licenseName,
                                   BestEffortResult& inoutResult) noexcept
{
    inoutResult.Inputs.Clear();
    inoutResult.InputDigests.Clear();
    for (usize i = 0; i < scene.SourceFiles.GetSize(); ++i)
    {
        if (AddInput(scene.SourceFiles[i].GetView(), scene.SourceDigests[i], inoutResult) == false)
        {
            return Status::OutOfMemory;
        }
    }
    for (usize i = 0; i < scene.DataFiles.GetSize(); ++i)
    {
        if (AddInput(scene.DataFiles[i].GetView(), scene.DataDigests[i], inoutResult) == false)
        {
            return Status::OutOfMemory;
        }
    }
    for (usize t = 0; t < scene.Textures.GetSize(); ++t)
    {
        const IrTexture& texture = scene.Textures[t];
        if (texture.File.IsEmpty() == true)
        {
            continue;
        }
        Digest digest;
        bool hasDigest = false;
        for (const CookedTexture& cooked : textures)
        {
            if (cooked.ReadSource != IR_NONE &&
                scene.Textures[cooked.ReadSource].File.GetView() == texture.File.GetView())
            {
                digest = cooked.ReadDigest;
                hasDigest = true;
                break;
            }
        }
        if (hasDigest == false)
        {
            String native;
            const std::string_view parts[] = {root, std::string_view("/"), texture.File.GetView()};
            if (native.TryAppendMany(parts) != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            const Status hashed = detail::HashFile(native.GetView(), digest);
            if (hashed == Status::NotFound)
            {
                continue;
            }
            if (hashed != Status::Ok)
            {
                return hashed;
            }
        }
        if (AddInput(texture.File.GetView(), digest, inoutResult) == false)
        {
            return Status::OutOfMemory;
        }
    }
    if (scene.Environment.File.IsEmpty() == false)
    {
        String path;
        constexpr usize PATH_PARTS = 3;
        const foundation::StaticArray<std::string_view, PATH_PARTS> parts{root, "/", scene.Environment.File.GetView()};
        Digest digest;
        if (path.TryAppendMany({parts.GetData(), parts.GetSize()}) != foundation::StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
        Status hashed = Status::Ok;
        if (lighting.HasEnvironmentSourceDigest == true)
        {
            digest = lighting.EnvironmentSourceDigest;
        }
        else
        {
            hashed = detail::HashFile(path.GetView(), digest);
        }
        if (hashed != Status::Ok)
        {
            return hashed;
        }
        if (AddInput(scene.Environment.File.GetView(), digest, inoutResult) == false)
        {
            return Status::OutOfMemory;
        }
    }
    if (licenseDigestOrNull != nullptr && AddInput(licenseName, *licenseDigestOrNull, inoutResult) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}
} // namespace

namespace
{
/// Clear every output of a best-effort import.
void ResetResult(BestEffortResult& outResult) noexcept
{
    outResult.Bundle.Clear();
    outResult.Report.Clear();
    outResult.License.Clear();
    outResult.Inputs.Clear();
    outResult.InputDigests.Clear();
    outResult.Summary = ImportSummary{};
    outResult.Format = SceneSourceFormat::Unknown;
    outResult.Profile = {};
}

/// Plan, bake, pack and report a SceneIR produced by any front-end. status is the
/// front-end's outcome; NotFound/IoError skip the report like an unreadable entry.
[[nodiscard]] bool CookImportedScene(std::string_view profile,
                                     SceneIr& inoutScene,
                                     DiagnosticLog& inoutLog,
                                     const String& root,
                                     Status status,
                                     std::string_view entryFile,
                                     const BestEffortOptions& options,
                                     BestEffortResult& outResult,
                                     CookError& outError,
                                     const Cancellation* cancelOrNull) noexcept
{
    SceneIr& scene = inoutScene;
    DiagnosticLog& log = inoutLog;
    CookPlan plan;
    if (status == Status::Ok)
    {
        status = GenerateMissingTangents(scene, log, cancelOrNull);
    }
    if (status == Status::Ok)
    {
        status = PlanBudget(scene, log, plan, cancelOrNull);
    }
    ImportSummary& summary = outResult.Summary;
    summary.SourceFiles = static_cast<uint32>(scene.SourceFiles.GetSize());
    summary.SourcePlacements = static_cast<uint32>(plan.Placements.GetSize());
    summary.SourceVertices = plan.SourceVertices;
    summary.SourceMaterials = static_cast<uint32>(scene.Materials.GetSize());
    summary.SourceTextures = static_cast<uint32>(scene.Textures.GetSize());
    summary.SourceAreaLights = scene.Lights.Area;
    summary.DroppedPlacements = plan.DroppedPlacements;
    Array<CookedTexture> textures;
    Array<CookedMesh> meshes;
    if (status == Status::Ok)
    {
        // Versions 4 and 5 keep meshes indexed: consumers derive the per-triangle bake
        // charts (format::TryGetBakeChartUv) instead of reading stored corners.
        status = BakeMeshes(scene, plan, meshes, cancelOrNull);
    }
    usize geometryBytes = 0;
    for (const CookedMesh& mesh : meshes)
    {
        geometryBytes += sizeof(format::MeshRecord) + mesh.Vertices.GetSize() * sizeof(float32) +
                         mesh.Indices.GetSize() * sizeof(uint32);
    }
    IrCamera camera;
    bool isFramed = false;
    ResolveCamera(scene, meshes, camera, isFramed);
    CookedLighting lighting;
    const float64 extent = math::Max(math::Length(camera.Eye - camera.Target), 1.0);
    const float64 lightRange = math::Min(extent * 100.0, 1000000.0);
    const usize lightingRecords =
        sizeof(format::LightingRecord) + scene.AnalyticLights.GetSize() * sizeof(format::LightRecord);
    // Header plus a material and object record and up to three texture records (base,
    // surface and emission maps) per slot; mesh records are in geometryBytes.
    constexpr usize MAX_TEXTURES_PER_SLOT = 3;
    const usize recordBytes =
        sizeof(format::Header) +
        plan.Materials.GetSize() * (MAX_TEXTURES_PER_SLOT * sizeof(format::TextureRecord) +
                                    sizeof(format::MaterialRecordV6) + sizeof(format::ObjectRecordV3));
    const usize bundleLimit = format::IMPORT_LIMITS.BundleBytes;
    const usize fixedBytes = geometryBytes + recordBytes + lightingRecords;
    const usize lightingBudget = bundleLimit > fixedBytes ? bundleLimit - fixedBytes : 0;
    if (status == Status::Ok)
    {
        status = CookAuthoredLighting(scene,
                                      root.GetView(),
                                      {
                                          .RangeMetres = lightRange,
                                          .EnvironmentByteBudget = lightingBudget,
                                          .ShouldDecodeEnvironment = options.IsDryRun == false,
                                      },
                                      lighting,
                                      log);
        if (status != Status::Ok)
        {
            (void)log.Add(DiagnosticSeverity::Error,
                          codes::LIMIT_EXCEEDED,
                          {},
                          0,
                          "authored lighting cannot be preserved within the bounded photometric scene profile");
        }
    }
    const usize lightingBytes = lightingRecords + lighting.Environment.Rgb.GetSize() * sizeof(float32);
    if (status == Status::Ok && options.IsDryRun == false)
    {
        const usize reserved = geometryBytes + recordBytes + lightingBytes;
        const usize remaining = bundleLimit > reserved ? bundleLimit - reserved : 0;
        const usize budget = math::Min(remaining, format::IMPORT_LIMITS.TotalTextureBytes);
        status = CookTextures(scene, root.GetView(), plan, budget, log, textures);
    }
    if (status == Status::Ok)
    {
        summary.CookedMeshes = static_cast<uint32>(meshes.GetSize());
        summary.CookedMaterials = static_cast<uint32>(plan.Materials.GetSize());
        summary.CookedTextures = static_cast<uint32>(textures.GetSize());
        summary.CookedObjects = static_cast<uint32>(plan.Materials.GetSize());
        for (const CookedMesh& mesh : meshes)
        {
            summary.CookedVertices += static_cast<uint32>(mesh.Vertices.GetSize() / VERTEX_FLOATS);
        }
    }
    if (status == Status::Ok && options.IsDryRun == false)
    {
        usize expected = geometryBytes + recordBytes + lightingBytes;
        for (const CookedTexture& texture : textures)
        {
            expected += texture.Image.Texels.GetSize();
        }
        // ByteWriter grows by exact amounts; reserving keeps packing linear.
        if (outResult.Bundle.TryEnsureCapacity(expected) == false)
        {
            return FailWith(outError, Status::OutOfMemory, "out of memory");
        }
        detail::ByteWriter blob(outResult.Bundle);
        PackBundle(camera, plan, textures, meshes, lighting, blob);
        if (blob.IsOk() == false)
        {
            status = blob.HasOverflowed() == true ? Status::Invalid : Status::OutOfMemory;
            (void)log.Add(DiagnosticSeverity::Error, codes::LIMIT_EXCEEDED, {}, 0, "a value exceeds the float32 range");
        }
        else if (outResult.Bundle.GetSize() > bundleLimit)
        {
            status = Status::Invalid;
            (void)log.Add(DiagnosticSeverity::Error, codes::LIMIT_EXCEEDED, {}, 0, "bundle exceeds 384 MiB");
        }
        summary.BundleBytes = outResult.Bundle.GetSize();
    }
    CountDiagnostics(log, summary);
    const std::string_view path = entryFile;
    const usize slash = path.rfind('/');
    ReportFacts facts;
    facts.Profile = profile;
    facts.EntryName = slash == std::string_view::npos ? path : path.substr(slash + 1);
    facts.IsDryRun = options.IsDryRun;
    facts.IsCameraFramed = isFramed;
    facts.CameraOrNull = &camera;
    facts.TexturesOrNull = options.IsDryRun == true ? nullptr : &textures;
    facts.MeshesOrNull = &meshes;
    facts.LightingOrNull = &lighting;
    if (status != Status::NotFound && status != Status::IoError)
    {
        WriteReport(scene, plan, log, summary, facts, outResult.Report);
    }
    if (status != Status::Ok)
    {
        outResult.Bundle.Clear();
        return FailWith(outError, status, DescribeStatus(status));
    }
    std::string_view licenseName;
    for (const std::string_view name : {std::string_view("LICENSE.txt"), std::string_view("LICENSE.md")})
    {
        String licensePath;
        const std::string_view parts[] = {root.GetView(), std::string_view("/"), name};
        if (licensePath.TryAppendMany(parts) == StringStatus::Ok &&
            detail::ReadFileBounded(licensePath.GetView(), MAX_SOURCE_BYTES, outResult.License) == Status::Ok)
        {
            licenseName = name;
            break;
        }
        outResult.License.Clear();
    }
    const Digest licenseDigest = Hash(outResult.License.AsSpan());
    const Status collected = CollectInputs(scene,
                                           root.GetView(),
                                           textures,
                                           lighting,
                                           licenseName.empty() == true ? nullptr : &licenseDigest,
                                           licenseName,
                                           outResult);
    return collected == Status::Ok ? true : FailWith(outError, collected, DescribeStatus(collected));
}

/// Resolve an entry file to its native path and import root (its directory).
/// An entry file's native path and its import root (the entry's directory).
struct ResolvedEntry final
{
    String Native;
    String Root;
};

[[nodiscard]] Status ResolveEntry(std::string_view entryFile, ResolvedEntry& outEntry) noexcept
{
    const Status resolved = detail::ResolvePath(entryFile, outEntry.Native);
    if (resolved != Status::Ok)
    {
        return resolved;
    }
    const std::string_view path = outEntry.Native.GetView();
    const usize slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0)
    {
        return Status::Invalid;
    }
    return outEntry.Root.TryAssign(path.substr(0, slash)) == StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

/// Bytes examined by DetectSceneSourceFile.
constexpr usize DETECTION_PREFIX_BYTES = 4096;

[[nodiscard]] bool StartsWith(std::string_view text, std::string_view prefix) noexcept
{
    return text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool HasExtension(std::string_view name, std::string_view extension) noexcept
{
    if (name.size() < extension.size())
    {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - extension.size());
    for (usize i = 0; i < extension.size(); ++i)
    {
        const char value = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
        if (value != extension[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

SceneSourceFormat DetectSceneSourceFormat(std::span<const uint8> prefix, std::string_view fileName) noexcept
{
    std::string_view text(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    // GLB header magic (glTF 2.0 section 4.4.3.1).
    if (StartsWith(text, "glTF") == true)
    {
        return SceneSourceFormat::Gltf;
    }
    constexpr std::string_view UTF8_BYTE_ORDER_MARK = "\xEF\xBB\xBF";
    if (StartsWith(text, UTF8_BYTE_ORDER_MARK) == true)
    {
        text.remove_prefix(UTF8_BYTE_ORDER_MARK.size());
    }
    // The first meaningful token decides: '{' opens glTF JSON, OBJ records are
    // lower-case keywords, pbrt directives are capitalised words.
    usize at = 0;
    while (at < text.size())
    {
        const char value = text[at];
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n')
        {
            ++at;
            continue;
        }
        if (value == '#')
        {
            while (at < text.size() && text[at] != '\n')
            {
                ++at;
            }
            continue;
        }
        if (value == '{')
        {
            return SceneSourceFormat::Gltf;
        }
        usize end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' && text[end] != '\n')
        {
            ++end;
        }
        const std::string_view word = text.substr(at, end - at);
        constexpr usize WAVEFRONT_KEYWORD_COUNT = 11;
        constexpr foundation::StaticArray<std::string_view, WAVEFRONT_KEYWORD_COUNT>
            WAVEFRONT_KEYWORDS{"v", "vt", "vn", "vp", "f", "o", "g", "s", "mtllib", "usemtl", "l"};
        for (const std::string_view keyword : WAVEFRONT_KEYWORDS)
        {
            if (word == keyword)
            {
                return SceneSourceFormat::Obj;
            }
        }
        if (word.empty() == false && word.front() >= 'A' && word.front() <= 'Z')
        {
            return SceneSourceFormat::Pbrt;
        }
        break;
    }
    // Nothing decisive (for example a comment-only prefix): fall back to the extension.
    if (HasExtension(fileName, ".pbrt") == true)
    {
        return SceneSourceFormat::Pbrt;
    }
    if (HasExtension(fileName, ".obj") == true)
    {
        return SceneSourceFormat::Obj;
    }
    if (HasExtension(fileName, ".gltf") == true || HasExtension(fileName, ".glb") == true)
    {
        return SceneSourceFormat::Gltf;
    }
    return SceneSourceFormat::Unknown;
}

Status DetectSceneSourceFile(std::string_view entryFile, SceneSourceFormat& outFormat) noexcept
{
    detail::SequentialFile file;
    const Status opened = file.Open(entryFile);
    if (opened != Status::Ok)
    {
        return opened;
    }
    foundation::StaticArray<uint8, DETECTION_PREFIX_BYTES> prefix{};
    usize size = 0;
    while (size < DETECTION_PREFIX_BYTES)
    {
        usize count = 0;
        const Status read = file.Read(std::span<uint8>(prefix.GetData() + size, DETECTION_PREFIX_BYTES - size), count);
        if (read != Status::Ok)
        {
            return read;
        }
        if (count == 0)
        {
            break;
        }
        size += count;
    }
    outFormat = DetectSceneSourceFormat(std::span<const uint8>(prefix.GetData(), size), entryFile);
    return Status::Ok;
}

bool ImportPbrtBestEffort(std::string_view entryFile,
                          const BestEffortOptions& options,
                          BestEffortResult& outResult,
                          CookError& outError,
                          const Cancellation* cancelOrNull) noexcept
{
    ResetResult(outResult);
    SceneIr scene;
    DiagnosticLog log;
    String root;
    const Status status = pbrt::ReadScene(entryFile, scene, log, root, cancelOrNull);
    outResult.Format = SceneSourceFormat::Pbrt;
    outResult.Profile = BEST_EFFORT_PROFILE;
    return CookImportedScene(BEST_EFFORT_PROFILE,
                             scene,
                             log,
                             root,
                             status,
                             entryFile,
                             options,
                             outResult,
                             outError,
                             cancelOrNull);
}

bool ImportObjBestEffort(std::string_view entryFile, // ludus-style: abbreviation OBJ names the Wavefront file format
                         const BestEffortOptions& options,
                         BestEffortResult& outResult,
                         CookError& outError,
                         const Cancellation* cancelOrNull) noexcept
{
    ResetResult(outResult);
    outResult.Format = SceneSourceFormat::Obj;
    outResult.Profile = OBJ_BEST_EFFORT_PROFILE;
    SceneIr scene;
    DiagnosticLog log;
    ResolvedEntry resolved;
    Status status = ResolveEntry(entryFile, resolved);
    const String& root = resolved.Root;
    const std::string_view native = resolved.Native.GetView();
    detail::SequentialFile file;
    if (status == Status::Ok)
    {
        status = file.Open(native);
    }
    if (status == Status::Ok)
    {
        FileByteSource source(file);
        NativeSourceResolver resolver(root.GetView());
        status = obj::ReadScene(source, detail::GetBaseName(native), resolver, scene, log, cancelOrNull);
    }
    return CookImportedScene(OBJ_BEST_EFFORT_PROFILE,
                             scene,
                             log,
                             root,
                             status,
                             entryFile,
                             options,
                             outResult,
                             outError,
                             cancelOrNull);
}

bool ImportGltfBestEffort(std::string_view entryFile,
                          const BestEffortOptions& options,
                          BestEffortResult& outResult,
                          CookError& outError,
                          const Cancellation* cancelOrNull) noexcept
{
    ResetResult(outResult);
    outResult.Format = SceneSourceFormat::Gltf;
    outResult.Profile = GLTF_BEST_EFFORT_PROFILE;
    SceneIr scene;
    DiagnosticLog log;
    Array<uint8> bytes;
    ResolvedEntry resolved;
    Status status = ResolveEntry(entryFile, resolved);
    const String& root = resolved.Root;
    const std::string_view native = resolved.Native.GetView();
    if (status == Status::Ok)
    {
        status = detail::ReadFileBounded(native, gltf::MAX_ENTRY_BYTES, bytes);
        if (status == Status::Limit)
        {
            (void)log.Add(DiagnosticSeverity::Error,
                          codes::LIMIT_EXCEEDED,
                          detail::GetBaseName(native),
                          0,
                          "glTF entry file exceeds the size limit");
            status = Status::Invalid;
        }
    }
    if (status == Status::Ok)
    {
        NativeSourceResolver resolver(root.GetView());
        status = gltf::ReadScene(bytes.AsSpan(), detail::GetBaseName(native), resolver, scene, log, cancelOrNull);
    }
    return CookImportedScene(GLTF_BEST_EFFORT_PROFILE,
                             scene,
                             log,
                             root,
                             status,
                             entryFile,
                             options,
                             outResult,
                             outError,
                             cancelOrNull);
}

bool ImportSceneBestEffort(std::string_view entryFile,
                           const BestEffortOptions& options,
                           BestEffortResult& outResult,
                           CookError& outError,
                           const Cancellation* cancelOrNull) noexcept
{
    SceneSourceFormat format = SceneSourceFormat::Unknown;
    const Status detected = DetectSceneSourceFile(entryFile, format);
    if (detected != Status::Ok)
    {
        ResetResult(outResult);
        return FailWith(outError, detected, DescribeStatus(detected));
    }
    switch (format)
    {
        case SceneSourceFormat::Pbrt:
            return ImportPbrtBestEffort(entryFile, options, outResult, outError, cancelOrNull);
        case SceneSourceFormat::Gltf:
            return ImportGltfBestEffort(entryFile, options, outResult, outError, cancelOrNull);
        case SceneSourceFormat::Obj:
            return ImportObjBestEffort(entryFile, options, outResult, outError, cancelOrNull);
        case SceneSourceFormat::Unknown:
            break;
    }
    ResetResult(outResult);
    return FailWith(outError, Status::Unsupported, "the entry file is not a pbrt-v4, glTF 2.0/GLB or OBJ scene");
}

bool WriteBestEffortImport(std::string_view output, const BestEffortResult& result, CookError& outError) noexcept
{
    if (result.Bundle.IsEmpty() == true)
    {
        return FailWith(outError, Status::Invalid, "nothing to write (dry-run result)");
    }
    String resolved;
    if (detail::MakeDirectories(output) != Status::Ok || detail::ResolvePath(output, resolved) != Status::Ok)
    {
        return FailWith(outError, Status::IoError, "cannot create the output directory");
    }
    struct OutputFile final
    {
        std::string_view Name;
        const Array<uint8>* Data;
    };
    const OutputFile files[] = {{"scene.luscn", &result.Bundle},
                                {"import-report.json", &result.Report},
                                {"LICENSE.txt", &result.License}};
    for (const OutputFile& file : files)
    {
        if (file.Data->IsEmpty() == true)
        {
            // Drop an output left by an earlier import into the same directory (for
            // example a licence the new source does not carry). A missing file is fine.
            String stale;
            const std::string_view parts[] = {resolved.GetView(), "/", file.Name};
            if (stale.TryAppendMany(parts) == StringStatus::Ok)
            {
                (void)detail::RemoveFile(stale.GetView());
            }
            continue;
        }
        if (detail::WriteFileAtomic(resolved.GetView(), file.Name, file.Data->AsSpan()) != Status::Ok)
        {
            return FailWith(outError, Status::IoError, "cannot write the import outputs");
        }
    }
    return true;
}
} // namespace ludus::content::scene_cook
