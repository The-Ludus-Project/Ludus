#include "entropy_fixture.h"

#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>

namespace
{
using namespace ludus::content;
using namespace ludus::content::identity_records;
using namespace ludus::content::identity_records::tests;

constexpr std::string_view ROOM_A = R"({"version": 1, "schema": "game.v1", "entities": [
  {"id": "player-start", "name": "Player", "components": {}},
  {"id": "guard-a", "name": "Guard", "components": {}},
  {"id": "entity-0123456789abcdef01234567", "name": "Added", "components": {}}]})";
constexpr std::string_view ROOM_B = R"({"version": 1, "schema": "game.v1", "entities": [
  {"id": "level-settings", "name": "Settings", "components": {}}]})";

[[nodiscard]] std::string_view GetText(const Bytes& bytes) noexcept
{
    return bytes.String();
}

[[nodiscard]] MigrationManifest Generate(CountingEntropy& inoutEntropy)
{
    const MigrationSource sources[] = {{"levels/room-b.world.json", ROOM_B}, {"levels/room-a.world.json", ROOM_A}};
    MigrationManifest manifest;
    IdentityRecordError error;
    REQUIRE(GenerateMigrationManifest(sources, inoutEntropy.GetSource(), manifest, error) ==
            IdentityRecordStatus::Success);
    REQUIRE(error.Status == IdentityRecordStatus::Success);
    return manifest;
}

[[nodiscard]] std::string_view ReadStatusName(std::string_view json)
{
    MigrationManifest manifest;
    IdentityRecordError error;
    return GetIdentityRecordStatusName(manifest.Read(json, error));
}
// Exercise the encoded byte limit and both semantic count limits in one input.
[[nodiscard]] Bytes MakeLargestManifest()
{
    Bytes input;
    REQUIRE(input.Resize(MAX_MANIFEST_BYTES) == true);
    ludus::foundation::parsing::JsonWriter writer(input.Data());
    CountingEntropy entropy;
    const auto writeUuid = [&]() {
        identity::Uuid uuid;
        REQUIRE(identity::TryGenerateUuidV4(entropy.GetSource(), uuid) == identity::UuidStatus::Success);
        StaticArray<char, identity::UUID_TEXT_BUFFER_BYTES> text;
        REQUIRE(identity::FormatUuid(uuid, text.GetData(), text.GetSize()) == identity::UuidStatus::Success);
        writer.String({text.GetData(), identity::UUID_TEXT_LENGTH});
    };
    const auto writeLabel = [&](usize index) {
        constexpr usize RADIX = 10;
        constexpr usize DIGITS = 3;
        StaticArray<char, DIGITS> text;
        for (usize position = DIGITS; position > 0; --position)
        {
            text[position - 1] = static_cast<char>('0' + index % RADIX);
            index /= RADIX;
        }
        writer.String({text.GetData(), text.GetSize()});
    };
    writer.Raw(R"({"format":"ludus.identity-migration","version":1,"documents":[)");
    for (usize i = 0; i < MAX_MANIFEST_DOCUMENTS; ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.Raw(R"({"path":)");
        writeLabel(i);
        writer.Raw(R"(,"document":)");
        writeUuid();
        writer.Raw(R"(,"objects":[)");
        for (usize j = 0; j < MAX_DOCUMENT_OBJECTS; ++j)
        {
            if (j != 0)
            {
                writer.Raw(",");
            }
            writer.Raw(R"({"legacy_id":)");
            writeLabel(j);
            writer.Raw(R"(,"object":)");
            writeUuid();
            writer.Raw("}");
        }
        writer.Raw("]}");
    }
    writer.Raw("]}");
    std::span<const uint8> encoded;
    REQUIRE(writer.Finish(encoded) == ludus::foundation::parsing::ParseStatus::Ok);
    for (uint8& byte : input.Data().subspan(encoded.size()))
    {
        byte = ' ';
    }
    return input;
}
} // namespace

