#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <QByteArray>
#include <QImage>
#include <QString>

#include <atomic>

// CPU half of the render-graph texture viewer: decodes one read-back subresource, describes
// probed texels in their storage format and maps channels, ranges and exposure to a display
// image. Pure functions of their inputs, so every rule here is unit tested without a GPU.
// See docs/development/render-graph-authoring.md#texture-viewer.
namespace ludus::editor
{
/// Largest channel count of any inspected encoding (RGBA).
constexpr foundation::uint32 MAX_TEXEL_CHANNELS = 4;

/// Storage encoding of an inspected subresource's bytes.
enum class TexelEncoding : foundation::uint8
{
    Rgba8Unorm,   ///< Four normalized bytes per texel in R, G, B, A order.
    Rgba16Float,  ///< Four IEEE binary16 values per texel, little-endian.
    Depth32Float, ///< One IEEE binary32 depth value per texel, little-endian.
};

/// What the display image shows of each texel.
enum class TextureChannel : foundation::uint8
{
    Rgba,      ///< Colour channels as stored; alpha is not composited.
    Red,       ///< Red (or depth) as grey.
    Green,     ///< Green as grey.
    Blue,      ///< Blue as grey.
    Alpha,     ///< Alpha as grey.
    Luminance, ///< Rec. 709 relative luminance of R, G and B as grey.
};

/// How the shown value range is chosen.
enum class TextureRangeMode : foundation::uint8
{
    Automatic, ///< Minimum and maximum of the finite shown values.
    Manual,    ///< The caller's Minimum and Maximum.
};

/// One tightly packed CPU copy of an inspected subresource, top row first.
struct InspectedTexture final
{
    TexelEncoding Encoding = TexelEncoding::Rgba8Unorm;
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    /// Width * Height * GetTexelBytes(Encoding) bytes.
    QByteArray Bytes;

