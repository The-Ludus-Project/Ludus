#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>

#include <ludus/content/string_dictionary/string_dictionary.h>
#include <ludus/content/string_dictionary/string_dictionary_writer.h>
#include <ludus/content/string_dictionary_format/lustr.h>

#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace format = ludus::content::string_dictionary_format;
using ludus::content::string_dictionary::CreateStringDictionaryWriter;
using ludus::content::string_dictionary::ReadStringDictionary;
using ludus::content::string_dictionary::RevisionDigest;
using ludus::content::string_dictionary::StringDictionaryDiagnostic;
using ludus::content::string_dictionary::StringDictionaryStatus;
using ludus::content::string_dictionary::StringDictionaryView;
using ludus::content::string_dictionary::StringDictionaryWriter;
using ludus::content::string_dictionary::StringDictionaryWriterConfig;
using ludus::foundation::AllocationDomain;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::StringIndex;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;

/// Counting test domain that can fail its Nth allocation attempt.
struct FaultDomain final
{
    usize Attempts{};
    usize FailAt{};
    usize LiveAllocations{};
    AllocationDomain Domain{this, Allocate, Free};

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        FaultDomain& self = *static_cast<FaultDomain*>(context);
        ++self.Attempts;
        if (self.FailAt != 0 && self.Attempts == self.FailAt)
        {
            return nullptr;
        }
        void* pointerOrNull = GetSystemAllocationDomain().TryAllocate(bytes, alignment);
        if (pointerOrNull != nullptr)
        {
            ++self.LiveAllocations;
        }
        return pointerOrNull;
    }
    static void Free(void* context, // NOLINT(bugprone-easily-swappable-parameters): AllocationDomain seam signature
                     void* pointer,
                     usize bytes,
                     usize alignment) noexcept
    {
        FaultDomain& self = *static_cast<FaultDomain*>(context);
        --self.LiveAllocations;
        GetSystemAllocationDomain().Free(pointer, bytes, alignment);
    }
};

RevisionDigest MakeRevision(uint8 seed)
{
    RevisionDigest revision{};
    for (usize i = 0; i < revision.GetSize(); ++i)
    {
        revision[i] = static_cast<uint8>(seed + i);
    }
    return revision;
}

StringDictionaryWriter MakeWriter(const StringDictionaryWriterConfig& config = {})
{
    StringDictionaryWriter writer;
    REQUIRE(CreateStringDictionaryWriter(config, GetSystemAllocationDomain(), writer) == StringDictionaryStatus::Ok);
    return writer;
}

Array<uint8> Encode(StringDictionaryWriter& inoutWriter, const RevisionDigest& revision)
{
    if (inoutWriter.IsFinished() == false)
    {
        REQUIRE(inoutWriter.TryFinish() == StringDictionaryStatus::Ok);
    }
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(inoutWriter.GetEncodedSize()) == true);
    REQUIRE(inoutWriter.TryEncode(revision, {bytes.GetData(), bytes.GetSize()}) == StringDictionaryStatus::Ok);
    return bytes;
}

using SpellingList = std::span<const std::string_view>;

Array<uint8>
EncodeSpellings(SpellingList spellings, const RevisionDigest& revision, const StringDictionaryWriterConfig& config)
{
    StringDictionaryWriter writer = MakeWriter(config);
    for (const std::string_view spelling : spellings)
    {
        StringIndex index;
        REQUIRE(writer.TryAdd(spelling, index) == StringDictionaryStatus::Ok);
    }
    return Encode(writer, revision);
}

BorrowedView<const uint8> View(const Array<uint8>& bytes)
{
    return {bytes.GetData(), bytes.GetSize()};
}

StringDictionaryStatus Read(const Array<uint8>& bytes,
                            const RevisionDigest& revision,
                            StringDictionaryView& outView,
                            StringDictionaryDiagnostic& outDiagnostic)
{
    return ReadStringDictionary(View(bytes), revision, outView, outDiagnostic);
}

StringDictionaryStatus Read(const Array<uint8>& bytes, const RevisionDigest& revision)
{
    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic;
    return Read(bytes, revision, view, diagnostic);
}

uint32 GetWord(const Array<uint8>& bytes, usize offset)
{
    uint32 value{};
    REQUIRE(ludus::foundation::TryReadLittleEndian({bytes.GetData() + offset, 4}, value) == true);
    return value;
}

