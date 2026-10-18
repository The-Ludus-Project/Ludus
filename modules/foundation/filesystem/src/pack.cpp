#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include "internal/pack_codec.hpp"
#if defined(LUDUS_FILESYSTEM_PACK_FAULT_TESTING)
#    include "internal/pack_test_hooks.hpp"
#endif

#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <string_view>

// Thanks to Bruno Sousa, "File Management Using Resource Files", Game
// Programming Gems 2, sec. 1.15, pp. 100-104: versioned per-entry metadata
// inspires this validated container boundary. Thanks to David L. Koenig,
// "Faster File Loading with Access-Based File Reordering", Game Programming
// Gems 6, sec. 1.9, pp. 103-108: payload placement is independent of sorted
// lookup. Original format/implementation, no article code copied; see
// docs/architecture/filesystem.md, "Contracts for F3".

namespace ludus::foundation::filesystem
{
namespace
{
constexpr usize HEADER_BYTES = 80;
constexpr usize ENTRY_BYTES = 32;
constexpr usize BLOCK_BYTES = 24;
constexpr uint8 MAGIC[]{'L', 'U', 'D', 'P', 'A', 'C', 'K', 0};

template <typename T, typename... Args>
T* AllocateObject(Args&&... args) noexcept
{
#if defined(LUDUS_FILESYSTEM_PACK_FAULT_TESTING)
    if (test::PackAllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T(Forward<Args>(args)...);
}
template <typename T>
T* AllocateArray(usize count) noexcept
{
    usize bytes = 0;
    isize admitted = 0;
    if (TryMultiply(count, sizeof(T), bytes) == false || TryIntegerCast(bytes, admitted) == false)
    {
        return nullptr;
    }
#if defined(LUDUS_FILESYSTEM_PACK_FAULT_TESTING)
    if (test::PackAllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T[count];
}
struct Reader final
{
    std::span<const uint8> Bytes;
    usize Position = 0;
    template <typename T>
    bool Read(T& output) noexcept
    {
        if (TryReadLittleEndian(Bytes.subspan(Position), output) == false)
        {
            return false;
        }
        Position += sizeof(T);
        return true;
    }
    bool Path(uint32 length, std::string_view& output) noexcept
    {
        if (length > Bytes.size() - Position)
        {
            return false;
        }
        output = {reinterpret_cast<const char*>(Bytes.data() + Position), length};
        Position += length;
        return ValidPath(output);
    }
};
struct Header final
{
    uint32 BlockSize = 0;
    uint32 Files = 0;
    uint32 Blocks = 0;
    uint64 IndexSize = 0;
    uint64 Payload = 0;
    uint64 Size = 0;
    uint32 IndexCrc = 0;
};
Result ReadExact(const ProviderFile& source, uint64 offset, std::span<uint8> destination) noexcept
{
    const auto result = source.ReadAt(offset, destination);
    if (result.Outcome.Succeeded() == false)
    {
        return result.Outcome;
    }
    return result.BytesRead == destination.size() ? Result{} : Result{Status::CorruptData};
}
Result ParseHeader(std::span<uint8> bytes, const PackLimits& limits, uint64 archiveSize, Header& header) noexcept
{
    if (std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
    {
        return {Status::CorruptData};
    }
    Reader reader{bytes, 8};
    uint32 version = 0, headerSize = 0, flags = 0, checksum = 0;
    uint64 indexOffset = 0, reserved = 0;
    if (reader.Read(version) == false || reader.Read(headerSize) == false || reader.Read(flags) == false ||
        reader.Read(header.BlockSize) == false || reader.Read(header.Files) == false ||
        reader.Read(header.Blocks) == false || reader.Read(indexOffset) == false ||
        reader.Read(header.IndexSize) == false || reader.Read(header.Payload) == false ||
        reader.Read(header.Size) == false || reader.Read(header.IndexCrc) == false || reader.Read(checksum) == false ||
        reader.Read(reserved) == false)
    {
        return {Status::CorruptData};
    }
    // Validate the header CRC before trusting feature dispatch or admission sizes.
    (void)TryWriteLittleEndian(uint32{0}, bytes.subspan(68, 4));
    if (internal::Crc32(bytes) != checksum)
    {
        return {Status::CorruptData};
    }
    if (version != 1 || flags != 0 || headerSize != HEADER_BYTES)
    {
        return {Status::Unsupported};
    }
    if (reserved != 0 || indexOffset != HEADER_BYTES || header.Size != archiveSize || header.BlockSize < 256 ||
        header.BlockSize > MAX_PACK_BLOCK_BYTES || (header.BlockSize & (header.BlockSize - 1)) != 0 ||
        header.Payload < HEADER_BYTES || header.Payload > header.Size ||
        header.IndexSize != header.Payload - HEADER_BYTES)
    {
        return {Status::CorruptData};
    }
    if (header.Files > limits.MaxFiles || header.Blocks > limits.MaxBlocks || header.IndexSize > limits.MaxIndexBytes ||
        header.BlockSize > limits.MaxBlockBytes)
    {
        return {Status::LimitExceeded};
    }
    // Prove minimum serialized table sizes before allocating native entry arrays.
    const uint64 minimum =
        static_cast<uint64>(header.Files) * (ENTRY_BYTES + 1) + static_cast<uint64>(header.Blocks) * BLOCK_BYTES;
    if (minimum > header.IndexSize)
    {
        return {Status::CorruptData};
    }
    return {};
}
struct Entry final
{
    std::string_view Path;
    uint32 First = 0;
    uint32 Count = 0;
    uint64 Decoded = 0;
    uint64 Stored = 0;
};
struct Block final
{
    uint64 Offset = 0;
    uint32 Stored = 0;
    uint32 Decoded = 0;
    uint32 Codec = 0;
    uint32 Crc = 0;
};
struct Range final
{
    uint64 Offset = 0;
    uint32 Length = 0;
};
class PackProvider;
class PackFile final : public ProviderFile
{
public:
    PackFile(const PackProvider& source, const Entry& entry) noexcept;
    ~PackFile() noexcept override
    {
        delete[] Scratch;
    }
    uint8* Scratch = nullptr;
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return mEntry.Decoded;
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept override;

private:
    ProviderHandle mOwner;
    const PackProvider& mSource;
    const Entry& mEntry;
    mutable std::mutex mMutex;
};
class PackProvider final : public Provider
{
public:
    ProviderFile* Archive = nullptr;
    uint8* Index = nullptr;
    Entry* Entries = nullptr;
    Block* Blocks = nullptr;
    Header Metadata;
    ~PackProvider() noexcept override
    {
        delete Archive;
        delete[] Index;
        delete[] Entries;
        delete[] Blocks;
    }
    [[nodiscard]] Result OpenRead(std::string_view path, ProviderFile*& output) const noexcept override
    {
        if (ValidPath(path) == false)
        {
            return {Status::InvalidArgument};
        }
        const auto revision = ReadExact(*Archive, 0, {});
        if (revision.Succeeded() == false)
        {
            return revision;
        }
        usize begin = 0, end = Metadata.Files;
        while (begin < end)
        {
            const usize middle = begin + (end - begin) / 2;
            if (Entries[middle].Path < path)
            {
                begin = middle + 1;
            }
            else
            {
                end = middle;
            }
        }
        if (begin == Metadata.Files || Entries[begin].Path != path)
        {
            return {Status::NotFound};
        }
        auto* next = AllocateObject<PackFile>(*this, Entries[begin]);
        if (next == nullptr)
        {
            return {Status::OutOfMemory};
        }
        if (Entries[begin].Count != 0)
        {
            next->Scratch = AllocateArray<uint8>(static_cast<usize>(Metadata.BlockSize) * 2);
            if (next->Scratch == nullptr)
            {
                delete next;
                return {Status::OutOfMemory};
            }
        }
        output = next;
        return {};
    }
};
PackFile::PackFile(const PackProvider& source, const Entry& entry) noexcept
    : mOwner(ProviderHandle::Share(source)), mSource(source), mEntry(entry)
{
}
ReadResult PackFile::ReadAt(uint64 offset, std::span<uint8> destination) const noexcept
{
    if (offset > mEntry.Decoded)
    {
        return {{Status::InvalidArgument}};
    }
    const std::lock_guard lock(mMutex);
    auto result = ReadExact(*mSource.Archive, 0, {});
    if (result.Succeeded() == false)
    {
        return {result};
    }
    usize requested = destination.size();
    const uint64 remaining = mEntry.Decoded - offset;
    if (remaining < requested)
    {
        requested = static_cast<usize>(remaining);
    }
    usize progress = 0;
    const uint64 blockSize = mSource.Metadata.BlockSize;
    while (progress < requested)
    {
        const uint64 logical = offset + progress; // Bounded by captured mEntry.Decoded.
        const auto relativeBlock = static_cast<uint32>(logical / blockSize);
        const auto& block = mSource.Blocks[mEntry.First + relativeBlock];
        const usize within = static_cast<usize>(logical % blockSize);
        auto stored = std::span(Scratch, block.Stored);
        auto decoded = std::span(Scratch + mSource.Metadata.BlockSize, block.Decoded);
        result = ReadExact(*mSource.Archive, block.Offset, stored);
        if (result.Succeeded() == false)
        {
            return {result, result.Code == Status::Changed ? 0 : progress};
        }
        if (block.Codec == 0)
        {
            std::memcpy(decoded.data(), stored.data(), stored.size());
        }
        else if (internal::DecodeLz4(stored, decoded) == false)
        {
            return {{Status::CorruptData}, progress};
        }
        if (internal::Crc32(decoded) != block.Crc)
        {
            return {{Status::CorruptData}, progress};
        }
        const usize count =
            (requested - progress < decoded.size() - within ? requested - progress : decoded.size() - within);
        std::memcpy(destination.data() + progress, decoded.data() + within, count);
        progress += count;
    }
    result = ReadExact(*mSource.Archive, 0, {});
    return {result, result.Code == Status::Changed ? 0 : progress};
}
Result ParseIndex(PackProvider& provider, usize indexSize, const PackLimits& limits) noexcept
{
    Reader reader{{provider.Index, indexSize}};
    const auto& header = provider.Metadata;
    uint32 nextBlock = 0;
    uint64 totalDecoded = 0;
    for (usize i = 0; i < header.Files; ++i)
    {
        auto& entry = provider.Entries[i];
        uint32 pathSize = 0, reserved = 0;
        if (reader.Read(pathSize) == false || reader.Read(entry.First) == false || reader.Read(entry.Count) == false ||
            reader.Read(reserved) == false || reader.Read(entry.Decoded) == false ||
            reader.Read(entry.Stored) == false || reader.Path(pathSize, entry.Path) == false || reserved != 0 ||
            entry.First != nextBlock || entry.Count > header.Blocks - nextBlock ||
            (i != 0 && provider.Entries[i - 1].Path >= entry.Path))
        {
            return {Status::CorruptData};
        }
        const uint64 expected = entry.Decoded / header.BlockSize + (entry.Decoded % header.BlockSize != 0 ? 1 : 0);
        if (expected != entry.Count || (entry.Count == 0 && entry.Stored != 0) ||
            TryAdd(totalDecoded, entry.Decoded, totalDecoded) == false)
        {
            return {Status::CorruptData};
        }
        if (entry.Decoded > limits.MaxFileBytes || totalDecoded > limits.MaxDecodedBytes)
        {
            return {Status::LimitExceeded};
        }
        nextBlock += entry.Count;
    }
    if (nextBlock != header.Blocks || indexSize - reader.Position != static_cast<uint64>(header.Blocks) * BLOCK_BYTES)
    {
        return {Status::CorruptData};
    }
    for (usize i = 0; i < header.Blocks; ++i)
    {
        auto& block = provider.Blocks[i];
        if (reader.Read(block.Offset) == false || reader.Read(block.Stored) == false ||
            reader.Read(block.Decoded) == false || reader.Read(block.Codec) == false || reader.Read(block.Crc) == false)
        {
            return {Status::CorruptData};
        }
        if (block.Codec > 1)
        {
            return {Status::Unsupported};
        }
        if (block.Stored == 0 || block.Decoded == 0 || block.Stored > header.BlockSize ||
            block.Decoded > header.BlockSize || block.Offset < header.Payload || block.Offset > header.Size ||
            block.Stored > header.Size - block.Offset || (block.Codec == 0 && block.Stored != block.Decoded) ||
            (block.Codec == 1 && block.Stored >= block.Decoded))
        {
            return {Status::CorruptData};
        }
    }
    for (usize i = 0; i < header.Files; ++i)
    {
        const auto& entry = provider.Entries[i];
        uint64 stored = 0, decoded = 0;
        for (uint32 j = 0; j < entry.Count; ++j)
        {
            const auto& block = provider.Blocks[entry.First + j];
            const uint64 expected =
                (static_cast<uint64>(header.BlockSize) < entry.Decoded - decoded ? static_cast<uint64>(header.BlockSize)
                                                                                 : entry.Decoded - decoded);
            if (block.Decoded != expected || TryAdd(stored, static_cast<uint64>(block.Stored), stored) == false)
            {
                return {Status::CorruptData};
            }
            decoded += block.Decoded;
        }
        if (stored != entry.Stored || decoded != entry.Decoded)
        {
            return {Status::CorruptData};
        }
    }
    if (header.Blocks == 0)
    {
        return header.Payload == header.Size ? Result{} : Result{Status::CorruptData};
    }
    auto* ranges = AllocateArray<Range>(header.Blocks);
    if (ranges == nullptr)
    {
        return {Status::OutOfMemory};
    }
    for (usize i = 0; i < header.Blocks; ++i)
    {
        ranges[i] = {provider.Blocks[i].Offset, provider.Blocks[i].Stored};
    }
    foundation::Sort(ranges, header.Blocks, [](const Range& a, const Range& b) noexcept {
        return a.Offset < b.Offset;
    });
    uint64 end = header.Payload;
    bool valid = true;
    for (usize i = 0; i < header.Blocks; ++i)
    {
        valid = valid && ranges[i].Offset == end;
        end = ranges[i].Offset + ranges[i].Length; // Already proved <= archive size.
    }
    valid = valid && end == header.Size;
    delete[] ranges;
    return valid ? Result{} : Result{Status::CorruptData};
}
} // namespace
Result CreatePackProvider(const ProviderHandle& storage,
                          std::string_view relativePath,
                          const PackLimits& limits,
                          ProviderHandle& output) noexcept
{
    if (storage.Get() == nullptr || ValidPath(relativePath) == false || limits.MaxBlockBytes == 0 ||
        limits.MaxBlockBytes > MAX_PACK_BLOCK_BYTES)
    {
        return {Status::InvalidArgument};
    }
    auto* next = AllocateObject<PackProvider>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    ProviderHandle owner(next);
    auto result = storage.Get()->OpenRead(relativePath, next->Archive);
    if (result.Succeeded() == false)
    {
        return result;
    }
    if (next->Archive == nullptr)
    {
        return {Status::IoError};
    }
    const uint64 archiveSize = next->Archive->Size();
    if (archiveSize > limits.MaxPackBytes)
    {
        return {Status::LimitExceeded};
    }
    if (archiveSize < HEADER_BYTES)
    {
        return {Status::CorruptData};
    }
    uint8 bytes[HEADER_BYTES]{};
    result = ReadExact(*next->Archive, 0, bytes);
    if (result.Succeeded() == false)
    {
        return result;
    }
    result = ParseHeader(bytes, limits, archiveSize, next->Metadata);
    if (result.Succeeded() == false)
    {
        return result;
    }
    usize indexSize = 0;
    isize admitted = 0;
    if (TryIntegerCast(next->Metadata.IndexSize, indexSize) == false || TryIntegerCast(indexSize, admitted) == false)
    {
        return {Status::LimitExceeded};
    }
    if (indexSize != 0)
    {
        next->Index = AllocateArray<uint8>(indexSize);
        if (next->Index == nullptr)
        {
            return {Status::OutOfMemory};
        }
    }
    result = ReadExact(*next->Archive, HEADER_BYTES, {next->Index, indexSize});
    if (result.Succeeded() == false)
    {
        return result;
    }
    if (internal::Crc32({next->Index, indexSize}) != next->Metadata.IndexCrc)
    {
        return {Status::CorruptData};
    }
    if (next->Metadata.Files != 0)
    {
        next->Entries = AllocateArray<Entry>(next->Metadata.Files);
        if (next->Entries == nullptr)
        {
            return {Status::OutOfMemory};
        }
    }
    if (next->Metadata.Blocks != 0)
    {
        next->Blocks = AllocateArray<Block>(next->Metadata.Blocks);
        if (next->Blocks == nullptr)
        {
            return {Status::OutOfMemory};
        }
    }
    result = ParseIndex(*next, indexSize, limits);
    if (result.Succeeded() == false)
    {
        return result;
    }
    // Ensure an ordinary native mutation during metadata validation is visible.
    result = ReadExact(*next->Archive, 0, {});
    if (result.Succeeded() == false)
    {
        return result;
    }
    output = Move(owner);
    return {};
}
} // namespace ludus::foundation::filesystem
