#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <initializer_list>
#include <ludus/content/content.h>
#include <ludus/content/limits.h>
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/parsing/parsing.hpp>
namespace ludus::content
{
struct JsonValue;
}
#include <ludus/content/json.h>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>

using namespace ludus::content;
namespace
{
class TemporaryContentRoot final
{
public:
    TemporaryContentRoot()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path();
        std::random_device random;
        const std::filesystem::path candidate = directory / ("ludus content " + std::to_string(random()));
        REQUIRE(std::filesystem::create_directory(candidate) == true);
        mPath = candidate;
    }

    ~TemporaryContentRoot()
    {
        std::error_code error;
        (void)std::filesystem::remove_all(mPath, error);
    }

    [[nodiscard]] std::string GetUtf8Path() const
    {
        const std::u8string bytes = mPath.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

private:
    std::filesystem::path mPath;
};
} // namespace

static_assert(std::is_same_v<decltype(&JsonValue::Get), JsonValue (JsonValue::*)(const char*) const noexcept>);
TEST_CASE("Catalog preserves identity and rejects malformed replacements")
{
    Catalog catalog;
    Diagnostic diagnostic;
    const char* json =
        R"({"version":1,"resources":[{"id":"sound/hit","kind":"sound","path":"hit.sound.json"},{"id":"audio/hit","kind":"audio-source","path":"hit.wav"}]})";
    REQUIRE(catalog.Read(json, diagnostic) == Status::Ok);
    REQUIRE(catalog.Entries()[0].Id.View() == "audio/hit");
    Bytes canonical;
    REQUIRE(catalog.Write(canonical) == Status::Ok);
    Catalog reopened;
    REQUIRE(reopened.Read(canonical.String(), diagnostic) == Status::Ok);
    Bytes second;
    REQUIRE(reopened.Write(second) == Status::Ok);
    REQUIRE(canonical.String() == second.String());
    for (const char* invalid :
         {R"({"version":1,"version":1,"resources":[]})",
          R"({"version":3,"resources":[]})",
          R"({"version":1,"resources":[{"id":"audio/x","kind":"audio-source","path":"../x.wav"}]})"})
    {
        CAPTURE(invalid);
        REQUIRE(catalog.Read(invalid, diagnostic) != Status::Ok);
        REQUIRE(catalog.Find("sound/hit") != nullptr);
    }
}
namespace
{
constexpr std::string_view ORIGINAL_CATALOG =
    R"({"version":1,"resources":[{"id":"audio/original","kind":"audio-source","path":"original.wav"}]})";
constexpr std::string_view REPLACEMENT_CATALOG =
    R"({"version":2,"resources":[{"id":"mesh/replacement","kind":"mesh","path":"mesh.json"},{"id":"geometry/replacement","kind":"cooked-geometry","path":"curve\u002elugeo"}]})";

struct CatalogAllocator final
{
    usize Attempts = 0;
    usize FailOnAttempt = 0;

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        CatalogAllocator& allocator = *static_cast<CatalogAllocator*>(context);
        ++allocator.Attempts;
        if (allocator.Attempts == allocator.FailOnAttempt)
        {
            return nullptr;
        }
        return GetSystemAllocationDomain().TryAllocate(bytes, alignment);
    }

    static void Free(void*, void* pointer, usize bytes, usize alignment) noexcept
    {
        GetSystemAllocationDomain().Free(pointer, bytes, alignment);
    }
};

std::string MakeCatalog(usize count)
{
    std::string json = R"({"version":1,"resources":[)";
    const usize indexWidth = std::to_string(MAX_RESOURCES).size();
    for (usize index = 0; index < count; ++index)
    {
        if (index != 0)
        {
            json += ',';
        }
        json += R"({"id":"audio/item-)";
        const std::string indexText = std::to_string(index);
        json.append(indexWidth - indexText.size(), '0');
        json += indexText;
        json += R"(","kind":"audio-source","path":"audio/item.wav"})";
    }
    json += "]}";
    return json;
}
} // namespace

