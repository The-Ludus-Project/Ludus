#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include "internal/pack_codec.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <span>
#include <string_view>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
// Borrow fuzz input for this invocation; no copy or preallocation based on a
// hostile input count. No owner escapes the invocation's stable byte lifetime.
class BytesFile final : public ProviderFile
{
public:
    explicit BytesFile(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return mBytes.size();
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> output) const noexcept override
    {
        if (offset > mBytes.size())
        {
            return {{Status::InvalidArgument}};
        }
        const usize count = std::min(output.size(), mBytes.size() - static_cast<usize>(offset));
        if (count != 0)
        {
            std::memcpy(output.data(), mBytes.data() + static_cast<usize>(offset), count);
        }
        return {{}, count};
    }

private:
    std::span<const uint8> mBytes;
};
class BytesProvider final : public Provider
{
public:
    explicit BytesProvider(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}
    [[nodiscard]] Result OpenRead(std::string_view, ProviderFile*& output) const noexcept override
    {
        auto* next = new (std::nothrow) BytesFile(mBytes);
        if (next == nullptr)
        {
            return {Status::OutOfMemory};
        }
        output = next;
        return {};
    }

private:
    std::span<const uint8> mBytes;
};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const uint8* data, usize size)
{
    if (size > 65536)
    {
        return 0;
    }
    uint8 repaired[65536];
    if (size != 0)
    {
        std::memcpy(repaired, data, size);
    }
    auto bytes = std::span<const uint8>(repaired, size);
    // Repair only integrity fields so mutations reach table/range admission,
    // while preserving all hostile counts, paths, offsets and codec bytes.
    if (size >= 80)
    {
        uint64 indexSize = 0;
        if (TryReadLittleEndian(MakeBorrowedView(bytes.subspan(40)), indexSize) && indexSize <= size - 80)
        {
            (void)TryWriteLittleEndian(internal::Crc32(bytes.subspan(80, static_cast<usize>(indexSize))),
                                       MakeBorrowedView(std::span(repaired, size).subspan(64, 4)));
        }
        (void)TryWriteLittleEndian(uint32{0}, MakeBorrowedView(std::span(repaired, size).subspan(68, 4)));
        (void)TryWriteLittleEndian(internal::Crc32(bytes.first(80)),
                                   MakeBorrowedView(std::span(repaired, size).subspan(68, 4)));
    }
    uint8 decoded[65536];
    const usize outputSize = size >= 2 ? (static_cast<usize>(data[0]) | (static_cast<usize>(data[1]) << 8)) : 0;
    (void)internal::DecodeLz4(bytes, std::span(decoded).first(outputSize));
    ProviderHandle source(new (std::nothrow) BytesProvider(bytes));
    ProviderHandle pack;
    PackLimits limits;
    limits.MaxPackBytes = 65536;
    limits.MaxIndexBytes = 32768;
    limits.MaxFiles = 256;
    limits.MaxBlocks = 1024;
    limits.MaxFileBytes = 131072;
    limits.MaxDecodedBytes = 1048576;
    if (CreatePackProvider(source, "archive", limits, pack).Succeeded())
    {
        std::string_view key = "compressed";
        uint32 pathSize = 0;
        if (size >= 112 && TryReadLittleEndian(MakeBorrowedView(bytes.subspan(80)), pathSize) && pathSize <= size - 112)
        {
            key = {reinterpret_cast<const char*>(repaired + 112), pathSize};
        }
        ProviderFile* file = nullptr;
        if (pack.Get()->OpenRead(key, file).Succeeded())
        {
            (void)file->ReadAt(0, decoded);
            (void)file->ReadAt(file->Size(), {});
            delete file;
        }
    }
    return 0;
}