TEST_CASE("Migration manifests sort documents and legacy IDs and mint version-4 UUIDs", "[identity][manifest]")
{
    CountingEntropy entropy;
    const MigrationManifest manifest = Generate(entropy);
    const std::span<const MigrationDocument> documents = manifest.GetDocuments();
    REQUIRE(documents.size() == 2);
    CHECK(documents[0].Path.View() == "levels/room-a.world.json");
    CHECK(documents[1].Path.View() == "levels/room-b.world.json");
    // One document UUID and one UUID per entity, each a single entropy call.
    CHECK(entropy.CallCount == 6);

    const std::span<const MigrationObject> objects = manifest.GetObjects(documents[0]);
    REQUIRE(objects.size() == 3);
    CHECK(objects[0].LegacyId.View() == "entity-0123456789abcdef01234567");
    CHECK(objects[1].LegacyId.View() == "guard-a");
    CHECK(objects[2].LegacyId.View() == "player-start");
    for (const MigrationObject& object : objects)
    {
        CHECK(ludus::foundation::identity::ValidateUuid(object.Object.Value,
                                                        ludus::foundation::identity::UuidPolicy::RequireVersion4) ==
              ludus::foundation::identity::UuidStatus::Success);
    }
    const MigrationObject* const guardOrNull = manifest.FindObject(documents[0], "guard-a");
    REQUIRE(guardOrNull != nullptr);
    CHECK(guardOrNull == &objects[1]);
    CHECK(manifest.FindObject(documents[0], "level-settings") == nullptr);
    CHECK(manifest.FindDocument("levels/room-c.world.json") == nullptr);
}

TEST_CASE("A migration manifest replays to identical bytes", "[identity][manifest]")
{
    CountingEntropy entropy;
    const MigrationManifest generated = Generate(entropy);
    Bytes first;
    REQUIRE(generated.Write(first) == IdentityRecordStatus::Success);

    MigrationManifest replayed;
    IdentityRecordError error;
    REQUIRE(replayed.Read(GetText(first), error) == IdentityRecordStatus::Success);
    Bytes second;
    REQUIRE(replayed.Write(second) == IdentityRecordStatus::Success);
    CHECK(GetText(second) == GetText(first));
    CHECK(GetText(first).starts_with("{\n  \"format\": \"ludus.identity-migration\",\n  \"version\": 1,\n"));
    CHECK(GetText(first).ends_with("}\n"));

    // Reading never mints: the identities are exactly the generated ones.
    const MigrationDocument* const roomOrNull = replayed.FindDocument("levels/room-a.world.json");
    REQUIRE(roomOrNull != nullptr);
    CHECK(roomOrNull->Document == generated.GetDocuments()[0].Document);
    CHECK(replayed.FindObject(*roomOrNull, "player-start")->Object ==
          generated.FindObject(generated.GetDocuments()[0], "player-start")->Object);
}

TEST_CASE("Unavailable entropy mints nothing and preserves the output manifest", "[identity][manifest][entropy]")
{
    CountingEntropy original;
    MigrationManifest manifest = Generate(original);
    Bytes before;
    REQUIRE(manifest.Write(before) == IdentityRecordStatus::Success);

    const MigrationSource sources[] = {{"levels/room-a.world.json", ROOM_A}};
    IdentityRecordError error;
    CHECK(GenerateMigrationManifest(sources, MakeUnavailableEntropy(), manifest, error) ==
          IdentityRecordStatus::EntropyUnavailable);
    CHECK(error.Status == IdentityRecordStatus::EntropyUnavailable);

    // Entropy that fails partway through also publishes no partial manifest.
    CountingEntropy failing;
    failing.CallsBeforeFailure = 2;
    CHECK(GenerateMigrationManifest(sources, failing.GetSource(), manifest, error) ==
          IdentityRecordStatus::EntropyUnavailable);
    CHECK(failing.CallCount == 2);

    Bytes after;
    REQUIRE(manifest.Write(after) == IdentityRecordStatus::Success);
    CHECK(GetText(after) == GetText(before));
}

TEST_CASE("Colliding candidates are retried and then reported as a conflict", "[identity][manifest][entropy]")
{
    const MigrationSource sources[] = {{"levels/room-b.world.json", ROOM_B}};
    CountingEntropy constant;
    constant.IsConstant = true;
    MigrationManifest manifest;
    IdentityRecordError error;
    CHECK(GenerateMigrationManifest(sources, constant.GetSource(), manifest, error) ==
          IdentityRecordStatus::IdentityConflict);
    // The document UUID took one call; the object tried every candidate.
    CHECK(constant.CallCount == 1 + MAX_UUID_GENERATION_ATTEMPTS);
    CHECK(manifest.GetDocuments().empty() == true);
}

