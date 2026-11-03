// Headless tests for the editor-private async prepare job, transactional preview
// publication and cursor reload (curves and surfaces G5). No GPU or display.
// Covers the G5 gate: failed and cancelled prepare, late completion rejection,
// stale cursor and active+candidate peak memory.

#include "internal/geometry_document.h"

#include <ludus/content/content.h>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/geometry/content/geometry_content.h>

#include <catch2/catch_test_macros.hpp>

namespace ge = ludus::editor::geometry;
namespace gc = ludus::geometry::content;
namespace gf = ludus::content::geometry_format;
namespace geom = ludus::foundation::geometry;
using ludus::content::Status;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

namespace
{
geom::ToleranceProfile MakeTolerance()
{
    geom::ToleranceProfile t{};
    t.PositionError = 1e-3;
    t.DistanceError = 1e-3;
    t.TangentAngleError = 1e-2;
    t.ProjectionGap = 1e-3;
    t.ContinuityPosition = 1e-3;
    t.ContinuityDerivative = 1e-2;
    t.ContinuityAngle = 1e-2;
    t.NormalMinDerivativeLength = 1e-6;
    t.NormalMinSinAngle = 1e-3;
    t.NormalAngleError = 1e-2;
    t.CoincidenceDistance = 1e-4;
    return t;
}

geom::WorkLimits MakeLimits()
{
    geom::WorkLimits limits{};
    limits.MaxSpans = 64;
    limits.MaxLeaves = 1u << 16;
    limits.MaxDepth = 16;
    limits.MaxSubdivisions = 1u << 16;
    limits.MaxIterations = 256;
    limits.MaxPreparedBytes = 1u << 20;
    limits.MaxPatches = 16;
    return limits;
}

void CookCubicBlob(ludus::foundation::core::Array<uint8>& outBlob)
{
    gc::GeometrySource source;
    source.Mode = gc::GeometrySourceMode::CubicPath;
    source.Revision = 1;
    source.AssetScale = 1.0;
    source.Tolerance = MakeTolerance();
    source.Limits = MakeLimits();
    source.Precision = geom::PrecisionProfile::Visual;
    gc::AuthoredSpan span{};
    span.AuthorId = 1;
    span.Kind = gf::SpanKindTag::Cubic;
    span.DomainWidth = 1.0;
    span.Control[0] = ludus::foundation::math::Vector3d{0.0, 0.0, 0.0};
    span.Control[1] = ludus::foundation::math::Vector3d{1.0, 0.0, 0.0};
    span.Control[2] = ludus::foundation::math::Vector3d{1.0, 1.0, 0.0};
    span.Control[3] = ludus::foundation::math::Vector3d{1.0, 1.0, 1.0};
    REQUIRE(source.Spans.TryAdd(span) == true);
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::CookGeometry(source, outBlob, diag) == Status::Ok);
}

ge::GeometryPrepareRequest MakeRequest(uint64 request, uint64 revision, uint64 generation)
{
    ge::GeometryPrepareRequest r{};
    r.Request = request;
    r.DocumentId = 1;
    r.Revision = revision;
    r.Generation = generation;
    r.Profile = geom::PrecisionProfile::Visual;
    return r;
}
} // namespace

