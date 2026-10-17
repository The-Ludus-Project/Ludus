#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::foundation::diagnostics
{
/// Result of best-effort report delivery.
enum class DeliveryStatus : uint8
{
    Unavailable, ///< No configured report socket.
    Delivered,   ///< Complete bounded datagram accepted by the OS.
    Failed       ///< Invalid input, full socket or transport failure.
};

/// Browser backend: native socket/control configuration is unsupported (false /
/// Failed), decisions always Terminate, and CI interaction is conservatively
/// suppressed. Emergency writes deliver bounded text to console.error and report
/// bridge failure explicitly. No pthreads, sockets, signals, or helper startup.
/// The common wire codec remains available but opens no transport.
///
/// Windows backend: socket/control configuration is likewise unsupported (false /
/// Failed) and decisions always Terminate. TryWriteEmergencyBytes reports
/// Unavailable; WriteEmergencyBytes makes bounded blocking attempts on the standard
/// error handle. CI detection reads the same environment signals as POSIX.
///
/// Healthy startup only, once, before threads can report failures. Duplicates a
/// connected AF_UNIX/SOCK_DGRAM descriptor and retains it until process exit.
/// macOS also enables SO_NOSIGPIPE on the duplicated socket (shared with its original).
/// No replacement/shutdown: published descriptor lifetime cannot race a failure.
[[nodiscard]] bool ConfigureEmergencySocket(int descriptor) noexcept;

/// Assertion transport: bounded attempts, nonblocking sends with SIGPIPE suppression. Never falls
/// back to stderr. No allocation, stdio, callbacks, assertions or waiting for an
/// application lock. OS scheduling is not a real-time guarantee.
[[nodiscard]] DeliveryStatus TryWriteEmergencyBytes(const char* data, usize size) noexcept;

/// Emergency bytes only, not a replacement for Logging. No allocation, stdio,
/// assertions or application callbacks. Native stderr fallback can BLOCK. Write
/// attempts are capped; SIGPIPE cleanup retries EINTR to avoid killing the logging caller.
/// Neither operation has a time bound. False means incomplete/failed delivery.
/// Caller supplies readable memory. Neither durable storage nor signal safety
/// is promised. For ordinary Logging only; assertions must use TryWrite instead.
[[nodiscard]] bool WriteEmergencyBytes(const char* data, usize size) noexcept;

// Control endpoint
///
/// A separate versioned channel for explicit development ASSERT decisions.
/// REQUIRE/FATAL remain terminal; only eligible ASSERT callers may resume.
///
/// The endpoint is a connected AF_UNIX control socket (Linux SOCK_SEQPACKET; macOS SOCK_STREAM) descriptor supplied by
/// the helper at launch and configured once during healthy startup, exactly like the report socket: it is never opened,
/// replaced, or torn down from a failure handler.
///
/// The wire format is an EXPLICIT little-endian byte encoding, not a
/// compiler-padded C++ struct sent raw. Every message is a fixed 16-byte header
/// followed by `Length` payload bytes:
///
///     offset 0  uint32  magic           CONTROL_PROTOCOL_MAGIC
///     offset 4  uint16  version         CONTROL_PROTOCOL_VERSION
///     offset 6  uint16  kind            ControlMessageType
///     offset 8  uint32  incidentId      0 for Hello/HelloAck; the active
///                                       incident for Decision*
///     offset 12 uint32  length          payload byte count that follows
///     offset 16 uint8[length]           payload (e.g. bounded owned report bytes
///                                       for a decision request; empty otherwise)
///
/// Encode/decode go through the helpers below so the byte layout is the single
/// source of truth on both sides. Lengths, versions, ids and kinds are validated;
/// an unknown/stale/duplicate/malformed frame never authorizes continuation.

/// Protocol identity. Bump the version on any wire-layout change so the engine
/// and helper refuse a mismatch instead of misreading frames. This IPC version is
/// independent of the terminal fatal-packet version.
inline constexpr uint32 CONTROL_PROTOCOL_MAGIC = 0x4C554443u; ///< "LUDC"
/// Current wire encoding version; rejects incompatible helper messages.
inline constexpr uint16 CONTROL_PROTOCOL_VERSION = 1u;
/// Fixed header byte count, independent of C++ structure padding.
inline constexpr usize CONTROL_HEADER_SIZE = 16u;
/// Maximum owned report or decision payload in bytes.
inline constexpr usize CONTROL_MAX_PAYLOAD = 2048u; ///< matches the report bound

/// C++ base type is uint8 (the four kinds fit); the wire still encodes `kind` as
/// a fixed 2-byte little-endian field via the header helpers, independent of this.
enum class ControlMessageType : uint8
{
    Hello = 1,           ///< engine -> helper: announce protocol; empty payload
    HelloAck = 2,        ///< helper -> engine: accept the agreed version; empty payload
    DecisionRequest = 3, ///< engine -> helper: incidentId + owned report payload
    DecisionReply = 4    ///< helper -> engine: incidentId + 1-byte ControlDecision
};

/// Only an explicit matching ContinueOnce reply authorizes continuation.
enum class ControlDecision : uint8
{
    Terminate = 0,   ///< default / safe outcome; also used when the channel fails
    ContinueOnce = 1 ///< Resume past this one known-broken development assertion.
};

/// Decoded header view. The payload is referenced separately, never owned here.
struct ControlHeader
{
    uint16 Kind = 0; ///< ControlMessageType
    /// Active assertion incident, or zero for the handshake.
    uint32 IncidentId = 0;
    uint32 Length = 0; ///< payload bytes following the header
};

/// Write a validated 16-byte header into `buffer` (>= CONTROL_HEADER_SIZE) using
/// the explicit little-endian layout above. Returns the header size, or 0 if the
/// buffer is too small or the payload length exceeds CONTROL_MAX_PAYLOAD. No
/// allocation; safe from any context.
[[nodiscard]] usize
EncodeControlHeader(char* buffer, usize capacity, ControlMessageType kind, uint32 incidentId, uint32 length) noexcept;

/// Parse and validate a received frame's header from `buffer`/`size`. Rejects a
/// short buffer, wrong magic/version, an unknown kind, or a payload length that
/// would exceed the frame; returns false and leaves `header` unchanged.
[[nodiscard]] bool DecodeControlHeader(const char* buffer, usize size, ControlHeader& header) noexcept;

/// Process-lifetime startup state of the optional control endpoint.
enum class ControlState : uint8
{
    Unconfigured, ///< no descriptor supplied
    Failed,       ///< descriptor supplied but invalid / handshake rejected
    Ready         ///< handshake completed; channel usable for explicit decisions
};

/// Healthy startup only, once, before any failure can occur. Duplicates the
/// connected AF_UNIX control socket (Linux SOCK_SEQPACKET; macOS SOCK_STREAM) descriptor (CLOEXEC), performs the
/// versioned Hello/HelloAck handshake (explicit byte encoding) with a bounded, nonblocking timeout, and retains the
/// owned descriptor until process exit. Returns Ready only when the helper acknowledged the agreed version. A
/// malformed/absent ack or a bad descriptor yields Failed; the engine still runs (report-only). No allocation, no
/// stdio, no assertions. Not usable from a failure handler. macOS reads complete stream frames against one two-second
/// handshake deadline. The socket must have a single reader/writer; do not use the original after startup.
ControlState ConfigureControlEndpoint(int descriptor) noexcept;

/// The last configured state. Cheap, lock-free; safe to read on the failure path
/// before attempting a decision exchange.
[[nodiscard]] ControlState ControlEndpointState() noexcept;

/// Ask the external helper for the active incident's Continue-once / Terminate
/// decision. Sends a DecisionRequest (incident id + bounded owned report bytes)
/// and waits for the matching DecisionReply. This is the ONLY function on the
/// assertion failure path that may block: a healthy dialog legitimately waits for
/// a human, so there is no auto-continue timeout. The wait is per this milestone's
/// contract, not a bounded-completion promise.
///
/// Darwin streams read exactly one bounded frame across fragmented/coalesced reads.
/// Returns ContinueOnce ONLY for a well-formed reply whose kind is DecisionReply,
/// whose incident id equals `incidentId`, and whose one-byte payload is
/// ContinueOnce. Anything else — endpoint not Ready, a send/recv error, a closed
/// helper, a wrong/duplicate/stale incident id, an unknown kind, a bad length, or
/// a payload that is not exactly ContinueOnce — returns Terminate. Only an
/// eligible caller (non-CI Debug with a completed handshake) should call this;
/// it never itself decides eligibility. No allocation; not signal-safe.
[[nodiscard]] ControlDecision RequestAssertDecision(uint32 incidentId, const char* report, usize size) noexcept;

/// True when the process appears to run under continuous integration. Reads the
/// environment (the generic CI marker plus common runner-specific variables, and
/// an explicit LUDUS_CI=0/1 override), biasing toward true on ambiguity so a
/// runner is never prompted. CI always forces report-only and vetoes an ASSERT continue. Query off the success path
/// only.
[[nodiscard]] bool IsContinuousIntegration() noexcept;
} // namespace ludus::foundation::diagnostics
