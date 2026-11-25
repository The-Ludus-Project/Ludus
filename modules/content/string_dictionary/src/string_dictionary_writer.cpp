#include <ludus/content/string_dictionary/string_dictionary_writer.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>

#include <cstring>
#include <new>
#include <span>

// Thanks to James Boer, "A Flexible Text Parsing System", Game Programming Gems 2,
// section 1.17, pp.112-117, for compiling deduplicated authored text into one
// indexed dictionary. Ludus sorts by unsigned bytes for deterministic output and
// remaps record indices instead of keeping arrival order. Original
// implementation; see docs/architecture/strings-gems-review.md.

namespace ludus::content::string_dictionary
{
namespace format = string_dictionary_format;

struct StringDictionaryWriter::State final
{
    struct SortedEntry final
    {
        std::string_view Spelling;
        uint32 ProvisionalIndex{};
    };

    const foundation::AllocationDomain* Domain;
    StringDictionaryWriterConfig Config;
    foundation::StringTable Table;
    uint32 TableToken{};
    uint32 EntryCount{1};
    usize BlobBytes{1};
    SortedEntry* SortedOrNull{};
    uint32* RemapOrNull{};

    State(const foundation::AllocationDomain& domain,
          const StringDictionaryWriterConfig& config,
          foundation::StringTable&& table,
          uint32 tableToken) noexcept
        : Domain(&domain), Config(config), Table(foundation::Move(table)), TableToken(tableToken)
    {
    }

    void FreeSorted() noexcept
    {
        Domain->Free(SortedOrNull, usize{EntryCount} * sizeof(SortedEntry), alignof(SortedEntry));
        Domain->Free(RemapOrNull, usize{EntryCount} * sizeof(uint32), alignof(uint32));
        SortedOrNull = nullptr;
        RemapOrNull = nullptr;
    }