TEST_CASE("Generation rejects documents it cannot migrate", "[identity][manifest]")
{
    CountingEntropy entropy;
    MigrationManifest manifest;
    IdentityRecordError error;
    const auto generate = [&](std::string_view path, std::string_view json) {
        const MigrationSource sources[] = {{path, json}};
        return GenerateMigrationManifest(sources, entropy.GetSource(), manifest, error);
    };
    CHECK(generate("a.world.json", R"({"version": 2, "schema": "s", "document": "x", "entities": []})") ==
          IdentityRecordStatus::UnsupportedVersion);
    CHECK(generate("a.world.json", R"({"version": 1, "schema": "s", "entities": [], "scene": {}})") ==
          IdentityRecordStatus::Unsupported);
    CHECK(generate("a.world.json",
                   R"({"version": 1, "schema": "s", "entities": [{"id": "a", "name": "A", "components": {},
                       "scene": {}}]})") == IdentityRecordStatus::Unsupported);
    CHECK(generate("a.world.json", R"({"version": 1, "schema": "s", "entities": [
          {"id": "a", "name": "A", "components": {}}, {"id": "a", "name": "B", "components": {}}]})") ==
          IdentityRecordStatus::InvalidLegacyId);
    CHECK(generate("a.world.json", R"({"version": 1, "schema": "s", "entities": [
          {"id": "has space", "name": "A", "components": {}}]})") == IdentityRecordStatus::InvalidLegacyId);
    CHECK(generate("../outside.world.json", ROOM_B) == IdentityRecordStatus::InvalidLegacyId);
    CHECK(generate("a.world.json", "{") == IdentityRecordStatus::Malformed);

    const MigrationSource repeated[] = {{"a.world.json", ROOM_B}, {"a.world.json", ROOM_A}};
    CHECK(GenerateMigrationManifest(repeated, entropy.GetSource(), manifest, error) ==
          IdentityRecordStatus::InvalidLegacyId);
    CHECK(entropy.CallCount == 0);
    CHECK(manifest.GetDocuments().empty() == true);
}

TEST_CASE("Manifest reading enforces the versioned schema and identity rules", "[identity][manifest]")
{
    constexpr std::string_view VALID = R"({"format": "ludus.identity-migration", "version": 1, "documents": [
      {"path": "a.world.json", "document": "11111111-1111-4111-8111-111111111111", "objects": [
        {"legacy_id": "alpha", "object": "22222222-2222-4222-8222-222222222222"},
        {"legacy_id": "beta", "object": "33333333-3333-4333-8333-333333333333"}]}]})";
    CHECK(ReadStatusName(VALID) == "success");

    const auto replace = [&](std::string_view from, std::string_view to) {
        char text[1024] = {};
        const usize at = VALID.find(from);
        REQUIRE(at != std::string_view::npos);
        usize size = 0;
        for (const std::string_view part : {VALID.substr(0, at), to, VALID.substr(at + from.size())})
        {
            for (const char item : part)
            {
                text[size++] = item;
            }
        }
        return ReadStatusName(std::string_view(text, size));
    };
    CHECK(replace("\"version\": 1", "\"version\": 2") == "unsupported-version");
    CHECK(replace("ludus.identity-migration", "ludus.other") == "unsupported-version");
    CHECK(replace("\"version\": 1,", "\"version\": 1, \"extra\": 0,") == "malformed");
    CHECK(replace("11111111-1111-4111", "00000000-0000-0000") == "invalid-identity");
    CHECK(replace("11111111-1111-4111-8111-111111111111", "00000000-0000-0000-0000-000000000000") ==
          "invalid-identity");
    CHECK(replace("22222222-2222-4222", "22222222-2222-1222") == "invalid-identity");
    CHECK(replace("33333333-3333-4333-8333-333333333333", "11111111-1111-4111-8111-111111111111") ==
          "duplicate-identity");
    CHECK(replace("\"beta\"", "\"aaa\"") == "invalid-legacy-id");
    CHECK(replace("\"beta\"", "\"alpha\"") == "invalid-legacy-id");
    CHECK(replace("\"beta\"", "\"be ta\"") == "invalid-legacy-id");
    CHECK(replace("a.world.json", "/a.world.json") == "invalid-legacy-id");
    CHECK(ReadStatusName("[]") == "malformed");

    // A failed read keeps the previous manifest.
    MigrationManifest manifest;
    IdentityRecordError error;
    REQUIRE(manifest.Read(VALID, error) == IdentityRecordStatus::Success);
    CHECK(manifest.Read("{}", error) == IdentityRecordStatus::Malformed);
    CHECK(error.Message.View().empty() == false);
    CHECK(manifest.GetDocuments().size() == 1);
}