void SetWord(Array<uint8>& inoutBytes, usize offset, uint32 value)
{
    REQUIRE(ludus::foundation::TryWriteLittleEndian(value, {inoutBytes.GetData() + offset, 4}) == true);
}

usize GetEntryField(usize index, usize field)
{
    return format::HEADER_BYTES + index * format::DIRECTORY_ENTRY_BYTES + field;
}

usize GetBlobOffset(const Array<uint8>& bytes)
{
    return format::HEADER_BYTES + usize{GetWord(bytes, format::ENTRY_COUNT_OFFSET)} * format::DIRECTORY_ENTRY_BYTES;
}

Array<uint8> Copy(const Array<uint8>& bytes)
{
    Array<uint8> copy;
    REQUIRE(copy.TryResize(bytes.GetSize()) == true);
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        copy[i] = bytes[i];
    }
    return copy;
}

constexpr std::string_view SAMPLE_SPELLINGS[] = {"material/base", "mesh", "Mesh", "", "mesh/lod1", "a"};
} // namespace

TEST_CASE("Writer and reader round trip with remapped indices", "[string_dictionary]")
{
    const RevisionDigest revision = MakeRevision(3);
    StringDictionaryWriter writer = MakeWriter();
    StringIndex provisional[std::size(SAMPLE_SPELLINGS)];
    for (usize i = 0; i < std::size(SAMPLE_SPELLINGS); ++i)
    {
        REQUIRE(writer.TryAdd(SAMPLE_SPELLINGS[i], provisional[i]) == StringDictionaryStatus::Ok);
    }
    REQUIRE(provisional[3].Value == format::EMPTY_ENTRY);
    REQUIRE(writer.GetEntryCount() == 6);

    StringIndex remapped;
    REQUIRE(writer.TryRemap(provisional[0], remapped) == StringDictionaryStatus::InvalidState);
    REQUIRE(writer.GetEncodedSize() == 0);
    const Array<uint8> bytes = Encode(writer, revision);
    REQUIRE(writer.IsFinished() == true);

    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic{ .Status = StringDictionaryStatus::Malformed, .Offset = 7 };
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Ok);
    REQUIRE(diagnostic.Status == StringDictionaryStatus::Ok);
    REQUIRE(diagnostic.Offset == 0);
    REQUIRE(view.IsValid() == true);
    REQUIRE(view.GetEntryCount() == 6);
    REQUIRE(view.GetBytePolicy() == format::BYTE_POLICY_UTF8);
    const RevisionDigest stored = view.GetRevision();
    for (usize i = 0; i < stored.GetSize(); ++i)
    {
        REQUIRE(stored[i] == revision[i]);
    }

    for (usize i = 0; i < std::size(SAMPLE_SPELLINGS); ++i)
    {
        REQUIRE(writer.TryRemap(provisional[i], remapped) == StringDictionaryStatus::Ok);
        std::string_view spelling;
        REQUIRE(view.TryResolve(remapped, spelling) == StringDictionaryStatus::Ok);
        REQUIRE(spelling == SAMPLE_SPELLINGS[i]);
        REQUIRE(spelling.data()[spelling.size()] == '\0');
        StringIndex found;
        REQUIRE(view.TryFind(SAMPLE_SPELLINGS[i], found) == StringDictionaryStatus::Ok);
        REQUIRE(found == remapped);
    }

    // Unsigned byte order: uppercase sorts before lowercase, prefixes before extensions.
    constexpr std::string_view EXPECTED_ORDER[] = {"", "Mesh", "a", "material/base", "mesh", "mesh/lod1"};
    for (uint32 i = 0; i < std::size(EXPECTED_ORDER); ++i)
    {
        std::string_view spelling;
        REQUIRE(view.TryResolve(StringIndex{i}, spelling) == StringDictionaryStatus::Ok);
        REQUIRE(spelling == EXPECTED_ORDER[i]);
    }

    std::string_view preserved = "kept";
    StringIndex preservedIndex{42};
    REQUIRE(view.TryResolve(StringIndex{6}, preserved) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(view.TryResolve(StringIndex{}, preserved) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(preserved == "kept");
    REQUIRE(view.TryFind("mes", preservedIndex) == StringDictionaryStatus::NotFound);
    REQUIRE(view.TryFind("mesh/lod2", preservedIndex) == StringDictionaryStatus::NotFound);
    REQUIRE(view.TryFind("zzz", preservedIndex) == StringDictionaryStatus::NotFound);
    REQUIRE(preservedIndex.Value == 42);
    REQUIRE(writer.TryRemap(StringIndex{6}, remapped) == StringDictionaryStatus::InvalidHandle);

    const StringDictionaryView empty;
    REQUIRE(empty.IsValid() == false);
    REQUIRE(empty.TryResolve(StringIndex{0}, preserved) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(empty.TryFind("", preservedIndex) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(empty.GetRevision()[0] == 0);
}

TEST_CASE("Encoding is a pinned canonical byte layout", "[string_dictionary]")
{
    constexpr std::string_view SPELLINGS[] = {"bc", "a"};
    const RevisionDigest revision = MakeRevision(0);
    const Array<uint8> bytes = EncodeSpellings(SPELLINGS, revision, {});
    // Header, three directory entries and the "\0a\0bc\0" blob.
    REQUIRE(bytes.GetSize() == 64 + 3 * 8 + 6);
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        REQUIRE(bytes[i] == static_cast<uint8>(format::MAGIC[i]));
    }
    REQUIRE(GetWord(bytes, 8) == 1);
    REQUIRE(GetWord(bytes, 12) == format::BYTE_POLICY_UTF8);
    REQUIRE(GetWord(bytes, 16) == 3);
    REQUIRE(GetWord(bytes, 20) == 24);
    REQUIRE(GetWord(bytes, 24) == 6);
    REQUIRE(GetWord(bytes, 28) == 0);
    for (usize i = 0; i < format::REVISION_DIGEST_BYTES; ++i)
    {
        REQUIRE(bytes[32 + i] == i);
    }
    constexpr uint32 DIRECTORY[] = {0, 0, 1, 1, 3, 2};
    for (usize i = 0; i < std::size(DIRECTORY); ++i)
    {
        REQUIRE(GetWord(bytes, 64 + i * 4) == DIRECTORY[i]);
    }
    constexpr uint8 BLOB[] = {0, 'a', 0, 'b', 'c', 0};
    for (usize i = 0; i < std::size(BLOB); ++i)
    {
        REQUIRE(bytes[88 + i] == BLOB[i]);
    }
}

TEST_CASE("Writer output is independent of arrival order and duplicates", "[string_dictionary]")
{
    const RevisionDigest revision = MakeRevision(9);
    constexpr std::string_view REORDERED[] = {"a", "mesh/lod1", "mesh", "Mesh", "mesh", "material/base", "a", ""};
    const Array<uint8> first = EncodeSpellings(SAMPLE_SPELLINGS, revision, {});
    const Array<uint8> second = EncodeSpellings(REORDERED, revision, {});
    REQUIRE(first.GetSize() == second.GetSize());
    for (usize i = 0; i < first.GetSize(); ++i)
    {
        REQUIRE(first[i] == second[i]);
    }

    StringDictionaryWriter writer = MakeWriter();
    StringIndex firstIndex;
    StringIndex duplicateIndex;
    REQUIRE(writer.TryAdd("name", firstIndex) == StringDictionaryStatus::Ok);
    REQUIRE(writer.TryAdd("name", duplicateIndex) == StringDictionaryStatus::Ok);
    REQUIRE(firstIndex == duplicateIndex);
    REQUIRE(writer.GetEntryCount() == 2);
}

TEST_CASE("Byte policy governs UTF-8 and high-bit ordering", "[string_dictionary]")
{
    StringDictionaryWriter utf8 = MakeWriter();
    StringIndex index{77};
    REQUIRE(utf8.TryAdd(std::string_view{"\xff", 1}, index) == StringDictionaryStatus::InvalidUtf8);
    REQUIRE(utf8.TryAdd(std::string_view{"a\0b", 3}, index) == StringDictionaryStatus::EmbeddedZero);
    REQUIRE(index.Value == 77);
    REQUIRE(utf8.TryAdd("caf\xc3\xa9", index) == StringDictionaryStatus::Ok);

    StringDictionaryWriterConfig config;
    config.BytePolicy = format::BYTE_POLICY_BYTES;
    constexpr std::string_view SPELLINGS[] = {"\xff", "z", "\x80\x01"};
    const RevisionDigest revision = MakeRevision(1);
    const Array<uint8> bytes = EncodeSpellings(SPELLINGS, revision, config);
    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic;
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Ok);
    REQUIRE(view.GetBytePolicy() == format::BYTE_POLICY_BYTES);
    constexpr std::string_view EXPECTED_ORDER[] = {"", "z", "\x80\x01", "\xff"};
    for (uint32 i = 0; i < std::size(EXPECTED_ORDER); ++i)
    {
        std::string_view spelling;
        REQUIRE(view.TryResolve(StringIndex{i}, spelling) == StringDictionaryStatus::Ok);
        REQUIRE(spelling == EXPECTED_ORDER[i]);
    }

    // The same bytes relabelled as UTF-8 are rejected at the first invalid sequence.
    Array<uint8> relabelled = Copy(bytes);
    SetWord(relabelled, format::BYTE_POLICY_OFFSET, format::BYTE_POLICY_UTF8);
    REQUIRE(Read(relabelled, revision, view, diagnostic) == StringDictionaryStatus::InvalidUtf8);
    REQUIRE(diagnostic.Offset == GetBlobOffset(relabelled) + 3);
}

TEST_CASE("Writer enforces configuration, limits and lifecycle", "[string_dictionary]")
{
    StringDictionaryWriter writer;
    StringIndex index;
    REQUIRE(writer.IsValid() == false);
    REQUIRE(writer.TryAdd("a", index) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(writer.TryFinish() == StringDictionaryStatus::InvalidHandle);
    REQUIRE(writer.TryRemap(StringIndex{0}, index) == StringDictionaryStatus::InvalidHandle);
    REQUIRE(writer.GetEncodedSize() == 0);

    StringDictionaryWriterConfig invalid;
    invalid.BytePolicy = 0;
    REQUIRE(CreateStringDictionaryWriter(invalid, GetSystemAllocationDomain(), writer) ==
            StringDictionaryStatus::InvalidArgument);
    invalid = {};
    invalid.MaxEntries = format::MAX_ENTRIES + 1;
    REQUIRE(CreateStringDictionaryWriter(invalid, GetSystemAllocationDomain(), writer) ==
            StringDictionaryStatus::InvalidArgument);
    invalid = {};
    invalid.MaxSpellingBytes = usize{format::MAX_SPELLING_BYTES} + 1;
    REQUIRE(CreateStringDictionaryWriter(invalid, GetSystemAllocationDomain(), writer) ==
            StringDictionaryStatus::InvalidArgument);
    invalid = {};
    invalid.MaxBlobBytes = 0;
    REQUIRE(CreateStringDictionaryWriter(invalid, GetSystemAllocationDomain(), writer) ==
            StringDictionaryStatus::InvalidArgument);
    REQUIRE(writer.IsValid() == false);

    StringDictionaryWriterConfig config;
    config.MaxEntries = 3;
    config.MaxSpellingBytes = 4;
    config.MaxBlobBytes = 1 + 5 + 2;
    writer = MakeWriter(config);
    StringDictionaryWriter occupied = MakeWriter();
    REQUIRE(CreateStringDictionaryWriter(config, GetSystemAllocationDomain(), occupied) ==
            StringDictionaryStatus::InvalidArgument);

    REQUIRE(writer.TryAdd("abcde", index) == StringDictionaryStatus::LimitExceeded);
    REQUIRE(writer.TryAdd("abcd", index) == StringDictionaryStatus::Ok);
    REQUIRE(writer.TryAdd("xy", index) == StringDictionaryStatus::LimitExceeded); // blob budget
    REQUIRE(writer.TryAdd("x", index) == StringDictionaryStatus::Ok);
    REQUIRE(writer.TryAdd("y", index) == StringDictionaryStatus::LimitExceeded); // entry budget
    // Existing spellings still resolve at full capacity.
    REQUIRE(writer.TryAdd("abcd", index) == StringDictionaryStatus::Ok);
    REQUIRE(index.Value == 1);
    REQUIRE(writer.GetEntryCount() == 3);

    REQUIRE(writer.TryFinish() == StringDictionaryStatus::Ok);
    REQUIRE(writer.TryFinish() == StringDictionaryStatus::InvalidState);
    REQUIRE(writer.TryAdd("abcd", index) == StringDictionaryStatus::InvalidState);
    REQUIRE(writer.GetEncodedSize() == format::HEADER_BYTES + 3 * format::DIRECTORY_ENTRY_BYTES + 8);

    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(writer.GetEncodedSize() + 1) == true);
    bytes[0] = 0x5a;
    const RevisionDigest revision = MakeRevision(0);
    REQUIRE(writer.TryEncode(revision, {bytes.GetData(), bytes.GetSize()}) == StringDictionaryStatus::InvalidArgument);
    REQUIRE(writer.TryEncode(revision, {bytes.GetData(), bytes.GetSize() - 2}) ==
            StringDictionaryStatus::InvalidArgument);
    REQUIRE(writer.TryEncode(revision, {nullptr, writer.GetEncodedSize()}) == StringDictionaryStatus::InvalidArgument);
    REQUIRE(bytes[0] == 0x5a);

    StringDictionaryWriter moved = ludus::foundation::Move(writer);
    REQUIRE(writer.IsValid() == false);
    REQUIRE(moved.IsFinished() == true);
    REQUIRE(moved.TryEncode(revision, {bytes.GetData(), bytes.GetSize() - 1}) == StringDictionaryStatus::Ok);
}

TEST_CASE("Writer allocation failures preserve state and release storage", "[string_dictionary]")
{
    FaultDomain counter;
    {
        StringDictionaryWriter writer;
        REQUIRE(CreateStringDictionaryWriter({}, counter.Domain, writer) == StringDictionaryStatus::Ok);
        for (const std::string_view spelling : SAMPLE_SPELLINGS)
        {
            StringIndex index;
            REQUIRE(writer.TryAdd(spelling, index) == StringDictionaryStatus::Ok);
        }
        REQUIRE(writer.TryFinish() == StringDictionaryStatus::Ok);
    }
    REQUIRE(counter.LiveAllocations == 0);
    const usize totalAttempts = counter.Attempts;
    REQUIRE(totalAttempts > 2);

    const RevisionDigest revision = MakeRevision(4);
    const Array<uint8> expected = EncodeSpellings(SAMPLE_SPELLINGS, revision, {});
    for (usize failAt = 1; failAt <= totalAttempts; ++failAt)
    {
        FaultDomain faults;
        faults.FailAt = failAt;
        {
            StringDictionaryWriter writer;
            StringDictionaryStatus status = CreateStringDictionaryWriter({}, faults.Domain, writer);
            if (status != StringDictionaryStatus::Ok)
            {
                REQUIRE(status == StringDictionaryStatus::OutOfMemory);
                REQUIRE(writer.IsValid() == false);
                faults.FailAt = 0;
                REQUIRE(CreateStringDictionaryWriter({}, faults.Domain, writer) == StringDictionaryStatus::Ok);
            }
            for (const std::string_view spelling : SAMPLE_SPELLINGS)
            {
                StringIndex index{99};
                const uint32 before = writer.GetEntryCount();
                status = writer.TryAdd(spelling, index);
                if (status != StringDictionaryStatus::Ok)
                {
                    REQUIRE(status == StringDictionaryStatus::OutOfMemory);
                    REQUIRE(index.Value == 99);
                    REQUIRE(writer.GetEntryCount() == before);
                    faults.FailAt = 0;
                    REQUIRE(writer.TryAdd(spelling, index) == StringDictionaryStatus::Ok);
                }
            }
            status = writer.TryFinish();
            if (status != StringDictionaryStatus::Ok)
            {
                REQUIRE(status == StringDictionaryStatus::OutOfMemory);
                REQUIRE(writer.IsFinished() == false);
                faults.FailAt = 0;
            }
            const Array<uint8> bytes = Encode(writer, revision);
            REQUIRE(bytes.GetSize() == expected.GetSize());
            for (usize i = 0; i < bytes.GetSize(); ++i)
            {
                REQUIRE(bytes[i] == expected[i]);
            }
        }
        REQUIRE(faults.LiveAllocations == 0);
    }
}

TEST_CASE("Reader rejects malformed headers and preserves outputs", "[string_dictionary]")
{
    const RevisionDigest revision = MakeRevision(5);
    const Array<uint8> valid = EncodeSpellings(SAMPLE_SPELLINGS, revision, {});
    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic;

    REQUIRE(ReadStringDictionary({nullptr, 4}, revision, view, diagnostic) == StringDictionaryStatus::InvalidArgument);
    REQUIRE(ReadStringDictionary({}, revision, view, diagnostic) == StringDictionaryStatus::Malformed);

    struct HeaderCase final
    {
        usize Offset;
        uint32 Value;
        StringDictionaryStatus Expected;
    };
    const HeaderCase cases[] = {
        { .Offset = format::MAGIC_OFFSET, .Value = 0x5453554dU, .Expected = StringDictionaryStatus::Malformed },
        { .Offset = format::VERSION_OFFSET, .Value = 2, .Expected = StringDictionaryStatus::UnsupportedVersion },
        { .Offset = format::VERSION_OFFSET, .Value = 0, .Expected = StringDictionaryStatus::UnsupportedVersion },
        { .Offset = format::BYTE_POLICY_OFFSET, .Value = 3, .Expected = StringDictionaryStatus::Malformed },
        { .Offset = format::RESERVED_OFFSET, .Value = 1, .Expected = StringDictionaryStatus::Malformed },
        { .Offset = format::ENTRY_COUNT_OFFSET, .Value = 0, .Expected = StringDictionaryStatus::Malformed },
        {
            .Offset = format::ENTRY_COUNT_OFFSET,
            .Value = 0xffffffffU,
            .Expected = StringDictionaryStatus::LimitExceeded,
        },
        { .Offset = format::ENTRY_COUNT_OFFSET, .Value = 7, .Expected = StringDictionaryStatus::Malformed },
        { .Offset = format::DIRECTORY_BYTES_OFFSET, .Value = 40, .Expected = StringDictionaryStatus::Malformed },
        {
            .Offset = format::BLOB_BYTES_OFFSET,
            .Value = 0xffffffffU,
            .Expected = StringDictionaryStatus::LimitExceeded,
        },
        { .Offset = format::BLOB_BYTES_OFFSET, .Value = 1, .Expected = StringDictionaryStatus::Malformed },
    };
    for (const HeaderCase& testCase : cases)
    {
        Array<uint8> bytes = Copy(valid);
        SetWord(bytes, testCase.Offset, testCase.Value);
        REQUIRE(Read(bytes, revision, view, diagnostic) == testCase.Expected);
        REQUIRE(diagnostic.Status == testCase.Expected);
        REQUIRE(view.IsValid() == false);
    }

    Array<uint8> mismatched = Copy(valid);
    mismatched[format::REVISION_DIGEST_OFFSET + 31] =
        static_cast<uint8>(mismatched[format::REVISION_DIGEST_OFFSET + 31] ^ 1U);
    REQUIRE(Read(mismatched, revision, view, diagnostic) == StringDictionaryStatus::RevisionMismatch);
    REQUIRE(diagnostic.Offset == format::REVISION_DIGEST_OFFSET + 31);
    REQUIRE(Read(valid, MakeRevision(6)) == StringDictionaryStatus::RevisionMismatch);

    Array<uint8> trailing;
    REQUIRE(trailing.TryResize(valid.GetSize() + 1) == true);
    for (usize i = 0; i < valid.GetSize(); ++i)
    {
        trailing[i] = valid[i];
    }
    REQUIRE(Read(trailing, revision) == StringDictionaryStatus::Malformed);

    // Every truncation fails, and a failed read leaves a previous view intact.
    REQUIRE(Read(valid, revision, view, diagnostic) == StringDictionaryStatus::Ok);
    for (usize size = 0; size < valid.GetSize(); ++size)
    {
        REQUIRE(ReadStringDictionary({valid.GetData(), size}, revision, view, diagnostic) ==
                StringDictionaryStatus::Malformed);
        REQUIRE(view.GetEntryCount() == 6);
    }
}

TEST_CASE("Reader rejects malformed directories and blobs", "[string_dictionary]")
{
    const RevisionDigest revision = MakeRevision(8);
    constexpr std::string_view SPELLINGS[] = {"alpha", "beta", "gamma"};
    const Array<uint8> valid = EncodeSpellings(SPELLINGS, revision, {});
    const usize blob = GetBlobOffset(valid);
    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic;
    // Blob: "\0alpha\0beta\0gamma\0"; entries at offsets 0, 1, 7, 12.

    Array<uint8> bytes = Copy(valid);
    SetWord(bytes, GetEntryField(2, format::ENTRY_OFFSET_FIELD), 8);
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);
    REQUIRE(diagnostic.Offset == GetEntryField(2, format::ENTRY_OFFSET_FIELD));

    bytes = Copy(valid);
    SetWord(bytes, GetEntryField(3, format::ENTRY_LENGTH_FIELD), 6);
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);

    bytes = Copy(valid);
    SetWord(bytes, GetEntryField(1, format::ENTRY_LENGTH_FIELD), format::MAX_SPELLING_BYTES + 1);
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::LimitExceeded);

    bytes = Copy(valid);
    bytes[blob + 6] = 'x'; // "alpha" terminator
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);
    REQUIRE(diagnostic.Offset == blob + 6);

    bytes = Copy(valid);
    bytes[blob + 9] = 0; // inside "beta"
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::EmbeddedZero);
    REQUIRE(diagnostic.Offset == blob + 9);

    bytes = Copy(valid);
    bytes[blob + 2] = 0xc3; // "a" + "lpha": truncated two-byte sequence
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::InvalidUtf8);
    REQUIRE(diagnostic.Offset == blob + 2);

    bytes = Copy(valid);
    bytes[blob + 7] = 'a'; // "beta" -> "aeta" sorts before "alpha"
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);
    REQUIRE(diagnostic.Offset == GetEntryField(2, 0));

    // A duplicate spelling is not strictly increasing.
    constexpr std::string_view DUPLICATE_SOURCE[] = {"ab", "ac"};
    bytes = EncodeSpellings(DUPLICATE_SOURCE, revision, {});
    bytes[GetBlobOffset(bytes) + 5] = 'b';
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);
    REQUIRE(diagnostic.Offset == GetEntryField(2, 0));

    // Entry 0 must be empty: grow it to cover "alpha" and shift nothing else.
    bytes = Copy(valid);
    SetWord(bytes, GetEntryField(0, format::ENTRY_LENGTH_FIELD), 1);
    bytes[blob] = 'q';
    bytes[blob + 1] = 0;
    REQUIRE(Read(bytes, revision, view, diagnostic) == StringDictionaryStatus::Malformed);

    // Unused blob bytes after the last terminator are rejected.
    Array<uint8> padded;
    REQUIRE(padded.TryResize(valid.GetSize() + 1) == true);
    for (usize i = 0; i < valid.GetSize(); ++i)
    {
        padded[i] = valid[i];
    }
    padded[valid.GetSize()] = 0;
    SetWord(padded, format::BLOB_BYTES_OFFSET, GetWord(valid, format::BLOB_BYTES_OFFSET) + 1);
    REQUIRE(Read(padded, revision, view, diagnostic) == StringDictionaryStatus::Malformed);
    REQUIRE(diagnostic.Offset == valid.GetSize());
    REQUIRE(view.IsValid() == false);
}