TEST_CASE("Catalog reports semantic paths and preserves the complete previous catalog", "[content][catalog]")
{
    CatalogAllocator allocator;
    const AllocationDomain domain(&allocator, CatalogAllocator::Allocate, CatalogAllocator::Free);
    Catalog catalog(domain);
    Diagnostic diagnostic;
    REQUIRE(catalog.Read(ORIGINAL_CATALOG, diagnostic) == Status::Ok);
    const Resource* previous = catalog.Find("audio/original");
    REQUIRE(previous != nullptr);
    Bytes original;
    REQUIRE(catalog.Write(original) == Status::Ok);

    struct RejectedCatalog final
    {
        std::string_view Json;
        Status Result;
        std::string_view Field;
    };
    const std::initializer_list<RejectedCatalog> rejections = {
        {R"([])", Status::Invalid, "$"},
        {R"({"resources":[]})", Status::Invalid, "version"},
        {R"({"version":3,"resources":[]})", Status::Invalid, "version"},
        {R"({"version":1})", Status::Invalid, "resources"},
        {R"({"version":1,"resources":{}})", Status::Limit, "resources"},
        {R"({"version":1,"resources":[],"extra":0})", Status::Invalid, "$"},
        {R"({"version":1,"resources":[null]})", Status::Invalid, "resources[0]"},
        {R"({"version":1,"resources":[{"kind":"mesh","path":"mesh.json"}]})", Status::Invalid, "resources[0].id"},
        {R"({"version":1,"resources":[{"id":"mesh/a","kind":"mesh","path":17}]})",
         Status::Invalid,
         "resources[0].path"},
        {R"({"version":1,"resources":[{"id":"mesh/a","kind":"cooked-geometry","path":"a.lugeo"}]})",
         Status::Invalid,
         "resources[0].kind"},
        {R"({"version":1,"resources":[{"id":"mesh/a","kind":"mesh","path":"mesh.json","extra":0}]})",
         Status::Invalid,
         "resources[0]"},
        {R"({"version":1,"resources":[{"id":"mesh/a","kind":"mesh","path":"a.json"},{"id":"mesh/a","kind":"mesh","path":"b.json"}]})",
         Status::Invalid,
         "resources[1].id"},
    };
    for (const RejectedCatalog& rejected : rejections)
    {
        CAPTURE(rejected.Json);
        CHECK(catalog.Read(rejected.Json, diagnostic) == rejected.Result);
        CHECK(diagnostic.Result == rejected.Result);
        CHECK(std::string_view(diagnostic.Field) == rejected.Field);
        CHECK(diagnostic.Offset == ludus::foundation::parsing::UNKNOWN_BYTE_OFFSET);
        CHECK(catalog.Entries().data() == previous);
        CHECK(GetAllocationStatistics(domain).LiveRequestedBytes == sizeof(Resource));
    }

    // A bad field after several valid entries reports the authored index,
    // even though successful publication would sort those entries by ID.
    std::string lateFailure = MakeCatalog(11);
    const usize lastPath = lateFailure.rfind("audio/item.wav");
    REQUIRE(lastPath != std::string::npos);
    lateFailure.replace(lastPath, std::string_view("audio/item.wav").size(), "../item.wav");
    CHECK(catalog.Read(lateFailure, diagnostic) == Status::Invalid);
    CHECK(std::string_view(diagnostic.Field) == "resources[10].path");
    CHECK(GetAllocationStatistics(domain).LiveRequestedBytes == sizeof(Resource));

    CHECK(catalog.Read(R"({"version":1,"resources":[?]})", diagnostic) == Status::Invalid);
    CHECK(std::string_view(diagnostic.Field).empty());
    CHECK(diagnostic.Offset == std::string_view(R"({"version":1,"resources":[)").size());

    CHECK(catalog.Read(MakeCatalog(MAX_RESOURCES + 1), diagnostic) == Status::Limit);
    CHECK(std::string_view(diagnostic.Field) == "resources");
    CHECK(catalog.Entries().data() == previous);
    CHECK(GetAllocationStatistics(domain).LiveRequestedBytes == sizeof(Resource));
    Bytes after;
    REQUIRE(catalog.Write(after) == Status::Ok);
    CHECK(after.String() == original.String());
}

