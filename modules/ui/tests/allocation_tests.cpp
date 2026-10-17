// Isolated allocation/failure injection test; sanitizer builds use the ordinary
// context suite because sanitizer runtimes replace global new/delete.

#include <ludus/ui/context.h>

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
long gFailAfter = -1;

void* AlignedAlloc(std::size_t n, std::size_t al) noexcept
{
    if (gFailAfter == 0)
    {
        return nullptr;
    }
    if (gFailAfter > 0)
    {
        --gFailAfter;
    }
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

using namespace ludus::ui;

TEST_CASE("UI resolution and event routing allocate nothing after initialization", "[ui][alloc]")
{
    Context ui;
    REQUIRE(ui.TryInitialize(64) == Status::Ok);
    Element elements[64];
    for (usize index = 0; index < 64; ++index)
    {
        elements[index].Id = index + 1;
        elements[index].Placement.Width = {100, Unit::Pixels};
        elements[index].Placement.Height = {40, Unit::Pixels};
        elements[index].SemanticRole = Role::Button;
    }
    gCounters = {};
    gTracking = true;
    bool success = true;
    for (usize iteration = 0; iteration < 1000; ++iteration)
    {
        success = success && ui.TrySetDocument(elements, {{0, 0, 320, 200}}) == Status::Ok;
        success = success && ui.Route({EventKind::PointerDown, 20, 20}).Consumed;
        success = success && ui.Route({EventKind::PointerUp, 20, 20}).Activated == 64;
        success = success && ui.Route({EventKind::FocusNext}).Focused != 0;
        (void)ui.GetPaintCommands();
    }
    gTracking = false;
    REQUIRE(success);
    CHECK(gCounters.news == 0);
    CHECK(gCounters.deletes == 0);
}

TEST_CASE("UI every initialization allocation failure permits retry", "[ui][alloc]")
{
    // State, four arrays per buffer and one hash array = ten allocations.
    for (long failedAllocation = 0; failedAllocation < 10; ++failedAllocation)
    {
        Context ui;
        gFailAfter = failedAllocation;
        const auto status = ui.TryInitialize(64);
        gFailAfter = -1;
        REQUIRE(status == Status::OutOfMemory);
        REQUIRE(ui.GetLayout().empty());
        REQUIRE(ui.TryInitialize(64) == Status::Ok);
    }
}
