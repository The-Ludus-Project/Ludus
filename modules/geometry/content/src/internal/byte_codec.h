#pragma once

// Private byte codec helpers for Ludus::GeometryContent. A growable little-endian
// writer (like modules/content/scene_cook's ByteWriter) and a bounded reader
// (like modules/runtime/resources SceneDefinition's Reader), both built on the
// bounded FoundationBase byte-order codecs so host endianness and padding never
// leak into the cooked blob. Floats are bit-cast to their unsigned storage and
// written/read through the unsigned codecs.

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <bit>
#include <span>

namespace ludus::geometry::content::internal
{
using namespace ludus::foundation;

/// @brief Growable little-endian byte writer with a sticky overflow flag.
class ByteWriter final
{
public:
    /// @return False once any append has failed to allocate.
    [[nodiscard]] bool IsOk() const noexcept
    {
        return mIsOk;
    }
    /// @return Number of bytes written so far.
    [[nodiscard]] usize GetSize() const noexcept
    {
        return mBytes.GetSize();
    }
    /// @return Writable view of the accumulated bytes (for back-patching).
    [[nodiscard]] std::span<uint8> GetMutableBytes() noexcept
    {
        return {mBytes.GetData(), mBytes.GetSize()};
    }
    /// Transfer the accumulated bytes into the caller's array on success.
    [[nodiscard]] bool Release(core::Array<uint8>& outBytes) noexcept
    {
        if (mIsOk == false)
        {
            return false;
        }
        outBytes = static_cast<core::Array<uint8>&&>(mBytes);
        return true;
    }

    [[nodiscard]] bool WriteUint8(uint8 value) noexcept
    {
        return append(&value, 1);
    }
    [[nodiscard]] bool WriteUint32(uint32 value) noexcept
    {
        uint8 buffer[sizeof(uint32)]{};
        (void)core::TryWriteLittleEndian<uint32>(value, buffer);
        return append(buffer, sizeof(buffer));
    }
    [[nodiscard]] bool WriteUint64(uint64 value) noexcept
    {
        uint8 buffer[sizeof(uint64)]{};
        (void)core::TryWriteLittleEndian<uint64>(value, buffer);
        return append(buffer, sizeof(buffer));
    }
    [[nodiscard]] bool WriteFloat32(float32 value) noexcept
    {
        return WriteUint32(std::bit_cast<uint32>(value));
    }
    [[nodiscard]] bool WriteFloat64(float64 value) noexcept
    {
        return WriteUint64(std::bit_cast<uint64>(value));
    }
    [[nodiscard]] bool WriteBytes(std::span<const uint8> bytes) noexcept
    {
        return append(bytes.data(), bytes.size());
    }

private:
    [[nodiscard]] bool append(const uint8* dataOrNull, usize count) noexcept
    {
        if (mIsOk == false)
        {
            return false;
        }
        const usize start = mBytes.GetSize();
        if (mBytes.TryResize(start + count) == false)
        {
            mIsOk = false;
            return false;
        }
        for (usize i = 0; i < count; ++i)
        {
            mBytes[start + i] = dataOrNull[i];
        }
        return true;
    }

    core::Array<uint8> mBytes;
    bool mIsOk = true;
};

/// @brief Bounded little-endian reader over a borrowed byte span.
///
/// Every read is bounds-checked; a short read leaves the output unchanged and
/// returns false. Float reads additionally reject non-finite values.
class ByteReader final
{
public:
    explicit ByteReader(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}

    [[nodiscard]] usize GetOffset() const noexcept
    {
        return mOffset;
    }
    [[nodiscard]] usize GetRemaining() const noexcept
    {
        return mBytes.size() - mOffset;
    }
    /// Move the cursor to an absolute offset; false if out of range.
    [[nodiscard]] bool SeekTo(usize offset) noexcept
    {
        if (offset > mBytes.size())
        {
            return false;
        }
        mOffset = offset;
        return true;
    }
    [[nodiscard]] bool ReadUint8(uint8& out) noexcept
    {
        if (GetRemaining() < 1)
        {
            return false;
        }
        out = mBytes[mOffset];
        mOffset += 1;
        return true;
    }
    [[nodiscard]] bool ReadUint32(uint32& out) noexcept
    {
        uint32 value = 0;
        if (core::TryReadLittleEndian<uint32>(mBytes.subspan(mOffset), value) == false)
        {
            return false;
        }
        mOffset += sizeof(uint32);
        out = value;
        return true;
    }
    [[nodiscard]] bool ReadUint64(uint64& out) noexcept
    {
        uint64 value = 0;
        if (core::TryReadLittleEndian<uint64>(mBytes.subspan(mOffset), value) == false)
        {
            return false;
        }
        mOffset += sizeof(uint64);
        out = value;
        return true;
    }
    [[nodiscard]] bool ReadFloat32(float32& out) noexcept
    {
        uint32 bits = 0;
        if (ReadUint32(bits) == false)
        {
            return false;
        }
        const float32 value = std::bit_cast<float32>(bits);
        if (math::IsFinite(value) == false)
        {
            return false;
        }
        out = value;
        return true;
    }
    [[nodiscard]] bool ReadFloat64(float64& out) noexcept
    {
        uint64 bits = 0;
        if (ReadUint64(bits) == false)
        {
            return false;
        }
        const float64 value = std::bit_cast<float64>(bits);
        if (math::IsFinite(value) == false)
        {
            return false;
        }
        out = value;
        return true;
    }
    [[nodiscard]] bool ReadBytes(std::span<uint8> out) noexcept
    {
        if (GetRemaining() < out.size())
        {
            return false;
        }
        for (usize i = 0; i < out.size(); ++i)
        {
            out[i] = mBytes[mOffset + i];
        }
        mOffset += out.size();
        return true;
    }

private:
    std::span<const uint8> mBytes;
    usize mOffset = 0;
};
} // namespace ludus::geometry::content::internal
