// Golden and behaviour tests for the native scene asset catalog (scene_assets.py port).

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/runtime/resources/scene.h>

#include "python_json.h"
#include "uuid.h"

#include <ludus/foundation/parsing/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
using namespace ludus::foundation;
namespace content = ludus::content;
namespace detail = ludus::content::scene_cook::detail;
using namespace ludus::content::scene_cook;

constexpr std::string_view GOLDENS = LUDUS_SCENE_GOLDENS_DIR;
constexpr std::string_view VIRTUAL_ORIGIN = "/ludus-golden/source";

std::string ToText(const Array<uint8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

std::span<const uint8> ToSpan(const Array<uint8>& bytes)
{
    return {bytes.GetData(), bytes.GetSize()};
}

std::span<const uint8> ToSpan(std::string_view text)
{
    return {reinterpret_cast<const uint8*>(text.data()), text.size()};
}

std::string HexOf(std::string_view data)
{
    static constexpr char HEX[] = "0123456789abcdef";
    const content::Digest digest = content::Hash(ToSpan(data));
    std::string text;
    for (const uint8 value : digest.Data)
    {
        text += HEX[value >> 4];
        text += HEX[value & 0xf];
    }
    return text;
}

std::string ReadWhole(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

struct GoldenRecord
{
    std::string Sha256;
    uint64 Bytes = 0;
    bool IsInline = false;
};

/// expected/goldens.json "outputs".
std::map<std::string, GoldenRecord> ReadGoldenOutputs()
{
    const std::string text = ReadWhole(std::filesystem::path(GOLDENS) / "expected/goldens.json");
    parsing::JsonDocument document;
    parsing::ParseError error{};
    parsing::JsonLimits limits;
    limits.MaxObjectMembers = parsing::MAX_JSON_OBJECT_MEMBERS;
    limits.MaxInputBytes = usize{16} << 20;
    limits.MaxDepth = 64;
    REQUIRE(document.Read(text, error, limits) == parsing::ParseStatus::Ok);
    const parsing::JsonValue outputs = document.Root().Get("outputs");
    std::map<std::string, GoldenRecord> records;
    for (usize i = 0; i < outputs.MemberCount(); ++i)
    {
        std::string_view name;
        parsing::JsonValue value;
        REQUIRE(outputs.MemberAt(i, name, value));
        GoldenRecord record;
        std::string_view sha;
        REQUIRE(value.Get("sha256").String(sha));
        REQUIRE(value.Get("bytes").Integer(record.Bytes));
        REQUIRE(value.Get("inline").Boolean(record.IsInline));
        record.Sha256 = std::string(sha);
        records.emplace(std::string(name), record);
    }
    return records;
}

/// Compare one produced file with its golden record and, when inline, its stored bytes.
void CheckGolden(const std::map<std::string, GoldenRecord>& goldens, const std::string& name, const std::string& data)
{
    INFO(name);
    const std::map<std::string, GoldenRecord>::const_iterator found = goldens.find(name);
    REQUIRE(found != goldens.end());
    CHECK(data.size() == found->second.Bytes);
    CHECK(HexOf(data) == found->second.Sha256);
    if (found->second.IsInline == true)
    {
        CHECK(data == ReadWhole(std::filesystem::path(GOLDENS) / "expected/files" / name));
    }
}

/// Every regular file below root, as capture.py's tree() listed them.
std::map<std::string, std::string> ReadTree(const std::filesystem::path& root)
{
    std::map<std::string, std::string> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file())
        {
            files.emplace(entry.path().lexically_relative(root).generic_string(), ReadWhole(entry.path()));
        }
    }
    return files;
}

/// uuid.UUID(int=n) for n = 1, 2, ... in call order.
struct SequentialUuids
{
    uint64 Next = 0;
    static void Generate(void* context, uint8 (&outBytes)[16]) noexcept
    {
        SequentialUuids* self = static_cast<SequentialUuids*>(context);
        const uint64 value = ++self->Next;
        for (usize i = 0; i < 16; ++i)
        {
            outBytes[i] = i < 8 ? 0 : static_cast<uint8>(value >> ((15 - i) * 8));
        }
    }
};

/// Controlled entropy: bytes 0, 1, 2, ... in request order, failing every request
/// after AllowedCalls successful ones.
struct CountingEntropy
{
    usize AllowedCalls = ~usize{0};
    usize Calls = 0;
    uint8 Next = 0;

    static bool Fill(void* contextOrNull, uint8* outBytes, usize size) noexcept
    {
        CountingEntropy* self = static_cast<CountingEntropy*>(contextOrNull);
        ++self->Calls;
        if (self->Calls > self->AllowedCalls)
        {
            return false;
        }
        for (usize i = 0; i < size; ++i)
        {
            outBytes[i] = self->Next++;
        }
        return true;
    }

    [[nodiscard]] identity::EntropySource GetSource() noexcept
    {
        return identity::EntropySource{ .FillBytesOrNull = &Fill, .ContextOrNull = this };
    }
};

std::filesystem::path MakeWorkDirectory()
{
    char directory[] = "/tmp/ludus-scene-assets-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    return directory;
}

std::string Describe(const AssetError& error)
{
    return std::string(GetAssetErrorCodeName(error.Code)) + ": " + std::string(error.Message.View());
}

/// A project with one imported and saved level, using the golden normalisation.
struct GoldenProject
{
    std::filesystem::path Work = MakeWorkDirectory();
    std::filesystem::path Project = Work / "project";
    std::string Source = std::string(GOLDENS) + "/source";
    SequentialUuids Counter;
    UuidSource Uuids{&SequentialUuids::Generate, &Counter};
    AssetOptions Options;
    Array<uint8> Level;
    Array<uint8> Saved;

    GoldenProject()
    {
        Options.ShouldPreserveReferenceMaterials = false;
        Options.UuidsOrNull = &Uuids;
        Options.SourceOrigin = VIRTUAL_ORIGIN;
        std::filesystem::create_directories(Project);
        AssetError error;
        Array<uint8> empty;
        REQUIRE(CreateLevel("Golden", Options, empty, error));
        INFO(Describe(error));
        REQUIRE(ImportScene(Project.string(), Source, ToSpan(empty), Options, Level, error));
        REQUIRE(SaveLevel(Project.string(), "scenes/main.lulevel", ToSpan(Level), nullptr, Saved, error));
    }

    ~GoldenProject()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Work, ignored);
    }
};
} // namespace

