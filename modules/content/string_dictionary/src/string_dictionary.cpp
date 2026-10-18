#include <ludus/content/string_dictionary/string_dictionary.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <span>

// Thanks to James Boer, "A Flexible Text Parsing System", Game Programming Gems 2,
// section 1.17, pp.112-117, for compiling authored text into a compact indexed
// dictionary, and to Jason Hughes, "Pointer Patching Assets", Game Engine Gems 2,
// chapter 20, pp.345-357, for preparing bounded immutable data before
// publication. LUSTR validates checked little-endian relative offsets instead of
// patching native pointers. Original implementation; see
// docs/architecture/strings-gems-review.md and docs/architecture/strings.md.

namespace ludus::content::string_dictionary
{
namespace format = string_dictionary_format;

// Every directory index must stay below the shared StringIndex sentinel.
static_assert(format::MAX_ENTRIES <= foundation::StringIndex::INVALID_VALUE);

namespace
{
uint32 ReadWord(const uint8* bytes, usize offset) noexcept
{
    uint32 value{};
    const std::span<const uint8> field{bytes + offset, sizeof(uint32)};
    [[maybe_unused]] const bool isRead = foundation::TryReadLittleEndian(field, value);
    LUDUS_ASSERT(isRead == true);
    return value;
}

usize GetBlobOffset(uint32 entryCount) noexcept
{
    return format::HEADER_BYTES + usize{entryCount} * format::DIRECTORY_ENTRY_BYTES;
}

std::string_view MakeSpelling(const uint8* bytes, usize offset, usize length) noexcept
{
    return {reinterpret_cast<const char*>(bytes + offset), length};
}

StringDictionaryStatus
Fail(StringDictionaryDiagnostic& outDiagnostic, StringDictionaryStatus status, usize offset) noexcept
{
    outDiagnostic = { .Status = status, .Offset = offset };
    return status;
}

StringDictionaryStatus ValidateHeader(const uint8* bytes,
                                      usize size,
                                      const RevisionDigest& expectedRevision,
                                      StringDictionaryDiagnostic& outDiagnostic) noexcept
{
    if (size < format::HEADER_BYTES)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, size);
    }
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        if (bytes[format::MAGIC_OFFSET + i] != static_cast<uint8>(format::MAGIC[i]))
        {
            return Fail(outDiagnostic, StringDictionaryStatus::Malformed, format::MAGIC_OFFSET + i);
        }
    }
    if (ReadWord(bytes, format::VERSION_OFFSET) != format::VERSION_1)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::UnsupportedVersion, format::VERSION_OFFSET);
    }
    const uint32 bytePolicy = ReadWord(bytes, format::BYTE_POLICY_OFFSET);
    if (bytePolicy != format::BYTE_POLICY_UTF8 && bytePolicy != format::BYTE_POLICY_BYTES)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, format::BYTE_POLICY_OFFSET);
    }
    if (ReadWord(bytes, format::RESERVED_OFFSET) != 0)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, format::RESERVED_OFFSET);
    }
    const uint32 entryCount = ReadWord(bytes, format::ENTRY_COUNT_OFFSET);
    if (entryCount == 0)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, format::ENTRY_COUNT_OFFSET);
    }
    if (entryCount > format::MAX_ENTRIES)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::LimitExceeded, format::ENTRY_COUNT_OFFSET);
    }
    // MAX_ENTRIES keeps this product within uint32.
    if (ReadWord(bytes, format::DIRECTORY_BYTES_OFFSET) != entryCount * format::DIRECTORY_ENTRY_BYTES)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, format::DIRECTORY_BYTES_OFFSET);
    }
    const uint32 blobBytes = ReadWord(bytes, format::BLOB_BYTES_OFFSET);
    if (blobBytes > format::MAX_BLOB_BYTES)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::LimitExceeded, format::BLOB_BYTES_OFFSET);
    }
    usize total = 0;
    if (foundation::TryAdd(GetBlobOffset(entryCount), usize{blobBytes}, total) == false || total != size)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, total < size ? total : size);
    }
    for (usize i = 0; i < format::REVISION_DIGEST_BYTES; ++i)
    {
        if (bytes[format::REVISION_DIGEST_OFFSET + i] != expectedRevision[i])
        {
            return Fail(outDiagnostic, StringDictionaryStatus::RevisionMismatch, format::REVISION_DIGEST_OFFSET + i);
        }
    }
    return StringDictionaryStatus::Ok;
}