TEST_CASE("Reader accepts unaligned input and survives corruption at every byte", "[string_dictionary]")
{
    const RevisionDigest revision = MakeRevision(2);
    const Array<uint8> valid = EncodeSpellings(SAMPLE_SPELLINGS, revision, {});
    Array<uint8> shifted;
    REQUIRE(shifted.TryResize(valid.GetSize() + 1) == true);
    for (usize i = 0; i < valid.GetSize(); ++i)
    {
        shifted[i + 1] = valid[i];
    }
    StringDictionaryView view;
    StringDictionaryDiagnostic diagnostic;
    REQUIRE(ReadStringDictionary({shifted.GetData() + 1, valid.GetSize()}, revision, view, diagnostic) ==
            StringDictionaryStatus::Ok);
    StringIndex index;
    REQUIRE(view.TryFind("mesh/lod1", index) == StringDictionaryStatus::Ok);
    REQUIRE(index.Value == 5);

    constexpr uint8 FLIPS[] = {0x01, 0x80, 0xff};
    for (usize offset = 0; offset < valid.GetSize(); ++offset)
    {
        for (const uint8 flip : FLIPS)
        {
            Array<uint8> bytes = Copy(valid);
            bytes[offset] = static_cast<uint8>(bytes[offset] ^ flip);
            StringDictionaryView corrupt;
            const StringDictionaryStatus status = Read(bytes, revision, corrupt, diagnostic);
            if (status == StringDictionaryStatus::Ok)
            {
                // Any accepted variant still satisfies the canonical contract.
                std::string_view previous;
                for (uint32 i = 0; i < corrupt.GetEntryCount(); ++i)
                {
                    std::string_view spelling;
                    REQUIRE(corrupt.TryResolve(StringIndex{i}, spelling) == StringDictionaryStatus::Ok);
                    REQUIRE((i == 0 ? spelling.empty() : spelling.compare(previous) > 0) == true);
                    previous = spelling;
                }
            }
            else
            {
                REQUIRE(corrupt.IsValid() == false);
                REQUIRE(diagnostic.Status == status);
                REQUIRE(diagnostic.Offset <= bytes.GetSize());
            }
        }
    }
}
