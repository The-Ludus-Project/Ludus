#pragma once

// Hosted-window channel protocol, version 2 (private to Platform).
//
// The editor endpoint and the hosted window speak this bounded binary protocol
// over one Unix stream socket. It is separate from the GameHost control protocol
// and from the tooling stdio events. The contract is owned by
// docs/architecture/editor-gui-systems.md, "Hosted window channel".
//
// Every message is a 4-byte header (uint16 kind, uint16 payload bytes) followed by
// a fixed-size payload. Integers are little-endian. Both sides reject unknown
// kinds, wrong payload sizes and out-of-range values as a protocol violation.
//
// Version 2 adds the hosted surface: the Welcome carries the macOS Mach
// rendezvous nonce, the window sends Images and Present, and the endpoint sends
// Release. A version-1 peer is refused as IncompatibleVersion.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/hosted_window.h>

#include <string_view>

namespace ludus::platform::hosted
{
using foundation::float32;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Wire protocol version carried by Hello and Welcome.
inline constexpr uint16 PROTOCOL_VERSION = 2;
/// First field of every Hello payload: "LHWN" read as little-endian bytes.
inline constexpr uint32 HELLO_MAGIC = 0x4E57484C;
/// Random session token length; the environment carries it as lowercase hexadecimal.
inline constexpr usize TOKEN_BYTES = 32;
/// Hexadecimal digits that spell one token.
inline constexpr usize TOKEN_HEX_BYTES = TOKEN_BYTES * 2;
/// Message header: uint16 kind and uint16 payload size.
inline constexpr usize HEADER_BYTES = 4;
/// Held-key baseline bit mask, one bit per dense Ludus key slot.
inline constexpr usize KEY_MASK_BYTES = (input::KEY_COUNT + 7) / 8;
/// Largest backing-pixel extent either side accepts on one axis.
inline constexpr uint32 MAX_EXTENT_PIXELS = 32768;
/// Largest accepted backing scale (backing pixels per logical point).
inline constexpr float32 MAX_SCALE = 16.0F;
/// Longest socket path, in bytes. It fits sockaddr_un on macOS (104) and Linux (108)
/// with the terminating zero.
inline constexpr usize MAX_SOCKET_PATH_BYTES = 103;
/// Random bytes that make one session's Mach rendezvous name unique (macOS); zero elsewhere.
inline constexpr usize RENDEZVOUS_NONCE_BYTES = 16;
/// Prefix of a channel address: "1:<token hex>:<socket path>". The address format
/// has its own version, independent of PROTOCOL_VERSION.
inline constexpr std::string_view ADDRESS_PREFIX = "1:";
/// Longest channel address: prefix, token, separator and socket path.
inline constexpr usize MAX_ADDRESS_BYTES = ADDRESS_PREFIX.size() + TOKEN_HEX_BYTES + 1 + MAX_SOCKET_PATH_BYTES;

/// One session token.
using Token = foundation::StaticArray<uint8, TOKEN_BYTES>;
/// A bounded socket path.
using SocketPath = foundation::StaticString<MAX_SOCKET_PATH_BYTES>;
/// A bounded channel address, as published in HOSTED_WINDOW_ENVIRONMENT_VARIABLE.
using ChannelAddressText = foundation::StaticString<MAX_ADDRESS_BYTES>;
/// Per-session rendezvous nonce, carried by the Welcome; all zero when there is no rendezvous.
using RendezvousNonce = foundation::StaticArray<uint8, RENDEZVOUS_NONCE_BYTES>;
/// A Vulkan device or driver UUID.
using AdapterUuid = foundation::StaticArray<uint8, HOSTED_ADAPTER_UUID_BYTES>;

/// Message kinds. Values are wire values and never reused.
// NOLINTNEXTLINE(performance-enum-size)
enum class MessageKind : uint16
{
    /// Window -> endpoint: magic, version, key count and token.
    Hello = 1,
    /// Endpoint -> window: accepted; carries the initial extent, scale and visibility.
    Welcome = 2,
    /// Endpoint -> window: refused with a HostedRefusal; the endpoint then closes the channel.
    Refused = 3,
    /// Endpoint -> window: extent in backing pixels plus scale.
    Resize = 4,
    /// Endpoint -> window: WindowVisibility.
    Visibility = 5,
    /// Endpoint -> window: FocusEntered, FocusLost or Overflow reset with a held-key mask.
    Reset = 6,
    /// Endpoint -> window: one physical key transition.
    Key = 7,
    /// Endpoint -> window: the host closed the window.
    Close = 8,
    /// Window -> endpoint: a hosted surface generation. Its sharing handles travel
    /// alongside: SCM_RIGHTS on Linux, the Mach rendezvous on macOS.
    Images = 9,
    /// Window -> endpoint: a completed frame (generation, image, sequence).
    Present = 10,
    /// Endpoint -> window: the consumer has finished with a presented image.
    Release = 11,
};

/// Payload sizes; each kind has exactly one.
inline constexpr usize HELLO_PAYLOAD_BYTES = 4 + 2 + 2 + TOKEN_BYTES;
inline constexpr usize WELCOME_PAYLOAD_BYTES = 2 + 1 + 1 + 4 + 4 + 4 + RENDEZVOUS_NONCE_BYTES;
inline constexpr usize REFUSED_PAYLOAD_BYTES = 1;
inline constexpr usize RESIZE_PAYLOAD_BYTES = 4 + 4 + 4;
inline constexpr usize VISIBILITY_PAYLOAD_BYTES = 1;
inline constexpr usize RESET_PAYLOAD_BYTES = 1 + KEY_MASK_BYTES;
inline constexpr usize KEY_PAYLOAD_BYTES = 2 + 1 + 1;
inline constexpr usize CLOSE_PAYLOAD_BYTES = 0;
inline constexpr usize IMAGES_PAYLOAD_BYTES =
    4 + 4 + 4 + 1 + 1 + 2 + (8 * HOSTED_SURFACE_IMAGE_COUNT) + 4 + (2 * HOSTED_ADAPTER_UUID_BYTES);
inline constexpr usize PRESENT_PAYLOAD_BYTES = 4 + 1 + 3 + 8;
inline constexpr usize RELEASE_PAYLOAD_BYTES = 4 + 1 + 3;
/// Largest payload of any version-2 message.
inline constexpr usize MAX_PAYLOAD_BYTES = IMAGES_PAYLOAD_BYTES;
/// Largest encoded version-2 message.
inline constexpr usize MAX_MESSAGE_BYTES = HEADER_BYTES + MAX_PAYLOAD_BYTES;
/// Largest payload any version may frame. A peer reading a longer frame stops at once; a
/// shorter one, even of a future Hello, is framed so its version can be reported.
inline constexpr usize MAX_FRAME_PAYLOAD_BYTES = 255;
/// Largest frame of any version; receive buffers hold at least this much.
inline constexpr usize MAX_FRAME_BYTES = HEADER_BYTES + MAX_FRAME_PAYLOAD_BYTES;
static_assert(RESET_PAYLOAD_BYTES <= MAX_PAYLOAD_BYTES && WELCOME_PAYLOAD_BYTES <= MAX_PAYLOAD_BYTES &&
              HELLO_PAYLOAD_BYTES <= MAX_PAYLOAD_BYTES && MAX_PAYLOAD_BYTES <= MAX_FRAME_PAYLOAD_BYTES);

/// Key payload flag: the transition is an operating-system auto-repeat.
inline constexpr uint8 KEY_FLAG_REPEAT = 0x01;

/// Decoded message. Only the fields of `Kind` are meaningful.
struct Message final
{
    MessageKind Kind = MessageKind::Close;
    /// Hello and Welcome.
    uint16 Version = PROTOCOL_VERSION;
    /// Hello: the sender's input::KEY_COUNT.
    uint16 KeyCount = 0;
    /// Hello.
    Token SessionToken = {};
    /// Welcome and Resize.
    uint32 Width = 0;
    /// Welcome and Resize.
    uint32 Height = 0;
    /// Welcome and Resize.
    float32 Scale = 1.0F;
    /// Welcome and Visibility.
    WindowVisibility Visibility = WindowVisibility::Visible;
    /// Refused.
    HostedRefusal Refusal = HostedRefusal::None;
    /// Reset: FocusEntered, FocusLost or Overflow.
    input::ResetReason Reason = input::ResetReason::None;
    /// Reset: held keys. Focused is true only for FocusEntered.
    input::FocusBaseline Baseline = {};
    /// Key.
    input::Key PhysicalKey = input::Key::Unknown;
    /// Key.
    input::KeyTransition Transition = input::KeyTransition::Down;
    /// Key.
    bool IsRepeat = false;
    /// Welcome: the endpoint's Mach rendezvous nonce, or zero.
    RendezvousNonce Nonce = {};
    /// Images, Present and Release: nonzero surface generation.
    uint32 Generation = 0;
    /// Present and Release: image index, below HOSTED_SURFACE_IMAGE_COUNT.
    uint32 Image = 0;
    /// Present: frame sequence number.
    uint64 Sequence = 0;
    /// Images (with Width and Height, each 1 to MAX_EXTENT_PIXELS).
    HostedImageFormat Format = HostedImageFormat::Rgba8Unorm;
    /// Images: MachPort or OpaqueFileDescriptor.
    HostedHandleKind HandleKind = HostedHandleKind::None;
    /// Images: exported allocation sizes (Vulkan).
    foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT> AllocationSizes = {};
    /// Images: memory type index (Vulkan).
    uint32 MemoryTypeIndex = 0;
    /// Images: deviceUUID (Vulkan).
    AdapterUuid DeviceUuid = {};
    /// Images: driverUUID (Vulkan).
    AdapterUuid DriverUuid = {};
};

/// Result of decoding the front of a byte stream.
enum class DecodeStatus : uint8
{
    /// One message was decoded; `outConsumed` bytes belong to it.
    Ok,
    /// The bytes hold only part of a message.
    NeedMore,
    /// The stream violates the protocol; the channel must be closed.
    Malformed,
};

/// @brief Encode one message.
/// @param message A message whose fields are in range for its kind.
/// @param outBytes Destination with at least MAX_MESSAGE_BYTES of room.
/// @param outSize Encoded size; unchanged on failure.
/// @return false when a field is out of range or the destination is too small; nothing is written then.
[[nodiscard]] bool
TryEncodeMessage(const Message& message, foundation::BorrowedView<uint8> outBytes, usize& outSize) noexcept;

/// @brief Decode the first message of a received byte stream.
///
/// Hello is decoded far enough to report a version or key-count mismatch even when
/// its payload size differs from version 1, so a peer built from another SDK is
/// refused as IncompatibleVersion rather than Malformed.
/// @param bytes Received bytes, starting at a message boundary.
/// @param outMessage Decoded message; unchanged unless Ok.
/// @param outConsumed Size of the decoded message; unchanged unless Ok.
[[nodiscard]] DecodeStatus
TryDecodeMessage(foundation::BorrowedView<const uint8> bytes, Message& outMessage, usize& outConsumed) noexcept;

/// True when `scale` is finite, positive and at most MAX_SCALE.
[[nodiscard]] bool IsValidScale(float32 scale) noexcept;

/// True when `message` carries a hosted surface generation: nonzero generation, a
/// nonzero extent within MAX_EXTENT_PIXELS, a known format and a sharing handle kind.
[[nodiscard]] bool IsValidImagesMessage(const Message& message) noexcept;

/// Constant-time comparison of two tokens.
[[nodiscard]] bool AreTokensEqual(const Token& left, const Token& right) noexcept;

/// @brief Format "1:<token hex>:<socket path>".
/// @return false when the path is empty, relative, longer than MAX_SOCKET_PATH_BYTES or contains a
/// zero byte; `outAddress` is unchanged then.
[[nodiscard]] bool
TryFormatChannelAddress(const Token& token, std::string_view socketPath, ChannelAddressText& outAddress) noexcept;

/// @brief Parse a channel address produced by TryFormatChannelAddress.
/// @return false for any other version, a malformed token or a missing, relative or overlong path;
/// outputs are unchanged then.
[[nodiscard]] bool
TryParseChannelAddress(std::string_view address, Token& outToken, SocketPath& outSocketPath) noexcept;
} // namespace ludus::platform::hosted
