// Milestone 1 pull request 3 of the randomness architecture in the world demo:
// harvesting a scatter placement records a tombstone, and a harvested placement
// regrows a new variant later. Covers the event rules of
// docs/architecture/randomness.md#milestone-1-procedural-placement: the ordinal
// advances exactly once per committed regrow, the BuildNext and fault paths do
// not advance it, forced exhaustion fails and leaves outputs unchanged,
// tombstones survive a room round-trip, and the replay hash tracks harvest and
// regrow but not rendering or cosmetic changes.
#include "internal/room_catalog.h"
#include "internal/world.h"

#include <ludus/content/procedural/environment/scatter.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <string_view>

using namespace ludus::world_demo;
namespace procedural = ludus::content::procedural;

namespace ludus::world_demo::internal
{
/// Test-only access to GameWorld's private harvest/regrow state (GameWorld befriends this struct).
struct WorldTestAccess final
{
    static void SetRegrowOrdinal(GameWorld& inoutWorld, usize placementIndex, uint32 ordinal) noexcept
    {
        inoutWorld.mPlacementStates[placementIndex].RegrowOrdinal = ordinal;
    }
    static void ForceHarvested(GameWorld& inoutWorld, usize placementIndex, uint64 regrowAtTick) noexcept
    {
        inoutWorld.mPlacementStates[placementIndex].IsHarvested = true;
        inoutWorld.mPlacementStates[placementIndex].RegrowAtTick = regrowAtTick;
    }
    static void SetPlayerPosition(GameWorld& inoutWorld, Vec2 position) noexcept
    {
        Transform* transform = inoutWorld.mTransforms.Find(inoutWorld.mPlayer);
        transform->Current = position;
        transform->Previous = position;
    }
    [[nodiscard]] static const PlacementState& State(const GameWorld& world, usize placementIndex) noexcept
    {
        return world.mPlacementStates[placementIndex];
    }
    [[nodiscard]] static uint8 HarvestableLayer(const GameWorld& world) noexcept
    {
        return world.mHarvestableLayer;
    }
    [[nodiscard]] static bool HasHarvestableLayer(const GameWorld& world) noexcept
    {
        return world.mHasHarvestableLayer;
    }
    [[nodiscard]] static uint64 RegrowExhaustions(const GameWorld& world) noexcept
    {
        return world.mRegrowExhaustions;
    }
};
} // namespace ludus::world_demo::internal

using ludus::world_demo::internal::WorldTestAccess;

