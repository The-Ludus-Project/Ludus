#include <ludus/content/procedural/core/generation_bytes.h>

namespace ludus::content::procedural
{
namespace
{
constexpr uint32 BITS_PER_BYTE = 8;
constexpr usize UINT16_BYTES = 2;
constexpr usize UINT32_BYTES = 4;
constexpr usize UINT64_BYTES = 8;
constexpr uint64 BYTE_MASK = 0xFFU;
} // namespace

bool GenerationDigestLess::operator()(const GenerationDigest& left, const GenerationDigest& right) const noexcept
{
    for (usize i = 0; i < GENERATION_DIGEST_BYTES; ++i)
    {
        if (left.Data[i] != right.Data[i])
        {
            return left.Data[i] < right.Data[i];
        }
    }
    return false;
}

GenerationDigest ComputeGenerationDigest(foundation::BorrowedView<const uint8> bytes) noexcept
{
    if (bytes.IsValid() == false)
    {
        return content::Hash({});
    }
    return content::Hash({bytes.GetDataOrNull(), bytes.GetSize()});
}

template <usize BYTE_COUNT>
void GenerationByteWriter::writeUnsigned(uint64 value) noexcept
{
    if (mHasFailed == true)
    {
        return;
    }
    if (mBytes.TryEnsureCapacity(mBytes.GetSize() + BYTE_COUNT) == false)
    {
        mHasFailed = true;
        return;
    }
    for (usize i = 0; i < BYTE_COUNT; ++i)
    {
        mBytes.Add(static_cast<uint8>((value >> (i * BITS_PER_BYTE)) & BYTE_MASK));
    }
}

void GenerationByteWriter::WriteUInt8(uint8 value) noexcept
{
    writeUnsigned<1>(value);
}

void GenerationByteWriter::WriteUInt16(uint16 value) noexcept
{
    writeUnsigned<UINT16_BYTES>(value);
}

void GenerationByteWriter::WriteUInt32(uint32 value) noexcept
{
    writeUnsigned<UINT32_BYTES>(value);
}

void GenerationByteWriter::WriteUInt64(uint64 value) noexcept
{
    writeUnsigned<UINT64_BYTES>(value);
}

void GenerationByteWriter::WriteInt16(int16 value) noexcept
{
    writeUnsigned<UINT16_BYTES>(static_cast<uint16>(value));
}

void GenerationByteWriter::WriteInt32(int32 value) noexcept
{
    writeUnsigned<UINT32_BYTES>(static_cast<uint32>(value));
}

void GenerationByteWriter::WriteDigest(const GenerationDigest& digest) noexcept
{
    for (const uint8 byte : digest.Data)
    {
        writeUnsigned<1>(byte);
    }
}

GenerationDigest GenerationByteWriter::ComputeDigest() const noexcept
{
    return ComputeGenerationDigest(GetBytes());
}

void GenerationByteWriter::Clear() noexcept
{
    mBytes.Clear();
    mHasFailed = false;
}

GenerationByteReader::GenerationByteReader(foundation::BorrowedView<const uint8> bytes) noexcept
    : mBytes(bytes), mHasFailed(bytes.IsValid() == false)
{
}

uint64 GenerationByteReader::readUnsigned(usize byteCount) noexcept
{
    if (mHasFailed == true || mBytes.GetSize() - mOffset < byteCount)
    {
        mHasFailed = true;
        return 0;
    }
    uint64 value = 0;
    for (usize i = 0; i < byteCount; ++i)
    {
        value |= static_cast<uint64>(mBytes[mOffset + i]) << (i * BITS_PER_BYTE);
    }
    mOffset += byteCount;
    return value;
}

uint8 GenerationByteReader::ReadUInt8() noexcept
{
    return static_cast<uint8>(readUnsigned(1));
}

uint16 GenerationByteReader::ReadUInt16() noexcept
{
    return static_cast<uint16>(readUnsigned(UINT16_BYTES));
}

uint32 GenerationByteReader::ReadUInt32() noexcept
{
    return static_cast<uint32>(readUnsigned(UINT32_BYTES));
}

uint64 GenerationByteReader::ReadUInt64() noexcept
{
    return readUnsigned(UINT64_BYTES);
}

int16 GenerationByteReader::ReadInt16() noexcept
{
    return static_cast<int16>(ReadUInt16());
}

int32 GenerationByteReader::ReadInt32() noexcept
{
    return static_cast<int32>(ReadUInt32());
}

GenerationDigest GenerationByteReader::ReadDigest() noexcept
{
    GenerationDigest digest;
    for (uint8& byte : digest.Data)
    {
        byte = static_cast<uint8>(readUnsigned(1));
    }
    return mHasFailed == true ? GenerationDigest{} : digest;
}
} // namespace ludus::content::procedural