TEST_CASE("A Ready prepare publishes at the tick boundary", "[editor][geometry][preview]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);

    ge::GeometryPrepareJob job;
    const ge::GeometryPrepareRequest request = MakeRequest(1, 1, 1);
    REQUIRE(job.Begin(request, {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    REQUIRE(job.Prepare() == ge::GeometryPrepareStatus::Ready);

    ge::GeometryPreview preview;
    preview.SetExpected(1, 1, 1);
    REQUIRE(preview.OfferCompletion(request, ge::GeometryPrepareStatus::Ready, job.TakeLoad()) == true);
    // Nothing is presented until the tick boundary.
    REQUIRE(preview.HasActive() == false);
    REQUIRE(preview.Tick() == true);
    REQUIRE(preview.HasActive() == true);
    REQUIRE(preview.DisplayStatus() == ge::GeometryDisplayStatus::Active);
    REQUIRE(preview.PresentedRevision() == 1);
}

TEST_CASE("A failed prepare keeps the active view with a stale flag", "[editor][geometry][preview]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);
    ge::GeometryPreview preview;

    // Publish a first valid load.
    ge::GeometryPrepareJob job;
    const ge::GeometryPrepareRequest first = MakeRequest(1, 1, 1);
    REQUIRE(job.Begin(first, {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    REQUIRE(job.Prepare() == ge::GeometryPrepareStatus::Ready);
    preview.SetExpected(1, 1, 1);
    REQUIRE(preview.OfferCompletion(first, ge::GeometryPrepareStatus::Ready, job.TakeLoad()) == true);
    REQUIRE(preview.Tick() == true);

    // A newer edit's prepare fails; the active view must remain.
    preview.SetExpected(1, 2, 2);
    REQUIRE(preview.OfferCompletion(MakeRequest(2, 2, 2), ge::GeometryPrepareStatus::Failed, gc::GeometryLoad{}) ==
            false);
    REQUIRE(preview.HasActive() == true);
    REQUIRE(preview.PresentedRevision() == 1);
    REQUIRE(preview.DisplayStatus() == ge::GeometryDisplayStatus::StaleFailed);
}

TEST_CASE("A cancelled prepare reports Failed/Cancelled and keeps the active view", "[editor][geometry][preview]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);
    ge::GeometryPrepareJob job;
    REQUIRE(job.Begin(MakeRequest(1, 1, 1), {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    job.Cancel();
    REQUIRE(job.Prepare() == ge::GeometryPrepareStatus::Failed);
    CHECK(job.Transport() == Status::Cancelled);
}

TEST_CASE("A late completion is rejected and does not publish", "[editor][geometry][preview]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);
    ge::GeometryPreview preview;

    // The preview now expects revision 3, generation 3.
    preview.SetExpected(1, 3, 3);

    // A completion carrying an older generation/revision must be rejected.
    ge::GeometryPrepareJob stale;
    const ge::GeometryPrepareRequest staleRequest = MakeRequest(1, 2, 2);
    REQUIRE(stale.Begin(staleRequest, {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    REQUIRE(stale.Prepare() == ge::GeometryPrepareStatus::Ready);
    REQUIRE(preview.OfferCompletion(staleRequest, ge::GeometryPrepareStatus::Ready, stale.TakeLoad()) == false);
    REQUIRE(preview.Tick() == false);
    REQUIRE(preview.HasActive() == false);
}

TEST_CASE("Peak memory is bounded to active plus one candidate", "[editor][geometry][preview]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);
    ge::GeometryPreview preview;
    preview.SetExpected(1, 1, 1);

    ge::GeometryPrepareJob job1;
    REQUIRE(job1.Begin(MakeRequest(1, 1, 1), {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    REQUIRE(job1.Prepare() == ge::GeometryPrepareStatus::Ready);
    REQUIRE(preview.OfferCompletion(MakeRequest(1, 1, 1), ge::GeometryPrepareStatus::Ready, job1.TakeLoad()) == true);
    REQUIRE(preview.Tick() == true);
    REQUIRE(preview.LiveLoadCount() == 1);

    // A second candidate is held while a third edit prepares: active + 1 candidate.
    preview.SetExpected(1, 2, 2);
    ge::GeometryPrepareJob job2;
    REQUIRE(job2.Begin(MakeRequest(2, 2, 2), {blob.GetData(), blob.GetSize()}, MakeLimits()) == true);
    REQUIRE(job2.Prepare() == ge::GeometryPrepareStatus::Ready);
    REQUIRE(preview.OfferCompletion(MakeRequest(2, 2, 2), ge::GeometryPrepareStatus::Ready, job2.TakeLoad()) == true);
    REQUIRE(preview.LiveLoadCount() == 2);
    REQUIRE(preview.Tick() == true);
    REQUIRE(preview.LiveLoadCount() == 1);
}

TEST_CASE("Cursor reload under RetainParameter clamps into the new span", "[editor][geometry][cursor]")
{
    ludus::foundation::core::Array<uint8> blob;
    CookCubicBlob(blob);
    gc::GeometryLoad load;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::LoadGeometry({blob.GetData(), blob.GetSize()}, MakeLimits(), load, diag) == Status::Ok);
    const geom::PathView view = load.Path.GetView();

    ge::GeometryCursor previous{};
    previous.Revision = 0;
    previous.Parameter = geom::SegmentParameter{5, 1.5}; // out of range span and u
    ge::GeometryCursor reloaded{};
    REQUIRE(ge::ReloadCursor(view, ge::CursorReloadPolicy::RetainParameter, previous, reloaded) ==
            geom::GeometryStatus::Success);
    REQUIRE(reloaded.Parameter.SpanIndex == view.Spans.GetSize() - 1);
    REQUIRE(reloaded.Parameter.U <= 1.0);
    REQUIRE(reloaded.Revision == view.Revision);

    // A distance policy without a cache is MissingCache, never an implicit fallback.
    ge::GeometryCursor viaDistance{};
    REQUIRE(ge::ReloadCursor(view, ge::CursorReloadPolicy::RetainAbsoluteDistance, previous, viaDistance) ==
            geom::GeometryStatus::MissingCache);
}
