#pragma once

// Shared test profiles and fixture adapters for FoundationGeometry tests.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/geometry/tessellation.hpp>

#include <curves_surfaces/reference_fixtures.hpp>

#include <cstring>

namespace ludus::foundation::geometry::test_support
{
namespace fixtures = ludus::geometry_fixtures;

/// Distance gap used by the corpus tests, asset units.
inline constexpr float64 CORPUS_DISTANCE_ERROR = 1e-7;
/// Work limit large enough for every corpus fixture at CORPUS_DISTANCE_ERROR.
inline constexpr usize CORPUS_WORK_LIMIT = 1U << 20U;
/// Inversion work limit: enough bisection steps to reach float64 resolution.
inline constexpr usize CORPUS_QUERY_ITERATIONS = 256;

/// A valid profile with every field inside its ADR 0024 range.
[[nodiscard]] inline ToleranceProfile MakeProfile() noexcept
{
    return ToleranceProfile
    {
        .PositionError = 1e-5,
        .RelativePositionError = 0.0,
        .DistanceError = CORPUS_DISTANCE_ERROR,
        .TangentAngleError = 1e-3,
        .ProjectionGap = 1e-5,
        .ContinuityPosition = 1e-6,
        .ContinuityDerivative = 1e-9,
        .ContinuityAngle = 1e-6,
        .NormalMinDerivativeLength = 1e-6,
        .NormalMinSinAngle = 1e-3,
        .NormalAngleError = 1e-2,
        .CoincidenceDistance = 1e-6,
    };
}

/// Generous limits for corpus tests.
[[nodiscard]] inline WorkLimits MakeLimits() noexcept
{
    return WorkLimits
    {
        .MaxSpans = CORPUS_WORK_LIMIT,
        .MaxLeaves = CORPUS_WORK_LIMIT,
        .MaxDepth = 64,
        .MaxSubdivisions = CORPUS_WORK_LIMIT,
        .MaxIterations = CORPUS_QUERY_ITERATIONS,
        .MaxPreparedBytes = CORPUS_WORK_LIMIT * 256U,
        .MaxPatches = CORPUS_WORK_LIMIT,
    };
}

[[nodiscard]] inline PathPreparation MakePreparation() noexcept
{
    return PathPreparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() };
}

[[nodiscard]] inline math::Vector3d ToVector(const fixtures::ControlPoint& point) noexcept
{
    return math::Vector3d{static_cast<float64>(point.X), static_cast<float64>(point.Y), static_cast<float64>(point.Z)};
}

/// Cubic corpus fixture by name; the first fixture when the name is unknown.
[[nodiscard]] inline const fixtures::CubicFixture& FindCubic(const char* name) noexcept
{
    for (const fixtures::CubicFixture& fixture : fixtures::CUBIC_FIXTURES)
    {
        if (std::strcmp(fixture.Name, name) == 0)
        {
            return fixture;
        }
    }
    return fixtures::CUBIC_FIXTURES[0];
}

/// One cubic source span from a corpus fixture's exact float32 controls.
[[nodiscard]] inline PathSpanSource MakeSpan(const fixtures::CubicFixture& fixture) noexcept
{
    PathSpanSource span;
    span.Kind = SpanKind::Cubic;
    for (usize i = 0; i < SPAN_CONTROL_COUNT; ++i)
    {
        span.Control[i] = ToVector(fixture.Control[i]);
    }
    return span;
}

/// Prepare a single-span path from a corpus fixture, at the origin.
[[nodiscard]] inline GeometryStatus
PrepareFixture(const fixtures::CubicFixture& fixture, PathSpanSource& outSpan, PreparedPath& outPath) noexcept
{
    outSpan = MakeSpan(fixture);
    const PathSource source{ .Spans = BorrowedView<const PathSpanSource>(&outSpan, 1), .Revision = 1 };
    return TryPreparePath(source, MakePreparation(), outPath, nullptr);
}

[[nodiscard]] inline DistancePreparation MakeDistancePreparation() noexcept
{
    return DistancePreparation{ .Precision = PrecisionProfile::Strict, .Limits = MakeLimits() };
}

/// Largest span count of the multi-span G3 test paths.
inline constexpr usize MAX_TEST_SPANS = 8;
/// Dyadic depth used by frame and sweep tests: singular ranges are 2^-30 wide.
inline constexpr usize FRAME_TEST_DEPTH = 30;

/// A prepared path and its distance map built from caller-listed source spans.
struct PreparedFixture final
{
    core::StaticArray<PathSpanSource, MAX_TEST_SPANS> Spans{};
    usize SpanCount = 0;
    PreparedPath Path;
    PreparedDistanceMap Map;
};

