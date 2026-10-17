// Zero-allocation assertion for the hot reducer paths (K12). We replace global
// operator new/delete with counting versions and verify that after construction
// (the single allowed allocation), repeated Ingest/ConsumeStep/GetAction/
// ReplaceBindings do not allocate. Built into its own executable and only when
// sanitizers are OFF (the sanitizer runtimes provide their own new/delete).

#include <ludus/input/keyboard.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
struct AllocCounters
{
    long news = 0;
    long deletes = 0;
};
AllocCounters gCounters;
bool gTracking = false;

void* AlignedAlloc(std::size_t n, std::size_t al) noexcept
{
    if (al < alignof(std::max_align_t))
    {
        al = alignof(std::max_align_t);
    }
    std::size_t rounded = (n + al - 1) & ~(al - 1);
    if (rounded == 0)
    {
        rounded = al;
    }
    return std::aligned_alloc(al, rounded);
}
void CountNew() noexcept
{
    if (gTracking)
    {
        ++gCounters.news;
    }
}
void CountDelete(void* p) noexcept
{
    if (p != nullptr && gTracking)
    {
        ++gCounters.deletes;
    }
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t n)
{
    CountNew();
    void* p = AlignedAlloc(n, alignof(std::max_align_t));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, std::align_val_t al)
{
    CountNew();
    void* p = AlignedAlloc(n, static_cast<std::size_t>(al));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    CountNew();
    return AlignedAlloc(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    CountNew();
    return AlignedAlloc(n, static_cast<std::size_t>(al));
}
void operator delete(void* p) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::input;

TEST_CASE("Construction allocates once; hot paths allocate nothing", "[input][alloc]")
{
    gCounters = AllocCounters{};
    gTracking = true;
    {
        InputSystem system;
        REQUIRE(system.IsValid());
        // Exactly one allocation so far (the reducer storage).
        REQUIRE(gCounters.news == 1);

        FocusBaseline baseline;
        baseline.Focused = true;
        system.RequestReset(ResetReason::FocusEntered, baseline);

        Binding bindings[] = {
            Binding{ .Action = 0, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
            Binding
            {
                .Action = 1,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Positive,
                .PhysicalKey = Key::KeyD,
            },
            Binding
            {
                .Action = 1,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Negative,
                .PhysicalKey = Key::KeyA,
            },
        };
        REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);

        const long afterSetup = gCounters.news;

        // Hot loop: ingest and consume many times; query actions and snapshots.
        for (ludus::foundation::uint64 step = 1; step <= 2000; ++step)
        {
            (void)system.Ingest(KeyboardRecord{ .Transition = KeyTransition::Down, .PhysicalKey = Key::Space });
            (void)system.Ingest(KeyboardRecord{ .Transition = KeyTransition::Up, .PhysicalKey = Key::Space });
            (void)system.Ingest(KeyboardRecord{ .Transition = KeyTransition::Down, .PhysicalKey = Key::KeyD });
            (void)system.Ingest(KeyboardRecord{ .Transition = KeyTransition::Up, .PhysicalKey = Key::KeyD });
            (void)system.ConsumeStep(step);
            ActionState state;
            (void)system.GetAction(0, state);
            (void)system.GetAction(1, state);
            (void)system.GetKeyboardSnapshot();
            (void)system.GetStepEvents();
        }
        // No allocation occurred across the hot loop.
        REQUIRE(gCounters.news == afterSetup);
    }
    gTracking = false;
}