    static void Destroy(State* stateOrNull) noexcept
    {
        if (stateOrNull != nullptr)
        {
            const foundation::AllocationDomain* domain = stateOrNull->Domain;
            stateOrNull->FreeSorted();
            stateOrNull->~State();
            domain->Free(stateOrNull, sizeof(State), alignof(State));
        }
    }
};

namespace
{
StringDictionaryStatus ConvertStatus(foundation::StringStatus status) noexcept
{
    switch (status)
    {
        case foundation::StringStatus::Ok:
            return StringDictionaryStatus::Ok;
        case foundation::StringStatus::OutOfMemory:
            return StringDictionaryStatus::OutOfMemory;
        case foundation::StringStatus::TooLarge:
        case foundation::StringStatus::CapacityExceeded:
        case foundation::StringStatus::TokenExhausted:
            return StringDictionaryStatus::LimitExceeded;
        case foundation::StringStatus::InvalidUtf8:
            return StringDictionaryStatus::InvalidUtf8;
        case foundation::StringStatus::EmbeddedZero:
            return StringDictionaryStatus::EmbeddedZero;
        case foundation::StringStatus::InvalidArgument:
            return StringDictionaryStatus::InvalidArgument;
        case foundation::StringStatus::NotFound:
            return StringDictionaryStatus::NotFound;
        case foundation::StringStatus::InvalidHandle:
        case foundation::StringStatus::WrongTable:
            return StringDictionaryStatus::InvalidHandle;
        case foundation::StringStatus::ProbeExhausted:
        // Byte-range, text-slice and UTF-16 operations are not used by the writer.
        case foundation::StringStatus::OutOfRange:
        case foundation::StringStatus::SplitsCodePoint:
        case foundation::StringStatus::InvalidUtf16:
            return StringDictionaryStatus::InvalidState;
    }
    return StringDictionaryStatus::InvalidState;
}

bool IsConfigValid(const StringDictionaryWriterConfig& config) noexcept
{
    return (config.BytePolicy == format::BYTE_POLICY_UTF8 || config.BytePolicy == format::BYTE_POLICY_BYTES) &&
           config.MaxEntries != 0 && config.MaxEntries <= format::MAX_ENTRIES &&
           config.MaxSpellingBytes <= format::MAX_SPELLING_BYTES && config.MaxBlobBytes != 0 &&
           config.MaxBlobBytes <= format::MAX_BLOB_BYTES;
}

void WriteWord(std::span<uint8> destination, usize offset, usize value) noexcept
{
    // Callers pass values bounded by the format limits, all below 2^32.
    LUDUS_ASSERT(value <= usize{0xffffffffU});
    [[maybe_unused]] const bool isWritten =
        foundation::TryWriteLittleEndian(static_cast<uint32>(value),
                                         ludus::foundation::MakeBorrowedView(destination.subspan(offset)));
    LUDUS_ASSERT(isWritten == true);
}
} // namespace

StringDictionaryWriter::~StringDictionaryWriter() noexcept
{
    State::Destroy(mStateOrNull);
}

StringDictionaryWriter::StringDictionaryWriter(StringDictionaryWriter&& other) noexcept
    : mStateOrNull(other.mStateOrNull)
{
    other.mStateOrNull = nullptr;
}

StringDictionaryWriter& StringDictionaryWriter::operator=(StringDictionaryWriter&& other) noexcept
{
    if (this != &other)
    {
        State::Destroy(mStateOrNull);
        mStateOrNull = other.mStateOrNull;
        other.mStateOrNull = nullptr;
    }
    return *this;
}

bool StringDictionaryWriter::IsValid() const noexcept
{
    return mStateOrNull != nullptr;
}

bool StringDictionaryWriter::IsFinished() const noexcept
{
    return mStateOrNull != nullptr && mStateOrNull->SortedOrNull != nullptr;
}

uint32 StringDictionaryWriter::GetEntryCount() const noexcept
{
    return mStateOrNull == nullptr ? 0 : mStateOrNull->EntryCount;
}

StringDictionaryStatus StringDictionaryWriter::TryAdd(std::string_view spelling,
                                                      foundation::StringIndex& outProvisionalIndex) noexcept
{
    if (mStateOrNull == nullptr)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    State& state = *mStateOrNull;
    if (IsFinished() == true)
    {
        return StringDictionaryStatus::InvalidState;
    }
    foundation::NameId id;
    const foundation::StringStatus found = state.Table.TryFind(spelling, id);
    if (found == foundation::StringStatus::Ok)
    {
        outProvisionalIndex = id.Index;
        return StringDictionaryStatus::Ok;
    }
    if (found != foundation::StringStatus::NotFound)
    {
        return ConvertStatus(found);
    }
    usize blobBytes = 0;
    if (state.EntryCount == state.Config.MaxEntries ||
        foundation::TryAdd(state.BlobBytes, spelling.size() + 1, blobBytes) == false ||
        blobBytes > state.Config.MaxBlobBytes)
    {
        return StringDictionaryStatus::LimitExceeded;
    }
    const foundation::StringStatus interned = state.Table.TryIntern(spelling, id);
    if (interned != foundation::StringStatus::Ok)
    {
        return ConvertStatus(interned);
    }
    LUDUS_ASSERT(id.Index.Value == state.EntryCount);
    ++state.EntryCount;
    state.BlobBytes = blobBytes;
    outProvisionalIndex = id.Index;
    return StringDictionaryStatus::Ok;
}

StringDictionaryStatus StringDictionaryWriter::TryFinish() noexcept
{
    if (mStateOrNull == nullptr)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    State& state = *mStateOrNull;
    if (IsFinished() == true)
    {
        return StringDictionaryStatus::InvalidState;
    }
    const usize count = state.EntryCount;
    // MAX_ENTRIES bounds both products far below the address space.
    State::SortedEntry* sortedOrNull = static_cast<State::SortedEntry*>(
        state.Domain->TryAllocate(count * sizeof(State::SortedEntry), alignof(State::SortedEntry)));
    uint32* remapOrNull = static_cast<uint32*>(state.Domain->TryAllocate(count * sizeof(uint32), alignof(uint32)));
    if (sortedOrNull == nullptr || remapOrNull == nullptr)
    {
        state.Domain->Free(sortedOrNull, count * sizeof(State::SortedEntry), alignof(State::SortedEntry));
        state.Domain->Free(remapOrNull, count * sizeof(uint32), alignof(uint32));
        return StringDictionaryStatus::OutOfMemory;
    }
    State::SortedEntry* sorted = sortedOrNull;
    uint32* remap = remapOrNull;
    for (uint32 i = 0; i < state.EntryCount; ++i)
    {
        std::string_view spelling;
        const foundation::NameId id{ .TableToken = state.TableToken, .Index = foundation::StringIndex{i} };
        [[maybe_unused]] const foundation::StringStatus resolved = state.Table.TryResolve(id, spelling);
        LUDUS_ASSERT(resolved == foundation::StringStatus::Ok);
        new (sorted + i) State::SortedEntry{ .Spelling = spelling, .ProvisionalIndex = i };
        new (remap + i) uint32{};
    }
    // std::string_view compares char_traits<char>, which orders bytes as unsigned char.
    foundation::Sort(sorted, count, [](const State::SortedEntry& left, const State::SortedEntry& right) noexcept {
        return left.Spelling.compare(right.Spelling) < 0;
    });
    for (uint32 i = 0; i < state.EntryCount; ++i)
    {
        remap[sorted[i].ProvisionalIndex] = i;
    }
    LUDUS_ASSERT(sorted[format::EMPTY_ENTRY].Spelling.empty() == true);
    state.SortedOrNull = sorted;
    state.RemapOrNull = remap;
    return StringDictionaryStatus::Ok;
}

StringDictionaryStatus StringDictionaryWriter::TryRemap(foundation::StringIndex provisionalIndex,
                                                        foundation::StringIndex& outIndex) const noexcept
{
    if (mStateOrNull == nullptr)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    if (IsFinished() == false)
    {
        return StringDictionaryStatus::InvalidState;
    }
    if (provisionalIndex.Value >= mStateOrNull->EntryCount)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    outIndex = foundation::StringIndex{mStateOrNull->RemapOrNull[provisionalIndex.Value]};
    return StringDictionaryStatus::Ok;
}

usize StringDictionaryWriter::GetEncodedSize() const noexcept
{
    if (IsFinished() == false)
    {
        return 0;
    }
    return format::HEADER_BYTES + usize{mStateOrNull->EntryCount} * format::DIRECTORY_ENTRY_BYTES +
           mStateOrNull->BlobBytes;
}

StringDictionaryStatus StringDictionaryWriter::TryEncode(const RevisionDigest& revision,
                                                         foundation::BorrowedView<uint8> destination) const noexcept
{
    if (mStateOrNull == nullptr)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    if (IsFinished() == false)
    {
        return StringDictionaryStatus::InvalidState;
    }
    if (destination.IsValid() == false || destination.GetSize() != GetEncodedSize())
    {
        return StringDictionaryStatus::InvalidArgument;
    }
    const State& state = *mStateOrNull;
    const std::span<uint8> bytes{destination.GetDataOrNull(), destination.GetSize()};
    const usize directoryBytes = usize{state.EntryCount} * format::DIRECTORY_ENTRY_BYTES;
    const usize blobOffset = format::HEADER_BYTES + directoryBytes;

    std::memcpy(bytes.data() + format::MAGIC_OFFSET, format::MAGIC.data(), format::MAGIC.size());
    WriteWord(bytes, format::VERSION_OFFSET, format::CURRENT_VERSION);
    WriteWord(bytes, format::BYTE_POLICY_OFFSET, state.Config.BytePolicy);
    WriteWord(bytes, format::ENTRY_COUNT_OFFSET, state.EntryCount);
    WriteWord(bytes, format::DIRECTORY_BYTES_OFFSET, directoryBytes);
    WriteWord(bytes, format::BLOB_BYTES_OFFSET, state.BlobBytes);
    WriteWord(bytes, format::RESERVED_OFFSET, 0);
    for (usize i = 0; i < format::REVISION_DIGEST_BYTES; ++i)
    {
        bytes[format::REVISION_DIGEST_OFFSET + i] = revision[i];
    }

    usize offset = 0;
    for (uint32 i = 0; i < state.EntryCount; ++i)
    {
        const std::string_view spelling = state.SortedOrNull[i].Spelling;
        const usize entry = format::HEADER_BYTES + usize{i} * format::DIRECTORY_ENTRY_BYTES;
        WriteWord(bytes, entry + format::ENTRY_OFFSET_FIELD, offset);
        WriteWord(bytes, entry + format::ENTRY_LENGTH_FIELD, spelling.size());
        if (spelling.empty() == false)
        {
            std::memcpy(bytes.data() + blobOffset + offset, spelling.data(), spelling.size());
        }
        bytes[blobOffset + offset + spelling.size()] = 0;
        offset += spelling.size() + 1;
    }
    LUDUS_ASSERT(offset == state.BlobBytes);
    return StringDictionaryStatus::Ok;
}

StringDictionaryStatus CreateStringDictionaryWriter(const StringDictionaryWriterConfig& config,
                                                    const foundation::AllocationDomain& domain,
                                                    StringDictionaryWriter& outWriter) noexcept
{
    if (outWriter.IsValid() == true || IsConfigValid(config) == false)
    {
        return StringDictionaryStatus::InvalidArgument;
    }
    foundation::StringTableConfig tableConfig;
    tableConfig.MaxSpellingBytes = config.MaxSpellingBytes;
    tableConfig.MaxEntries = config.MaxEntries;
    tableConfig.MaxAllocatedBytes = config.MaxTableBytes;
    tableConfig.HashPolicy = foundation::StringHashPolicy::Trusted;
    tableConfig.RequireUtf8 = config.BytePolicy == format::BYTE_POLICY_UTF8;
    tableConfig.RejectEmbeddedZero = true;
    foundation::StringTable table;
    const foundation::StringStatus created = foundation::CreateTable(tableConfig, domain, table);
    if (created != foundation::StringStatus::Ok)
    {
        return ConvertStatus(created);
    }
    foundation::NameId empty;
    [[maybe_unused]] const foundation::StringStatus found = table.TryFind({}, empty);
    LUDUS_ASSERT(found == foundation::StringStatus::Ok && empty.Index.Value == format::EMPTY_ENTRY);
    using State = StringDictionaryWriter::State;
    void* storageOrNull = domain.TryAllocate(sizeof(State), alignof(State));
    if (storageOrNull == nullptr)
    {
        return StringDictionaryStatus::OutOfMemory;
    }
    outWriter.mStateOrNull = new (storageOrNull) State(domain, config, foundation::Move(table), empty.TableToken);
    return StringDictionaryStatus::Ok;
}
} // namespace ludus::content::string_dictionary
