#pragma once

#include <ludus/foundation/base/types.h>

/// @file
/// Byte layout of a render graph frame capture file (`.lurgcap`), shared by the Renderer's
/// encoder and offline readers such as `scripts/reference-hdr`. Every field is written
/// little-endian at the offset named here; host struct bytes are never copied. The pixel record
/// matches the RT1 reference capture (`LUHDR01`) so readers convert both the same way. See
/// docs/architecture/render-graph-frame-capture.md.
namespace ludus::content::render_graph_format
{
/// Eight magic bytes at offset zero.
inline constexpr char CAPTURE_MAGIC[] = "LURGCAP";
/// Bytes of CAPTURE_MAGIC including its terminating zero, which the file stores.
inline constexpr foundation::usize CAPTURE_MAGIC_BYTES = sizeof(CAPTURE_MAGIC);
/// Layout version written at CAPTURE_VERSION_OFFSET.
inline constexpr foundation::uint32 CAPTURE_VERSION = 1;
/// Fixed header size; unused header bytes are zero.
inline constexpr foundation::uint32 CAPTURE_HEADER_BYTES = 256;
/// One pixel: three float32 mean radiance channels, then the uint32 committed sample count.
inline constexpr foundation::uint32 CAPTURE_PIXEL_BYTES = 16;

/// uint32 layout version.
inline constexpr foundation::usize CAPTURE_VERSION_OFFSET = 8;
static_assert(CAPTURE_MAGIC_BYTES == CAPTURE_VERSION_OFFSET, "the magic occupies the first eight bytes");
/// uint32 header size, CAPTURE_HEADER_BYTES.
inline constexpr foundation::usize CAPTURE_HEADER_SIZE_OFFSET = 12;
/// uint32 pixel record size, CAPTURE_PIXEL_BYTES.
inline constexpr foundation::usize CAPTURE_PIXEL_SIZE_OFFSET = 16;
/// uint32 CaptureFlags bits.
inline constexpr foundation::usize CAPTURE_FLAGS_OFFSET = 20;
/// uint32 image width in pixels.
inline constexpr foundation::usize CAPTURE_WIDTH_OFFSET = 24;
/// uint32 image height in pixels.
inline constexpr foundation::usize CAPTURE_HEIGHT_OFFSET = 28;
/// uint32 requested samples per pixel.
inline constexpr foundation::usize CAPTURE_TARGET_SAMPLES_OFFSET = 32;
/// uint32 frames whose samples were submitted; a pixel's own count can be lower after a failure.
inline constexpr foundation::usize CAPTURE_SUBMITTED_SAMPLES_OFFSET = 36;
/// uint32 pixels whose estimate latched a shader failure.
inline constexpr foundation::usize CAPTURE_FAILED_PIXELS_OFFSET = 40;
/// uint32 logical plan resource that was captured.
inline constexpr foundation::usize CAPTURE_OUTPUT_RESOURCE_OFFSET = 44;
/// uint32 plan pass whose Accumulate mean was read.
inline constexpr foundation::usize CAPTURE_ACCUMULATION_PASS_OFFSET = 48;
/// uint32 sample limit authored on that pass.
inline constexpr foundation::usize CAPTURE_AUTHORED_LIMIT_OFFSET = 52;
/// uint64 scene revision submitted with the frozen scene; it also seeds graph sampling.
inline constexpr foundation::usize CAPTURE_SCENE_REVISION_OFFSET = 56;
/// 32 bytes identifying the runtime package, normally the SHA-256 of its bytes; zero when unknown.
inline constexpr foundation::usize CAPTURE_PACKAGE_IDENTITY_OFFSET = 64;
/// 32 bytes identifying the caller's scene inputs; zero when unknown.
inline constexpr foundation::usize CAPTURE_SCENE_IDENTITY_OFFSET = 96;
/// Bytes of each identity field.
inline constexpr foundation::usize CAPTURE_IDENTITY_BYTES = 32;
/// 16 float32 world-to-clip matrix entries, column-major.
inline constexpr foundation::usize CAPTURE_WORLD_TO_CLIP_OFFSET = 128;
/// Three float32 camera position components, in world space.
inline constexpr foundation::usize CAPTURE_CAMERA_POSITION_OFFSET = 192;
/// float32 values in the world-to-clip matrix.
inline constexpr foundation::usize CAPTURE_MATRIX_VALUES = 16;
/// float32 values in the camera position.
inline constexpr foundation::usize CAPTURE_POSITION_VALUES = 3;

/// Bits of the CAPTURE_FLAGS_OFFSET field. Unlisted bits are zero.
// NOLINTNEXTLINE(performance-enum-size): serialized as an explicit 32-bit field.
enum class CaptureFlag : foundation::uint32
{
    Partial = 1,       ///< Stopped before every pixel reached the target.
    Failed = 2,        ///< At least one pixel latched a shader failure.
    LimitOverride = 4, ///< The target replaced the authored Accumulate limit for this capture.
    Orthographic = 8,  ///< The camera is orthographic.
    ReverseDepth = 16, ///< The camera uses reverse-Z depth.
    InfiniteFar = 32,  ///< The camera's far plane is at infinity.
};
/// Mask of every defined CaptureFlag bit.
inline constexpr foundation::uint32 CAPTURE_FLAG_MASK = 63;
} // namespace ludus::content::render_graph_format
