// Counts ordinary, aligned and nothrow C++ allocations on production evaluation/view paths.
#include "internal/camera_view.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <ludus/foundation/base/types.h>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <new>
namespace
{
ludus::foundation::uint64 gNews = 0;
ludus::foundation::uint64 gDeletes = 0;
bool gTracking = false;

void* AlignedAlloc(ludus::foundation::usize n, ludus::foundation::usize al) noexcept
{
    if (al < __STDCPP_DEFAULT_NEW_ALIGNMENT__)
    {
        al = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    }
    ludus::foundation::usize rounded = (n + al - 1) & ~(al - 1);
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
        ++gNews;
    }
}
void CountDelete(void* p) noexcept
{
    if (p != nullptr && gTracking)
    {
        ++gDeletes;
    }
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(ludus::foundation::usize n)
{
    CountNew();
    void* p = AlignedAlloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(ludus::foundation::usize n, std::align_val_t al)
{
    CountNew();
    void* p = AlignedAlloc(n, static_cast<ludus::foundation::usize>(al));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(ludus::foundation::usize n, const std::nothrow_t&) noexcept
{
    CountNew();
    return AlignedAlloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void operator delete(void* p) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, ludus::foundation::usize) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, ludus::foundation::usize, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

TEST_CASE("Camera evaluation traces and renderer extraction allocate nothing", "[camera][allocation]")
{
    using namespace ludus::gameplay::camera;
    using namespace ludus::world_demo;
    const CameraRigDefinition fixed;
    const CameraRigDefinition follow{ .Recipe = CameraRecipe::OrthographicFollow };
    CameraTargetSample target;
    CameraSample sample;
    CameraTrace trace;
    CameraRenderView view;
    PlanarCameraView planar;
    bool passed = true;
    gNews = 0;
    gDeletes = 0;
    gTracking = true;
    for (ludus::foundation::uint64 frame = 0; frame < 10000; ++frame)
    {
        target.Position.X = static_cast<ludus::foundation::float64>(frame) / 100;
        passed &= TryEvaluateCamera(frame % 2 == 0 ? fixed : follow,
                                    { .PresentationSequence = frame },
                                    &target,
                                    sample,
                                    &trace) == CameraStatus::Success;
        passed &= TryBuildCameraView(sample, {0, 0, 800, 600}, {}, 1e6, view) == CameraStatus::Success;
        passed &= TryBuildPlanarCameraView(sample, {0, 0, 480, 740}, {}, 1e6, planar) == CameraStatus::Success;
        passed &= TryEvaluateCamera(follow, {}, nullptr, sample, &trace) == CameraStatus::MissingTarget;
    }
    gTracking = false;
    REQUIRE(passed);
    REQUIRE(gNews == 0);
    REQUIRE(gDeletes == 0);
}