TEST_CASE("Catalog allocation failures leave the live entries intact and allow retry", "[content][catalog]")
{
    CatalogAllocator allocator;
    const AllocationDomain domain(&allocator, CatalogAllocator::Allocate, CatalogAllocator::Free);
    {
        Catalog catalog(domain);
        Diagnostic diagnostic;
        REQUIRE(catalog.Read(ORIGINAL_CATALOG, diagnostic) == Status::Ok);
        const Resource* previous = catalog.Find("audio/original");
        REQUIRE(previous != nullptr);
        const usize originalBytes = GetAllocationStatistics(domain).LiveRequestedBytes;
        const usize workspaceAttempt = 1;
        const usize candidateAttempt = 2;
        for (const usize attempt : {workspaceAttempt, candidateAttempt})
        {
            CAPTURE(attempt);
            allocator.Attempts = 0;
            allocator.FailOnAttempt = attempt;
            CHECK(catalog.Read(REPLACEMENT_CATALOG, diagnostic) == Status::OutOfMemory);
            CHECK(diagnostic.Result == Status::OutOfMemory);
            CHECK(std::string_view(diagnostic.Field) == (attempt == workspaceAttempt ? "" : "resources"));
            CHECK(allocator.Attempts == attempt);
            REQUIRE(catalog.Entries().size() == 1);
            REQUIRE(catalog.Find("audio/original") == previous);
            CHECK(previous->Id.View() == "audio/original");
            CHECK(previous->Type == Kind::AudioSource);
            CHECK(previous->Path.View() == "original.wav");
            CHECK(catalog.Find("mesh/replacement") == nullptr);
            const AllocationStatistics statistics = GetAllocationStatistics(domain);
            CHECK(statistics.LiveRequestedBytes == originalBytes);
            CHECK(statistics.LiveBlocks == 1);
            CHECK(statistics.Validity == AccountingValidity::Valid);
        }

        allocator.FailOnAttempt = 0;
        {
            std::string source(REPLACEMENT_CATALOG);
            REQUIRE(catalog.Read(source, diagnostic) == Status::Ok);
            source.assign(source.size(), 'x');
        }
        CHECK(diagnostic.Result == Status::Ok);
        CHECK(std::string_view(diagnostic.Field).empty());
        CHECK(catalog.Find("audio/original") == nullptr);
        REQUIRE(catalog.Entries().size() == 2);
        CHECK(catalog.Entries()[0].Id.View() == "geometry/replacement");
        CHECK(catalog.Entries()[0].Path.View() == "curve.lugeo");
        CHECK(catalog.Entries()[0].Type == Kind::CookedGeometry);
        CHECK(GetAllocationStatistics(domain).FailureCount == 2);
        CHECK(GetAllocationStatistics(domain).LiveRequestedBytes == 2 * sizeof(Resource));

        Catalog moved(Move(catalog));
        CHECK(catalog.Entries().empty());
        CHECK(moved.Entries().size() == 2);
        Catalog replacement;
        replacement = Move(moved);
        CHECK(moved.Entries().empty());
        REQUIRE(replacement.Read(R"({"version":1,"resources":[]})", diagnostic) == Status::Ok);
        CHECK(replacement.Entries().empty());
    }
    CHECK(GetAllocationStatistics(domain).LiveRequestedBytes == 0);
    CHECK(GetAllocationStatistics(domain).Validity == AccountingValidity::Valid);
}

TEST_CASE("Catalog measures peak requested bytes for a bounded replacement", "[content][catalog]")
{
    usize count = 2;
    bool shouldPadInput = false;
    SECTION("small authored fixture")
    {
        count = 2;
    }
    SECTION("at both the record and encoded byte limits")
    {
        count = MAX_RESOURCES;
        shouldPadInput = true;
    }
    std::string input = count == MAX_RESOURCES ? MakeCatalog(count) : std::string(REPLACEMENT_CATALOG);
    if (shouldPadInput == true)
    {
        REQUIRE(input.size() <= MAX_DOCUMENT_BYTES);
        input.resize(MAX_DOCUMENT_BYTES, ' ');
    }

    CatalogAllocator allocator;
    const AllocationDomain domain(&allocator, CatalogAllocator::Allocate, CatalogAllocator::Free);
    Catalog catalog(domain);
    Resource previous;
    REQUIRE(previous.Id.Set("audio/original") == true);
    REQUIRE(previous.Path.Set("original.wav") == true);
    REQUIRE(catalog.Put(previous) == Status::Ok);
    const AllocationStatistics before = GetAllocationStatistics(domain);

    Diagnostic diagnostic;
    REQUIRE(catalog.Read(input, diagnostic) == Status::Ok);
    const AllocationStatistics after = GetAllocationStatistics(domain);
    const usize candidateBytes = count * sizeof(Resource);
    REQUIRE(after.AllocationCount - before.AllocationCount == 2);
    REQUIRE(after.LiveRequestedBytes == candidateBytes);
    REQUIRE(after.PeakRequestedBytes > before.LiveRequestedBytes + candidateBytes);
    CHECK(after.PeakRequestedBytes == before.LiveRequestedBytes + after.AllocatedBytes - before.AllocatedBytes);
    const usize readPeakBytes = after.PeakRequestedBytes - before.LiveRequestedBytes;
    const usize workspaceBytes = readPeakBytes - candidateBytes;
    CAPTURE(count, input.size(), candidateBytes, workspaceBytes, readPeakBytes, after.PeakRequestedBytes);
    CHECK(catalog.Entries().size() == count);
    CHECK(after.LiveBlocks == 1);
    CHECK(after.Validity == AccountingValidity::Valid);
    CHECK(workspaceBytes <= ludus::foundation::parsing::JsonLimits{}.MaxWorkspaceBytes);
}

