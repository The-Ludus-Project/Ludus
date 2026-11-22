#include <ludus/content/procedural/storage/file_generation_cache.h>

#include <ludus/content/content.h>

#include <span>

namespace ludus::content::procedural
{
namespace
{
using foundation::Array;
using foundation::BorrowedView;
using foundation::StringStatus;

/// "LGCF" read as a little-endian word: Ludus generation cache file record.
constexpr uint32 FILE_GENERATION_CACHE_RECORD_MAGIC = 0x4643474CU;
/// Bytes a record digest covers: every field before the trailing digest.
constexpr usize RECORD_FIELD_BYTES = FILE_GENERATION_CACHE_RECORD_BYTES - GENERATION_DIGEST_BYTES;
/// Hexadecimal characters of one digest.
constexpr usize DIGEST_HEX_BYTES = 2 * GENERATION_DIGEST_BYTES;
/// Bits in one hexadecimal digit.
constexpr uint32 NIBBLE_BITS = 4;
/// Mask of the low hexadecimal digit of a byte.
constexpr uint8 NIBBLE_MASK = 0x0FU;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr std::string_view RECORD_SUFFIX = ".entry";
constexpr std::string_view OBJECT_SEPARATOR = "-";
constexpr std::string_view OBJECT_SUFFIX = ".object";

static_assert(DIGEST_HEX_BYTES + RECORD_SUFFIX.size() <= MAX_FILE_GENERATION_CACHE_NAME_BYTES);
static_assert((2 * DIGEST_HEX_BYTES) + OBJECT_SEPARATOR.size() + OBJECT_SUFFIX.size() <=
              MAX_FILE_GENERATION_CACHE_NAME_BYTES);
static_assert(FILE_GENERATION_CACHE_RECORD_BYTES == sizeof(uint32) + sizeof(uint32) + GENERATION_DIGEST_BYTES +
                                                        sizeof(uint8) + sizeof(uint64) + GENERATION_DIGEST_BYTES +
                                                        GENERATION_DIGEST_BYTES);

/// Whether a record publishes an entry or records its removal.
enum class RecordState : uint8
{
    Removed,
    Complete,
};

/// The fields of a completion record.
struct Record final
{
    GenerationDigest Key;
    RecordState State = RecordState::Removed;
    uint64 ObjectLength = 0;
    GenerationDigest Object;
};

void AppendHex(const GenerationDigest& digest, FileGenerationCacheName& inoutName) noexcept
{
    for (const uint8 byte : digest.Data)
    {
        const usize high = byte >> NIBBLE_BITS;
        const usize low = byte & NIBBLE_MASK;
        // The name capacity covers two digests and a suffix (static_asserts above), so an append cannot fail.
        static_cast<void>(inoutName.TryAppend(HEX_DIGITS.substr(high, 1)));
        static_cast<void>(inoutName.TryAppend(HEX_DIGITS.substr(low, 1)));
    }
}

[[nodiscard]] std::span<const uint8> ToSpan(BorrowedView<const uint8> bytes) noexcept
{
    return {bytes.GetDataOrNull(), bytes.GetSize()};
}

[[nodiscard]] GenerationDigest HashBytes(const Bytes& bytes) noexcept
{
    return Hash(bytes.Data());
}

[[nodiscard]] bool TryWriteRecord(const Record& record, GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(FILE_GENERATION_CACHE_RECORD_MAGIC);
    outWriter.WriteUInt32(FILE_GENERATION_CACHE_VERSION);
    outWriter.WriteDigest(record.Key);
    outWriter.WriteUInt8(static_cast<uint8>(record.State));
    outWriter.WriteUInt64(record.ObjectLength);
    outWriter.WriteDigest(record.Object);
    outWriter.WriteDigest(outWriter.ComputeDigest());
    return outWriter.HasFailed() == false;
}

/// Parse and verify a record: exact size, magic, version, the expected key, a known state and its own digest.
[[nodiscard]] bool TryReadRecord(const Bytes& bytes, const GenerationDigest& key, Record& outRecord) noexcept
{
    const std::span<const uint8> data = bytes.Data();
    if (data.size() != FILE_GENERATION_CACHE_RECORD_BYTES ||
        Hash(data.first(RECORD_FIELD_BYTES)) !=
            GenerationByteReader({data.data() + RECORD_FIELD_BYTES, GENERATION_DIGEST_BYTES}).ReadDigest())
    {
        return false;
    }
    GenerationByteReader reader({data.data(), RECORD_FIELD_BYTES});
    const uint32 magic = reader.ReadUInt32();
    const uint32 version = reader.ReadUInt32();
    Record record;
    record.Key = reader.ReadDigest();
    const uint8 state = reader.ReadUInt8();
    record.ObjectLength = reader.ReadUInt64();
    record.Object = reader.ReadDigest();
    if (reader.IsAtEnd() == false || magic != FILE_GENERATION_CACHE_RECORD_MAGIC ||
        version != FILE_GENERATION_CACHE_VERSION || record.Key != key ||
        state > static_cast<uint8>(RecordState::Complete))
    {
        return false;
    }
    record.State = static_cast<RecordState>(state);
    outRecord = record;
    return true;
}
} // namespace

FileGenerationCacheName GetFileGenerationCacheRecordName(const GenerationDigest& key) noexcept
{
    FileGenerationCacheName name;
    AppendHex(key, name);
    static_cast<void>(name.TryAppend(RECORD_SUFFIX));
    return name;
}

FileGenerationCacheName GetFileGenerationCacheObjectName(const GenerationDigest& key,
                                                         const GenerationDigest& object) noexcept
{
    FileGenerationCacheName name;
    AppendHex(key, name);
    static_cast<void>(name.TryAppend(OBJECT_SEPARATOR));
    AppendHex(object, name);
    static_cast<void>(name.TryAppend(OBJECT_SUFFIX));
    return name;
}

bool FileGenerationCacheStore::TryOpen(std::string_view root, usize maximumEntryBytes) noexcept
{
    foundation::StaticString<content::MAX_ROOT_BYTES> stored;
    if (root.empty() == true || maximumEntryBytes > MAX_FILE_GENERATION_CACHE_ENTRY_BYTES ||
        stored.TryAssign(root) != StringStatus::Ok)
    {
        return false;
    }
    mRoot = stored;
    mMaximumEntryBytes = maximumEntryBytes;
    return true;
}

GenerationStoreRead FileGenerationCacheStore::TryRead(const GenerationDigest& key, Array<uint8>& outBytes) noexcept
{
    if (IsOpen() == false)
    {
        return GenerationStoreRead::Failed;
    }
    const FileGenerationCacheName recordName = GetFileGenerationCacheRecordName(key);
    Bytes recordBytes;
    const Status recordStatus =
        ReadFile(mRoot.GetView(), recordName.GetView(), FILE_GENERATION_CACHE_RECORD_BYTES, recordBytes);
    if (recordStatus == Status::NotFound)
    {
        return GenerationStoreRead::Absent;
    }
    Record record;
    bool isCorrupt = recordStatus == Status::Limit;
    if (recordStatus == Status::Ok)
    {
        isCorrupt = TryReadRecord(recordBytes, key, record) == false ||
                    (record.State == RecordState::Complete && record.ObjectLength > mMaximumEntryBytes);
    }
    else if (isCorrupt == false)
    {
        return GenerationStoreRead::Failed;
    }
    if (isCorrupt == false && record.State == RecordState::Removed)
    {
        return GenerationStoreRead::Absent;
    }
    Bytes object;
    if (isCorrupt == false)
    {
        const FileGenerationCacheName objectName = GetFileGenerationCacheObjectName(key, record.Object);
        const Status objectStatus =
            ReadFile(mRoot.GetView(), objectName.GetView(), static_cast<usize>(record.ObjectLength), object);
        if (objectStatus != Status::Ok && objectStatus != Status::NotFound && objectStatus != Status::Limit)
        {
            return GenerationStoreRead::Failed;
        }
        isCorrupt = objectStatus != Status::Ok || object.Data().size() != record.ObjectLength ||
                    HashBytes(object) != record.Object;
    }
    if (isCorrupt == true)
    {
        Remove(key);
        return GenerationStoreRead::Corrupt;
    }
    Array<uint8> bytes;
    if (bytes.TryAddRange({object.Data().data(), object.Data().size()}) == false)
    {
        return GenerationStoreRead::Failed;
    }
    outBytes = foundation::Move(bytes);
    return GenerationStoreRead::Found;
}

bool FileGenerationCacheStore::tryReplaceFile(std::string_view name, BorrowedView<const uint8> bytes) noexcept
{
    // SaveFile replaces only the revision it is shown, so read the current one first.
    Bytes existing;
    const Status readStatus = ReadFile(mRoot.GetView(), name, MAX_SAVE_BYTES, existing);
    if (readStatus != Status::Ok && readStatus != Status::NotFound)
    {
        return false;
    }
    const GenerationDigest expected = HashBytes(existing);
    if (readStatus == Status::Ok && existing.Data().size() == bytes.GetSize() &&
        expected == ComputeGenerationDigest(bytes))
    {
        return true;
    }
    return SaveFile(mRoot.GetView(), name, ToSpan(bytes), readStatus == Status::Ok ? &expected : nullptr) == Status::Ok;
}

bool FileGenerationCacheStore::TryWrite(const GenerationDigest& key, BorrowedView<const uint8> bytes) noexcept
{
    if (IsOpen() == false || bytes.IsValid() == false || bytes.GetSize() > mMaximumEntryBytes)
    {
        return false;
    }
    Record record;
    record.Key = key;
    record.State = RecordState::Complete;
    record.ObjectLength = bytes.GetSize();
    record.Object = ComputeGenerationDigest(bytes);
    const FileGenerationCacheName objectName = GetFileGenerationCacheObjectName(key, record.Object);
    if (tryReplaceFile(objectName.GetView(), bytes) == false)
    {
        return false;
    }
    // Verify the staged object as a reader will see it before any record names it.
    Bytes staged;
    if (ReadFile(mRoot.GetView(), objectName.GetView(), bytes.GetSize(), staged) != Status::Ok ||
        staged.Data().size() != record.ObjectLength || HashBytes(staged) != record.Object)
    {
        return false;
    }
    const FileGenerationCacheName recordName = GetFileGenerationCacheRecordName(key);
    GenerationByteWriter writer;
    return TryWriteRecord(record, writer) == true && tryReplaceFile(recordName.GetView(), writer.GetBytes()) == true;
}

void FileGenerationCacheStore::Remove(const GenerationDigest& key) noexcept
{
    if (IsOpen() == false)
    {
        return;
    }
    const FileGenerationCacheName recordName = GetFileGenerationCacheRecordName(key);
    Bytes existing;
    if (ReadFile(mRoot.GetView(), recordName.GetView(), MAX_SAVE_BYTES, existing) == Status::NotFound)
    {
        return;
    }
    Record removal;
    removal.Key = key;
    GenerationByteWriter writer;
    if (TryWriteRecord(removal, writer) == true)
    {
        // A failed removal leaves an entry that reads as Corrupt or Found; it is never trusted unverified.
        static_cast<void>(tryReplaceFile(recordName.GetView(), writer.GetBytes()));
    }
}
} // namespace ludus::content::procedural