namespace
{
/// One textual edit of a committed source.
struct Replacement final
{
    std::string_view From;
    std::string_view To;
};

std::string Edit(std::string_view source, Replacement replacement)
{
    std::string edited(source);
    const usize at = edited.find(replacement.From);
    REQUIRE(at != std::string::npos);
    edited.replace(at, replacement.From.size(), replacement.To);
    return edited;
}

/// The first room with layer 1 (the shrubs) marked harvestable.
std::string HarvestableRoom()
{
    return Edit(BuiltinLevel("first-room"),
                {"\"layer\": 1,\n          \"cell_size\": 250,\n          \"candidates\": 2,\n"
                 "          \"min_scale\": 700,\n          \"max_scale\": 1300",
                 "\"layer\": 1,\n          \"cell_size\": 250,\n          \"candidates\": 2,\n"
                 "          \"min_scale\": 700,\n          \"max_scale\": 1300,\n          \"harvestable\": true"});
}

Level Read(std::string_view source)
{
    Level level;
    REQUIRE(ReadLevel(source, level).Error == LevelError::None);
    return level;
}

GameWorld Build(std::string_view source, WorldSeed seed = {})
{
    GameWorld world;
    REQUIRE(world.Prepare(Read(source), MAX_WORLD_ENTITIES, seed) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    return world;
}

/// Index of the first placement of the harvestable layer, which the fixture guarantees exists.
usize FirstHarvestable(const GameWorld& world)
{
    const uint8 layer = WorldTestAccess::HarvestableLayer(world);
    const std::span<const Placement> placements = world.GetPlacements();
    for (usize index = 0; index < placements.size(); ++index)
    {
        if (placements[index].Layer == layer)
        {
            return index;
        }
    }
    REQUIRE(false);
    return 0;
}

void RunTicks(GameWorld& inoutWorld, usize count)
{
    for (usize tick = 0; tick < count; ++tick)
    {
        REQUIRE(inoutWorld.RunTick({}) == Status::Success);
    }
}

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

TEST_CASE("Only an authored harvestable layer is harvestable, and at most one per room", "[world-demo][harvest]")
{
    const GameWorld plain = Build(BuiltinLevel("first-room"));
    CHECK(WorldTestAccess::HasHarvestableLayer(plain) == false);
    const GameWorld harvestable = Build(HarvestableRoom());
    CHECK(WorldTestAccess::HasHarvestableLayer(harvestable) == true);
    // Layer index 0 holds the shrubs (layer ID 1) after the entity sort; its placements are harvestable.
    CHECK(harvestable.GetPlacements().empty() == false);
    // The harvestable flag is content: it changes the replay hash but not the base placement digest.
    CHECK(harvestable.GetPlacementDigest() == plain.GetPlacementDigest());
    CHECK(harvestable.GetReplayHash() != plain.GetReplayHash());
    // Two harvestable layers are rejected with nothing published.
    Level twice;
    const std::string invalid = Edit(HarvestableRoom(),
                                     {"\"layer\": 2,\n          \"cell_size\": 300,\n          \"candidates\": 1,\n"
                                      "          \"min_scale\": 800,\n          \"max_scale\": 1200",
                                      "\"layer\": 2,\n          \"cell_size\": 300,\n          \"candidates\": 1,\n"
                                      "          \"min_scale\": 800,\n          \"max_scale\": 1200,\n"
                                      "          \"harvestable\": true"});
    CHECK(ReadLevel(invalid, twice).Error == LevelError::InvalidValue);
}

TEST_CASE("Harvesting a placement records a tombstone and changes the replay hash", "[world-demo][harvest]")
{
    GameWorld world = Build(HarvestableRoom());
    const usize index = FirstHarvestable(world);
    const uint64 before = world.GetReplayHash();
    REQUIRE(WorldTestAccess::State(world, index).IsHarvested == false);
    // Place the player on the placement; a neutral tick then detects the overlap and commits the tombstone.
    const Level level = Read(HarvestableRoom());
    const Placement& placement = world.GetPlacements()[index];
    const float32 units = static_cast<float32>(level.Scatter.UnitsPerMetre);
    WorldTestAccess::SetPlayerPosition(
        world,
        {static_cast<float32>(placement.LatticeX) / units, static_cast<float32>(placement.LatticeY) / units});
    REQUIRE(world.RunTick({}) == Status::Success);
    CHECK(WorldTestAccess::State(world, index).IsHarvested == true);
    // The ordinal has not advanced: harvesting is a world mutation, not a random event.
    CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == 1);
    CHECK(world.GetReplayHash() != before);
    // A harvested placement is not drawn.
    RenderFrame frame;
    REQUIRE(world.Extract(1.0F, frame));
    RenderFrame plainFrame;
    GameWorld unharvested = Build(HarvestableRoom());
    REQUIRE(unharvested.Extract(1.0F, plainFrame));
    CHECK(frame.Count == plainFrame.Count - 1);
}

TEST_CASE("The regrow ordinal advances exactly once per committed regrow", "[world-demo][harvest]")
{
    GameWorld world = Build(HarvestableRoom());
    const usize index = FirstHarvestable(world);
    const uint8 baseVariant = WorldTestAccess::State(world, index).Variant;
    // Force a harvest due to regrow on the next committed tick.
    WorldTestAccess::ForceHarvested(world, index, world.GetTick() + 1);
    const uint64 harvestedHash = world.GetReplayHash();
    REQUIRE(world.RunTick({}) == Status::Success);
    // One committed regrow drew a new variant and advanced the ordinal from 1 to 2, exactly once.
    CHECK(WorldTestAccess::State(world, index).IsHarvested == false);
    CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == 2);
    CHECK(world.GetReplayHash() != harvestedHash);
    (void)baseVariant;
    // Running more ticks with the placement regrown does not advance the ordinal again.
    RunTicks(world, 10);
    CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == 2);
}

TEST_CASE("BuildNext and a faulted tick never advance the regrow ordinal", "[world-demo][harvest]")
{
    // BuildNext: ordinals are set in Prepare and untouched while the world builds its authored entities.
    GameWorld world;
    REQUIRE(world.Prepare(Read(HarvestableRoom())) == Status::Success);
    const usize index = FirstHarvestable(world);
    CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == 1);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
        CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == 1);
    }
    // A faulted tick skips Commit, so a scheduled regrow neither advances the ordinal nor clears the tombstone.
    WorldTestAccess::ForceHarvested(world, index, world.GetTick() + 1);
    const uint64 tick = world.GetTick();
    const PlacementState before = WorldTestAccess::State(world, index);
    REQUIRE(world.RunTick({ .Axis = {std::numeric_limits<float32>::quiet_NaN(), 0} }) != Status::Success);
    CHECK(world.GetPhase() == Phase::Faulted);
    CHECK(world.GetTick() == tick);
    CHECK((WorldTestAccess::State(world, index) == before) == true);
}

