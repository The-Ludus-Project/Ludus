#pragma once

// Sequential byte input for the streaming pbrt tokenizer.

#include <ludus/content/content.h>
#include <ludus/foundation/base/core.h>

#include <span>

namespace ludus::content::scene_cook::importers
{
using foundation::core::uint8;
using foundation::core::usize;

/// Pull-style byte stream; Read fills a prefix of the buffer.
class ByteSource
{
public:
    ByteSource() noexcept = default;
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;
    virtual ~ByteSource() noexcept = default;
    /// Read up to outBuffer.size() bytes; outCount is zero at end of input.
    /// @return Ok, or IoError when the underlying read failed.
    [[nodiscard]] virtual Status Read(std::span<uint8> outBuffer, usize& outCount) noexcept = 0;
};

/// ByteSource over caller-owned memory that must outlive the source.
class MemoryByteSource final : public ByteSource
{
public:
    explicit MemoryByteSource(std::span<const uint8> data) noexcept : mData(data) {}
    [[nodiscard]] Status Read(std::span<uint8> outBuffer, usize& outCount) noexcept override
    {
        const usize remaining = mData.size() - mOffset;
        outCount = remaining < outBuffer.size() ? remaining : outBuffer.size();
        for (usize i = 0; i < outCount; ++i)
        {
            outBuffer[i] = mData[mOffset + i];
        }
        mOffset += outCount;
        return Status::Ok;
    }

private:
    std::span<const uint8> mData;
    usize mOffset = 0;
};

/// ByteSource that forwards another source and hashes every byte it yields, so the
/// digest covers exactly the bytes the reader consumed.
class HashingByteSource final : public ByteSource
{
public:
    explicit HashingByteSource(ByteSource& inoutSource) noexcept : mSource(inoutSource) {}
    [[nodiscard]] Status Read(std::span<uint8> outBuffer, usize& outCount) noexcept override
    {
        const Status status = mSource.Read(outBuffer, outCount);
        if (status == Status::Ok && mHasher.Add(std::span<const uint8>(outBuffer.data(), outCount)) == false)
        {
            return Status::Limit;
        }
        return status;
    }
    /// Hash the bytes the reader left unread (a reader may stop at an error).
    [[nodiscard]] Status Drain() noexcept
    {
        uint8 buffer[4096];
        while (true)
        {
            usize count = 0;
            const Status status = Read(std::span<uint8>(buffer, sizeof(buffer)), count);
            if (status != Status::Ok || count == 0)
            {
                return status;
            }
        }
    }
    [[nodiscard]] Digest GetDigest() const noexcept
    {
        return mHasher.Finish();
    }

private:
    ByteSource& mSource;
    Hasher mHasher;
};
} // namespace ludus::content::scene_cook::importers