/// Prepare spanCount spans already stored in inoutFixture.Spans, then build the distance map.
[[nodiscard]] inline GeometryStatus
PrepareStoredSpans(usize spanCount, bool isClosed, PreparedFixture& inoutFixture) noexcept
{
    inoutFixture.SpanCount = spanCount;
    const PathSource source
    {
        .Spans = BorrowedView<const PathSpanSource>(inoutFixture.Spans.GetData(), spanCount),
        .IsClosed = isClosed,
        .Revision = 1,
    };
    GeometryStatus status = TryPreparePath(source, MakePreparation(), inoutFixture.Path, nullptr);
    if (status == GeometryStatus::Success)
    {
        status = TryBuildDistanceMap(inoutFixture.Path.GetView(), MakeDistancePreparation(), inoutFixture.Map, nullptr);
    }
    return status;
}

/// Prepare a single cubic corpus fixture with its distance map.
[[nodiscard]] inline GeometryStatus PrepareCubic(const fixtures::CubicFixture& fixture,
                                                 PreparedFixture& outFixture) noexcept
{
    outFixture.Spans[0] = MakeSpan(fixture);
    return PrepareStoredSpans(1, false, outFixture);
}

/// Prepare a G3 frame corpus fixture (unit-width cubic spans) with its distance map.
[[nodiscard]] inline GeometryStatus PrepareFrameFixture(const fixtures::FrameFixture& fixture,
                                                        PreparedFixture& outFixture) noexcept
{
    for (usize spanIndex = 0; spanIndex < fixture.Spans.GetSize(); ++spanIndex)
    {
        PathSpanSource& span = outFixture.Spans[spanIndex];
        span.Kind = SpanKind::Cubic;
        span.SourceElement = spanIndex;
        for (usize i = 0; i < SPAN_CONTROL_COUNT; ++i)
        {
            span.Control[i] = ToVector(fixture.Spans[spanIndex][i]);
        }
    }
    return PrepareStoredSpans(fixture.Spans.GetSize(), fixture.IsClosed, outFixture);
}

/// Frame fixture by name; the first fixture when the name is unknown.
[[nodiscard]] inline const fixtures::FrameFixture& FindFrameFixture(const char* name) noexcept
{
    for (const fixtures::FrameFixture& fixture : fixtures::FRAME_FIXTURES)
    {
        if (std::strcmp(fixture.Name, name) == 0)
        {
            return fixture;
        }
    }
    return fixtures::FRAME_FIXTURES[0];
}

/// Visual frame preparation with the corpus limits and a caller seed.
[[nodiscard]] inline FramePreparation MakeFramePreparation(const math::Vector3d& seedUp) noexcept
{
    WorkLimits limits = MakeLimits();
    limits.MaxDepth = FRAME_TEST_DEPTH;
    return FramePreparation{ .SeedUp = seedUp, .Limits = limits };
}

/// Patch corpus fixture by name; the first fixture when the name is unknown.
[[nodiscard]] inline const fixtures::PatchFixture& FindPatch(const char* name) noexcept
{
    for (const fixtures::PatchFixture& fixture : fixtures::PATCH_FIXTURES)
    {
        if (std::strcmp(fixture.Name, name) == 0)
        {
            return fixture;
        }
    }
    return fixtures::PATCH_FIXTURES[0];
}

/// One source patch from a corpus fixture's exact float32 controls.
[[nodiscard]] inline SurfacePatchSource MakePatchSource(const fixtures::PatchFixture& fixture, uint64 id) noexcept
{
    SurfacePatchSource patch;
    patch.Id = id;
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            patch.Control[i][j] = ToVector(fixture.Control[i][j]);
        }
    }
    return patch;
}

[[nodiscard]] inline SurfacePreparation MakeSurfacePreparation() noexcept
{
    return SurfacePreparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() };
}

/// Prepare a surface from borrowed patches and adjacency records at the origin.
[[nodiscard]] inline GeometryStatus PrepareSurface(BorrowedView<const SurfacePatchSource> patches,
                                                   BorrowedView<const SurfaceAdjacencySource> adjacency,
                                                   PreparedSurface& outSurface,
                                                   GeometryDiagnostic* outDiagnosticOrNull = nullptr) noexcept
{
    const SurfaceSource source{ .Patches = patches, .Adjacency = adjacency, .Revision = 7 };
    return TryPrepareSurface(source, MakeSurfacePreparation(), outSurface, outDiagnosticOrNull);
}

/// Generous mesh budgets for corpus tests.
[[nodiscard]] inline SurfaceMeshLimits MakeMeshLimits(usize maxLevel) noexcept
{
    return SurfaceMeshLimits
    {
        .MaxLevel = maxLevel,
        .MaxCellTests = CORPUS_WORK_LIMIT * 16U,
        .MaxVertices = CORPUS_WORK_LIMIT,
        .MaxTriangles = CORPUS_WORK_LIMIT,
        .MaxOutputBytes = CORPUS_WORK_LIMIT * 256U,
    };
}

/// Strict mesh profile with the given position error and level limit.
[[nodiscard]] inline SurfaceMeshProfile MakeMeshProfile(float64 positionError, usize maxLevel) noexcept
{
    return SurfaceMeshProfile
    {
        .Precision = PrecisionProfile::Strict,
        .PositionError = positionError,
        .Degeneracy = SurfaceDegeneracyPolicy::Report,
        .Limits = MakeMeshLimits(maxLevel),
    };
}
} // namespace ludus::foundation::geometry::test_support