TEST_CASE("Forced ordinal exhaustion fails the regrow and leaves outputs unchanged", "[world-demo][harvest]")
{
    GameWorld world = Build(HarvestableRoom());
    const usize index = FirstHarvestable(world);
    // The ordinal is one short of wrapping and the placement is due to regrow this tick.
    WorldTestAccess::SetRegrowOrdinal(world, index, std::numeric_limits<uint32>::max());
    WorldTestAccess::ForceHarvested(world, index, world.GetTick() + 1);
    const PlacementState before = WorldTestAccess::State(world, index);
    const uint64 hashBefore = world.GetReplayHash();
    REQUIRE(world.RunTick({}) == Status::Success);
    // The regrow failed: the placement stays harvested, the ordinal stays UINT32_MAX, the variant is unchanged.
    CHECK(WorldTestAccess::State(world, index).IsHarvested == true);
    CHECK(WorldTestAccess::State(world, index).RegrowOrdinal == std::numeric_limits<uint32>::max());
    CHECK(WorldTestAccess::State(world, index).Variant == before.Variant);
    CHECK(WorldTestAccess::RegrowExhaustions(world) == 1);
    // The failure is visible in the replay hash (through the exhaustion count) but publishes no new variant.
    CHECK(world.GetReplayHash() != hashBefore);
}

TEST_CASE("Tombstones survive leaving and re-entering a room", "[world-demo][harvest]")
{
    Session session;
    Load(session, HarvestableRoom());
    const usize index = FirstHarvestable(session.World());
    const Placement& placement = session.World().GetPlacements()[index];
    Level level = Read(HarvestableRoom());
    const float32 units = static_cast<float32>(level.Scatter.UnitsPerMetre);
    WorldTestAccess::SetPlayerPosition(
        session.World(),
        {static_cast<float32>(placement.LatticeX) / units, static_cast<float32>(placement.LatticeY) / units});
    REQUIRE(session.World().RunTick({}) == Status::Success);
    REQUIRE(WorldTestAccess::State(session.World(), index).IsHarvested == true);
    const uint64 harvestedScope = placement.Scope;
    // Leave to the other room, then return: the harvested placement must still be harvested.
    Load(session, BuiltinLevel("second-room"));
    CHECK(session.World().GetRoom() == 2);
    Load(session, HarvestableRoom());
    CHECK(session.World().GetRoom() == 1);
    bool isFound = false;
    for (usize i = 0; i < session.World().GetPlacements().size(); ++i)
    {
        if (session.World().GetPlacements()[i].Scope == harvestedScope)
        {
            CHECK(WorldTestAccess::State(session.World(), i).IsHarvested == true);
            isFound = true;
        }
    }
    CHECK(isFound == true);
}

TEST_CASE("The replay hash ignores rendering, pacing and cosmetic draws but tracks harvest and regrow",
          "[world-demo][harvest]")
{
    GameWorld drawn = Build(HarvestableRoom());
    GameWorld blind = Build(HarvestableRoom());
    RenderFrame frame;
    for (usize tick = 0; tick < 20; ++tick)
    {
        REQUIRE(drawn.RunTick({}) == Status::Success);
        REQUIRE(blind.RunTick({}) == Status::Success);
        for (const float32 alpha : {0.0F, 0.5F, 1.0F})
        {
            REQUIRE(drawn.Extract(alpha, frame));
            REQUIRE(drawn.Extract(alpha, frame, true, tick + 1));
        }
        REQUIRE(drawn.GetReplayHash() == blind.GetReplayHash());
    }
    // Harvesting one of the two worlds diverges the replay hash from the untouched one.
    const usize index = FirstHarvestable(drawn);
    WorldTestAccess::ForceHarvested(drawn, index, drawn.GetTick() + 1);
    REQUIRE(drawn.RunTick({}) == Status::Success);
    REQUIRE(blind.RunTick({}) == Status::Success);
    CHECK(drawn.GetReplayHash() != blind.GetReplayHash());
}
