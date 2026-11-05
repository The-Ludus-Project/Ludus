#pragma once

// Production entropy adapter for FoundationIdentity UUIDv4 generation. The
// contract (no fallback source, failure reported as EntropyUnavailable, the same
// behavior on native and browser targets) is owned by
// docs/architecture/object-handles-uuid.md, "Persistent UUID representation and
// generation".
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>

namespace ludus::platform
{
/// @brief Fill a buffer with cryptographically secure random bytes from the operating system.
///
/// macOS and Linux call `getentropy(3)` in requests of at most 256 bytes, its
/// per-call limit. Windows uses the system-preferred CNG generator with the same
/// request bound. Browser builds call Web Crypto `crypto.getRandomValues` (the
/// global Web Crypto object under Node) in requests of the same size; a missing
/// or throwing Web Crypto object reports failure instead of raising a JavaScript
/// exception. Other targets have no adapter yet and fail every nonempty request.
/// There is never a fallback to wall time, addresses, `rand` or a pseudo-random
/// generator.
///
/// Thread-safe and allocation-free. Linux may block briefly during early boot
/// until the kernel entropy pool is initialized; call it outside latency-critical
/// frame work.
/// @param outBytes Destination for `size` bytes; must be nonnull (null reports failure).
/// @param size Number of bytes to write; zero succeeds without an operating-system call.
/// @return true when all `size` bytes were written; false when entropy is
/// unavailable. On failure the destination contents are unspecified and must not
/// be used.
[[nodiscard]] bool TryFillSystemEntropy(foundation::uint8* outBytes, foundation::usize size) noexcept;

/// @brief Return the production entropy source for foundation::identity::TryGenerateUuidV4().
///
/// The source calls TryFillSystemEntropy() and carries no context or state, so
/// it may be copied, stored and used concurrently from any thread. A failure is
/// reported by TryGenerateUuidV4() as `UuidStatus::EntropyUnavailable` with its
/// output unchanged.
/// @return A source whose `FillBytesOrNull` is never null.
[[nodiscard]] foundation::identity::EntropySource GetSystemEntropySource() noexcept;
} // namespace ludus::platform