TEST_CASE("SHA256 matches published empty and abc vectors")
{
    const Digest empty = Hash({});
    REQUIRE(empty.Data[0] == 0xe3);
    REQUIRE(empty.Data[31] == 0x55);
    const uint8 abc[] = {'a', 'b', 'c'};
    const auto digest = Hash(abc);
    const uint8 expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                              0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                              0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    for (usize i = 0; i < 32; ++i)
    {
        REQUIRE(digest.Data[i] == expected[i]);
    }
}
TEST_CASE("SHA256 matches multi-block published vectors for every split")
{
    // FIPS 180-4 examples: the 56-byte message needs a second padding block.
    const std::string twoBlock = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const uint8 twoBlockExpected[] = {0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
                                      0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
                                      0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
    const std::span<const uint8> message(reinterpret_cast<const uint8*>(twoBlock.data()), twoBlock.size());
    for (usize split = 0; split <= message.size(); ++split)
    {
        Hasher hasher;
        REQUIRE(hasher.Add(message.first(split)) == true);
        REQUIRE(hasher.Add(message.subspan(split)) == true);
        const Digest digest = hasher.Finish();
        CAPTURE(split);
        REQUIRE(std::equal(digest.Data, digest.Data + 32, twoBlockExpected) == true);
    }
    const uint8 millionExpected[] = {0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
                                     0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
                                     0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};
    const std::string million(1000000, 'a');
    const std::span<const uint8> bytes(reinterpret_cast<const uint8*>(million.data()), million.size());
    const Digest oneShot = Hash(bytes);
    REQUIRE(std::equal(oneShot.Data, oneShot.Data + 32, millionExpected) == true);
    // Uneven chunk sizes exercise buffered partial blocks and in-place whole blocks.
    Hasher chunked;
    usize offset = 0;
    for (usize chunk = 1; offset < bytes.size(); chunk = chunk * 7 % 1021 + 1)
    {
        const usize take = chunk < bytes.size() - offset ? chunk : bytes.size() - offset;
        REQUIRE(chunked.Add(bytes.subspan(offset, take)) == true);
        offset += take;
    }
    REQUIRE(chunked.Finish() == oneShot);
}
TEST_CASE("Root bounded saves preserve conflicts and accept UTF-8 names")
{
    const TemporaryContentRoot root;
    const std::string path = root.GetUtf8Path();
    const uint8 a[] = {'a'}, b[] = {'b'};
    REQUIRE(SaveFile(path, "document.json", a, nullptr) == Status::Ok);
    Bytes data;
    REQUIRE(ReadFile(path, "document.json", 10, data) == Status::Ok);
    const auto digest = Hash(data.Data());
    REQUIRE(SaveFile(path, "document.json", b, &digest) == Status::Ok);
    REQUIRE(SaveFile(path, "document.json", a, &digest) == Status::Conflict);
    REQUIRE(ReadFile(path, "../secret", 10, data) == Status::Invalid);
    REQUIRE(ReadFile(path, "document.json", 10, data) == Status::Ok);
    CHECK(data.Data()[0] == 'b');
    REQUIRE(SaveFile(path, "音.json", a, nullptr) == Status::Ok);
    CHECK(SaveFile(path, "音.json", b, nullptr) == Status::Conflict);
    REQUIRE(ReadFile(path, "音.json", 10, data) == Status::Ok);
    CHECK(data.Data()[0] == 'a');
#if LUDUS_TARGET_OS != LUDUS_OS_WINDOWS
    const std::filesystem::path outside = std::filesystem::temp_directory_path();
    std::filesystem::create_directory_symlink(outside, std::filesystem::path(path) / "escape");
    REQUIRE(ReadFile(path, "escape/x", 10, data) != Status::Ok);
#endif
}
TEST_CASE("Descriptor-bound revisions survive atomic replacement and detect in-place changes")
{
    const TemporaryContentRoot root;
    const std::string directory = root.GetUtf8Path();
    const uint8 first[] = {'a', 'b', 'c'}, second[] = {'d', 'e'};
    REQUIRE(SaveFile(directory, "source.wav", first, nullptr) == Status::Ok);
    FileReader reader, clone;
    REQUIRE(reader.Open(directory, "source.wav") == Status::Ok);
    REQUIRE(reader.Clone(clone) == Status::Ok);
    const auto digest = Hash(first);
    REQUIRE(SaveFile(directory, "source.wav", second, &digest) == Status::Ok);
    uint8 bytes[3];
    usize count = 0;
    REQUIRE(reader.Read(bytes, count) == Status::Ok);
    REQUIRE(count == 3);
    REQUIRE(bytes[0] == 'a');
    REQUIRE(clone.Read(bytes, count) == Status::Ok);
    REQUIRE(bytes[0] == 'a');
    FileReader current;
    REQUIRE(current.Open(directory, "source.wav") == Status::Ok);
    REQUIRE(current.Size() == 2);
    REQUIRE(ValidPath(std::string_view("bad\xff", 4)) == false);
    REQUIRE(ValidPath(std::string_view("bad\xc0\x80", 5)) == false);
    REQUIRE(ValidPath("audio/音.wav"));
    ResourceId corrupt;
    corrupt.Length = 129;
    REQUIRE(corrupt.View().empty());
    Hasher partial;
    REQUIRE(partial.Add({first, 1}));
    REQUIRE(partial.Add({first + 1, 2}));
    REQUIRE(partial.Finish() == Hash(first));
}

TEST_CASE("file-size admission respects the cap and preserves prior output", "[primitive][content]")
{
    const TemporaryContentRoot root;
    const std::string directory = root.GetUtf8Path();
    const uint8 content[] = {1, 2, 3};
    REQUIRE(SaveFile(directory, "bytes.bin", content, nullptr) == Status::Ok);
    Bytes output;
    REQUIRE(output.Resize(2));
    output.Data()[0] = 0xAA;
    output.Data()[1] = 0xBB;
    REQUIRE(ReadFile(directory, "bytes.bin", 2, output) == Status::Limit);
    REQUIRE(output.Data().size() == 2);
    REQUIRE(output.Data()[0] == 0xAA);
    REQUIRE(output.Data()[1] == 0xBB);
    REQUIRE(ReadFile(directory, "bytes.bin", 3, output) == Status::Ok);
    REQUIRE(output.Data().size() == 3);
    REQUIRE(output.Data()[2] == 3);
}

TEST_CASE("Filesystem-backed content reads preserve capped failures and empty data")
{
    const TemporaryContentRoot root;
    const std::string directory = root.GetUtf8Path();
    const uint8 initial[] = {'a', 'b', 'c'};
    REQUIRE(SaveFile(directory, "data", initial, nullptr) == Status::Ok);
    Bytes output;
    REQUIRE(output.Resize(1));
    output.Data()[0] = 'z';
    REQUIRE(ReadFile(directory, "data", 2, output) == Status::Limit);
    REQUIRE(output.Data().size() == 1);
    REQUIRE(output.Data()[0] == 'z');
    REQUIRE(ReadFile(directory, "absent/data", 3, output) == Status::NotFound);
    REQUIRE(output.Data()[0] == 'z');
    REQUIRE(SaveFile(directory, "empty", {}, nullptr) == Status::Ok);
    REQUIRE(ReadFile(directory, "empty", 0, output) == Status::Ok);
    REQUIRE(output.Data().empty());
    FileReader reader;
    REQUIRE(reader.Open(directory, "empty") == Status::Ok);
    usize count = 99;
    uint8 bytes[1]{};
    REQUIRE(reader.Read(bytes, count) == Status::Ok);
    REQUIRE(count == 0);
    REQUIRE(reader.Seek(-1, true) == Status::Invalid);
    REQUIRE(reader.Seek(-9223372036854775807LL - 1, true) == Status::Invalid);
}

TEST_CASE("Content JSON adapter preserves document limits and numeric behavior", "[content][parsing]")
{
    Diagnostic diagnostic;
    JsonDocument document;
    REQUIRE(document.Read({}, diagnostic) == Status::Limit);
    REQUIRE(document.Read(R"({"a":1,"\u0061":2})", diagnostic) == Status::Invalid);
    REQUIRE(document.Root().Value == nullptr);
    REQUIRE(document.Read("{", diagnostic) == Status::Invalid);
    REQUIRE(document.Root().Value == nullptr);
    const std::string exact = std::string(32, '[') + "0" + std::string(32, ']');
    REQUIRE(document.Read("[" + exact + "]", diagnostic) == Status::Invalid);
    REQUIRE(document.Root().Value == nullptr);
    REQUIRE(document.Read(exact, diagnostic) == Status::Ok);
    const auto root = document.Root();
    REQUIRE(document.Read("0", diagnostic) == Status::Limit);
    REQUIRE(document.Root().Value == root.Value);

    JsonDocument members;
    std::string object = "{";
    for (usize i = 0; i < 33; ++i)
    {
        if (i != 0)
        {
            object += ',';
        }
        object += '"';
        object += std::to_string(i);
        object += "\":0";
    }
    object += '}';
    REQUIRE(members.Read(object, diagnostic) == Status::Invalid);
    REQUIRE(members.Root().Value == nullptr);
    object = object.substr(0, object.rfind(',')) + '}';
    REQUIRE(members.Read(object, diagnostic) == Status::Ok);

    JsonDocument numbers;
    REQUIRE(numbers.Read("[18446744073709551615,18446744073709551616,-1,1.0]", diagnostic) == Status::Ok);
    uint64 number = 7;
    REQUIRE(numbers.Root().At(0).Integer(number));
    REQUIRE(number == ~uint64{0});
    REQUIRE_FALSE(numbers.Root().At(1).Integer(number));
    REQUIRE_FALSE(numbers.Root().At(2).Integer(number));
    REQUIRE_FALSE(numbers.Root().At(3).Integer(number));
    float64 real = 0;
    REQUIRE(numbers.Root().At(1).Number(real));
}

TEST_CASE("Content JSON canonical output and failed writes preserve the caller", "[content][parsing]")
{
    uint8 storage[128]{};
    JsonWriter writer(storage);
    writer.Raw("{\"s\":");
    writer.String("a\n\"\\");
    writer.Raw(",\"n\":");
    writer.Number(1.25);
    writer.Raw(",\"b\":");
    writer.Boolean(false);
    writer.Raw("}");
    Bytes output;
    REQUIRE(writer.Finish(output) == Status::Ok);
    REQUIRE(output.String() == "{\"s\":\"a\\u000a\\\"\\\\\",\"n\":1.25,\"b\":false}\n");
    REQUIRE(writer.Finish(output) == Status::Ok);
    REQUIRE(output.String().back() == '\n');
    JsonWriter tooSmall(std::span<uint8>{storage, 1});
    tooSmall.Integer(1);
    REQUIRE(tooSmall.Finish(output) == Status::Limit);
    REQUIRE(output.String().starts_with("{\"s\":"));
    JsonWriter invalid(storage);
    invalid.String("\xff");
    REQUIRE(invalid.Finish(output) == Status::Limit);
    REQUIRE(output.String().starts_with("{\"s\":"));
}

TEST_CASE("Content JSON byte admission uses its own inclusive document policy", "[content][parsing][limits]")
{
    std::string input(MAX_DOCUMENT_BYTES, ' ');
    input[0] = '0';
    Diagnostic diagnostic;
    JsonDocument exact;
    REQUIRE(exact.Read(input, diagnostic) == Status::Ok);
    JsonDocument oversized;
    input += ' ';
    REQUIRE(oversized.Read(input, diagnostic) == Status::Limit);
    REQUIRE(oversized.Root().Value == nullptr);
    input.pop_back();
    REQUIRE(oversized.Read(input, diagnostic) == Status::Ok);
}
