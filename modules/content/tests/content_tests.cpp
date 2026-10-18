#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <ludus/content/content.h>
#include <ludus/content/limits.h>
#include <ludus/foundation/base/config.h>
namespace ludus::content
{
struct JsonValue;
}
#include <ludus/content/json.h>
#include <string>
#include <type_traits>
#include <unistd.h>

using namespace ludus::content;
static_assert(std::is_same_v<decltype(&JsonValue::Get), JsonValue (JsonValue::*)(const char*) const noexcept>);
TEST_CASE("Catalog preserves identity and rejects malformed replacements")
{
    Catalog catalog;
    Diagnostic d;
    const auto json =
        R"({"version":1,"resources":[{"id":"sound/hit","kind":"sound","path":"hit.sound.json"},{"id":"audio/hit","kind":"audio-source","path":"hit.wav"}]})";
    REQUIRE(catalog.Read(json, d) == Status::Ok);
    REQUIRE(catalog.Entries()[0].Id.View() == "audio/hit");
    Bytes canonical;
    REQUIRE(catalog.Write(canonical) == Status::Ok);
    Catalog reopened;
    REQUIRE(reopened.Read(canonical.String(), d) == Status::Ok);
    Bytes second;
    REQUIRE(reopened.Write(second) == Status::Ok);
    REQUIRE(canonical.String() == second.String());
    for (const auto* invalid :
         {R"({"version":1,"version":1,"resources":[]})",
          R"({"version":2,"resources":[]})",
          R"({"version":1,"resources":[{"id":"audio/x","kind":"audio-source","path":"../x.wav"}]})"})
    {
        REQUIRE(catalog.Read(invalid, d) != Status::Ok);
        REQUIRE(catalog.Find("sound/hit") != nullptr);
    }
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
TEST_CASE("Root bounded saves preserve conflicts and reject symlink traversal")
{
    const auto root = std::filesystem::temp_directory_path() / std::string("ludus-content-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    const auto path = root.string();
    const uint8 a[] = {'a'}, b[] = {'b'};
    REQUIRE(SaveFile(path, "document.json", a, nullptr) == Status::Ok);
    Bytes data;
    REQUIRE(ReadFile(path, "document.json", 10, data) == Status::Ok);
    const auto digest = Hash(data.Data());
    REQUIRE(SaveFile(path, "document.json", b, &digest) == Status::Ok);
    REQUIRE(SaveFile(path, "document.json", a, &digest) == Status::Conflict);
    REQUIRE(ReadFile(path, "../secret", 10, data) == Status::Invalid);
    std::filesystem::create_directory_symlink("/tmp", root / "escape");
    REQUIRE(ReadFile(path, "escape/x", 10, data) != Status::Ok);
    std::filesystem::remove_all(root);
}
TEST_CASE("Descriptor-bound revisions survive atomic replacement and detect in-place changes")
{
    char directory[] = "/tmp/ludus-file-reader-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
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
    std::filesystem::remove_all(directory);
}

TEST_CASE("file-size admission respects the cap and preserves prior output", "[primitive][content]")
{
    char directory[] = "/tmp/ludus-file-size-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
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
    std::filesystem::remove_all(directory);
}

TEST_CASE("Filesystem-backed content reads preserve capped failures and empty data")
{
    char directory[] = "/tmp/ludus-content-capped-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
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
    std::filesystem::remove_all(directory);
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