    /// True when Bytes holds exactly the declared extent.
    [[nodiscard]] bool IsValid() const noexcept;
};

/// One decoded texel. Unused channels are zero; Bits keeps the stored words of each channel.
struct TexelValue final
{
    foundation::StaticArray<foundation::float32, MAX_TEXEL_CHANNELS> Channels{};
    foundation::StaticArray<foundation::uint32, MAX_TEXEL_CHANNELS> Bits{};
    foundation::uint32 ChannelCount = 0;
};

/// Display controls. Exposure applies only to HDR encodings (see IsHighDynamicRange).
struct TextureDisplaySettings final
{
    TextureChannel Channel = TextureChannel::Rgba;
    TextureRangeMode Range = TextureRangeMode::Manual;
    foundation::float32 Minimum = 0;
    foundation::float32 Maximum = 1;
    /// Scale by 2^ExposureStops before the range remap.
    foundation::float32 ExposureStops = 0;
    /// Leave the far plane (depth exactly 1, the cleared background) out of the automatic range.
    bool ShouldIgnoreDepthBackground = true;
};

/// A value range found in an inspected texture.
struct TextureValueRange final
{
    foundation::float32 Minimum = 0;
    foundation::float32 Maximum = 1;
    /// False when no finite value contributed; Minimum and Maximum are then 0 and 1.
    bool HasValues = false;
};

/// Cooperative cancellation, owned until a conversion has finished.
/// Cancel may run on the GUI thread while the worker polls between rows.
class TextureConversionCancellation final
{
public:
    void Cancel() noexcept;
    /// Begin another conversion. Must not overlap any previous conversion's token reads.
    void Reset() noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;

private:
    std::atomic<bool> mIsCancelled{false};
};

/// Bytes per texel of an encoding.
[[nodiscard]] foundation::uint32 GetTexelBytes(TexelEncoding encoding) noexcept;
/// Channels stored per texel: MAX_TEXEL_CHANNELS for colour encodings, 1 for depth.
[[nodiscard]] foundation::uint32 GetChannelCount(TexelEncoding encoding) noexcept;
/// True for float encodings whose values may exceed one and take an exposure control.
[[nodiscard]] bool IsHighDynamicRange(TexelEncoding encoding) noexcept;
/// True when the channel can be shown for the encoding; depth admits Rgba and Red only.
[[nodiscard]] bool IsChannelAvailable(TexelEncoding encoding, TextureChannel channel) noexcept;
/// Display name of an encoding, matching the render-graph format vocabulary.
[[nodiscard]] QString GetEncodingName(TexelEncoding encoding);

/// IEEE binary16 to binary32, including subnormals, infinities and NaN.
[[nodiscard]] foundation::float32 DecodeHalf(foundation::uint16 bits) noexcept;
/// Reassemble four little-endian bytes into one binary32 value without changing a bit.
[[nodiscard]] foundation::float32 DecodeFloat(foundation::uint8 byte0,
                                              foundation::uint8 byte1,
                                              foundation::uint8 byte2,
                                              foundation::uint8 byte3) noexcept;

/// Decode the texel at (x, y), counted from the top-left texel.
/// @return False with outValue unchanged when the texture is invalid or the coordinate is outside it.
[[nodiscard]] bool
ReadTexel(const InspectedTexture& texture, foundation::uint32 x, foundation::uint32 y, TexelValue& outValue) noexcept;
/// The value the display maps for a channel: luminance combines R, G and B; Rgba reads Red.
[[nodiscard]] foundation::float32 SelectChannel(const TexelValue& value, TextureChannel channel) noexcept;
/// Map value from [minimum, maximum] to [0, 1], clamped. An empty range is a step: values at or
/// below minimum map to 0 and values above it to 1 (a flat depth plane stays visible).
[[nodiscard]] foundation::float32
Remap(foundation::float32 value, foundation::float32 minimum, foundation::float32 maximum) noexcept;
/// Range of the shown channel over the finite values of the texture (all colour channels for Rgba).
/// Cancellation returns the default empty range; the optional token must outlive this call.
[[nodiscard]] TextureValueRange
FindValueRange(const InspectedTexture& texture,
               TextureChannel channel,
               bool shouldIgnoreDepthBackground,
               const TextureConversionCancellation* cancellationOrNull = nullptr) noexcept;
/// Probe text for one texel: coordinates, decoded values and stored bits in the texture's format.
/// @return An empty string outside the texture.
[[nodiscard]] QString DescribeTexel(const InspectedTexture& texture, foundation::uint32 x, foundation::uint32 y);
/// Build the RGBA8 display image. Non-finite values show magenta so they cannot hide in the range.
/// @param outRange Receives the range actually applied (the automatic one when requested).
/// @param cancellationOrNull Optional thread-safe cancellation polled between rows in both passes.
/// @return A null image on invalid input, allocation failure or cancellation; preserves outRange then.
[[nodiscard]] QImage MakeDisplayImage(const InspectedTexture& texture,
                                      const TextureDisplaySettings& settings,
                                      TextureValueRange& outRange,
                                      const TextureConversionCancellation* cancellationOrNull = nullptr);

/// Nearest-neighbour view of a texture in a widget: texel (u, v) covers widget pixels
/// [Offset + u * Zoom, Offset + (u + 1) * Zoom).
struct TextureViewTransform final
{
    foundation::float32 Zoom = 1;
    foundation::float32 OffsetX = 0;
    foundation::float32 OffsetY = 0;
};

/// Smallest and largest zoom; the largest shows one texel as a 64-pixel block.
constexpr foundation::float32 MINIMUM_TEXTURE_ZOOM = 0.0625F;
constexpr foundation::float32 MAXIMUM_TEXTURE_ZOOM = 64.0F;

/// Centre the texture at the largest whole zoom that fits (or the fitting fraction when it is
/// larger than the widget), so texels stay square blocks.
[[nodiscard]] TextureViewTransform FitTexture(foundation::uint32 textureWidth,
                                              foundation::uint32 textureHeight,
                                              foundation::float32 viewWidth,
                                              foundation::float32 viewHeight) noexcept;
/// Multiply zoom by factor (clamped to the zoom bounds), keeping the texture point under the
/// anchor fixed.
[[nodiscard]] TextureViewTransform ZoomTexture(const TextureViewTransform& transform,
                                               foundation::float32 factor,
                                               foundation::float32 anchorX,
                                               foundation::float32 anchorY) noexcept;
/// Texel under a widget position.
/// @return False outside the texture; outX and outY are unchanged then.
[[nodiscard]] bool MapToTexel(const TextureViewTransform& transform,
                              foundation::uint32 textureWidth,
                              foundation::uint32 textureHeight,
                              foundation::float32 viewX,
                              foundation::float32 viewY,
                              foundation::uint32& outX,
                              foundation::uint32& outY) noexcept;
} // namespace ludus::editor
