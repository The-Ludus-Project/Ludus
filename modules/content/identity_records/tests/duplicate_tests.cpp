// Stage 4 slice C: graph duplication as one transaction.
// Covers the "Validation and performance gates" cases owned by this slice:
// graph duplication (old->new remap), external references retained, bounded
// retry -> IdentityConflict, and a failed step publishing nothing.
// See docs/architecture/object-handles-uuid.md ("Authoring and prefab instances").

#include <ludus/content/content.h>
#include <ludus/content/identity_records/world_records.h>
#include <ludus/foundation/identity/uuid.hpp>

#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "entropy_fixture.h"

namespace
{
using namespace ludus::content;
using namespace ludus::content::identity_records;
using ludus::content::identity_records::tests::CountingEntropy;

// Schema with one entity-typed property, enemy.target.
constexpr std::string_view SCHEMA = R"({"version": 1, "id": "game.v1", "components": [
  {"id": "enemy", "properties": [
    {"id": "target", "type": "entity"},
    {"id": "sprite", "type": "string"}]}]})";

// A version-2 record: guard targets player (internal when both copied), loner
// targets guard (an external reference when only guard is copied).
constexpr std::string_view RECORD = R"({
  "version": 2,
  "schema": "game.v1",
  "document": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
  "entities": [
    { "uuid": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "label": "guard-a", "name": "Guard",
      "components": { "enemy": { "target": "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "sprite": "g" } } },
    { "uuid": "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "label": "player", "name": "Player",
      "components": {} },
    { "uuid": "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "label": "loner", "name": "Loner",
      "components": { "enemy": { "target": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "sprite": "l" } } }
  ]
})";

constexpr std::string_view GUARD = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr std::string_view PLAYER = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

// Count how many times `needle` occurs in `haystack`.
[[nodiscard]] ludus::foundation::usize CountOccurrences(std::string_view haystack, std::string_view needle) noexcept
{
    ludus::foundation::usize count = 0;
    for (ludus::foundation::usize pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size()))
    {
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("Duplicating a pair rewrites internal references and keeps external ones", "[identity][duplicate]")
{
    CountingEntropy entropy;
    entropy.NextValue = 0x40;
    Bytes output;
    IdentityRecordError error;
    const std::array<std::string_view, 2> selected{GUARD, PLAYER};
    REQUIRE(DuplicateEntities(RECORD, SCHEMA, selected, entropy.GetSource(), output, error) ==
            IdentityRecordStatus::Success);
    const std::string_view text = output.String();

    // Five entities now: the three originals plus two clones.
    REQUIRE(CountOccurrences(text, "\"uuid\"") == 5);
    // The originals are preserved verbatim, so each original UUID still appears.
    REQUIRE(CountOccurrences(text, GUARD) >= 1);
    REQUIRE(CountOccurrences(text, PLAYER) >= 1);
    // The guard clone's target was in the copied set, so it points at the player
    // clone's NEW uuid, not the original player uuid. The original player uuid
    // therefore appears exactly: once as the original entity, once as the original
    // guard's target, and once as the original loner... (loner targets guard, not
    // player). So PLAYER appears twice in originals; the guard clone must NOT add a
    // third occurrence.
    const ludus::foundation::usize playerBefore = CountOccurrences(RECORD, PLAYER);
    REQUIRE(CountOccurrences(text, PLAYER) == playerBefore);
}

TEST_CASE("Duplicating only the guard keeps its external target reference", "[identity][duplicate]")
{
    CountingEntropy entropy;
    entropy.NextValue = 0x50;
    Bytes output;
    IdentityRecordError error;
    const std::array<std::string_view, 1> selected{GUARD};
    REQUIRE(DuplicateEntities(RECORD, SCHEMA, selected, entropy.GetSource(), output, error) ==
            IdentityRecordStatus::Success);
    const std::string_view text = output.String();

    // Four entities: three originals + one clone. The guard clone targets player,
    // which was NOT copied, so the clone retains the original player uuid: the
    // original player uuid count rises by one (the clone's external reference).
    REQUIRE(CountOccurrences(text, "\"uuid\"") == 4);
    REQUIRE(CountOccurrences(text, PLAYER) == CountOccurrences(RECORD, PLAYER) + 1);
}

TEST_CASE("A failed mint publishes nothing", "[identity][duplicate]")
{
    Bytes output;
    IdentityRecordError error;
    const std::array<std::string_view, 1> selected{GUARD};

    SECTION("unavailable entropy mints nothing")
    {
        REQUIRE(DuplicateEntities(RECORD,
                                  SCHEMA,
                                  selected,
                                  ludus::content::identity_records::tests::MakeUnavailableEntropy(),
                                  output,
                                  error) == IdentityRecordStatus::EntropyUnavailable);
        REQUIRE(output.Data().empty());
    }
    SECTION("constant entropy that always collides reports IdentityConflict")
    {
        CountingEntropy entropy;
        entropy.IsConstant = true;
        // Force the minted value to equal an existing record UUID so every attempt
        // collides. 0xbb repeated matches guard's uuid bytes pattern enough that a
        // distinct constant also just needs to collide with itself after retry: the
        // second selected mint repeats the first. Use two selections of distinct
        // entities so the second mint always duplicates the first constant value.
        const std::array<std::string_view, 2> twoSelected{GUARD, PLAYER};
        entropy.NextValue = 0x11;
        REQUIRE(DuplicateEntities(RECORD, SCHEMA, twoSelected, entropy.GetSource(), output, error) ==
                IdentityRecordStatus::IdentityConflict);
        REQUIRE(output.Data().empty());
    }
}

TEST_CASE("Duplication rejects an unknown or repeated selection", "[identity][duplicate]")
{
    CountingEntropy entropy;
    entropy.NextValue = 0x60;
    Bytes output;
    IdentityRecordError error;

    SECTION("a selection that is not an entity of the record")
    {
        const std::array<std::string_view, 1> unknown{"99999999-9999-4999-8999-999999999999"};
        REQUIRE(DuplicateEntities(RECORD, SCHEMA, unknown, entropy.GetSource(), output, error) ==
                IdentityRecordStatus::UnresolvedReference);
        REQUIRE(output.Data().empty());
    }
    SECTION("a repeated selection")
    {
        const std::array<std::string_view, 2> repeated{GUARD, GUARD};
        REQUIRE(DuplicateEntities(RECORD, SCHEMA, repeated, entropy.GetSource(), output, error) ==
                IdentityRecordStatus::InvalidIdentity);
        REQUIRE(output.Data().empty());
    }
    SECTION("a version-1 record is not duplicated")
    {
        constexpr std::string_view V1 = R"({"version":1,"schema":"game.v1","entities":[]})";
        const std::array<std::string_view, 1> selected{GUARD};
        REQUIRE(DuplicateEntities(V1, SCHEMA, selected, entropy.GetSource(), output, error) ==
                IdentityRecordStatus::UnsupportedVersion);
        REQUIRE(output.Data().empty());
    }
}