TEST_CASE("SHA-1 and uuid5 match hashlib and the uuid module", "[scene_assets]")
{
    const auto sha1 = [](std::string_view text) {
        uint8 digest[20]{};
        detail::ComputeSha1(ToSpan(text), digest);
        static constexpr char HEX[] = "0123456789abcdef";
        std::string hex;
        for (const uint8 value : digest)
        {
            hex += HEX[value >> 4];
            hex += HEX[value & 0xf];
        }
        return hex;
    };
    CHECK(sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1(std::string(55, 'a')) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(sha1(std::string(56, 'a')) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(sha1(std::string(1000, 'a')) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");

    detail::Uuid space;
    space.Bytes[15] = 2;
    detail::Uuid named;
    REQUIRE(detail::MakeNameUuid(space, "mesh/0", named));
    Array<uint8> text;
    detail::ByteWriter writer(text);
    detail::AppendUuid(writer, named);
    CHECK(ToText(text) == "312d1234-1ad2-5894-9ef8-dc2bfad15547");
}

TEST_CASE("uuid.UUID() string forms are accepted as Python accepts them", "[scene_assets]")
{
    detail::Uuid value;
    CHECK(detail::ParseUuid("00000000-0000-0000-0000-00000000000a", value));
    CHECK(value.Bytes[15] == 0x0a);
    CHECK(detail::ParseUuid("urn:uuid:{00000000-0000-0000-0000-00000000000A}", value));
    CHECK(detail::ParseUuid("0000000000000000000000000000000a", value));
    CHECK_FALSE(detail::ParseUuid("00000000-0000-0000-0000-00000000000", value));
    CHECK_FALSE(detail::ParseUuid("g0000000-0000-0000-0000-00000000000a", value));
    CHECK_FALSE(detail::ParseUuid("", value));
}

TEST_CASE("Python json round trip keeps ints, floats, order and escapes", "[scene_assets]")
{
    detail::JsonTree tree;
    detail::JsonIndex root = detail::NO_JSON_NODE;
    content::MessageText message;
    const std::string_view input = R"({"b":[1,1.0,-0.0,2.5e-3,"\ud800x\u00e9",true,null],"a":{}, "c":"é"})";
    REQUIRE(detail::ParsePythonJson(ToSpan(input), tree, root, message));
    Array<uint8> output;
    REQUIRE(detail::WriteCanonicalJson(tree, root, output));
    CHECK(ToText(output) == "{\"a\":{},\"b\":[1,1.0,-0.0,0.0025,\"\\ud800x\\u00e9\",true,null],\"c\":\"\\u00e9\"}\n");
    CHECK(tree.GetKey(root, 0) == "b");

    detail::JsonTree duplicate;
    CHECK_FALSE(detail::ParsePythonJson(ToSpan(std::string_view(R"({"a":1,"a":2})")), duplicate, root, message));
    CHECK(message.View() == "Duplicate JSON field: a");

    detail::JsonTree special;
    REQUIRE(detail::ParsePythonJson(ToSpan(std::string_view("[NaN, Infinity]")), special, root, message));
    CHECK_FALSE(detail::WriteCanonicalJson(special, root, output));
}

TEST_CASE("New levels match the reference document", "[scene_assets]")
{
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    Array<uint8> level;
    AssetError error;
    REQUIRE(CreateLevel("Golden", options, level, error));
    CHECK(ToText(level) ==
          "{\"camera\":{\"eye\":[0,0,3],\"fov\":1.0471975511965976,\"target\":[0,0,0],\"up\":[0,1,0]},"
          "\"id\":\"00000000-0000-0000-0000-000000000001\",\"imports\":[],\"name\":\"Golden\",\"objects\":[],"
          "\"profile\":\"resident-reference-v1\",\"version\":1}\n");
    CHECK(ValidateLevel(ToSpan(level), error));
}

TEST_CASE("New identities are FoundationIdentity UUIDv4 values from the injected entropy", "[scene_assets]")
{
    CountingEntropy entropy;
    const identity::EntropySource entropySource = entropy.GetSource();
    AssetOptions options;
    options.EntropyOrNull = &entropySource;
    Array<uint8> level;
    AssetError error;
    REQUIRE(CreateLevel("Fresh", options, level, error));
    CHECK(entropy.Calls == 1);
    // Bytes 00..0f with the version nibble (byte 6) and variant bits (byte 8) applied.
    CHECK(ToText(level).find("\"id\":\"00010203-0405-4607-8809-0a0b0c0d0e0f\"") != std::string::npos);
    CHECK(ValidateLevel(ToSpan(level), error));
}

TEST_CASE("Entropy failure propagates without partial output", "[scene_assets]")
{
    SECTION("CreateLevel keeps its output when no source is supplied or the source fails")
    {
        CountingEntropy entropy;
        const identity::EntropySource entropySource = entropy.GetSource();
        AssetOptions options;
        options.EntropyOrNull = &entropySource;
        Array<uint8> level;
        AssetError error;
        REQUIRE(CreateLevel("Kept", options, level, error));
        const std::string before = ToText(level);

        AssetOptions missing;
        CHECK_FALSE(CreateLevel("Missing", missing, level, error));
        CHECK(error.Code == AssetErrorCode::MissingTools);
        CHECK(error.Message.View() == "No entropy source for UUIDs");
        CHECK(ToText(level) == before);

        entropy.AllowedCalls = entropy.Calls;
        error = AssetError{};
        CHECK_FALSE(CreateLevel("Failing", options, level, error));
        CHECK(error.Code == AssetErrorCode::MissingTools);
        CHECK(entropy.Calls == 2);
        CHECK(ToText(level) == before);
    }

    SECTION("Import fails at the namespace or at an object UUID and publishes nothing")
    {
        GoldenProject golden;
        const std::string before = ReadWhole(golden.Project / "assets/catalog.json");
        // Zero successful requests fail the import namespace; one fails the first object UUID.
        for (const usize allowedCalls : {usize{0}, usize{1}})
        {
            INFO(allowedCalls);
            CountingEntropy entropy;
            entropy.AllowedCalls = allowedCalls;
            const identity::EntropySource entropySource = entropy.GetSource();
            AssetOptions options = golden.Options;
            options.UuidsOrNull = nullptr;
            options.EntropyOrNull = &entropySource;
            AssetError error;
            Array<uint8> level;
            CHECK_FALSE(
                ImportScene(golden.Project.string(), golden.Source, ToSpan(golden.Level), options, level, error));
            CHECK(error.Code == AssetErrorCode::MissingTools);
            CHECK(entropy.Calls == allowedCalls + 1);
            CHECK(level.GetSize() == 0);
            CHECK(ReadWhole(golden.Project / "assets/catalog.json") == before);
        }
    }
}

TEST_CASE("Scene asset pipeline is byte-identical to the Python goldens", "[scene_assets][goldens]")
{
    const std::map<std::string, GoldenRecord> goldens = ReadGoldenOutputs();
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetError error;

    Array<uint8> reopened;
    REQUIRE(OpenLevel(root, "scenes/main.lulevel", reopened, error));
    CHECK(ToText(reopened) == ToText(golden.Level));

    // A second import of the same origin must reuse logical identities.
    Array<uint8> second;
    Array<uint8> secondImported;
    Array<uint8> secondSaved;
    REQUIRE(CreateLevel("Second", golden.Options, second, error));
    REQUIRE(ImportScene(root, golden.Source, ToSpan(second), golden.Options, secondImported, error));
    REQUIRE(SaveLevel(root, "scenes/second.lulevel", ToSpan(secondImported), nullptr, secondSaved, error));

    Array<uint8> bundle;
    Array<uint8> dump;
    Array<uint8> published;
    REQUIRE(AssembleLevel(root, ToSpan(golden.Level), {}, bundle, error));
    REQUIRE(DumpLevel(root, ToSpan(golden.Level), {}, DumpLayout::Canonical, dump, error));
    const std::filesystem::path release = golden.Work / "release";
    INFO(Describe(error));
    REQUIRE(PublishLevel(root, ToSpan(golden.Level), release.string(), DumpLayout::Canonical, published, error));
    CHECK(ToText(published) == ToText(dump));

    CheckGolden(goldens, "saved/main.lulevel", ToText(golden.Saved));
    CheckGolden(goldens, "assemble/main.luscn", ToText(bundle));
    CheckGolden(goldens, "dump/main.json", ToText(dump));
    usize expectedProject = 0;
    usize expectedRelease = 0;
    for (const std::pair<const std::string, GoldenRecord>& item : goldens)
    {
        const std::string& name = item.first;
        expectedProject += name.starts_with("project/") ? 1 : 0;
        expectedRelease += name.starts_with("release/") ? 1 : 0;
    }
    const std::map<std::string, std::string> project = ReadTree(golden.Project);
    const std::map<std::string, std::string> releaseFiles = ReadTree(release);
    CHECK(project.size() == expectedProject);
    CHECK(releaseFiles.size() == expectedRelease);
    for (const std::pair<const std::string, std::string>& item : project)
    {
        CheckGolden(goldens, "project/" + item.first, item.second);
    }
    for (const std::pair<const std::string, std::string>& item : releaseFiles)
    {
        CheckGolden(goldens, "release/" + item.first, item.second);
    }
}

TEST_CASE("Saving refuses to overwrite changed or unexpected levels", "[scene_assets]")
{
    GoldenProject golden;
    AssetError error;
    Array<uint8> saved;
    CHECK_FALSE(SaveLevel(golden.Project.string(), "scenes/main.lulevel", ToSpan(golden.Level), nullptr, saved, error));
    CHECK(error.Code == AssetErrorCode::Conflict);
    CHECK(SaveLevel(golden.Project.string(), "scenes/main.lulevel", ToSpan(golden.Level), &golden.Saved, saved, error));
    CHECK_FALSE(SaveLevel(golden.Project.string(), "scenes/main.json", ToSpan(golden.Level), nullptr, saved, error));
    CHECK(error.Message.View() == "Levels use the .lulevel extension");
    CHECK_FALSE(SaveLevel(golden.Project.string(), "../escape.lulevel", ToSpan(golden.Level), nullptr, saved, error));
    CHECK(error.Message.View() == "Project asset path must be relative and contained");
}

TEST_CASE("Corrupt artifacts and existing release directories are rejected", "[scene_assets]")
{
    GoldenProject golden;
    AssetError error;
    Array<uint8> dump;
    const std::filesystem::path release = golden.Work / "release";
    std::filesystem::create_directories(release);
    CHECK_FALSE(PublishLevel(golden.Project.string(),
                             ToSpan(golden.Level),
                             release.string(),
                             DumpLayout::Canonical,
                             dump,
                             error));
    CHECK(error.Code == AssetErrorCode::DestinationExists);

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(golden.Project / "assets/objects"))
    {
        if (entry.file_size() < 100)
        {
            std::filesystem::permissions(entry.path(),
                                         std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add);
            std::ofstream(entry.path(), std::ios::binary | std::ios::app) << "x";
            break;
        }
    }
    Array<uint8> level;
    CHECK_FALSE(OpenLevel(golden.Project.string(), "scenes/main.lulevel", level, error));
    CHECK(error.Code == AssetErrorCode::AssetCorrupt);
}

TEST_CASE("Saved-level import publishes the catalog and level together", "[scene_assets]")
{
    GoldenProject golden;
    AssetError error;
    Array<uint8> imported;
    // Reimporting the same origin reuses identities; the new objects receive fresh UUIDs.
    REQUIRE(ImportSavedLevel(golden.Project.string(),
                             "scenes/main.lulevel",
                             golden.Source,
                             golden.Options,
                             imported,
                             error));
    Array<uint8> reopened;
    REQUIRE(OpenLevel(golden.Project.string(), "scenes/main.lulevel", reopened, error));
    CHECK(ToText(reopened) == ToText(imported));
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(golden.Project / "scenes"))
    {
        CHECK(entry.path().filename().string().starts_with(".scene-import-") == false);
    }
}

TEST_CASE("Import cancellation leaves the published catalog untouched", "[scene_assets]")
{
    GoldenProject golden;
    const std::string before = ReadWhole(golden.Project / "assets/catalog.json");
    Cancellation cancellation;
    cancellation.Requested = [](void*) noexcept { return true; };
    AssetOptions options = golden.Options;
    options.CancelOrNull = &cancellation;
    AssetError error;
    Array<uint8> level;
    CHECK_FALSE(ImportScene(golden.Project.string(), golden.Source, ToSpan(golden.Level), options, level, error));
    CHECK(error.Code == AssetErrorCode::InstallCancelled);
    CHECK(ReadWhole(golden.Project / "assets/catalog.json") == before);
}

TEST_CASE("Prepared candidates are rejected after a concurrent catalog change", "[scene_assets]")
{
    GoldenProject golden;
    AssetError error;
    Array<uint8> empty;
    REQUIRE(CreateLevel("Candidate", golden.Options, empty, error));
    ImportCandidate candidate;
    REQUIRE(PrepareImport(golden.Project.string(), golden.Source, ToSpan(empty), golden.Options, candidate, error));
    CHECK(candidate.HasPublishedCatalog);
    std::ofstream(golden.Project / "assets/catalog.json", std::ios::binary | std::ios::app) << " ";
    CHECK_FALSE(CommitImport(golden.Project.string(),
                             ToSpan(candidate.Level),
                             ToSpan(candidate.Catalog),
                             &candidate.PublishedCatalog,
                             golden.Source,
                             golden.Options,
                             error));
    CHECK(error.Code == AssetErrorCode::Conflict);
}

TEST_CASE("Invalid levels report the reference messages", "[scene_assets]")
{
    AssetError error;
    CHECK_FALSE(ValidateLevel(ToSpan(std::string_view("{}")), error));
    CHECK(error.Message.View() == "Invalid level fields");
    const std::string_view degenerate =
        R"({"camera":{"eye":[0,0,0],"fov":1.0,"target":[0,0,0],"up":[0,1,0]},)"
        R"("id":"00000000-0000-0000-0000-000000000001","imports":[],"name":"x","objects":[],)"
        R"("profile":"resident-reference-v1","version":1})";
    CHECK_FALSE(ValidateLevel(ToSpan(degenerate), error));
    CHECK(error.Message.View() == "Degenerate authored camera");
}

namespace
{
/// The `level` member of an Editor response, as canonical bytes.
std::string GetResponseMember(const Array<uint8>& response, std::string_view member)
{
    detail::JsonTree tree;
    detail::JsonIndex root = detail::NO_JSON_NODE;
    content::MessageText message;
    REQUIRE(detail::ParsePythonJson(ToSpan(response), tree, root, message));
    const detail::JsonIndex value = tree.GetMember(root, member);
    REQUIRE(value != detail::NO_JSON_NODE);
    if (tree.GetKind(value) == detail::JsonKind::String)
    {
        return std::string(tree.GetText(value));
    }
    Array<uint8> bytes;
    REQUIRE(detail::WriteCanonicalJson(tree, value, bytes));
    return ToText(bytes);
}

/// Compare one Editor response and its scene with the scene_editor.py capture.
void CheckEditorStep(const EditorSceneResult& result, const std::string& step)
{
    INFO(step);
    const std::filesystem::path editor = std::filesystem::path(GOLDENS) / "expected/files/editor";
    CHECK(ToText(result.Response) == ReadWhole(editor / (step + ".json")) + "\n");
    const std::string scenes = ReadWhole(editor / "scenes.json");
    const usize at = scenes.find("\"" + step + "\"");
    if (at == std::string::npos)
    {
        CHECK(result.Scene.GetSize() == 0);
        return;
    }
    CHECK(scenes.find(HexOf(ToText(result.Scene)), at) == scenes.find("\"sha256\": \"", at) + 11);
}
} // namespace

TEST_CASE("In-process Editor operations match scene_editor.py responses", "[scene_assets][goldens]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    std::filesystem::create_directories(project);
    const std::string root = project.string();
    const std::string source = std::string(GOLDENS) + "/source";
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.ShouldPreserveReferenceMaterials = false;
    options.UuidsOrNull = &uuids;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    AssetError error;
    Array<uint8> level;
    REQUIRE(CreateLevel("Golden", options, level, error));

    usize stages = 0;
    EditorSceneProgress progress{[](void* context, std::string_view) noexcept { ++*static_cast<usize*>(context); },
                                 &stages};
    EditorSceneRequest request;
    request.Root = root;
    request.Source = source;
    request.ProgressOrNull = &progress;

    request.Operation = EditorSceneOperation::Import;
    request.Level = ToSpan(level);
    EditorSceneResult imported;
    INFO(Describe(error));
    REQUIRE(OperateEditorScene(request, options, imported, error));
    CheckEditorStep(imported, "1-import");
    CHECK(ToText(imported.Catalog) ==
          ReadWhole(std::filesystem::path(GOLDENS) / "expected/files/editor/1-import.catalog.json"));
    CHECK(stages > 0);
    CHECK(std::filesystem::exists(project / "assets/catalog.json") == false);

    const std::string candidate = GetResponseMember(imported.Response, "level");
    request.Operation = EditorSceneOperation::CommitImport;
    request.Level = ToSpan(candidate);
    request.Catalog = ToSpan(imported.Catalog);
    request.HasExpected = false;
    EditorSceneResult committed;
    REQUIRE(OperateEditorScene(request, options, committed, error));
    CheckEditorStep(committed, "2-commit");

    request.Operation = EditorSceneOperation::Save;
    request.Path = "scenes/main.lulevel";
    request.Catalog = {};
    EditorSceneResult saved;
    REQUIRE(OperateEditorScene(request, options, saved, error));
    CheckEditorStep(saved, "3-save");

    request.Operation = EditorSceneOperation::Open;
    request.Level = {};
    EditorSceneResult opened;
    REQUIRE(OperateEditorScene(request, options, opened, error));
    CheckEditorStep(opened, "4-open");

    std::string renamed = GetResponseMember(opened.Response, "level");
    const usize name = renamed.find("\"name\":", renamed.find("\"objects\":"));
    REQUIRE(name != std::string::npos);
    renamed.replace(name + 8, renamed.find('"', name + 8) - (name + 8), "Renamed");
    request.Operation = EditorSceneOperation::Assemble;
    request.Level = ToSpan(renamed);
    EditorSceneResult assembled;
    REQUIRE(OperateEditorScene(request, options, assembled, error));
    CheckEditorStep(assembled, "5-assemble");

    const std::string disk = GetResponseMember(opened.Response, "disk");
    request.Operation = EditorSceneOperation::Save;
    request.HasExpected = true;
    request.Expected = disk;
    EditorSceneResult resaved;
    REQUIRE(OperateEditorScene(request, options, resaved, error));
    CheckEditorStep(resaved, "6-save");

    // The bytes on disk changed: the stale digest now conflicts.
    REQUIRE_FALSE(OperateEditorScene(request, options, resaved, error));
    CHECK(error.Code == AssetErrorCode::Conflict);

    bool isCancelled = true;
    Cancellation cancel{[](void* context) noexcept { return *static_cast<bool*>(context); }, &isCancelled};
    AssetOptions cancelling = options;
    cancelling.CancelOrNull = &cancel;
    request.Operation = EditorSceneOperation::Import;
    request.Level = ToSpan(renamed);
    request.HasExpected = false;
    REQUIRE_FALSE(OperateEditorScene(request, cancelling, imported, error));
    CHECK(error.Code == AssetErrorCode::InstallCancelled);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

namespace
{
/// Write a small general pbrt-v4 scene (outside the reference profile) to directory/name.
void WriteBestEffortSource(const std::filesystem::path& directory, std::string_view name)
{
    std::filesystem::create_directories(directory);
    std::ofstream(directory / std::string(name), std::ios::binary)
        << "LookAt 0 1 4  0 0 0  0 1 0\nCamera \"perspective\" \"float fov\" [ 45 ]\n"
           "Film \"rgb\" \"integer xresolution\" [ 64 ] \"integer yresolution\" [ 64 ]\n"
           "Sampler \"halton\"\nWorldBegin\n"
           "LightSource \"infinite\" \"rgb L\" [ .2 .2 .2 ]\n"
           "Material \"diffuse\" \"rgb reflectance\" [ .6 .5 .4 ]\n"
           "Shape \"trianglemesh\" \"point3 P\" [ -1 0 -1  1 0 -1  1 0 1  -1 0 1 ]\n"
           "  \"integer indices\" [ 0 1 2  0 2 3 ]\n"
           "AttributeBegin\nAreaLightSource \"diffuse\" \"rgb L\" [ 4 4 4 ]\n"
           "Shape \"sphere\" \"float radius\" [ .25 ]\nAttributeEnd\n";
    std::ofstream(directory / "LICENSE.txt", std::ios::binary) << "CC0 synthetic best-effort fixture\n";
}
} // namespace

TEST_CASE("Editor import routes general pbrt files to pbrt-best-effort-v1", "[scene_assets]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    const std::filesystem::path source = work / "source";
    std::filesystem::create_directories(project);
    WriteBestEffortSource(source, "room.pbrt");
    const std::string root = project.string();
    const std::string sourceText = source.string();
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    options.SourceEntry = "room.pbrt";
    AssetError error;
    Array<uint8> level;
    REQUIRE(CreateLevel("Best effort", options, level, error));
    EditorSceneRequest request;
    request.Root = root;
    request.Source = sourceText;
    request.Level = ToSpan(level);

    // The dry-run preview writes nothing and describes the planned cook.
    request.Operation = EditorSceneOperation::PreviewImport;
    EditorSceneResult preview;
    INFO(Describe(error));
    REQUIRE(OperateEditorScene(request, options, preview, error));
    const std::string previewText = ToText(preview.Response);
    CHECK(GetResponseMember(preview.Response, "profile") == "pbrt-best-effort-v1");
    CHECK(previewText.find("\"dry_run\":true") != std::string::npos);
    CHECK(previewText.find("ENVIRONMENT_FALLBACK") != std::string::npos);
    CHECK(preview.Scene.IsEmpty() == true);
    CHECK(std::filesystem::exists(project / "assets") == false);

    // Import, commit and reassemble: the stored report comes back with the level.
    request.Operation = EditorSceneOperation::Import;
    EditorSceneResult imported;
    const bool isImported = OperateEditorScene(request, options, imported, error);
    INFO(Describe(error));
    REQUIRE(isImported);
    CHECK(imported.Scene.IsEmpty() == false);
    const std::string candidate = GetResponseMember(imported.Response, "level");
    request.Operation = EditorSceneOperation::CommitImport;
    request.Level = ToSpan(candidate);
    request.Catalog = ToSpan(imported.Catalog);
    EditorSceneResult committed;
    REQUIRE(OperateEditorScene(request, options, committed, error));
    request.Operation = EditorSceneOperation::Assemble;
    request.Catalog = {};
    EditorSceneResult assembled;
    REQUIRE(OperateEditorScene(request, options, assembled, error));
    const std::string reports = GetResponseMember(assembled.Response, "import_reports");
    CHECK(reports.find("\"importer\":\"pbrt-best-effort-v1\"") != std::string::npos);
    CHECK(reports.find("\"dry_run\":false") != std::string::npos);
    CHECK(GetResponseMember(assembled.Response, "warnings").find("ENVIRONMENT_FALLBACK") != std::string::npos);

    // A changed source input is detected before publication.
    std::ofstream(source / "room.pbrt", std::ios::app) << "# edited\n";
    request.Operation = EditorSceneOperation::CommitImport;
    request.Catalog = ToSpan(imported.Catalog);
    EditorSceneResult stale;
    CHECK(OperateEditorScene(request, options, stale, error) == false);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

TEST_CASE("Editor import routes glTF, GLB and OBJ entries to their best-effort profiles", "[scene_assets]")
{
    struct Fixture
    {
        std::string_view Directory;
        std::string_view Entry;
        std::string_view Profile;
        usize MaskedMaterials = 0;
        usize SurfaceMaps = 0;
        usize EmissionMaps = 0;
    };
    const Fixture fixtures[] = {
        {"gltf/instanced", "scene.gltf", "gltf-best-effort-v1", 0, 1},
        {"gltf/embedded", "scene.glb", "gltf-best-effort-v1", 0, 1},
        {"gltf/maps", "scene.gltf", "gltf-best-effort-v1", 0, 1, 1},
        {"obj/room", "room.obj", "obj-best-effort-v1", 0, 1},
        {"obj/cutout", "cutout.obj", "obj-best-effort-v1", 2},
    };
    for (const Fixture& fixture : fixtures)
    {
        INFO(fixture.Entry);
        const std::filesystem::path work = MakeWorkDirectory();
        const std::filesystem::path project = work / "project";
        const std::filesystem::path source = work / "source";
        std::filesystem::create_directories(project);
        std::filesystem::copy(std::filesystem::path(LUDUS_SCENE_IMPORT_FIXTURES_DIR) / std::string(fixture.Directory),
                              source,
                              std::filesystem::copy_options::recursive);
        const std::string root = project.string();
        const std::string sourceText = source.string();
        SequentialUuids counter;
        UuidSource uuids{&SequentialUuids::Generate, &counter};
        AssetOptions options;
        options.UuidsOrNull = &uuids;
        options.SourceOrigin = VIRTUAL_ORIGIN;
        options.SourceEntry = fixture.Entry;
        AssetError error;
        Array<uint8> level;
        REQUIRE(CreateLevel("Imported", options, level, error));
        EditorSceneRequest request;
        request.Root = root;
        request.Source = sourceText;
        request.Level = ToSpan(level);
        request.Operation = EditorSceneOperation::PreviewImport;
        EditorSceneResult preview;
        const bool isPreviewed = OperateEditorScene(request, options, preview, error);
        INFO(Describe(error));
        REQUIRE(isPreviewed);
        CHECK(GetResponseMember(preview.Response, "profile") == fixture.Profile);

        request.Operation = EditorSceneOperation::Import;
        EditorSceneResult imported;
        const bool isImported = OperateEditorScene(request, options, imported, error);
        INFO(Describe(error));
        REQUIRE(isImported);
        // The assembled scene is what the editor viewport installs.
        CHECK(imported.Scene.IsEmpty() == false);
        const std::string candidate = GetResponseMember(imported.Response, "level");
        request.Operation = EditorSceneOperation::CommitImport;
        request.Level = ToSpan(candidate);
        request.Catalog = ToSpan(imported.Catalog);
        EditorSceneResult committed;
        REQUIRE(OperateEditorScene(request, options, committed, error));
        request.Operation = EditorSceneOperation::Assemble;
        request.Catalog = {};
        EditorSceneResult assembled;
        REQUIRE(OperateEditorScene(request, options, assembled, error));
        REQUIRE(assembled.Scene.IsEmpty() == false);
        // Best-effort imports are LUSCN v7: indexed tangent-framed meshes with implicit bake
        // charts, and material alpha masks and maps that survive the catalog's descriptions.
        uint32 version = 0;
        REQUIRE(assembled.Scene.GetSize() > ludus::content::scene_format::MAGIC.size() + sizeof(uint32));
        REQUIRE(core::TryReadLittleEndian<uint32>(
                    {assembled.Scene.GetData() + ludus::content::scene_format::MAGIC.size(), sizeof(uint32)},
                    version) == true);
        CHECK(version == ludus::content::scene_format::VERSION_7);
        ludus::resources::SceneDefinition decoded;
        REQUIRE(ludus::resources::SceneDefinition::Decode(assembled.Scene.GetData(),
                                                          assembled.Scene.GetSize(),
                                                          decoded) == ludus::resources::Status::Ok);
        usize maskedCount = 0;
        usize surfaceCount = 0;
        usize emissionCount = 0;
        for (const ludus::resources::SceneMaterial& material : decoded.Materials)
        {
            maskedCount += material.IsAlphaMasked == true ? 1 : 0;
            if (material.SurfaceTexture != ludus::content::scene_format::NO_TEXTURE)
            {
                ++surfaceCount;
                CHECK(decoded.Textures[material.SurfaceTexture].IsLinear == true);
            }
            emissionCount += material.EmissionTexture != ludus::content::scene_format::NO_TEXTURE ? 1 : 0;
        }
        CHECK(maskedCount == fixture.MaskedMaterials);
        CHECK(surfaceCount == fixture.SurfaceMaps);
        CHECK(emissionCount == fixture.EmissionMaps);
        bool hasTangent = false;
        for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
        {
            REQUIRE(mesh.Tangents.GetSize() == mesh.Positions.GetSize());
            for (const ludus::foundation::math::Vector4& tangent : mesh.Tangents)
            {
                hasTangent = hasTangent == true || tangent.W != 0;
            }
        }
        CHECK(hasTangent == (fixture.SurfaceMaps != 0));
        bool hasSharedVertices = false;
        for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
        {
            CHECK(mesh.BakeTexcoords.IsEmpty() == true);
            hasSharedVertices = hasSharedVertices == true || mesh.Positions.GetSize() < mesh.Indices.GetSize();
        }
        CHECK(hasSharedVertices == true);
        const std::string reports = GetResponseMember(assembled.Response, "import_reports");
        CHECK(reports.find("\"importer\":\"" + std::string(fixture.Profile) + "\"") != std::string::npos);

        std::error_code ignored;
        std::filesystem::remove_all(work, ignored);
    }
}

TEST_CASE("local smoke test: a licensed scene imports through the editor catalog and assembles",
          "[scene_assets][licensed]")
{
    // Env-gated: LUDUS_EDITOR_IMPORT_ENTRY names a licensed glTF/OBJ/pbrt entry file
    // (for example Khronos Sponza). Its directory is the import root.
    const char* entryOrNull = std::getenv("LUDUS_EDITOR_IMPORT_ENTRY");
    if (entryOrNull == nullptr || entryOrNull[0] == '\0')
    {
        SKIP("set LUDUS_EDITOR_IMPORT_ENTRY to a licensed scene entry file");
    }
    const std::filesystem::path entry(entryOrNull);
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    std::filesystem::create_directories(project);
    const std::string root = project.string();
    const std::string sourceText = entry.parent_path().string();
    const std::string entryName = entry.filename().string();
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    options.SourceEntry = entryName;
    AssetError error;
    Array<uint8> level;
    REQUIRE(CreateLevel("Licensed", options, level, error));
    EditorSceneRequest request;
    request.Root = root;
    request.Source = sourceText;
    request.Level = ToSpan(level);
    request.Operation = EditorSceneOperation::Import;
    EditorSceneResult imported;
    const bool isImported = OperateEditorScene(request, options, imported, error);
    INFO(Describe(error));
    REQUIRE(isImported);
    const std::string candidate = GetResponseMember(imported.Response, "level");
    request.Operation = EditorSceneOperation::CommitImport;
    request.Level = ToSpan(candidate);
    request.Catalog = ToSpan(imported.Catalog);
    EditorSceneResult committed;
    REQUIRE(OperateEditorScene(request, options, committed, error));
    request.Operation = EditorSceneOperation::Assemble;
    request.Catalog = {};
    EditorSceneResult assembled;
    REQUIRE(OperateEditorScene(request, options, assembled, error));
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(assembled.Scene.GetData(), assembled.Scene.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize vertices = 0;
    usize indices = 0;
    for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
    {
        vertices += mesh.Positions.GetSize();
        indices += mesh.Indices.GetSize();
    }
    UNSCOPED_INFO("assembled " << decoded.Meshes.GetSize() << " meshes, " << vertices << " vertices, " << indices
                               << " indices, " << assembled.Scene.GetSize() << " bytes");
    CHECK(vertices > 0);
    if (entryName == "bistro_cafe.pbrt")
    {
        CHECK(vertices == 2892578);
        CHECK(indices == 8516880);
        CHECK(assembled.Scene.GetSize() <= MAX_ASSET_BYTES);
    }
    if (entry.filename() == "NewSponza_Main_glTF_003.gltf")
    {
        // Full supplied Intel Main Sponza, through the production import/catalog/
        // assembly path. Neither the former vertex nor triangle ceiling may trim it.
        CHECK(vertices == 2049137);
        CHECK(indices == 11241054);
        const std::string reports = GetResponseMember(assembled.Response, "import_reports");
        CHECK(reports.find("GEOMETRY_DROPPED") == std::string::npos);
        CHECK(reports.find("INSTANCES_DROPPED") == std::string::npos);
    }
    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

TEST_CASE("Catalogs larger than the 1 MiB metadata limit still import", "[scene_assets]")
{
    // Every catalog record carries its import's full source_inputs map, so the catalog grows with
    // resources x source files. Included parts with distinct materials each keep their own
    // texture, material and mesh (LUSCN version 5), so a hundred parts exceed 1 MiB.
    constexpr int PART_COUNT = 112;
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    const std::filesystem::path source = work / "source";
    std::filesystem::create_directories(project);
    std::filesystem::create_directories(source / "parts");
    std::ofstream scene(source / "many.pbrt", std::ios::binary);
    scene << "LookAt 0 1 4  0 0 0  0 1 0\nCamera \"perspective\" \"float fov\" [ 45 ]\n"
             "Film \"rgb\" \"integer xresolution\" [ 64 ] \"integer yresolution\" [ 64 ]\n"
             "Sampler \"halton\"\nWorldBegin\nLightSource \"infinite\" \"rgb L\" [ .2 .2 .2 ]\n";
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const std::string name = "parts/part_with_a_descriptive_long_name_" + std::to_string(part) + ".pbrt";
        std::ofstream(source / name, std::ios::binary)
            << "AttributeBegin\nMaterial \"diffuse\" \"rgb reflectance\" [ "
            << static_cast<double>(part + 1) / static_cast<double>(PART_COUNT + 1) << " .5 .4 ]\nTranslate " << part
            << " 0 0\n"
            << "Shape \"trianglemesh\" \"point3 P\" [ -1 0 -1  1 0 -1  1 0 1  -1 0 1 ]\n"
               "  \"integer indices\" [ 0 1 2  0 2 3 ]\nAttributeEnd\n";
        scene << "Include \"" << name << "\"\n";
    }
    scene.close();
    std::ofstream(source / "LICENSE.txt", std::ios::binary) << "CC0 synthetic catalog-size fixture\n";
    const std::string root = project.string();
    const std::string sourceText = source.string();
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    options.SourceEntry = "many.pbrt";
    AssetError error;
    Array<uint8> level;
    REQUIRE(CreateLevel("Many parts", options, level, error));
    EditorSceneRequest request;
    request.Root = root;
    request.Source = sourceText;
    request.Level = ToSpan(level);
    request.Operation = EditorSceneOperation::Import;
    EditorSceneResult imported;
    const bool isImported = OperateEditorScene(request, options, imported, error);
    INFO(Describe(error));
    REQUIRE(isImported);
    CHECK(imported.Catalog.GetSize() > MAX_METADATA_BYTES);
    CHECK(imported.Catalog.GetSize() <= MAX_CATALOG_BYTES);
    const std::string candidate = GetResponseMember(imported.Response, "level");
    request.Operation = EditorSceneOperation::CommitImport;
    request.Level = ToSpan(candidate);
    request.Catalog = ToSpan(imported.Catalog);
    EditorSceneResult committed;
    REQUIRE(OperateEditorScene(request, options, committed, error));
    // The published catalog reads back for assembly.
    request.Operation = EditorSceneOperation::Assemble;
    request.Catalog = {};
    EditorSceneResult assembled;
    const bool isAssembled = OperateEditorScene(request, options, assembled, error);
    INFO(Describe(error));
    CHECK(isAssembled);
    CHECK(std::filesystem::file_size(project / "assets" / "catalog.json") > MAX_METADATA_BYTES);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

TEST_CASE("A confirmed import must match the approved preview", "[scene_assets]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    const std::filesystem::path source = work / "source";
    std::filesystem::create_directories(project);
    WriteBestEffortSource(source, "room.pbrt");
    const std::string root = project.string();
    const std::string sourceText = source.string();
    CountingEntropy entropy;
    const identity::EntropySource entropySource = entropy.GetSource();
    AssetOptions options;
    options.EntropyOrNull = &entropySource;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    options.SourceEntry = "room.pbrt";
    AssetError error;
    Array<uint8> level;
    REQUIRE(CreateLevel("Preview token", options, level, error));
    EditorSceneRequest request;
    request.Root = root;
    request.Source = sourceText;
    request.Level = ToSpan(level);
    request.Operation = EditorSceneOperation::PreviewImport;
    EditorSceneResult preview;
    INFO(Describe(error));
    REQUIRE(OperateEditorScene(request, options, preview, error));
    const std::string digest = GetResponseMember(preview.Response, "source_digest");
    REQUIRE(digest.size() == 64); // hexadecimal SHA-256

    request.Operation = EditorSceneOperation::Import;
    options.ExpectedSourceDigest = digest;
    EditorSceneResult matching;
    CHECK(OperateEditorScene(request, options, matching, error) == true);

    std::ofstream(source / "room.pbrt", std::ios::app) << "# edited after the preview\n";
    EditorSceneResult stale;
    CHECK(OperateEditorScene(request, options, stale, error) == false);
    CHECK(error.Code == AssetErrorCode::Conflict);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

TEST_CASE("Two pbrt entries in one directory keep separate origins", "[scene_assets]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    const std::filesystem::path source = work / "source";
    std::filesystem::create_directories(project);
    WriteBestEffortSource(source, "room.pbrt");
    WriteBestEffortSource(source, "hall.pbrt");
    const std::string root = project.string();
    const std::string sourceText = source.string();
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    AssetError error;
    Array<uint8> created;
    REQUIRE(CreateLevel("Two entries", options, created, error));
    std::string level(reinterpret_cast<const char*>(created.GetData()), created.GetSize());
    EditorSceneRequest request;
    request.Root = root;
    request.Source = sourceText;
    for (const std::string_view entry : {std::string_view("room.pbrt"), std::string_view("hall.pbrt")})
    {
        options.SourceEntry = entry;
        request.Operation = EditorSceneOperation::Import;
        request.Level = ToSpan(level);
        request.Catalog = {};
        EditorSceneResult imported;
        INFO(Describe(error));
        REQUIRE(OperateEditorScene(request, options, imported, error));
        level = GetResponseMember(imported.Response, "level");
        const std::string expected = GetResponseMember(imported.Response, "catalog_expected");
        request.Operation = EditorSceneOperation::CommitImport;
        request.Level = ToSpan(level);
        request.Catalog = ToSpan(imported.Catalog);
        request.HasExpected = expected.size() == 64;
        request.Expected = expected;
        EditorSceneResult committed;
        const bool isCommitted = OperateEditorScene(request, options, committed, error);
        INFO(Describe(error));
        REQUIRE(isCommitted);
        request.HasExpected = false;
    }
    // Each entry got its own logical scene rather than replacing the first.
    const usize importsAt = level.find("\"imports\":[");
    REQUIRE(importsAt != std::string::npos);
    const std::string imports = level.substr(importsAt, level.find(']', importsAt) - importsAt);
    CHECK(imports.find(',') != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

TEST_CASE("scene-v4.pbrt preserves materials and falls back only on profile rejection", "[scene_assets]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    std::filesystem::create_directories(project);
    const std::string root = project.string();
    AssetOptions options;
    options.SourceEntry = "scene-v4.pbrt";
    AssetError error;
    EditorSceneRequest request;
    request.Root = root;
    request.Operation = EditorSceneOperation::PreviewImport;

    const std::string reference = std::string(GOLDENS) + "/source";
    request.Source = reference;
    EditorSceneResult kept;
    INFO(Describe(error));
    REQUIRE(OperateEditorScene(request, options, kept, error));
    CHECK(GetResponseMember(kept.Response, "profile") == MATERIAL_PROFILE);
    CHECK(ToText(kept.Response).find("reference_rejection") == std::string::npos);

    const std::filesystem::path general = work / "general";
    WriteBestEffortSource(general, "scene-v4.pbrt");
    const std::string generalText = general.string();
    request.Source = generalText;
    EditorSceneResult fallback;
    REQUIRE(OperateEditorScene(request, options, fallback, error));
    CHECK(GetResponseMember(fallback.Response, "profile") == "pbrt-best-effort-v1");
    CHECK(ToText(fallback.Response).find("\"reference_rejection\":") != std::string::npos);

    // An empty entry is the unchanged reference behaviour: no fallback.
    options.SourceEntry = {};
    EditorSceneResult rejected;
    CHECK(OperateEditorScene(request, options, rejected, error) == false);
    CHECK(error.Code == AssetErrorCode::UnsupportedScene);

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

namespace
{
namespace format = ludus::content::scene_format;
namespace resources = ludus::resources;
constexpr usize TRIANGLE_VERTICES = 3;
constexpr usize AUTHORED_VERTEX_COMPONENTS = 10;
constexpr uint64 TEST_AREA_SOURCE = 0xf1234567abcdef01ULL;
constexpr uint64 TEST_LIGHT_SOURCE = 0xfedcba9876543210ULL;

struct LightingFixtureProfile final
{
    uint32 Version = format::VERSION_3;
    uint32 EnvironmentWidth = 2;
    uint32 LightCount = 1;
    float64 EnvironmentScale = 2;
};

bool CookLightingFixture(std::string_view source,
                         CookResult& outResult,
                         CookError& outError,
                         const Cancellation* cancelOrNull,
                         const LightingFixtureProfile& profile) noexcept
{
    if (Cook(source, outResult, outError, cancelOrNull) == false)
    {
        return false;
    }
    outResult.Bundle.Clear();
    detail::ByteWriter writer(outResult.Bundle);
    writer.Append(format::MAGIC);
    writer.AppendUint32(profile.Version);
    writer.AppendUint32(1);
    writer.AppendUint32(1);
    writer.AppendUint32(0);
    writer.AppendUint32(1);
    // Eye, target, up, vertical FOV.
    constexpr StaticArray<float64, AUTHORED_VERTEX_COMPONENTS> CAMERA{0, 0, 3, 0, 0, 0, 0, 1, 0, 1};
    for (const float64 value : CAMERA)
    {
        writer.AppendFloat32(value);
    }
    writer.AppendUint32(format::NO_TEXTURE);
    writer.AppendUint32(format::ADDRESS_CLAMP);
    writer.AppendFloat32(0);
    writer.AppendFloat32(.5);
    writer.AppendFloat32(1);
    writer.AppendFloat32(2);
    writer.AppendFloat32(3);
    if (format::HasLightingRecords(profile.Version) == true)
    {
        writer.AppendUint32(format::EMISSION_TWO_SIDED | format::EMISSION_HAS_POWER);
        writer.AppendUint32(static_cast<uint32>(TEST_AREA_SOURCE));
        writer.AppendUint32(static_cast<uint32>(TEST_AREA_SOURCE >> 32));
        writer.AppendFloat32(9);
        if (format::HasMaterialAlpha(profile.Version) == true)
        {
            writer.AppendUint32(format::ALPHA_OPAQUE);
            writer.AppendFloat32(0);
        }
        if (format::HasSurfaceMaps(profile.Version) == true)
        {
            writer.AppendUint32(format::NO_TEXTURE);
            writer.AppendUint32(format::NO_TEXTURE);
            writer.AppendFloat32(0);
        }
        else
        {
            writer.AppendUint32(0);
        }
    }
    writer.AppendUint32(TRIANGLE_VERTICES);
    writer.AppendUint32(TRIANGLE_VERTICES);
    constexpr StaticArray<StaticArray<float64, AUTHORED_VERTEX_COMPONENTS>, TRIANGLE_VERTICES> VERTICES{
        StaticArray<float64, AUTHORED_VERTEX_COMPONENTS>{0, 0, 0, 0, 0, 1, 0, 0, .25, .25},
        StaticArray<float64, AUTHORED_VERTEX_COMPONENTS>{1, 0, 0, 0, 0, 1, 1, 0, .5, .25},
        StaticArray<float64, AUTHORED_VERTEX_COMPONENTS>{0, 1, 0, 0, 0, 1, 0, 1, .25, .5}};
    for (const StaticArray<float64, AUTHORED_VERTEX_COMPONENTS>& vertex : VERTICES)
    {
        const usize components = format::HasStoredBakeCoordinates(profile.Version) == true ? AUTHORED_VERTEX_COMPONENTS
                                                                                           : format::VERTEX_COMPONENTS;
        for (usize i = 0; i < components; ++i)
        {
            writer.AppendFloat32(vertex[i]);
        }
        if (format::HasSurfaceMaps(profile.Version) == true)
        {
            // Tangent +x with positive handedness.
            for (const float64 component : {1.0, 0.0, 0.0, 1.0})
            {
                writer.AppendFloat32(component);
            }
        }
    }
    writer.AppendUint32(0);
    writer.AppendUint32(1);
    writer.AppendUint32(2);
    writer.AppendUint32(0);
    writer.AppendUint32(0);
    for (usize i = 0; i < format::OBJECT_TRANSFORM_COMPONENTS; ++i)
    {
        writer.AppendFloat32(i == 0 || i == 4 || i == 8 ? 1 : 0);
    }
    if (format::HasLightingRecords(profile.Version) == true)
    {
        writer.AppendUint32(17);
        writer.AppendUint32(0xf0000000U);
        writer.AppendUint32(format::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
        writer.AppendUint32(profile.LightCount);
        writer.AppendUint32(profile.EnvironmentWidth);
        writer.AppendUint32(profile.EnvironmentWidth == 0 ? 0 : 1);
        for (usize channel = 0; channel < 3; ++channel)
        {
            writer.AppendFloat32(profile.EnvironmentScale);
        }
        for (usize i = 0; i < profile.LightCount; ++i)
        {
            const uint64 sourceId = TEST_LIGHT_SOURCE + i;
            writer.AppendUint32(static_cast<uint32>(sourceId));
            writer.AppendUint32(static_cast<uint32>(sourceId >> 32));
            writer.AppendUint32(format::LIGHT_POINT);
            writer.AppendUint32(format::LIGHT_ENABLED);
            constexpr usize LIGHT_FLOATS = 12;
            constexpr StaticArray<float64, LIGHT_FLOATS> VALUES{3, 4, 5, 1, 2, 3, 0, -1, 0, 10, 1, .5};
            for (const float64 value : VALUES)
            {
                writer.AppendFloat32(value);
            }
        }
        for (usize pixel = 0; pixel < profile.EnvironmentWidth; ++pixel)
        {
            for (usize channel = 0; channel < 3; ++channel)
            {
                writer.AppendFloat32(static_cast<float64>(pixel + channel + 1));
            }
        }
    }
    return writer.IsOk();
}

bool CookV3(std::string_view source,
            CookResult& outResult,
            CookError& outError,
            const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, {});
}
bool CookV4(std::string_view source,
            CookResult& outResult,
            CookError& outError,
            const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, { .Version = format::VERSION_4 });
}
bool CookV6(std::string_view source,
            CookResult& outResult,
            CookError& outError,
            const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, { .Version = format::VERSION_6 });
}
bool CookV2(std::string_view source,
            CookResult& outResult,
            CookError& outError,
            const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, { .Version = format::VERSION_2 });
}
bool CookConstant(std::string_view source,
                  CookResult& outResult,
                  CookError& outError,
                  const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, { .EnvironmentWidth = 0 });
}
bool CookOtherDimensions(std::string_view source,
                         CookResult& outResult,
                         CookError& outError,
                         const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source, outResult, outError, cancelOrNull, { .EnvironmentWidth = 1 });
}
bool CookManyLights(std::string_view source,
                    CookResult& outResult,
                    CookError& outError,
                    const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source,
                               outResult,
                               outError,
                               cancelOrNull,
                               { .EnvironmentWidth = 0, .LightCount = format::MAX_LIGHTS });
}
bool CookHighEnvironment(std::string_view source,
                         CookResult& outResult,
                         CookError& outError,
                         const Cancellation* cancelOrNull) noexcept
{
    return CookLightingFixture(source,
                               outResult,
                               outError,
                               cancelOrNull,
                               { .EnvironmentWidth = 0, .LightCount = 0, .EnvironmentScale = 6000 });
}

resources::SceneDefinition DecodeAssembled(const std::string& root, const Array<uint8>& level)
{
    Array<uint8> bytes;
    AssetError error;
    INFO(Describe(error));
    REQUIRE(AssembleLevel(root, ToSpan(level), {}, bytes, error));
    resources::SceneDefinition scene;
    REQUIRE(resources::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == resources::Status::Ok);
    return scene;
}
} // namespace

TEST_CASE("V3 lighting artifacts survive save reopen publish and relocation", "[scene_assets][lighting]")
{
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetOptions options = golden.Options;
    options.CookOrNull = CookV3;
    options.SourceOrigin = "/lighting/first";
    Array<uint8> empty;
    AssetError error;
    REQUIRE(CreateLevel("Lighting", options, empty, error));
    Array<uint8> level;
    INFO(Describe(error));
    REQUIRE(ImportScene(root, golden.Source, ToSpan(empty), options, level, error));
    const resources::SceneDefinition scene = DecodeAssembled(root, level);
    REQUIRE(scene.Lights.GetSize() == 1);
    CHECK(scene.Lights[0].SourceId != 0);
    CHECK(scene.Lights[0].IntensityRgb.Y == 4);
    CHECK(scene.Lights[0].PositionWorld.Z == 3);
    CHECK(scene.Environment.Width == 2);
    CHECK(scene.Environment.Pixels[0].X * scene.Environment.Rgb.X == 2);
    CHECK(scene.Environment.Pixels[1].Z * scene.Environment.Rgb.Z == 8);
    CHECK(scene.Materials[0].AreaSourceId != 0);
    CHECK(scene.Materials[0].IsEmissionTwoSided == true);
    CHECK(scene.Materials[0].HasEmissionPower == true);
    CHECK(scene.Materials[0].EmissionPower == 9);
    CHECK(scene.Meshes[0].BakeTexcoords[1].X == .5F);
    CHECK(scene.Objects[0].SourceId != 0);
    Array<uint8> saved;
    REQUIRE(SaveLevel(root, "scenes/lighting.lulevel", ToSpan(level), nullptr, saved, error));
    Array<uint8> reopened;
    REQUIRE(OpenLevel(root, "scenes/lighting.lulevel", reopened, error));
    Array<uint8> before;
    Array<uint8> after;
    REQUIRE(AssembleLevel(root, ToSpan(level), {}, before, error));
    REQUIRE(AssembleLevel(root, ToSpan(reopened), {}, after, error));
    CHECK(ToText(before) == ToText(after));
    const std::filesystem::path relocated = golden.Work / "relocated";
    std::filesystem::copy(golden.Project, relocated, std::filesystem::copy_options::recursive);
    REQUIRE(AssembleLevel(relocated.string(), ToSpan(reopened), {}, after, error));
    CHECK(ToText(before) == ToText(after));
    Array<uint8> published;
    REQUIRE(PublishLevel(root,
                         ToSpan(level),
                         (golden.Work / "lighting-release").string(),
                         DumpLayout::Canonical,
                         published,
                         error));
    CHECK(ReadTree(golden.Work / "lighting-release").empty() == false);
    CHECK(ToText(before) == ReadWhole(golden.Work / "lighting-release/scene.luscn"));
    Array<uint8> catalog;
    REQUIRE(ReadCatalog(root, catalog, error));
    CHECK(ToText(catalog).find("lighting/") != std::string::npos);
    CHECK(ToText(catalog).find("portable-rgba8-f32-le-v3") != std::string::npos);
    const std::string exactEmitter = "\"area_source_id\":[" + std::to_string(static_cast<uint32>(TEST_AREA_SOURCE)) +
                                     "," + std::to_string(static_cast<uint32>(TEST_AREA_SOURCE >> 32)) + "]";
    bool hasExactEmitter = false;
    for (const std::pair<const std::string, std::string>& artifact : ReadTree(golden.Project / "assets/objects"))
    {
        hasExactEmitter = hasExactEmitter == true || artifact.second.find(exactEmitter) != std::string::npos;
    }
    CHECK(hasExactEmitter == true);
    Array<uint8> reimported;
    REQUIRE(ImportScene(root, golden.Source, ToSpan(level), options, reimported, error));
    const resources::SceneDefinition repeated = DecodeAssembled(root, reimported);
    CHECK(repeated.Lights.GetSize() == 1);
    CHECK(repeated.Lights[0].SourceId == scene.Lights[0].SourceId);
    CHECK(repeated.Objects.GetSize() == 2);
    CHECK(repeated.Objects[0].SourceId == scene.Objects[0].SourceId);
    CHECK(repeated.Objects[0].SourceId != repeated.Objects[1].SourceId);
    CHECK(repeated.Materials[0].AreaSourceId == scene.Materials[0].AreaSourceId);
}

TEST_CASE("Multiple lighting imports scope IDs sum environments and upgrade legacy meshes", "[scene_assets][lighting]")
{
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetOptions options = golden.Options;
    options.CookOrNull = CookV3;
    options.SourceOrigin = "/lighting/a";
    AssetError error;
    Array<uint8> empty;
    Array<uint8> first;
    Array<uint8> second;
    REQUIRE(CreateLevel("Mixed", options, empty, error));
    REQUIRE(ImportScene(root, golden.Source, ToSpan(empty), options, first, error));
    options.SourceOrigin = "/lighting/b";
    REQUIRE(ImportScene(root, golden.Source, ToSpan(first), options, second, error));
    const resources::SceneDefinition pair = DecodeAssembled(root, second);
    REQUIRE(pair.Lights.GetSize() == 2);
    CHECK(pair.Lights[0].SourceId != pair.Lights[1].SourceId);
    CHECK(pair.Materials[0].AreaSourceId != pair.Materials[1].AreaSourceId);
    CHECK(pair.Objects[0].SourceId != pair.Objects[1].SourceId);
    CHECK(pair.Environment.Pixels[0].X * pair.Environment.Rgb.X == 4);
    options.SourceOrigin = "/lighting/c";
    options.CookOrNull = CookConstant;
    Array<uint8> third;
    REQUIRE(ImportScene(root, golden.Source, ToSpan(second), options, third, error));
    const resources::SceneDefinition mixedEnvironment = DecodeAssembled(root, third);
    CHECK(mixedEnvironment.Environment.Pixels[0].X * mixedEnvironment.Environment.Rgb.X == 6);
    options.SourceOrigin = "/lighting/legacy";
    options.CookOrNull = CookV2;
    Array<uint8> mixed;
    REQUIRE(ImportScene(root, golden.Source, ToSpan(third), options, mixed, error));
    const resources::SceneDefinition upgraded = DecodeAssembled(root, mixed);
    CHECK(upgraded.Lights.GetSize() == 3);
    CHECK(upgraded.Objects.GetSize() == 4);
    for (const resources::SceneMesh& mesh : upgraded.Meshes)
    {
        CHECK(mesh.BakeTexcoords.GetSize() == TRIANGLE_VERTICES);
    }
    Array<uint8> catalogBefore;
    REQUIRE(ReadCatalog(root, catalogBefore, error));
    options.CookOrNull = CookOtherDimensions;
    options.SourceOrigin = "/lighting/incompatible";
    CHECK_FALSE(ImportScene(root, golden.Source, ToSpan(first), options, mixed, error));
    Array<uint8> catalogAfter;
    REQUIRE(ReadCatalog(root, catalogAfter, error));
    CHECK(ToText(catalogBefore) == ToText(catalogAfter));
}

TEST_CASE("A version 4 import upgrades the assembled level and drops stored v3 bake coordinates",
          "[scene_assets][lighting]")
{
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetOptions options = golden.Options;
    options.CookOrNull = CookV3;
    options.SourceOrigin = "/indexed/v3";
    AssetError error;
    Array<uint8> empty;
    Array<uint8> first;
    Array<uint8> second;
    Array<uint8> third;
    REQUIRE(CreateLevel("Indexed", options, empty, error));
    INFO(Describe(error));
    REQUIRE(ImportScene(root, golden.Source, ToSpan(empty), options, first, error));
    CHECK(DecodeAssembled(root, first).Meshes[0].BakeTexcoords.GetSize() == TRIANGLE_VERTICES);
    options.CookOrNull = CookV2;
    options.SourceOrigin = "/indexed/legacy";
    REQUIRE(ImportScene(root, golden.Source, ToSpan(first), options, second, error));
    options.CookOrNull = CookV4;
    options.SourceOrigin = "/indexed/v4";
    REQUIRE(ImportScene(root, golden.Source, ToSpan(second), options, third, error));
    Array<uint8> bytes;
    REQUIRE(AssembleLevel(root, ToSpan(third), {}, bytes, error));
    REQUIRE(bytes.GetSize() > format::MAGIC.size() + sizeof(uint32));
    uint32 version = 0;
    REQUIRE(
        ludus::foundation::core::TryReadLittleEndian<uint32>({bytes.GetData() + format::MAGIC.size(), sizeof(uint32)},
                                                             version) == true);
    CHECK(version == format::VERSION_4);
    const resources::SceneDefinition upgraded = DecodeAssembled(root, third);
    CHECK(upgraded.Meshes.GetSize() == 3);
    CHECK(upgraded.Lights.GetSize() == 2);
    CHECK(upgraded.Objects.GetSize() == 3);
    for (const resources::SceneMesh& mesh : upgraded.Meshes)
    {
        CHECK(mesh.BakeTexcoords.IsEmpty() == true);
        CHECK(mesh.Positions.GetSize() == TRIANGLE_VERTICES);
        CHECK(mesh.Normals[0].Z == 1);
    }
    Array<uint8> catalog;
    REQUIRE(ReadCatalog(root, catalog, error));
    CHECK(ToText(catalog).find("portable-rgba8-f32-le-v4") != std::string::npos);
    Array<uint8> saved;
    REQUIRE(SaveLevel(root, "scenes/indexed.lulevel", ToSpan(third), nullptr, saved, error));
    Array<uint8> reopened;
    REQUIRE(OpenLevel(root, "scenes/indexed.lulevel", reopened, error));
    Array<uint8> after;
    REQUIRE(AssembleLevel(root, ToSpan(reopened), {}, after, error));
    CHECK(ToText(bytes) == ToText(after));
    Array<uint8> published;
    REQUIRE(PublishLevel(root,
                         ToSpan(third),
                         (golden.Work / "indexed-release").string(),
                         DumpLayout::Canonical,
                         published,
                         error));
    CHECK(ToText(bytes) == ReadWhole(golden.Work / "indexed-release/scene.luscn"));
}

TEST_CASE("A version 6 import gives older mesh parts absent tangents in the assembled level",
          "[scene_assets][lighting]")
{
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetOptions options = golden.Options;
    options.CookOrNull = CookV3;
    options.SourceOrigin = "/tangents/v3";
    AssetError error;
    Array<uint8> empty;
    Array<uint8> first;
    Array<uint8> second;
    Array<uint8> third;
    REQUIRE(CreateLevel("Tangents", options, empty, error));
    INFO(Describe(error));
    REQUIRE(ImportScene(root, golden.Source, ToSpan(empty), options, first, error));
    options.CookOrNull = CookV2;
    options.SourceOrigin = "/tangents/legacy";
    REQUIRE(ImportScene(root, golden.Source, ToSpan(first), options, second, error));
    options.CookOrNull = CookV6;
    options.SourceOrigin = "/tangents/v6";
    REQUIRE(ImportScene(root, golden.Source, ToSpan(second), options, third, error));
    Array<uint8> bytes;
    REQUIRE(AssembleLevel(root, ToSpan(third), {}, bytes, error));
    uint32 version = 0;
    REQUIRE(
        ludus::foundation::core::TryReadLittleEndian<uint32>({bytes.GetData() + format::MAGIC.size(), sizeof(uint32)},
                                                             version) == true);
    CHECK(version == format::VERSION_6);
    const resources::SceneDefinition upgraded = DecodeAssembled(root, third);
    REQUIRE(upgraded.Meshes.GetSize() == 3);
    usize framed = 0;
    for (const resources::SceneMesh& mesh : upgraded.Meshes)
    {
        CHECK(mesh.BakeTexcoords.IsEmpty() == true);
        REQUIRE(mesh.Tangents.GetSize() == TRIANGLE_VERTICES);
        CHECK(mesh.Normals[0].Z == 1);
        framed += mesh.Tangents[0].W == 1 && mesh.Tangents[0].X == 1 ? 1 : 0;
        if (mesh.Tangents[0].W == 0)
        {
            CHECK(mesh.Tangents[0].X == 0);
        }
    }
    CHECK(framed == 1);
    Array<uint8> catalog;
    REQUIRE(ReadCatalog(root, catalog, error));
    CHECK(ToText(catalog).find("portable-rgba8-f32-le-v6") != std::string::npos);
}

TEST_CASE("Combined authored light and environment overflow rejects catalog publication", "[scene_assets][lighting]")
{
    GoldenProject golden;
    AssetOptions options = golden.Options;
    AssetError error;
    Array<uint8> empty;
    REQUIRE(CreateLevel("Capacity", options, empty, error));
    for (const bool useLights : {false, true})
    {
        options.CookOrNull = useLights == true ? CookManyLights : CookHighEnvironment;
        options.SourceOrigin = useLights == true ? "/many/a" : "/energy/a";
        Array<uint8> first;
        REQUIRE(ImportScene(golden.Project.string(), golden.Source, ToSpan(empty), options, first, error));
        Array<uint8> catalogBefore;
        REQUIRE(ReadCatalog(golden.Project.string(), catalogBefore, error));
        options.SourceOrigin = useLights == true ? "/many/b" : "/energy/b";
        Array<uint8> rejected;
        CHECK_FALSE(ImportScene(golden.Project.string(), golden.Source, ToSpan(first), options, rejected, error));
        Array<uint8> catalogAfter;
        REQUIRE(ReadCatalog(golden.Project.string(), catalogAfter, error));
        CHECK(ToText(catalogBefore) == ToText(catalogAfter));
    }
}

namespace
{
enum class LightingCorruption : uint8
{
    ReservedMaterial,
    BakeCoordinates,
    NonfiniteLight,
    EqualCone,
    InnerBoundary,
    OuterBoundary,
    Dimensions,
    Truncated,
};

template <LightingCorruption Corruption>
bool CookCorruptLighting(std::string_view source,
                         CookResult& outResult,
                         CookError& outError,
                         const Cancellation* cancelOrNull) noexcept
{
    if (CookV3(source, outResult, outError, cancelOrNull) == false)
    {
        return false;
    }
    constexpr usize MESH_OFFSET = sizeof(format::Header) + sizeof(format::MaterialRecordV3);
    constexpr usize OBJECT_OFFSET = MESH_OFFSET + sizeof(format::MeshRecord) +
                                    TRIANGLE_VERTICES * sizeof(format::VertexRecordV3) +
                                    TRIANGLE_VERTICES * sizeof(uint32);
    constexpr usize LIGHTING_OFFSET = OBJECT_OFFSET + sizeof(format::ObjectRecordV3);
    usize offset = 0;
    uint32 word = 0;
    if constexpr (Corruption == LightingCorruption::ReservedMaterial)
    {
        offset = sizeof(format::Header) + sizeof(format::MaterialRecordV3) - sizeof(uint32);
        word = 1;
    }
    else if constexpr (Corruption == LightingCorruption::BakeCoordinates)
    {
        offset = MESH_OFFSET + sizeof(format::MeshRecord) + sizeof(format::VertexRecord);
        word = 0x40000000U; // Bake U = 2 is outside the normalized chart.
    }
    else if constexpr (Corruption == LightingCorruption::NonfiniteLight)
    {
        offset = LIGHTING_OFFSET + sizeof(format::LightingRecord) + sizeof(uint64) + 2 * sizeof(uint32);
        word = 0x7fc00000U;
    }
    else if constexpr (Corruption == LightingCorruption::EqualCone || Corruption == LightingCorruption::OuterBoundary)
    {
        offset = LIGHTING_OFFSET + sizeof(format::LightingRecord) + sizeof(format::LightRecord) - sizeof(float32);
        word = Corruption == LightingCorruption::EqualCone ? 0x3f800000U : 0xbf800000U;
    }
    else if constexpr (Corruption == LightingCorruption::InnerBoundary)
    {
        offset = LIGHTING_OFFSET + sizeof(format::LightingRecord) + sizeof(format::LightRecord) - 2 * sizeof(float32);
        word = 0xbf800000U;
    }
    else if constexpr (Corruption == LightingCorruption::Dimensions)
    {
        offset = LIGHTING_OFFSET + 3 * sizeof(uint32);
    }
    else
    {
        return outResult.Bundle.TryResize(outResult.Bundle.GetSize() - 1);
    }
    for (usize byte = 0; byte < sizeof(uint32); ++byte)
    {
        outResult.Bundle[offset + byte] = static_cast<uint8>(word >> (byte * 8));
    }
    return true;
}
} // namespace

TEST_CASE("Malformed v3 source fields never publish a candidate catalog", "[scene_assets][lighting]")
{
    GoldenProject golden;
    const std::string root = golden.Project.string();
    AssetOptions options = golden.Options;
    using FixtureCook = decltype(options.CookOrNull);
    constexpr usize CORRUPTION_CASES = 8;
    constexpr StaticArray<FixtureCook, CORRUPTION_CASES> COOKERS{
        CookCorruptLighting<LightingCorruption::ReservedMaterial>,
        CookCorruptLighting<LightingCorruption::BakeCoordinates>,
        CookCorruptLighting<LightingCorruption::NonfiniteLight>,
        CookCorruptLighting<LightingCorruption::EqualCone>,
        CookCorruptLighting<LightingCorruption::InnerBoundary>,
        CookCorruptLighting<LightingCorruption::OuterBoundary>,
        CookCorruptLighting<LightingCorruption::Dimensions>,
        CookCorruptLighting<LightingCorruption::Truncated>};
    AssetError error;
    Array<uint8> before;
    REQUIRE(ReadCatalog(root, before, error));
    Array<uint8> empty;
    REQUIRE(CreateLevel("Reject", options, empty, error));
    for (const FixtureCook cook : COOKERS)
    {
        options.CookOrNull = cook;
        options.SourceOrigin = "/lighting/corrupt";
        Array<uint8> rejected;
        CHECK_FALSE(ImportScene(root, golden.Source, ToSpan(empty), options, rejected, error));
        CHECK(error.Code == AssetErrorCode::InvalidScene);
        Array<uint8> after;
        REQUIRE(ReadCatalog(root, after, error));
        CHECK(ToText(before) == ToText(after));
    }
}

TEST_CASE("Scene catalog reads admit their byte ceiling and reject overflow without replacing output",
          "[scene_assets][limits]")
{
    const std::filesystem::path work = MakeWorkDirectory();
    std::filesystem::create_directories(work / "assets");
    const std::filesystem::path path = work / "assets/catalog.json";
    constexpr std::string_view EMPTY_CATALOG = "{\"resources\":{},\"version\":1}\n";
    std::string input(EMPTY_CATALOG);
    input.resize(MAX_CATALOG_BYTES, ' ');
    {
        std::ofstream file(path, std::ios::binary);
        file.write(input.data(), static_cast<std::streamsize>(input.size()));
        file.close();
        REQUIRE(file.good() == true);
    }
    Array<uint8> catalog;
    AssetError error;
    REQUIRE(ReadCatalog(work.string(), catalog, error) == true);
    CHECK(ToText(catalog) == EMPTY_CATALOG);
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file.put(' ');
        file.close();
        REQUIRE(file.good() == true);
    }
    CHECK(ReadCatalog(work.string(), catalog, error) == false);
    CHECK(error.Code == AssetErrorCode::AssetCorrupt);
    CHECK(ToText(catalog) == EMPTY_CATALOG);
    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
}

namespace
{
/// Source edits the synthetic cook makes after the reference cook read its copied
/// source, so the import's recheck must see them. Inputs are checked in key order:
/// LICENSE.txt first, then models/, scene-v4.pbrt, textures/Tiles.tga and last
/// textures/wood5.tga.
enum class SourceEdit : uint8
{
    None,
    /// The first input changes and the last is removed: the change is reported.
    ChangeFirstRemoveLast,
    /// The first input is removed and the last changes: the missing file is reported.
    RemoveFirstChangeLast,
    /// A middle input becomes a symlink out of the source and the last changes: the escape is reported.
    EscapeMiddleChangeLast,
    /// The first input changes and a middle one escapes: the change is reported.
    ChangeFirstEscapeMiddle,
};

constexpr std::string_view FIRST_INPUT = "LICENSE.txt";
constexpr std::string_view MIDDLE_INPUT = "textures/Tiles.tga";
constexpr std::string_view LAST_INPUT = "textures/wood5.tga";

SourceEdit sourceEditAfterCook = SourceEdit::None;

void ChangeSource(const std::filesystem::path& source, std::string_view name)
{
    std::ofstream(source / name, std::ios::binary | std::ios::app) << "edited";
}

void RemoveSource(const std::filesystem::path& source, std::string_view name)
{
    std::error_code ignored;
    std::filesystem::remove(source / name, ignored);
}

/// Replace an input with a symlink to work/outside.tga, beside the source directory.
void EscapeSource(const std::filesystem::path& source, std::string_view name)
{
    RemoveSource(source, name);
    std::error_code ignored;
    std::filesystem::create_symlink(source.parent_path() / "outside.tga", source / name, ignored);
}

bool CookThenEditSource(std::string_view source,
                        CookResult& outResult,
                        CookError& outError,
                        const Cancellation* cancelOrNull) noexcept
{
    if (Cook(source, outResult, outError, cancelOrNull) == false)
    {
        return false;
    }
    // Test-only: a failed edit fails the cook rather than escaping the noexcept hook.
    try
    {
        const std::filesystem::path root(source);
        switch (sourceEditAfterCook)
        {
            case SourceEdit::None:
                break;
            case SourceEdit::ChangeFirstRemoveLast:
                ChangeSource(root, FIRST_INPUT);
                RemoveSource(root, LAST_INPUT);
                break;
            case SourceEdit::RemoveFirstChangeLast:
                RemoveSource(root, FIRST_INPUT);
                ChangeSource(root, LAST_INPUT);
                break;
            case SourceEdit::EscapeMiddleChangeLast:
                EscapeSource(root, MIDDLE_INPUT);
                ChangeSource(root, LAST_INPUT);
                break;
            case SourceEdit::ChangeFirstEscapeMiddle:
                ChangeSource(root, FIRST_INPUT);
                EscapeSource(root, MIDDLE_INPUT);
                break;
        }
    }
    catch (...)
    {
        return false;
    }
    return true;
}

/// What an import of an edited source copy produced, with the scratch path replaced by <work>.
struct EditedImport final
{
    bool IsImported = false;
    AssetErrorCode Code = AssetErrorCode::InvalidScene;
    std::string Message;
    std::string Level;
    /// After an unedited import: the commit of a fresh candidate whose source then changed.
    bool IsStaleCommitted = false;
    AssetErrorCode StaleCode = AssetErrorCode::InvalidScene;
    std::string StaleMessage;
};

std::string WithoutWorkPath(std::string_view message, const std::filesystem::path& work)
{
    std::string text(message);
    const std::string prefix = std::filesystem::canonical(work).string();
    const usize at = text.find(prefix);
    if (at != std::string::npos)
    {
        text.replace(at, prefix.size(), "<work>");
    }
    return text;
}

/// Import a fresh copy of the golden source through CookThenEditSource.
EditedImport ImportEditedSource(SourceEdit edit, ludus::foundation::threading::JobSystem* jobsOrNull)
{
    const std::filesystem::path work = MakeWorkDirectory();
    const std::filesystem::path project = work / "project";
    const std::filesystem::path source = work / "source";
    std::filesystem::create_directories(project);
    std::filesystem::copy(std::string(GOLDENS) + "/source", source, std::filesystem::copy_options::recursive);
    std::ofstream(work / "outside.tga", std::ios::binary) << "outside";
    SequentialUuids counter;
    UuidSource uuids{&SequentialUuids::Generate, &counter};
    AssetOptions options;
    options.UuidsOrNull = &uuids;
    options.SourceOrigin = VIRTUAL_ORIGIN;
    options.CookOrNull = CookThenEditSource;
    options.JobsOrNull = jobsOrNull;
    AssetError error;
    Array<uint8> empty;
    REQUIRE(CreateLevel("Edited", options, empty, error));
    EditedImport result;
    Array<uint8> level;
    sourceEditAfterCook = edit;
    result.IsImported = ImportScene(project.string(), source.string(), ToSpan(empty), options, level, error);
    sourceEditAfterCook = SourceEdit::None;
    result.Code = error.Code;
    result.Message = WithoutWorkPath(error.Message.View(), work);
    result.Level = ToText(level);
    if (result.IsImported == true)
    {
        // The commit rechecks the source the candidate was prepared from.
        ImportCandidate candidate;
        REQUIRE(PrepareImport(project.string(), source.string(), ToSpan(level), options, candidate, error));
        ChangeSource(source, FIRST_INPUT);
        AssetError stale;
        const content::Digest* publishedOrNull =
            candidate.HasPublishedCatalog == true ? &candidate.PublishedCatalog : nullptr;
        result.IsStaleCommitted = CommitImport(project.string(),
                                               ToSpan(candidate.Level),
                                               ToSpan(candidate.Catalog),
                                               publishedOrNull,
                                               source.string(),
                                               options,
                                               stale);
        result.StaleCode = stale.Code;
        result.StaleMessage = WithoutWorkPath(stale.Message.View(), work);
    }
    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
    return result;
}
} // namespace

TEST_CASE("Source input rechecks report the serial result for every worker count", "[scene_assets][jobs]")
{
    struct Expectation
    {
        SourceEdit Edit = SourceEdit::None;
        AssetErrorCode Code = AssetErrorCode::InvalidScene;
        std::string_view Message;
    };
    const Expectation expectations[] = {
        {SourceEdit::None, AssetErrorCode::Conflict, "Source changed before catalog publication"},
        {SourceEdit::ChangeFirstRemoveLast, AssetErrorCode::Conflict, "Source changed during conversion"},
        {SourceEdit::RemoveFirstChangeLast,
         AssetErrorCode::AssetCorrupt,
         "No such file or directory: '<work>/source/LICENSE.txt'"},
        {SourceEdit::EscapeMiddleChangeLast,
         AssetErrorCode::UnsupportedScene,
         "asset symlink escapes source directory"},
        {SourceEdit::ChangeFirstEscapeMiddle, AssetErrorCode::Conflict, "Source changed during conversion"},
    };
    constexpr usize WORKER_BUDGET_COUNT = 3;
    constexpr StaticArray<uint32, WORKER_BUDGET_COUNT> WORKER_BUDGETS{0, 1, 3};
    for (const Expectation& expectation : expectations)
    {
        INFO("edit " << static_cast<int>(expectation.Edit));
        const EditedImport serial = ImportEditedSource(expectation.Edit, nullptr);
        INFO(serial.Message);
        if (expectation.Edit == SourceEdit::None)
        {
            REQUIRE(serial.IsImported == true);
            CHECK(serial.IsStaleCommitted == false);
            CHECK(serial.StaleCode == expectation.Code);
            CHECK(serial.StaleMessage == expectation.Message);
        }
        else
        {
            CHECK(serial.IsImported == false);
            CHECK(serial.Code == expectation.Code);
            CHECK(serial.Message == expectation.Message);
        }
        for (const uint32 workers : WORKER_BUDGETS)
        {
            INFO("workers " << workers);
            ludus::foundation::threading::JobSystem system;
            REQUIRE(system.Initialize({ .WorkerCount = workers }) == ludus::foundation::threading::JobStatus::Ok);
            const EditedImport parallel = ImportEditedSource(expectation.Edit, &system);
            CHECK(parallel.IsImported == serial.IsImported);
            CHECK(parallel.Code == serial.Code);
            CHECK(parallel.Message == serial.Message);
            CHECK(parallel.Level == serial.Level);
            CHECK(parallel.IsStaleCommitted == serial.IsStaleCommitted);
            CHECK(parallel.StaleCode == serial.StaleCode);
            CHECK(parallel.StaleMessage == serial.StaleMessage);
        }
    }
}

TEST_CASE("Source-preserving PBRT materials survive project publication and moved reopen", "[scene_assets][materials]")
{
    namespace format = ludus::content::scene_format;
    GoldenProject project;
    AssetOptions options = project.Options;
    options.ShouldPreserveReferenceMaterials = true; // Exercise the production default route.
    Array<uint8> empty;
    AssetError error;
    REQUIRE(CreateLevel("Materials", options, empty, error) == true);
    Array<uint8> imported;
    REQUIRE(ImportScene(project.Project.string(), project.Source, ToSpan(empty), options, imported, error) == true);
    Array<uint8> bundle;
    REQUIRE(AssembleLevel(project.Project.string(), ToSpan(imported), {}, bundle, error) == true);
    ludus::resources::SceneDefinition scene;
    REQUIRE(ludus::resources::SceneDefinition::Decode(bundle.GetData(), bundle.GetSize(), scene) ==
            ludus::resources::Status::Ok);
    bool hasPaint = false;
    bool hasBrass = false;
    bool hasCoating = false;
    usize emitterCount = 0;
    std::map<float32, bool> emissions;
    for (const ludus::resources::SceneMaterial& material : scene.Materials)
    {
        REQUIRE(material.HasSourceMaterial == true);
        CHECK(material.SourceIdentity != 0);
        if (material.Name.View() == "Paint")
        {
            hasPaint = true;
            CHECK(material.Scattering.Model == format::MaterialModel::Diffuse);
            CHECK(material.Reflectance.X == 0.2F);
            CHECK(material.Reflectance.Y == 0.4F);
            CHECK(material.Reflectance.Z == 0.6F);
            CHECK(material.Texture == format::NO_TEXTURE);
        }
        if (material.Name.View() == "Brass")
        {
            hasBrass = true;
            CHECK(material.Scattering.Model == format::MaterialModel::Conductor);
            CHECK(material.Scattering.Eta[0] == 0.444F);
            CHECK(material.Scattering.K[2] == 1.829F);
            CHECK(material.Scattering.AlphaU == 0.1F);
            CHECK(material.Scattering.AlphaV == 0.1F);
        }
        if (material.Name.View() == "Tiles")
        {
            hasCoating = true;
            CHECK(material.Scattering.Model == format::MaterialModel::CoatedDiffuse);
            CHECK(material.Scattering.AlphaU == 0.01F);
        }
        if (material.Emission.X > 0)
        {
            ++emitterCount;
            emissions[material.Emission.X] = true;
            CHECK(material.IsEmissionTwoSided == false);
        }
    }
    REQUIRE(hasPaint == true);
    REQUIRE(hasBrass == true);
    REQUIRE(hasCoating == true);
    CHECK(emitterCount == 4);
    CHECK(emissions.count(10.0F) == 1);
    CHECK(emissions.count(3.0F) == 1);
    CHECK(emissions.count(20.0F) == 1);
    CHECK(emissions.count(0.5F) == 1);
    usize extensionOffset = sizeof(format::Header);
    for (const ludus::resources::SceneTexture& texture : scene.Textures)
    {
        extensionOffset += sizeof(format::TextureRecord) + texture.Pixels.GetSize();
    }
    extensionOffset += sizeof(format::MaterialRecordV6);
    Array<uint8> malformed;
    REQUIRE(malformed.TryAddRange(bundle.AsSpan()) == true);
    REQUIRE(TryWriteLittleEndian(~uint32{0},
                                 {malformed.GetData() + extensionOffset + offsetof(format::MaterialExtensionV7, Model),
                                  sizeof(uint32)}) == true);
    const usize preservedCount = scene.Materials.GetSize();
    CHECK(ludus::resources::SceneDefinition::Decode(malformed.GetData(), malformed.GetSize(), scene) !=
          ludus::resources::Status::Ok);
    CHECK(scene.Materials.GetSize() == preservedCount);
    REQUIRE(TryWriteLittleEndian(uint32{0},
                                 {malformed.GetData() + extensionOffset + offsetof(format::MaterialExtensionV7, Model),
                                  sizeof(uint32)}) == true);
    REQUIRE(
        TryWriteLittleEndian(static_cast<uint32>(format::MAX_MATERIAL_NAME_BYTES + 1),
                             {malformed.GetData() + extensionOffset + offsetof(format::MaterialExtensionV7, NameBytes),
                              sizeof(uint32)}) == true);
    CHECK(ludus::resources::SceneDefinition::Decode(malformed.GetData(), malformed.GetSize(), scene) !=
          ludus::resources::Status::Ok);
    CHECK(scene.Materials.GetSize() == preservedCount);
    Array<uint8> saved;
    REQUIRE(SaveLevel(project.Project.string(), "scenes/materials.lulevel", ToSpan(imported), nullptr, saved, error) ==
            true);
    const std::filesystem::path moved = project.Work / "moved-materials";
    std::filesystem::copy(project.Project, moved, std::filesystem::copy_options::recursive);
    Array<uint8> reopened;
    REQUIRE(OpenLevel(moved.string(), "scenes/materials.lulevel", reopened, error) == true);
    Array<uint8> assembled;
    REQUIRE(AssembleLevel(moved.string(), ToSpan(reopened), {}, assembled, error) == true);
    CHECK(ToText(bundle) == ToText(assembled));
    // One truncated extension must not publish a replacement definition.
    const usize oldMaterials = scene.Materials.GetSize();
    CHECK(ludus::resources::SceneDefinition::Decode(bundle.GetData(), bundle.GetSize() - 1, scene) !=
          ludus::resources::Status::Ok);
    CHECK(scene.Materials.GetSize() == oldMaterials);
}

TEST_CASE("Named material and grouped mesh resources survive reordered PBRT declarations", "[scene_assets][materials]")
{
    GoldenProject project;
    const std::filesystem::path source = project.Work / "reordered-source";
    std::filesystem::copy(project.Source, source, std::filesystem::copy_options::recursive);
    AssetOptions options = project.Options;
    options.ShouldPreserveReferenceMaterials = true;
    Array<uint8> empty;
    Array<uint8> first;
    AssetError error;
    REQUIRE(CreateLevel("Identities", options, empty, error) == true);
    REQUIRE(ImportScene(project.Project.string(), source.string(), ToSpan(empty), options, first, error) == true);
    const auto collect = [&project]() {
        std::map<std::string, std::string> result;
        detail::JsonTree catalog;
        detail::JsonIndex root = detail::NO_JSON_NODE;
        content::MessageText message;
        const std::string bytes = ReadWhole(project.Project / "assets/catalog.json");
        REQUIRE(detail::ParsePythonJson(ToSpan(bytes), catalog, root, message) == true);
        const detail::JsonIndex resources = catalog.GetMember(root, "resources");
        for (usize index = 0; index < catalog.GetCount(resources); ++index)
        {
            const std::string identity(catalog.GetKey(resources, index));
            const detail::JsonIndex record = catalog.GetValue(resources, index);
            if (catalog.IsString(catalog.GetMember(record, "kind"), "mesh") == true)
            {
                result["mesh/" + identity] = std::string(catalog.GetText(catalog.GetMember(record, "sha256")));
            }
            if (catalog.IsString(catalog.GetMember(record, "kind"), "material") == false)
            {
                continue;
            }
            const std::string path(catalog.GetText(catalog.GetMember(record, "path")));
            const std::string materialBytes = ReadWhole(project.Project / path);
            detail::JsonTree material;
            detail::JsonIndex materialRoot = detail::NO_JSON_NODE;
            REQUIRE(detail::ParsePythonJson(ToSpan(materialBytes), material, materialRoot, message) == true);
            const detail::JsonIndex sourceMaterial = material.GetMember(materialRoot, "source_material");
            if (sourceMaterial == detail::NO_JSON_NODE)
            {
                continue;
            }
            const std::string name(material.GetText(material.GetMember(sourceMaterial, "name")));
            if (name.empty() == false)
            {
                result["material/" + name] = identity;
            }
        }
        return result;
    };
    const std::map<std::string, std::string> before = collect();
    REQUIRE(before.empty() == false);
    const std::filesystem::path entry = source / "scene-v4.pbrt";
    std::string text = ReadWhole(entry);
    const usize begin = text.find("MakeNamedMaterial \"Brass\"");
    const usize end = text.find("MakeNamedMaterial \"Paint\"", begin);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    const std::string brass = text.substr(begin, end - begin);
    text.erase(begin, end - begin);
    const usize destination = text.find("MakeNamedMaterial \"WallPaper\"");
    REQUIRE(destination != std::string::npos);
    text.insert(destination, brass);
    std::ofstream(entry, std::ios::binary) << text;
    Array<uint8> second;
    REQUIRE(ImportScene(project.Project.string(), source.string(), ToSpan(first), options, second, error) == true);
    CHECK(collect() == before);
}