TEST_CASE("Manifest read reports precise locations and keeps the accepted tables", "[identity][manifest]")
{
    constexpr std::string_view REPLACEMENT = R"({"format":"ludus.identity-migration","version":1,"documents":[
      {"path":"different.world.json","document":"11111111-1111-4111-8111-111111111111","objects":[
        {"legacy_id":"alpha","object":"22222222-2222-4222-8222-222222222222"},
        {"legacy_id":"beta","object":"33333333-3333-4333-8333-333333333333"}]}]})";
    CountingEntropy entropy;
    MigrationManifest manifest = Generate(entropy);
    const MigrationDocument* accepted = manifest.GetDocuments().data();
    const MigrationObject* acceptedObjects = manifest.GetObjects(*accepted).data();
    Bytes before;
    REQUIRE(manifest.Write(before) == IdentityRecordStatus::Success);
    IdentityRecordError error;
    const auto checkFailure = [&](std::string_view json, IdentityRecordStatus status, std::string_view location) {
        REQUIRE(manifest.Read(json, error) == status);
        CHECK(error.Status == status);
        CHECK(error.Message.View().ends_with(location) == true);
        REQUIRE(manifest.GetDocuments().data() == accepted);
        CHECK(manifest.GetObjects(*accepted).data() == acceptedObjects);
        Bytes after;
        REQUIRE(manifest.Write(after) == IdentityRecordStatus::Success);
        CHECK(after.String() == before.String());
    };
    checkFailure("{", IdentityRecordStatus::Malformed, "byte 1");
    checkFailure("{}", IdentityRecordStatus::Malformed, "$.format");
    const auto replace =
        [&](std::string_view from, std::string_view to, IdentityRecordStatus status, std::string_view location) {
            const usize offset = REPLACEMENT.find(from);
            REQUIRE(offset != std::string_view::npos);
            std::string replacement(REPLACEMENT);
            replacement.replace(offset, from.size(), to);
            checkFailure(replacement, status, location);
        };
    replace("different.world.json", "../outside", IdentityRecordStatus::InvalidLegacyId, "$.documents[0].path");
    replace(R"json("beta")json", "false", IdentityRecordStatus::Malformed, "$.documents[0].objects[1].legacy_id");
    replace("beta", "alpha", IdentityRecordStatus::InvalidLegacyId, "$.documents[0].objects[1].legacy_id");
    replace("33333333-3333-4333-8333-333333333333",
            "11111111-1111-4111-8111-111111111111",
            IdentityRecordStatus::DuplicateIdentity,
            "$.documents[0].objects[1].object");

    // Copy decoded strings from mutable input and destroy it after publication.
    {
        std::string input(REPLACEMENT);
        REQUIRE(manifest.Read(input, error) == IdentityRecordStatus::Success);
        input.assign(input.size(), '_');
    }
    REQUIRE(manifest.GetDocuments().size() == 1);
    CHECK(manifest.GetDocuments()[0].Path.View() == "different.world.json");
    CHECK(manifest.GetObjects(manifest.GetDocuments()[0])[1].LegacyId.View() == "beta");
    CHECK(error.Status == IdentityRecordStatus::Success);
    CHECK(error.Message.View().empty() == true);
}

