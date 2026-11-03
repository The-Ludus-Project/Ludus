// Allocation probe for Ludus::GeometryContent. Global operator new/delete are
// replaced with counting versions that can fail the N-th nothrow allocation.
// Every allocation of a successful cook and load is failed in turn: the call
// must return a transport failure (OutOfMemory for a fallible allocation) and
// leave the active owner unchanged. Evaluation after a successful load must not
// allocate. Built without sanitizers, whose runtimes provide their own
// operator new/delete. Mirrors modules/foundation/geometry/tests/allocation_tests.cpp.

#include <ludus/geometry/content/geometry_content.h>

#include <ludus/content/geometry_format/geometry_format.h>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <new>

namespace gc = ludus::geometry::content;
namespace gf = ludus::content::geometry_format;
namespace geom = ludus::foundation::geometry;
using ludus::content::Status;
using ludus::foundation::float64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

namespace
{
bool isCountingAllocations = false;
usize gAllocationCount = 0;
usize gFailAt = 0;

void* AllocateAligned(usize bytes, usize alignment) noexcept
{
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    usize rounded = (bytes + alignment - 1) & ~(alignment - 1);
    if (rounded == 0)
    {
        rounded = alignment;
    }
    return std::aligned_alloc(alignment, rounded);
}

[[nodiscard]] bool ShouldFail() noexcept
{
    if (isCountingAllocations == false)
    {
        return false;
    }
    ++gAllocationCount;
    return gFailAt != 0 && gAllocationCount == gFailAt;
}

struct CountingScope final
{
    explicit CountingScope(usize failAt) noexcept
    {
        gAllocationCount = 0;
        gFailAt = failAt;
        isCountingAllocations = true;
    }
    CountingScope(const CountingScope&) = delete;
    CountingScope& operator=(const CountingScope&) = delete;
    ~CountingScope()
    {
        isCountingAllocations = false;
        gFailAt = 0;
    }
};

void MakeSource(gc::GeometrySource& outSource)
{
    outSource.Mode = gc::GeometrySourceMode::CubicPath;
    outSource.Revision = 1;
    outSource.AssetScale = 1.0;
    outSource.Tolerance = geom::ToleranceProfile
    {
        .PositionError = 1e-3,
        .DistanceError = 1e-3,
        .TangentAngleError = 1e-2,
        .ProjectionGap = 1e-3,
        .ContinuityPosition = 1e-3,
        .ContinuityDerivative = 1e-2,
        .ContinuityAngle = 1e-2,
        .NormalMinDerivativeLength = 1e-6,
        .NormalMinSinAngle = 1e-3,
        .NormalAngleError = 1e-2,
        .CoincidenceDistance = 1e-4,
    };
    outSource.Limits = geom::WorkLimits
    {
        .MaxSpans = 1024,
        .MaxLeaves = 1u << 16,
        .MaxDepth = 16,
        .MaxSubdivisions = 1u << 16,
        .MaxIterations = 256,
        .MaxPreparedBytes = 1u << 20,
        .MaxPatches = 16,
    };
    gc::AuthoredSpan span{};
    span.AuthorId = 1;
    span.Kind = gf::SpanKindTag::Cubic;
    span.DomainWidth = 1.0;
    span.Control[0] = ludus::foundation::math::Vector3d{0.0, 0.0, 0.0};
    span.Control[1] = ludus::foundation::math::Vector3d{1.0, 0.0, 0.0};
    span.Control[2] = ludus::foundation::math::Vector3d{1.0, 1.0, 0.0};
    span.Control[3] = ludus::foundation::math::Vector3d{1.0, 1.0, 1.0};
    REQUIRE(outSource.Spans.TryAdd(span) == true);
}
} // namespace

TEST_CASE("Every cook allocation failure returns a failure and preserves no partial blob", "[geometry_content][alloc]")
{
    gc::GeometrySource source;
    MakeSource(source);

    for (usize failAt = 1; failAt <= 64; ++failAt)
    {
        ludus::foundation::core::Array<uint8> blob;
        gc::GeometryDiagnosticReport diag{};
        Status status = Status::Ok;
        {
            CountingScope scope(failAt);
            status = gc::CookGeometry(source, blob, diag);
        }
        if (status == Status::Ok)
        {
            // Past the last allocation; nothing more to probe.
            break;
        }
        CHECK(diag.Transport != Status::Ok);
        CHECK(blob.IsEmpty() == true);
    }
}

TEST_CASE("Every load allocation failure returns a failure and leaves outLoad empty", "[geometry_content][alloc]")
{
    gc::GeometrySource source;
    MakeSource(source);
    ludus::foundation::core::Array<uint8> blob;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::CookGeometry(source, blob, diag) == Status::Ok);

    for (usize failAt = 1; failAt <= 64; ++failAt)
    {
        gc::GeometryLoad load;
        gc::GeometryDiagnosticReport loadDiag{};
        Status status = Status::Ok;
        {
            CountingScope scope(failAt);
            status = gc::LoadGeometry({blob.GetData(), blob.GetSize()}, source.Limits, load, loadDiag);
        }
        if (status == Status::Ok)
        {
            break;
        }
        CHECK(loadDiag.Transport != Status::Ok);
        CHECK(load.Path.IsEmpty() == true);
    }
}

TEST_CASE("Evaluation after load does not allocate", "[geometry_content][alloc]")
{
    gc::GeometrySource source;
    MakeSource(source);
    ludus::foundation::core::Array<uint8> blob;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::CookGeometry(source, blob, diag) == Status::Ok);
    gc::GeometryLoad load;
    gc::GeometryDiagnosticReport loadDiag{};
    REQUIRE(gc::LoadGeometry({blob.GetData(), blob.GetSize()}, source.Limits, load, loadDiag) == Status::Ok);

    const geom::PathView view = load.Path.GetView();
    ludus::foundation::math::Vector3 position{};
    {
        CountingScope scope(0);
        const usize before = gAllocationCount;
        const geom::GeometryStatus status = geom::TryEvaluatePath(view, geom::SegmentParameter{0, 0.5}, position);
        CHECK(status == geom::GeometryStatus::Success);
        CHECK(gAllocationCount == before);
    }
}

void* operator new(usize bytes)
{
    (void)ShouldFail();
    void* allocation = AllocateAligned(bytes, alignof(std::max_align_t));
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}
void* operator new(usize bytes, std::align_val_t alignment)
{
    (void)ShouldFail();
    void* allocation = AllocateAligned(bytes, static_cast<usize>(alignment));
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}
void* operator new(usize bytes, const std::nothrow_t&) noexcept
{
    return ShouldFail() == true ? nullptr : AllocateAligned(bytes, alignof(std::max_align_t));
}
void* operator new(usize bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return ShouldFail() == true ? nullptr : AllocateAligned(bytes, static_cast<usize>(alignment));
}
void operator delete(void* allocationOrNull) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, usize) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, std::align_val_t) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, usize, std::align_val_t) noexcept
{
    std::free(allocationOrNull);
}
