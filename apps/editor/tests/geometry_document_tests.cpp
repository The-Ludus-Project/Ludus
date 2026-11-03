// Headless tests for the editor-private curve document and live-build debounce
// (curves and surfaces G5). No GPU or display: these exercise the undo owner,
// validated edit transactions, handle-mode conversion and the build-identity
// supersession, mirroring document_history_tests.cpp.

#include "internal/geometry_document.h"

#include <catch2/catch_test_macros.hpp>

namespace ge = ludus::editor::geometry;
namespace gc = ludus::geometry::content;
using ludus::foundation::math::Vector3d;
using ludus::foundation::uint64;

namespace
{
ge::ControlHandle MakeHandle(uint64 id, double x)
{
    ge::ControlHandle handle{};
    handle.AuthorId = id;
    handle.Position = Vector3d{x, 0.0, 0.0};
    handle.Mode = ge::gc::HandleMode::Automatic;
    return handle;
}
} // namespace

TEST_CASE("Curve edits are undoable and the revision is monotonic", "[editor][geometry]")
{
    ge::CurveDocument document;
    document.Reset(42);
    gc::GeometryDiagnosticReport diag{};

    REQUIRE(document.AddControl(MakeHandle(1, 0.0), diag) == true);
    const uint64 afterFirst = document.Revision();
    REQUIRE(document.AddControl(MakeHandle(2, 1.0), diag) == true);
    REQUIRE(document.Revision() > afterFirst);

    REQUIRE(document.CanUndo() == true);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Draft().Handles.GetSize() == 1);
    REQUIRE(document.CanRedo() == true);
    REQUIRE(document.Redo() == true);
    REQUIRE(document.Draft().Handles.GetSize() == 2);
}

TEST_CASE("Moving an automatic handle converts it to manual visibly", "[editor][geometry]")
{
    ge::CurveDocument document;
    document.Reset(1);
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(document.AddControl(MakeHandle(7, 0.0), diag) == true);
    REQUIRE(document.Draft().Handles[0].Mode == ge::gc::HandleMode::Automatic);

    REQUIRE(document.MoveControl(7, Vector3d{2.0, 0.0, 0.0}, false, diag) == true);
    REQUIRE(document.Draft().Handles[0].Mode == ge::gc::HandleMode::Manual);
    REQUIRE(document.Draft().Handles[0].Position.X == 2.0);
}

TEST_CASE("A drag coalesces into one undo step", "[editor][geometry]")
{
    ge::CurveDocument document;
    document.Reset(1);
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(document.AddControl(MakeHandle(7, 0.0), diag) == true);
    document.BreakEditGroup();
    const uint64 before = document.Revision();
    REQUIRE(document.MoveControl(7, Vector3d{1.0, 0.0, 0.0}, true, diag) == true);
    REQUIRE(document.MoveControl(7, Vector3d{2.0, 0.0, 0.0}, true, diag) == true);
    REQUIRE(document.MoveControl(7, Vector3d{3.0, 0.0, 0.0}, true, diag) == true);
    // One coalesced step undoes the whole drag back to the handle's start.
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Draft().Handles[0].Position.X == 0.0);
    (void)before;
}

TEST_CASE("Duplicate control IDs are rejected with a diagnostic", "[editor][geometry]")
{
    ge::CurveDocument document;
    document.Reset(1);
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(document.AddControl(MakeHandle(9, 0.0), diag) == true);
    REQUIRE(document.AddControl(MakeHandle(9, 1.0), diag) == false);
    CHECK(diag.Transport != ludus::content::Status::Ok);
    CHECK(diag.Repair[0] != '\0');
}

TEST_CASE("Live build supersedes a stale identity", "[editor][geometry]")
{
    ge::CurveDocument document;
    document.Reset(1);
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(document.AddControl(MakeHandle(1, 0.0), diag) == true);

    ge::GeometryLiveBuild live;
    const ludus::content::Digest identityA = ge::ComputeGeometryBuildIdentity(document.Draft(), ludus::foundation::geometry::PrecisionProfile::Visual);
    live.Schedule(identityA);
    REQUIRE(live.IsBuildRequested() == true);
    live.MarkRequested();
    REQUIRE(live.IsBuildRequested() == false);

    // A new edit produces a new identity that supersedes the dispatched one.
    REQUIRE(document.MoveControl(1, Vector3d{5.0, 0.0, 0.0}, false, diag) == true);
    const ludus::content::Digest identityB = ge::ComputeGeometryBuildIdentity(document.Draft(), ludus::foundation::geometry::PrecisionProfile::Visual);
    REQUIRE((identityA == identityB) == false);
    live.Schedule(identityB);
    REQUIRE(live.IsBuildRequested() == true);

    // Busy suppresses dispatch until cleared.
    live.SetBusy(true);
    REQUIRE(live.IsBuildRequested() == false);
    live.SetBusy(false);
    REQUIRE(live.IsBuildRequested() == true);
}
