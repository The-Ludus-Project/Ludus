// Milestone 1 of the randomness architecture in the world demo: room-scoped
// scatter placements generated when a room is prepared. Covers the isolation
// rows of docs/architecture/randomness.md#milestone-1-procedural-placement that
// the kernel tests cannot: presentation, content, catalog/manifest rejection and
// load failures that publish nothing.
#include "internal/random_domains.h"
#include "internal/room_catalog.h"
#include "internal/world.h"

#include <ludus/content/procedural/environment/scatter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace ludus::world_demo;
using ludus::foundation::time::NANOSECONDS_PER_SECOND;
using ludus::foundation::time::Timestamp;
namespace procedural = ludus::content::procedural;
namespace math = ludus::foundation::math;

namespace
{
/// Pinned digest of the committed first room's placements with DEFAULT_WORLD_SEED. The browser harness
/// compares the same value; a change means every saved placement of the room moved and needs a new scatter
/// layout version, never an updated constant.
constexpr uint64 FIRST_ROOM_PLACEMENT_DIGEST = 0x599315e43924865aULL;
constexpr uint64 MILLISECOND = NANOSECONDS_PER_SECOND / 1000;
constexpr uint64 EPOCH = uint64{1} << 62U;

Timestamp At(uint64 nanoseconds)
{
    return {EPOCH + nanoseconds};
}

Level Read(std::string_view source)
{
    Level level;
    REQUIRE(ReadLevel(source, level).Error == LevelError::None);
    return level;
}

GameWorld Build(const Level& level, WorldSeed seed = {})
{
    GameWorld world;
    REQUIRE(world.Prepare(level, MAX_WORLD_ENTITIES, seed) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    return world;
}

GameWorld Build(std::string_view source, WorldSeed seed = {})
{
    return Build(Read(source), seed);
}

/// One textual edit of a committed source.
struct Replacement final
{
    std::string_view From;
    std::string_view To;
};

/// Replace the first occurrence of `replacement.From` in the committed source.
std::string Edit(std::string_view source, Replacement replacement)
{
    std::string edited(source);
    const usize at = edited.find(replacement.From);
    REQUIRE(at != std::string::npos);
    edited.replace(at, replacement.From.size(), replacement.To);
    return edited;
}

bool AreSamePlacements(std::span<const Placement> first, std::span<const Placement> second)
{
    if (first.size() != second.size())
    {
        return false;
    }
    for (usize i = 0; i < first.size(); ++i)
    {
        if ((first[i] == second[i]) == false)
        {
            return false;
        }
    }
    return true;
}

/// Placements of one layer ID, in order.
usize CollectLayer(std::span<const Placement> placements, uint8 layer, Placement* outPlacements, usize capacity)
{
    usize count = 0;
    for (const Placement& placement : placements)
    {
        if (procedural::UnpackScatterScope(placement.Scope).Layer == layer)
        {
            REQUIRE(count < capacity);
            outPlacements[count++] = placement;
        }
    }
    return count;
}

/// A world's placements filtered by layer, owned by the test.
struct LayerPlacements final
{
    Placement Items[MAX_ROOM_PLACEMENTS] = {};
    usize Count = 0;
    explicit LayerPlacements(const GameWorld& world, uint8 layer)
    {
        Count = CollectLayer(world.GetPlacements(), layer, Items, MAX_ROOM_PLACEMENTS);
    }
    [[nodiscard]] std::span<const Placement> View() const noexcept
    {
        return {Items, Count};
    }
};

void Load(Session& inoutSession, std::string_view source)
{
    REQUIRE(inoutSession.RequestLoad(source).Error == LevelError::None);
    while (inoutSession.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(inoutSession.PollLoad() == Status::Success);
    }
    REQUIRE(inoutSession.Activate());
}
} // namespace

TEST_CASE("The committed rooms generate placements from the session seed and their catalog ordinal",
          "[world-demo][scatter]")
{
    const GameWorld first = Build(BuiltinLevel("first-room"));
    const GameWorld second = Build(BuiltinLevel("second-room"));
    REQUIRE(first.GetRoom() == 1);
    REQUIRE(second.GetRoom() == 2);
    REQUIRE(first.GetWorldSeed().Value == DEFAULT_WORLD_SEED);
    REQUIRE(first.GetPlacements().empty() == false);
    REQUIRE(second.GetPlacements().empty() == false);
    REQUIRE(first.GetPlacements().size() <= MAX_ROOM_PLACEMENTS);
    CHECK(first.GetPlacementDigest() == FIRST_ROOM_PLACEMENT_DIGEST);
    CHECK(first.GetPlacementDigest() != second.GetPlacementDigest());
    // Every placement lies in its room's region with the room ordinal in its identity.
    const Level level = Read(BuiltinLevel("first-room"));
    for (const Placement& placement : first.GetPlacements())
    {
        const procedural::ScatterIdentity identity = procedural::UnpackScatterScope(placement.Scope);
        CHECK(identity.Room == 1);
        CHECK(placement.Layer < level.Scatter.LayerCount);
        CHECK(placement.Variant < level.Scatter.Layers[placement.Layer].VariantCount);
        CHECK(identity.Layer == level.Scatter.Layers[placement.Layer].Layer);
        const float32 x = static_cast<float32>(placement.LatticeX) / static_cast<float32>(level.Scatter.UnitsPerMetre);
        const float32 y = static_cast<float32>(placement.LatticeY) / static_cast<float32>(level.Scatter.UnitsPerMetre);
        CHECK(x >= level.Scatter.Minimum.X);
        CHECK(x < level.Scatter.Maximum.X);
        CHECK(y >= level.Scatter.Minimum.Y);
        CHECK(y < level.Scatter.Maximum.Y);
        CHECK(placement.Scale >= level.Scatter.Layers[placement.Layer].MinimumScale);
        CHECK(placement.Scale <= level.Scatter.Layers[placement.Layer].MaximumScale);
    }
    // The session seed drives generation; the level's authored Seed is content data.
    const GameWorld reseeded = Build(BuiltinLevel("first-room"), WorldSeed{ .Value = 7 });
    CHECK(reseeded.GetPlacementDigest() != first.GetPlacementDigest());
    const GameWorld contentSeed = Build(Edit(BuiltinLevel("first-room"), {"\"seed\": \"42\"", "\"seed\": \"43\""}));
    CHECK(contentSeed.GetPlacementDigest() == first.GetPlacementDigest());
    CHECK(contentSeed.GetReplayHash() != first.GetReplayHash());
    // Two rooms never share a placement identity.
    for (const Placement& a : first.GetPlacements())
    {
        for (const Placement& b : second.GetPlacements())
        {
            CHECK(a.Scope != b.Scope);
        }
    }
}

TEST_CASE("A session records its seed and prepares candidates with it", "[world-demo][scatter]")
{
    Session session;
    Load(session, BuiltinLevel("first-room"));
    const uint64 digest = session.World().GetPlacementDigest();
    CHECK(session.World().GetWorldSeed().Value == DEFAULT_WORLD_SEED);
    session.Seed.Value = 99;
    Load(session, BuiltinLevel("first-room"));
    CHECK(session.World().GetWorldSeed().Value == 99);
    CHECK(session.World().GetPlacementDigest() != digest);
}

TEST_CASE("Presentation isolation: rendering, pacing, interpolation and cosmetic draws leave the replay hash",
          "[world-demo][scatter]")
{
    GameWorld drawn = Build(BuiltinLevel("first-room"));
    GameWorld blind = Build(BuiltinLevel("first-room"));
    const Level level = Read(BuiltinLevel("first-room"));
    RenderFrame frame;
    RenderFrame again;
    for (usize tick = 0; tick < 90; ++tick)
    {
        const TickInput input{ .Axis = {tick < 45 ? 1.0F : -0.5F, 0.25F}, .Pressed = tick % 30 == 0 };
        REQUIRE(drawn.RunTick(input) == Status::Success);
        REQUIRE(blind.RunTick(input) == Status::Success);
        // Extra cosmetic draws: several extractions per tick with different alphas, cameras and sequences.
        for (const float32 alpha : {0.0F, 0.25F, 0.5F, 0.9F, 1.0F})
        {
            REQUIRE(drawn.Extract(alpha, frame));
            REQUIRE(drawn.Extract(alpha, again, true, tick * 7 + 1));
        }
        REQUIRE(drawn.GetReplayHash() == blind.GetReplayHash());
    }
    // The frame holds every placement behind the entities, tinted per placement but identically every frame.
    REQUIRE(drawn.Extract(0.5F, frame));
    REQUIRE(drawn.Extract(0.5F, again));
    REQUIRE(frame.Count == ROOM_FLOOR_DRAWS + level.CollisionCount + drawn.GetPlacements().size() + 3);
    bool hasDistinctTint = false;
    for (usize i = 0; i < drawn.GetPlacements().size(); ++i)
    {
        const Draw& draw = frame.Draws[ROOM_FLOOR_DRAWS + level.CollisionCount + i];
        const Draw& repeated = again.Draws[ROOM_FLOOR_DRAWS + level.CollisionCount + i];
        for (usize channel = 0; channel < 4; ++channel)
        {
            CHECK(draw.Color[channel] == repeated.Color[channel]);
        }
        CHECK(draw.Position.X == repeated.Position.X);
        CHECK(draw.Rotation == repeated.Rotation);
        const Placement& placement = drawn.GetPlacements()[i];
        const Placement& previous = drawn.GetPlacements()[i == 0 ? 0 : i - 1];
        const bool isSameVariant = placement.Layer == previous.Layer && placement.Variant == previous.Variant;
        if (i != 0 && isSameVariant == true &&
            draw.Color[1] != frame.Draws[ROOM_FLOOR_DRAWS + level.CollisionCount + i - 1].Color[1])
        {
            hasDistinctTint = true;
        }
    }
    CHECK(hasDistinctTint == true);
    // Different frame pacing over the same accepted second runs the same ticks and hash.
    GameWorld fast = Build(BuiltinLevel("first-room"));
    GameWorld slow = Build(BuiltinLevel("first-room"));
    TickDriver fastDriver;
    TickDriver slowDriver;
    REQUIRE(fastDriver.Advance(fast, At(0), {}) == Status::Success);
    REQUIRE(slowDriver.Advance(slow, At(0), {}) == Status::Success);
    for (uint64 frameIndex = 1; frameIndex <= 120; ++frameIndex)
    {
        REQUIRE(fastDriver.Advance(fast, At(frameIndex * NANOSECONDS_PER_SECOND / 120), { .Axis = {1, 0} }) ==
                Status::Success);
        RenderFrame interpolated;
        REQUIRE(fast.Extract(fastDriver.GetAlpha(), interpolated));
    }
    for (uint64 frameIndex = 1; frameIndex <= 40; ++frameIndex)
    {
        REQUIRE(slowDriver.Advance(slow, At(frameIndex * 25 * MILLISECOND), { .Axis = {1, 0} }) == Status::Success);
    }
    REQUIRE(fast.GetTick() == 60);
    REQUIRE(slow.GetTick() == 60);
    CHECK(fast.GetReplayHash() == slow.GetReplayHash());
}

TEST_CASE("Content isolation: another room's recipe, a new layer and transform ranges leave placements",
          "[world-demo][scatter]")
{
    const std::string_view firstRoom = BuiltinLevel("first-room");
    const std::string_view secondRoom = BuiltinLevel("second-room");
    const GameWorld first = Build(firstRoom);
    const GameWorld second = Build(secondRoom);
    // Editing room B's recipe changes B and leaves A untouched.
    const GameWorld editedSecond = Build(Edit(secondRoom, {"\"cell_size\": 200", "\"cell_size\": 350"}));
    CHECK(AreSamePlacements(editedSecond.GetPlacements(), second.GetPlacements()) == false);
    CHECK(AreSamePlacements(Build(firstRoom).GetPlacements(), first.GetPlacements()) == true);
    // Adding a layer to room A keeps every existing layer's placements and adds its own.
    const std::string_view anchor = "    {\n      \"id\": \"level-settings\",";
    const std::string added =
        Edit(firstRoom,
             {anchor,
              "    {\n      \"id\": \"moss\",\n      \"name\": \"moss\",\n      \"components\": {\n"
              "        \"scatter_layer\": {\n          \"layer\": 7,\n          \"cell_size\": 400,\n"
              "          \"candidates\": 1,\n          \"min_scale\": 1000,\n          \"max_scale\": 1000\n"
              "        }\n      }\n    },\n"
              "    {\n      \"id\": \"moss-patch\",\n      \"name\": \"moss-patch\",\n      \"components\": {\n"
              "        \"scatter_variant\": {\n          \"layer\": 7,\n          \"exclusion_radius\": 0,\n"
              "          \"half_x\": 0.2,\n          \"half_y\": 0.2,\n          \"red\": 0.3,\n"
              "          \"green\": 0.4,\n          \"blue\": 0.2\n        }\n      }\n    },\n" +
                  std::string(anchor)});
    const GameWorld withLayer = Build(added);
    REQUIRE(withLayer.GetPlacements().size() > first.GetPlacements().size());
    for (const uint8 layer : {uint8{1}, uint8{2}})
    {
        const LayerPlacements before(first, layer);
        const LayerPlacements after(withLayer, layer);
        REQUIRE(before.Count > 0);
        CHECK(AreSamePlacements(before.View(), after.View()) == true);
    }
    CHECK(LayerPlacements(withLayer, 7).Count > 0);
    // Changing a layer's transform range moves no placement and changes no variant.
    const GameWorld rescaled = Build(Edit(firstRoom, {"\"max_scale\": 1300", "\"max_scale\": 1100"}));
    REQUIRE(rescaled.GetPlacements().size() == first.GetPlacements().size());
    bool hasDifferentScale = false;
    for (usize i = 0; i < first.GetPlacements().size(); ++i)
    {
        const Placement& a = first.GetPlacements()[i];
        const Placement& b = rescaled.GetPlacements()[i];
        CHECK(a.Scope == b.Scope);
        CHECK(a.LatticeX == b.LatticeX);
        CHECK(a.LatticeY == b.LatticeY);
        CHECK(a.Variant == b.Variant);
        CHECK(a.Rotation == b.Rotation);
        hasDifferentScale = hasDifferentScale || a.Scale != b.Scale;
    }
    CHECK(hasDifferentScale == true);
}

TEST_CASE("The room catalog and domain manifest reject duplicates and unknown rooms", "[world-demo][scatter]")
{
    using ludus::foundation::identity::Uuid;
    const RoomCatalogEntry valid[] = {
        { .Document = { .Value = Uuid{{1}} }, .LevelId = "a", .Room = 1 },
        { .Document = { .Value = Uuid{{2}} }, .LevelId = "b", .Room = 2 },
    };
    CHECK(CheckRoomCatalog(valid, 2).IsValid == true);
    const RoomCatalogEntry repeatedRoom[] = {valid[0],
                                             {
                                                 .Document =
                                                 {
                                                     .Value = Uuid{{2}},
                                                 },
                                                 .LevelId = "b",
                                                 .Room = 1,
                                             }};
    const RoomCatalogEntry repeatedDocument[] = {valid[0],
                                                 { .Document = { .Value = Uuid{{1}} }, .LevelId = "b", .Room = 2 }};
    const RoomCatalogEntry repeatedLevel[] = {valid[0],
                                              {
                                                  .Document =
                                                  {
                                                      .Value = Uuid{{2}},
                                                  },
                                                  .LevelId = "a",
                                                  .Room = 2,
                                              }};
    const RoomCatalogEntry zeroRoom[] = {{ .Document = { .Value = Uuid{{1}} }, .LevelId = "a", .Room = 0 }};
    const RoomCatalogEntry nilDocument[] = {{ .Document = {}, .LevelId = "a", .Room = 1 }};
    for (const RoomCatalogEntry* catalog : {repeatedRoom, repeatedDocument, repeatedLevel})
    {
        const RoomCatalogCheck check = CheckRoomCatalog(catalog, 2);
        CHECK(check.IsValid == false);
        CHECK(check.Index == 1);
    }
    CHECK(CheckRoomCatalog(zeroRoom, 1).IsValid == false);
    CHECK(CheckRoomCatalog(nilDocument, 1).IsValid == false);
    CHECK(FindRoom(valid, 2, {}, "b") == 2);
    CHECK(FindRoom(valid, 2, { .Value = Uuid{{1}} }, "b") == 1);
    CHECK(FindRoom(valid, 2, { .Value = Uuid{{3}} }, "b") == 0);
    CHECK(FindCatalogRoom({}, "third-room") == 0);
    CHECK(FindCatalogRoom({}, "second-room") == 2);

    const math::RandomDomain repeated[] = {{ .Id = 1, .Label = "a" }, { .Id = 1, .Label = "b" }};
    const math::RandomDomain engineRange[] = {{ .Id = math::RANDOM_ENGINE_DOMAIN_FIRST, .Label = "a" }};
    CHECK(math::CheckRandomDomains(repeated, math::RandomDomainOwner::Game).Status ==
          math::MathStatus::InvalidArgument);
    CHECK(math::CheckRandomDomains(repeated, math::RandomDomainOwner::Game).Index == 1);
    CHECK(math::CheckRandomDomains(engineRange, math::RandomDomainOwner::Game).Status ==
          math::MathStatus::InvalidArgument);
    CHECK(math::CheckRandomDomains(RANDOM_DOMAINS, math::RandomDomainOwner::Game).Status == math::MathStatus::Success);

    // A level with a scatter but no catalog entry is rejected when prepared; the active world is preserved.
    Session session;
    Load(session, BuiltinLevel("first-room"));
    const uint64 hash = session.World().GetReplayHash();
    const std::string unknownRoom =
        Edit(BuiltinLevel("first-room"), {"\"id\": \"first-room\"", "\"id\": \"third-room\""});
    Level level;
    REQUIRE(ReadLevel(unknownRoom, level).Error == LevelError::None);
    GameWorld world;
    CHECK(world.Prepare(level) == Status::InvalidConfiguration);
    REQUIRE(session.RequestLoad(unknownRoom).Error == LevelError::None);
    CHECK(session.PollLoad() == Status::InvalidConfiguration);
    CHECK(session.GetLoadStage() == LoadStage::Failed);
    CHECK(session.World().GetReplayHash() == hash);
    CHECK(session.World().GetRoom() == 1);
    // A version-2 record is found by its document identity even when its label changes.
    Level recorded = level;
    recorded.Document = ROOM_CATALOG[0].Document;
    CHECK(Build(recorded).GetRoom() == 1);
    // A level without a scatter needs no catalog entry.
    Level plain = Read(BuiltinLevel("first-room"));
    plain.HasScatter = false;
    plain.Scatter = {};
    plain.Id = { .Text = "third-room" };
    const GameWorld bare = Build(plain);
    CHECK(bare.GetRoom() == 0);
    CHECK(bare.GetPlacements().empty() == true);
}

TEST_CASE("Invalid scatter recipes fail at load and publish nothing", "[world-demo][scatter]")
{
    const std::string_view source = BuiltinLevel("first-room");
    Level level = Read(source);
    const Level before = level;
    struct Rejection final
    {
        std::string_view From;
        std::string_view To;
        LevelError Error;
    };
    const Rejection rejections[] = {
        {
            .From = "\"layer\": 2,\n          \"exclusion_radius\"",
            .To = "\"layer\": 9,\n          \"exclusion_radius\"",
            .Error = LevelError::InvalidReference,
        },
        { .From = "\"cell_size\": 250", .To = "\"cell_size\": 0", .Error = LevelError::InvalidValue },
        { .From = "\"candidates\": 2", .To = "\"candidates\": 65", .Error = LevelError::InvalidValue },
        { .From = "\"exclusion_radius\": 60", .To = "\"exclusion_radius\": 400", .Error = LevelError::InvalidValue },
        { .From = "\"max_scale\": 1300", .To = "\"max_scale\": 600", .Error = LevelError::InvalidValue },
        { .From = "\"units_per_metre\": 100", .To = "\"units_per_metre\": 0", .Error = LevelError::InvalidValue },
        { .From = "\"min_x\": -11", .To = "\"min_x\": -13", .Error = LevelError::InvalidValue },
        { .From = "\"max_y\": 6", .To = "\"max_y\": -6", .Error = LevelError::InvalidValue },
        { .From = "\"keep_out_margin\": 0.5", .To = "\"keep_out_margin\": 11", .Error = LevelError::InvalidValue },
        { .From = "\"red\": 0.22", .To = "\"red\": 1.5", .Error = LevelError::InvalidValue },
        { .From = "\"half_x\": 0.35", .To = "\"half_x\": 0", .Error = LevelError::InvalidValue },
        {
            .From = "\"keep_out_margin\": 0.5",
            .To = "\"keep_out_margin\": 0.5, \"extra\": 1",
            .Error = LevelError::InvalidValue,
        },
        { .From = "\"id\": \"stone\"", .To = "\"id\": \"guard-a\"", .Error = LevelError::DuplicateField },
        // Sixty-four candidates in every one-metre cell: more than the room's candidate budget.
        {
            .From = "\"cell_size\": 250,\n          \"candidates\": 2",
            .To = "\"cell_size\": 100,\n          \"candidates\": 64",
            .Error = LevelError::Capacity,
        },
    };
    for (const Rejection& rejection : rejections)
    {
        const LevelResult result = ReadLevel(Edit(source, {rejection.From, rejection.To}), level);
        CHECK(result.Error == rejection.Error);
        CHECK(level.Scatter.LayerCount == before.Scatter.LayerCount);
        CHECK(level.Scatter.Layers[0].CellSize == before.Scatter.Layers[0].CellSize);
    }
    // Two layers with one stable ID, even when every variant still resolves.
    CHECK(ReadLevel(
              Edit(Edit(source, {"\"layer\": 2,\n          \"cell_size\"", "\"layer\": 1,\n          \"cell_size\""}),
                   {"\"layer\": 2,\n          \"exclusion_radius\"", "\"layer\": 1,\n          \"exclusion_radius\""}),
              level)
              .Error == LevelError::DuplicateField);
    // A layer whose variants all moved elsewhere has none; a second scatter entity repeats the room recipe.
    CHECK(ReadLevel(Edit(Edit(source,
                              {"\"layer\": 1,\n          \"exclusion_radius\": 60",
                               "\"layer\": 2,\n          \"exclusion_radius\": 60"}),
                         {"\"layer\": 1,\n          \"exclusion_radius\": 80",
                          "\"layer\": 2,\n          \"exclusion_radius\": 80"}),
                    level)
              .Error == LevelError::InvalidValue);
    const std::string_view anchor = "    {\n      \"id\": \"level-settings\",";
    CHECK(ReadLevel(
              Edit(source,
                   {anchor,
                    "    {\n      \"id\": \"scatter-two\",\n      \"name\": \"scatter-two\",\n      \"components\": {\n"
                    "        \"scatter\": {\n          \"units_per_metre\": 100,\n          \"min_x\": -11,\n"
                    "          \"min_y\": -6,\n          \"max_x\": 11,\n          \"max_y\": 6,\n"
                    "          \"keep_out_margin\": 0.5\n        }\n      }\n    },\n" +
                        std::string(anchor)}),
              level)
              .Error == LevelError::DuplicateField);
    // Layers without a scatter component are rejected by validation.
    Level orphan = before;
    orphan.HasScatter = false;
    CHECK(ValidateLevel(orphan) == LevelError::InvalidValue);
    // More accepted placements than the room keeps: rejected when prepared, nothing published.
    Session session;
    Load(session, source);
    const uint64 hash = session.World().GetReplayHash();
    const std::string dense = Edit(
        Edit(source,
             {"\"cell_size\": 250,\n          \"candidates\": 2", "\"cell_size\": 100,\n          \"candidates\": 4"}),
        {"\"exclusion_radius\": 60", "\"exclusion_radius\": 0"});
    REQUIRE(session.RequestLoad(dense).Error == LevelError::None);
    CHECK(session.PollLoad() == Status::CapacityExceeded);
    CHECK(session.GetLoadStage() == LoadStage::Failed);
    CHECK(session.World().GetReplayHash() == hash);
}