StringDictionaryStatus ValidateEntries(const uint8* bytes, StringDictionaryDiagnostic& outDiagnostic) noexcept
{
    const uint32 entryCount = ReadWord(bytes, format::ENTRY_COUNT_OFFSET);
    const uint32 bytePolicy = ReadWord(bytes, format::BYTE_POLICY_OFFSET);
    const usize blobOffset = GetBlobOffset(entryCount);
    const usize blobBytes = ReadWord(bytes, format::BLOB_BYTES_OFFSET);
    usize expectedOffset = 0;
    std::string_view previous;
    for (uint32 i = 0; i < entryCount; ++i)
    {
        const usize entry = format::HEADER_BYTES + usize{i} * format::DIRECTORY_ENTRY_BYTES;
        const usize offset = ReadWord(bytes, entry + format::ENTRY_OFFSET_FIELD);
        const usize length = ReadWord(bytes, entry + format::ENTRY_LENGTH_FIELD);
        // Canonical packing also bounds offset by the previous end within the blob.
        if (offset != expectedOffset)
        {
            return Fail(outDiagnostic, StringDictionaryStatus::Malformed, entry + format::ENTRY_OFFSET_FIELD);
        }
        if (length > format::MAX_SPELLING_BYTES)
        {
            return Fail(outDiagnostic, StringDictionaryStatus::LimitExceeded, entry + format::ENTRY_LENGTH_FIELD);
        }
        if (offset > blobBytes || length >= blobBytes - offset)
        {
            return Fail(outDiagnostic, StringDictionaryStatus::Malformed, entry + format::ENTRY_LENGTH_FIELD);
        }
        if (bytes[blobOffset + offset + length] != 0)
        {
            return Fail(outDiagnostic, StringDictionaryStatus::Malformed, blobOffset + offset + length);
        }
        const std::string_view spelling = MakeSpelling(bytes, blobOffset + offset, length);
        for (usize j = 0; j < length; ++j)
        {
            if (spelling[j] == '\0')
            {
                return Fail(outDiagnostic, StringDictionaryStatus::EmbeddedZero, blobOffset + offset + j);
            }
        }
        if (bytePolicy == format::BYTE_POLICY_UTF8)
        {
            foundation::Utf8View validated;
            usize errorOffset = 0;
            if (foundation::ValidateUtf8(spelling, validated, errorOffset) != foundation::StringStatus::Ok)
            {
                return Fail(outDiagnostic, StringDictionaryStatus::InvalidUtf8, blobOffset + offset + errorOffset);
            }
        }
        // std::string_view compares char_traits<char>, which orders bytes as unsigned char.
        // Entry 0 must be empty; every later entry is strictly greater, so unique.
        if ((i == format::EMPTY_ENTRY && length != 0) || (i != format::EMPTY_ENTRY && spelling.compare(previous) <= 0))
        {
            return Fail(outDiagnostic, StringDictionaryStatus::Malformed, entry);
        }
        previous = spelling;
        expectedOffset = offset + length + 1;
    }
    if (expectedOffset != blobBytes)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::Malformed, blobOffset + expectedOffset);
    }
    return StringDictionaryStatus::Ok;
}
} // namespace

RevisionDigest StringDictionaryView::GetRevision() const noexcept
{
    RevisionDigest revision{};
    if (mBytesOrNull != nullptr)
    {
        for (usize i = 0; i < format::REVISION_DIGEST_BYTES; ++i)
        {
            revision[i] = mBytesOrNull[format::REVISION_DIGEST_OFFSET + i];
        }
    }
    return revision;
}

std::string_view StringDictionaryView::getSpelling(uint32 index) const noexcept
{
    const usize entry = format::HEADER_BYTES + usize{index} * format::DIRECTORY_ENTRY_BYTES;
    const usize offset = ReadWord(mBytesOrNull, entry + format::ENTRY_OFFSET_FIELD);
    const usize length = ReadWord(mBytesOrNull, entry + format::ENTRY_LENGTH_FIELD);
    return MakeSpelling(mBytesOrNull, GetBlobOffset(mEntryCount) + offset, length);
}

StringDictionaryStatus StringDictionaryView::TryResolve(foundation::StringIndex index,
                                                        std::string_view& outSpelling) const noexcept
{
    if (IsValid() == false || index.Value >= mEntryCount)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    outSpelling = getSpelling(index.Value);
    return StringDictionaryStatus::Ok;
}

StringDictionaryStatus StringDictionaryView::TryFind(std::string_view spelling,
                                                     foundation::StringIndex& outIndex) const noexcept
{
    if (IsValid() == false)
    {
        return StringDictionaryStatus::InvalidHandle;
    }
    uint32 low = 0;
    uint32 high = mEntryCount;
    while (low < high)
    {
        const uint32 middle = low + (high - low) / 2;
        const foundation::int32 order = getSpelling(middle).compare(spelling);
        if (order == 0)
        {
            outIndex = foundation::StringIndex{middle};
            return StringDictionaryStatus::Ok;
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return StringDictionaryStatus::NotFound;
}

StringDictionaryStatus ReadStringDictionary(foundation::BorrowedView<const uint8> bytes,
                                            const RevisionDigest& expectedRevision,
                                            StringDictionaryView& outDictionary,
                                            StringDictionaryDiagnostic& outDiagnostic) noexcept
{
    if (bytes.IsValid() == false)
    {
        return Fail(outDiagnostic, StringDictionaryStatus::InvalidArgument, 0);
    }
    const uint8* data = bytes.GetDataOrNull();
    StringDictionaryStatus status = ValidateHeader(data, bytes.GetSize(), expectedRevision, outDiagnostic);
    if (status == StringDictionaryStatus::Ok)
    {
        status = ValidateEntries(data, outDiagnostic);
    }
    if (status != StringDictionaryStatus::Ok)
    {
        return status;
    }
    outDictionary.mBytesOrNull = data;
    outDictionary.mEntryCount = ReadWord(data, format::ENTRY_COUNT_OFFSET);
    outDictionary.mBytePolicy = ReadWord(data, format::BYTE_POLICY_OFFSET);
    outDiagnostic = { .Status = StringDictionaryStatus::Ok, .Offset = 0 };
    return StringDictionaryStatus::Ok;
}
} // namespace ludus::content::string_dictionary
