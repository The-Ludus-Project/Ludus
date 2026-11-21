#pragma once

// Little-endian writer for the canonical byte layouts hashed into skeleton compatibility
// identities and clip fingerprints. Private to the module; never installed.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>

#include <bit>
#include <span>

namespace ludus::animation::detail
{
/// Writer over a buffer sized exactly for one canonical layout; every write must fit.
class CanonicalByteWriter final
{
public:
    explicit CanonicalByteWriter(std::span<foundation::uint8> bytes) noexcept : mBytes(bytes) {}

    void WriteUint8(foundation::uint8 value) noexcept
    {
        LUDUS_ASSERT(mOffset < mBytes.size());
        mBytes[mOffset] = value;
        ++mOffset;
    }

    void WriteUint32(foundation::uint32 value) noexcept
    {
        [[maybe_unused]] const bool isWritten = foundation::TryWriteLittleEndian(value, mBytes.subspan(mOffset));
        LUDUS_ASSERT(isWritten == true);
        mOffset += sizeof(foundation::uint32);
    }

    void WriteUint64(foundation::uint64 value) noexcept
    {
        [[maybe_unused]] const bool isWritten = foundation::TryWriteLittleEndian(value, mBytes.subspan(mOffset));
        LUDUS_ASSERT(isWritten == true);
        mOffset += sizeof(foundation::uint64);
    }

    void WriteFloat32(foundation::float32 value) noexcept
    {
        // Positive and negative zero describe the same value; hash one spelling.
        const foundation::float32 canonical = value == 0 ? 0.0F : value;
        WriteUint32(std::bit_cast<foundation::uint32>(canonical));
    }

    void WriteFloat64(foundation::float64 value) noexcept
    {
        const foundation::float64 canonical = value == 0 ? 0.0 : value;
        WriteUint64(std::bit_cast<foundation::uint64>(canonical));
    }

    [[nodiscard]] foundation::usize GetOffset() const noexcept
    {
        return mOffset;
    }

private:
    std::span<foundation::uint8> mBytes;
    foundation::usize mOffset = 0;
};
} // namespace ludus::animation::detail
