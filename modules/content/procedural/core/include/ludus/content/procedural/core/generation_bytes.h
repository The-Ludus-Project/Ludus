#pragma once

// Canonical bytes and content digests for procedural generation: the encoding of
// cache keys, cached stage products and manifests. Contract:
// docs/architecture/procedural-content-generation.md#incremental-cooking-and-cache-keys.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural
{
using foundation::core::int16;
using foundation::core::int32;
using foundation::core::uint16;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::uint8;
using foundation::core::usize;

/// Bytes in a generation digest.
inline constexpr usize GENERATION_DIGEST_BYTES = sizeof(content::Digest::Data);

/// @brief Content digest of canonical bytes: the Content module's SHA-256 digest.
///
/// Identifies a cache key, a stage product or a manifest. Equal digests are treated as equal content; the bytes a
/// digest covers are always the explicit little-endian encodings below, never native struct representation.
using GenerationDigest = content::Digest;

/// @brief Strict weak order over digests: lexicographic over the digest bytes. For sorted containers.
struct GenerationDigestLess final
{
    /// Whether `left` orders before `right`.
    [[nodiscard]] bool operator()(const GenerationDigest& left, const GenerationDigest& right) const noexcept;
};

/// @brief Digest of a byte sequence.
/// @param bytes A valid view; an empty view digests the empty sequence.
/// @return SHA-256 of the bytes, or the empty-sequence digest when the view is invalid.
[[nodiscard]] GenerationDigest ComputeGenerationDigest(foundation::BorrowedView<const uint8> bytes) noexcept;

/// @brief Appends fixed-width little-endian fields to an owned byte buffer.
///
/// Allocation failure latches HasFailed and ignores later writes, so a caller checks once after encoding. A failed
/// writer's bytes are incomplete and must not be digested or stored. Single-owner; not thread-safe.
class GenerationByteWriter final
{
public:
    /// Append one byte.
    void WriteUInt8(uint8 value) noexcept;
    /// Append two bytes, least significant first.
    void WriteUInt16(uint16 value) noexcept;
    /// Append four bytes, least significant first.
    void WriteUInt32(uint32 value) noexcept;
    /// Append eight bytes, least significant first.
    void WriteUInt64(uint64 value) noexcept;
    /// Append the two's-complement bits as WriteUInt16.
    void WriteInt16(int16 value) noexcept;
    /// Append the two's-complement bits as WriteUInt32.
    void WriteInt32(int32 value) noexcept;
    /// Append the 32 digest bytes in digest order.
    void WriteDigest(const GenerationDigest& digest) noexcept;

    /// Whether an allocation failed; the bytes are then incomplete.
    [[nodiscard]] LUDUS_INLINE bool HasFailed() const noexcept
    {
        return mHasFailed;
    }
    /// The bytes written so far; valid until the next write or Clear.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetBytes() const noexcept
    {
        return {mBytes.GetData(), mBytes.GetSize()};
    }
    /// Digest of the bytes written so far.
    [[nodiscard]] GenerationDigest ComputeDigest() const noexcept;
    /// Remove every byte and the failure latch, keeping capacity.
    void Clear() noexcept;

private:
    template <usize BYTE_COUNT>
    void writeUnsigned(uint64 value) noexcept;

    foundation::Array<uint8> mBytes;
    bool mHasFailed = false;
};

/// @brief Reads fixed-width little-endian fields from borrowed bytes.
///
/// Reading past the end latches HasFailed and returns zero for that and every later field. The caller keeps the
/// bytes alive while the reader is used.
class GenerationByteReader final
{
public:
    /// Read from a borrowed view; an invalid view reads as failed.
    explicit GenerationByteReader(foundation::BorrowedView<const uint8> bytes) noexcept;

    /// Read one byte.
    [[nodiscard]] uint8 ReadUInt8() noexcept;
    /// Read two bytes, least significant first.
    [[nodiscard]] uint16 ReadUInt16() noexcept;
    /// Read four bytes, least significant first.
    [[nodiscard]] uint32 ReadUInt32() noexcept;
    /// Read eight bytes, least significant first.
    [[nodiscard]] uint64 ReadUInt64() noexcept;
    /// Read a two's-complement 16-bit value.
    [[nodiscard]] int16 ReadInt16() noexcept;
    /// Read a two's-complement 32-bit value.
    [[nodiscard]] int32 ReadInt32() noexcept;
    /// Read 32 digest bytes.
    [[nodiscard]] GenerationDigest ReadDigest() noexcept;

    /// Whether a read ran past the end or the view was invalid.
    [[nodiscard]] LUDUS_INLINE bool HasFailed() const noexcept
    {
        return mHasFailed;
    }
    /// Whether every byte was consumed without failure.
    [[nodiscard]] LUDUS_INLINE bool IsAtEnd() const noexcept
    {
        return mHasFailed == false && mOffset == mBytes.GetSize();
    }
    /// Bytes not yet read.
    [[nodiscard]] LUDUS_INLINE usize GetRemainingSize() const noexcept
    {
        return mHasFailed == true ? 0 : mBytes.GetSize() - mOffset;
    }

private:
    [[nodiscard]] uint64 readUnsigned(usize byteCount) noexcept;

    foundation::BorrowedView<const uint8> mBytes;
    usize mOffset = 0;
    bool mHasFailed = false;
};
} // namespace ludus::content::procedural
