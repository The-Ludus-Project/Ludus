#include <ludus/content/procedural/core/generation_cache.h>

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::content::procedural
{
namespace
{
/// "LGCE" read as a little-endian word: Ludus generation cache entry.
constexpr uint32 GENERATION_CACHE_ENTRY_MAGIC = 0x4543474CU;

// Frames a payload: magic, entry version, key, schema, payload length, payload digest, payload.
[[nodiscard]] bool TryFrameEntry(const GenerationDigest& key,
                                 uint32 schema,
                                 foundation::BorrowedView<const uint8> payload,
                                 GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(GENERATION_CACHE_ENTRY_MAGIC);
    outWriter.WriteUInt32(GENERATION_CACHE_ENTRY_VERSION);
    outWriter.WriteDigest(key);
    outWriter.WriteUInt32(schema);
    outWriter.WriteUInt64(payload.GetSize());
    outWriter.WriteDigest(ComputeGenerationDigest(payload));
    for (const uint8 byte : payload)
    {
        outWriter.WriteUInt8(byte);
    }
    return outWriter.HasFailed() == false;
}
} // namespace

MemoryGenerationCacheStore::MemoryGenerationCacheStore(GenerationCacheLimits limits) noexcept : mLimits(limits) {}

GenerationStoreRead MemoryGenerationCacheStore::TryRead(const GenerationDigest& key,
                                                        foundation::Array<uint8>& outBytes) noexcept
{
    const foundation::Array<uint8>* entryOrNull = mEntries.Find(key);
    if (entryOrNull == nullptr)
    {
        return GenerationStoreRead::Absent;
    }
    foundation::Array<uint8> bytes;
    if (bytes.TryAddRange({entryOrNull->GetData(), entryOrNull->GetSize()}) == false)
    {
        return GenerationStoreRead::Failed;
    }
    outBytes = foundation::Move(bytes);
    return GenerationStoreRead::Found;
}

bool MemoryGenerationCacheStore::TryWrite(const GenerationDigest& key,
                                          foundation::BorrowedView<const uint8> bytes) noexcept
{
    if (bytes.IsValid() == false)
    {
        return false;
    }
    const foundation::Array<uint8>* existingOrNull = mEntries.Find(key);
    const usize retainedBytes = mByteCount - (existingOrNull == nullptr ? 0 : existingOrNull->GetSize());
    usize totalBytes = 0;
    const usize totalEntries = mEntries.GetSize() + (existingOrNull == nullptr ? 1 : 0);
    if (foundation::core::TryAdd(retainedBytes, bytes.GetSize(), totalBytes) == false ||
        totalBytes > mLimits.MaximumBytes || totalEntries > mLimits.MaximumEntries)
    {
        return false;
    }
    foundation::Array<uint8> copy;
    if (copy.TryAddRange(bytes) == false)
    {
        return false;
    }
    if (existingOrNull != nullptr)
    {
        *mEntries.Find(key) = foundation::Move(copy);
    }
    else if (mEntries.TryAddInPlace(key, foundation::Move(copy)).Value == nullptr)
    {
        return false;
    }
    mByteCount = totalBytes;
    return true;
}

void MemoryGenerationCacheStore::Remove(const GenerationDigest& key) noexcept
{
    const foundation::Array<uint8>* entryOrNull = mEntries.Find(key);
    if (entryOrNull != nullptr)
    {
        mByteCount -= entryOrNull->GetSize();
        static_cast<void>(mEntries.Remove(key));
    }
}

foundation::BorrowedView<uint8> MemoryGenerationCacheStore::FindEntryBytes(const GenerationDigest& key) noexcept
{
    foundation::Array<uint8>* entryOrNull = mEntries.Find(key);
    if (entryOrNull == nullptr)
    {
        return {};
    }
    return {entryOrNull->GetData(), entryOrNull->GetSize()};
}

GenerationCacheOutcome TryFindGenerationProduct(GenerationCacheStore& inoutStore,
                                                const GenerationDigest& key,
                                                uint32 schema,
                                                foundation::Array<uint8>& outPayload) noexcept
{
    foundation::Array<uint8> entry;
    const GenerationStoreRead read = inoutStore.TryRead(key, entry);
    if (read == GenerationStoreRead::Absent)
    {
        return GenerationCacheOutcome::Absent;
    }
    if (read == GenerationStoreRead::Failed)
    {
        return GenerationCacheOutcome::ReadFailed;
    }
    GenerationByteReader reader({entry.GetData(), entry.GetSize()});
    const uint32 magic = reader.ReadUInt32();
    const uint32 version = reader.ReadUInt32();
    const GenerationDigest storedKey = reader.ReadDigest();
    const uint32 storedSchema = reader.ReadUInt32();
    const uint64 payloadSize = reader.ReadUInt64();
    const GenerationDigest payloadDigest = reader.ReadDigest();
    if (reader.HasFailed() == true || magic != GENERATION_CACHE_ENTRY_MAGIC ||
        version != GENERATION_CACHE_ENTRY_VERSION || storedKey != key || payloadSize != reader.GetRemainingSize())
    {
        inoutStore.Remove(key);
        return GenerationCacheOutcome::Corrupt;
    }
    const usize payloadOffset = entry.GetSize() - static_cast<usize>(payloadSize);
    const foundation::BorrowedView<const uint8> payload{entry.GetData() + payloadOffset,
                                                        static_cast<usize>(payloadSize)};
    if (ComputeGenerationDigest(payload) != payloadDigest)
    {
        inoutStore.Remove(key);
        return GenerationCacheOutcome::Corrupt;
    }
    if (storedSchema != schema)
    {
        inoutStore.Remove(key);
        return GenerationCacheOutcome::SchemaMismatch;
    }
    foundation::Array<uint8> result;
    if (result.TryAddRange(payload) == false)
    {
        return GenerationCacheOutcome::ReadFailed;
    }
    outPayload = foundation::Move(result);
    return GenerationCacheOutcome::Hit;
}

bool TryStoreGenerationProduct(GenerationCacheStore& inoutStore,
                               const GenerationDigest& key,
                               uint32 schema,
                               foundation::BorrowedView<const uint8> payload) noexcept
{
    if (payload.IsValid() == false)
    {
        return false;
    }
    GenerationByteWriter writer;
    if (TryFrameEntry(key, schema, payload, writer) == false)
    {
        return false;
    }
    return inoutStore.TryWrite(key, writer.GetBytes());
}
} // namespace ludus::content::procedural