TEST_CASE("Maximum manifest decode measures and enforces peak requested bytes", "[identity][manifest]")
{
    const Bytes input = MakeLargestManifest();
    constexpr usize OBJECT_COUNT = MAX_MANIFEST_DOCUMENTS * MAX_DOCUMENT_OBJECTS;
    constexpr usize TABLE_BYTES =
        MAX_MANIFEST_DOCUMENTS * sizeof(MigrationDocument) + OBJECT_COUNT * sizeof(MigrationObject);
    MigrationManifest manifest;
    IdentityRecordError error;
    usize peakBytes = 0;
    const AllocationDomain& system = GetSystemAllocationDomain();
    const AllocationDomain& containers = GetContainerAllocationDomain();
    const AllocationStatistics systemBefore = GetAllocationStatistics(system);
    const AllocationStatistics containersBefore = GetAllocationStatistics(containers);
    const IdentityRecordStatus decoded = manifest.Read(input.String(), error, peakBytes);
    const AllocationStatistics systemAfter = GetAllocationStatistics(system);
    const AllocationStatistics containersAfter = GetAllocationStatistics(containers);
    REQUIRE(decoded == IdentityRecordStatus::Success);
    CHECK(systemAfter.AllocationCount - systemBefore.AllocationCount == 1);
    CHECK(systemAfter.LiveRequestedBytes == systemBefore.LiveRequestedBytes);
    CHECK(containersAfter.AllocationCount - containersBefore.AllocationCount == 3);
    CHECK(containersAfter.LiveRequestedBytes - containersBefore.LiveRequestedBytes == TABLE_BYTES);
    const uint64 parserBytes = systemAfter.AllocatedBytes - systemBefore.AllocatedBytes;
    const uint64 indexedBytes = containersAfter.AllocatedBytes - containersBefore.AllocatedBytes;
    const uint64 parsingPeak = parserBytes + TABLE_BYTES;
    CAPTURE(peakBytes, parserBytes, indexedBytes);
    CHECK(peakBytes == (parsingPeak > indexedBytes ? parsingPeak : indexedBytes));
    CHECK(peakBytes <= MAX_MANIFEST_DECODE_BYTES);
    REQUIRE(manifest.GetDocuments().size() == MAX_MANIFEST_DOCUMENTS);
    const MigrationDocument* accepted = manifest.GetDocuments().data();
    const MigrationObject* acceptedObjects = manifest.GetObjects(*accepted).data();
    CHECK(manifest.GetObjects(manifest.GetDocuments().back()).size() == MAX_DOCUMENT_OBJECTS);

    const usize exactBudget = peakBytes;
    CHECK(manifest.Read(input.String(), error, peakBytes, 0) == IdentityRecordStatus::Limit);
    CHECK(peakBytes == 0);
    CHECK(error.Message.View().ends_with('$') == true);
    CHECK(manifest.Read(input.String(), error, peakBytes, exactBudget - 1) == IdentityRecordStatus::Limit);
    CHECK(peakBytes < exactBudget);
    CHECK(error.Message.View().ends_with("$.documents") == true);
    REQUIRE(manifest.GetDocuments().data() == accepted);
    CHECK(manifest.GetObjects(*accepted).data() == acceptedObjects);
    CHECK(manifest.GetDocuments().back().Path.View() == "063");
    CHECK(manifest.GetObjects(manifest.GetDocuments().back()).back().LegacyId.View() == "127");
    CHECK(GetAllocationStatistics(system).LiveRequestedBytes == systemAfter.LiveRequestedBytes);
    CHECK(GetAllocationStatistics(containers).LiveRequestedBytes == containersAfter.LiveRequestedBytes);

    REQUIRE(manifest.Read(input.String(), error, peakBytes, exactBudget) == IdentityRecordStatus::Success);
    CHECK(peakBytes == exactBudget);
}

TEST_CASE("Legacy IDs follow the Editor World ID rule", "[identity][manifest]")
{
    CHECK(IsLegacyId("player-start") == true);
    CHECK(IsLegacyId("starter.cube") == true);
    CHECK(IsLegacyId("Ns:Item_2") == true);
    CHECK(IsLegacyId("") == false);
    CHECK(IsLegacyId("a/b") == false);
    CHECK(IsLegacyId(std::string_view("0123456789012345678901234567890123456789012345678901234567890123", 64)) == true);
    CHECK(IsLegacyId("01234567890123456789012345678901234567890123456789012345678901234") == false);
}
