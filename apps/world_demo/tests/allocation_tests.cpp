#include "internal/world.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <new>
using namespace ludus::world_demo;
namespace
{
isize gFailAt = -1;
usize gCalls = 0;
} // namespace
// Containers allocate through this single aligned nothrow seam. Catch2's
// ordinary allocations remain untouched, and no assertion executes while a
// failure is armed. Kept in a separate non-sanitized executable.
// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(usize bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    const auto call = gCalls++;
    if (gFailAt >= 0 && call == static_cast<usize>(gFailAt))
    {
        return nullptr;
    }
    void* memory = nullptr;
    const auto align = static_cast<usize>(alignment);
    return posix_memalign(&memory, align < sizeof(void*) ? sizeof(void*) : align, bytes) == 0 ? memory : nullptr;
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)
TEST_CASE("Every candidate allocation failure preserves the active world", "[world-demo][allocation]")
{
    Session session;
    REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
    while (session.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(session.PollLoad() == Status::Success);
    }
    REQUIRE(session.Activate());
    const auto old = session.World().FindAuthored("player-start");
    const auto hash = session.World().GetReplayHash();
    // Two registry arrays, three arrays for each of eight component pools, then the scatter kernel's
    // candidate scratch and the room's placement storage.
    constexpr isize CANDIDATE_ALLOCATIONS = 28;
    REQUIRE(session.World().GetPlacements().empty() == false);
    for (isize allocation = 0; allocation < CANDIDATE_ALLOCATIONS; ++allocation)
    {
        REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
        gCalls = 0;
        gFailAt = allocation;
        const auto status = session.PollLoad();
        gFailAt = -1;
        REQUIRE(status == Status::AllocationFailure);
        REQUIRE(session.GetLoadStage() == LoadStage::Failed);
        REQUIRE(session.World().Registry().IsAlive(old));
        REQUIRE(session.World().GetReplayHash() == hash);
    }
}
TEST_CASE("Prepared ticks, commands, completions and render extraction never allocate", "[world-demo][allocation]")
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    GameWorld world;
    REQUIRE(world.Prepare(level) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    Recipe extra = level.Entities[1];
    extra.Id = { .Text = "extra-guard" };
    uint64 request = 0;
    RenderFrame frame;
    const auto before = gCalls;
    const auto spawn = world.QueueSpawn(extra, request);
    Status tick = Status::Success;
    bool rendered = true;
    for (usize index = 0; index < 120; ++index)
    {
        tick = world.RunTick({ .Pressed = index % 30 == 0 });
        rendered &= world.Extract(0.5F, frame);
        rendered &= world.Extract(0.5F, frame, true, index);
        world.ConsumeOutbox();
        if (tick != Status::Success)
        {
            break;
        }
    }
    const auto after = gCalls;
    REQUIRE(spawn == Status::Success);
    REQUIRE(tick == Status::Success);
    REQUIRE(rendered);
    REQUIRE(after == before);
    // Extraction drew the room's placements (with their cosmetic tint) without allocating.
    REQUIRE(frame.Count > level.CollisionCount + level.EntityCount);
}
