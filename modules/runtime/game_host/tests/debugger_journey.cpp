// Same-session reload debugger journey harness (tasks.md L14, design 11).
//
// A single process that an attached debugger steps through the ACTUAL live
// reload lifecycle in ONE host session: create generation A, run frames (a
// breakpoint lands in A's module code), live-reload to generation B from a
// distinct immutable path, then run frames (a breakpoint lands in B's distinct
// module code), proving per-generation symbol replacement within one process.
//
// Named functions below give the debugger stable, source-mapped stop points:
//   JourneyBeforeReload / JourneyAfterReload — the host-side observation points
//   (the module's own FixtureUpdate is where in-module breakpoints land).
// The harness returns non-zero on any lifecycle failure so the acceptance
// script can enforce the process exit status (a mocked launch proves nothing).

#include "internal/host_session.h"

#include <ludus/runtime/game_host/host.h>

#include <cstdio>
#include <cstring>

using namespace ludus::runtime::game_host;

namespace
{
// Keep these noinline + externally visible-ish so the debugger has stable,
// source-mapped stop points across optimization levels used for the journey.
__attribute__((noinline)) int JourneyBeforeReload(HostSession& session) noexcept
{
    for (int i = 0; i < 30; ++i)
    {
        session.AdvanceOneFrame();
    }
    return static_cast<int>(session.ActiveGeneration());
}

__attribute__((noinline)) int JourneyAfterReload(HostSession& session) noexcept
{
    for (int i = 0; i < 30; ++i)
    {
        session.AdvanceOneFrame();
    }
    return static_cast<int>(session.ActiveGeneration());
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <fixtureA.so> <fixtureB.so>\n", argv[0]);
        return 2;
    }
    const char* fixtureA = argv[1];
    const char* fixtureB = argv[2];

    HostSession session(0x11, 0x22, -1);
    if (session.LoadInitial(fixtureA) == false)
    {
        std::fprintf(stderr, "LoadInitial(A) failed\n");
        return 3;
    }

    // The GDB attach case starts this process independently, observes A's image,
    // attaches, and releases this owned pipe only after setting source stops.
    if (argc == 4 && std::strcmp(argv[3], "--wait-attach") == 0 && std::getchar() != 'g')
    {
        return 7;
    }

    const int genBefore = JourneyBeforeReload(session);
    if (genBefore != 1)
    {
        std::fprintf(stderr, "unexpected generation before reload: %d\n", genBefore);
        return 4;
    }

    // Live reload A -> B in the SAME session (new generation symbols load here).
    if (session.ReloadTo(fixtureB, 2) != protocol::CommandStatus::Ok)
    {
        std::fprintf(stderr, "ReloadTo(B) failed\n");
        return 5;
    }

    const int genAfter = JourneyAfterReload(session);
    if (genAfter != 2)
    {
        std::fprintf(stderr, "unexpected generation after reload: %d\n", genAfter);
        return 6;
    }

    std::printf("journey ok: gen %d -> %d\n", genBefore, genAfter);
    return 0;
}
