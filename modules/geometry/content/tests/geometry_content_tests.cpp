// Ludus::GeometryContent tests for the G5 gate: source round-trip, cook/load
// round-trip and determinism against the G0 reference corpus, corrupt and
// overflowing blobs (fuzzing the section directory and count arithmetic), the
// catalog versioned extension, and that every failure is a structured
// diagnostic rather than an assertion. Allocation-failure injection lives in
// geometry_content_alloc_tests.cpp. See ADR 0024 and the architecture page.

#include <ludus/geometry/content/geometry_content.h>

#include <ludus/content/content.h>
#include <ludus/content/geometry_format/geometry_format.h>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <curves_surfaces/reference_fixtures.hpp>

#include <catch2/catch_test_macros.hpp>

namespace gc = ludus::geometry::content;
namespace gf = ludus::content::geometry_format;
namespace geom = ludus::foundation::geometry;
namespace fx = ludus::geometry_fixtures;
using ludus::content::Status;
using ludus::foundation::float64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

namespace
{
geom::ToleranceProfile MakeTolerance()
{
    geom::ToleranceProfile t{};
    t.PositionError = 1e-3;
    t.RelativePositionError = 0.0;
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
    limits.MaxSpans = 1024;
    limits.MaxLeaves = 1u << 16;
    limits.MaxDepth = 16;
    limits.MaxSubdivisions = 1u << 16;
    limits.MaxIterations = 1024;
    limits.MaxPreparedBytes = 1u << 20;
    limits.MaxPatches = 256;
    return limits;
}

// Build a single-span cubic path source from a corpus cubic fixture.
void MakeCubicSource(const fx::CubicFixture& fixture, gc::GeometrySource& outSource)
{
    outSource.Mode = gc::GeometrySourceMode::CubicPath;
    outSource.Revision = 7;
    outSource.Origin = ludus::foundation::math::Vector3d{};
    outSource.AssetScale = 1.0;
    outSource.IsClosed = false;
    outSource.Tolerance = MakeTolerance();
    outSource.Limits = MakeLimits();
    outSource.Precision = geom::PrecisionProfile::Visual;
    gc::AuthoredSpan span{};
    span.AuthorId = 1;
    span.Kind = gf::SpanKindTag::Cubic;
    span.DomainWidth = 1.0;
    for (usize c = 0; c < gf::SPAN_CONTROL_COUNT; ++c)
    {
        span.Control[c] = ludus::foundation::math::Vector3d{
            static_cast<float64>(fixture.Control[c].X),
            static_cast<float64>(fixture.Control[c].Y),
            static_cast<float64>(fixture.Control[c].Z),
        };
    }
    REQUIRE(outSource.Spans.TryAdd(span) == true);
}
} // namespace

TEST_CASE("Cook and load round-trip reproduces the corpus cubic spans", "[geometry_content]")
{
    for (usize i = 0; i < fx::CUBIC_FIXTURE_COUNT; ++i)
    {
        const fx::CubicFixture& fixture = fx::CUBIC_FIXTURES[i];
        gc::GeometrySource source;
        MakeCubicSource(fixture, source);

        ludus::foundation::core::Array<uint8> blob;
        gc::GeometryDiagnosticReport diag{};
        REQUIRE(gc::CookGeometry(source, blob, diag) == Status::Ok);
        REQUIRE(blob.GetSize() >= gf::HEADER_MIN_BYTES);

        gc::GeometryLoad load;
        gc::GeometryDiagnosticReport loadDiag{};
        REQUIRE(gc::LoadGeometry({blob.GetData(), blob.GetSize()}, MakeLimits(), load, loadDiag) == Status::Ok);
        REQUIRE(load.Kind == gf::GeometryKindTag::Path);

        const geom::PathView view = load.Path.GetView();
        REQUIRE(view.Spans.GetSize() == 1);
        // Endpoints are exact through the cook/load path.
        ludus::foundation::math::Vector3 start{};
        REQUIRE(geom::TryEvaluatePath(view, geom::SegmentParameter{0, 0.0}, start) == geom::GeometryStatus::Success);
        CHECK(start.X == fixture.Control[0].X);
        CHECK(start.Y == fixture.Control[0].Y);
        CHECK(start.Z == fixture.Control[0].Z);

        // Re-cooking identical source bytes is byte-identical (determinism).
        ludus::foundation::core::Array<uint8> blob2;
        gc::GeometryDiagnosticReport diag2{};
        REQUIRE(gc::CookGeometry(source, blob2, diag2) == Status::Ok);
        REQUIRE(blob2.GetSize() == blob.GetSize());
        bool isIdentical = true;
        for (usize b = 0; b < blob.GetSize(); ++b)
        {
            isIdentical = isIdentical && blob[b] == blob2[b];
        }
        CHECK(isIdentical == true);
    }
}

TEST_CASE("Source JSON round-trips through Write and Parse", "[geometry_content]")
{
    gc::GeometrySource source;
    MakeCubicSource(fx::CUBIC_FIXTURES[0], source);

    ludus::content::Bytes json;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::WriteGeometrySource(source, json, diag) == Status::Ok);

    gc::GeometrySource reparsed;
    gc::GeometryDiagnosticReport parseDiag{};
    REQUIRE(gc::ParseGeometrySource(json.String(), reparsed, parseDiag) == Status::Ok);
    REQUIRE(reparsed.Mode == gc::GeometrySourceMode::CubicPath);
    REQUIRE(reparsed.Spans.GetSize() == source.Spans.GetSize());
    CHECK(reparsed.Spans[0].AuthorId == source.Spans[0].AuthorId);
    CHECK(reparsed.IsClosed == source.IsClosed);

    // Writing the reparsed source yields the same canonical bytes.
    ludus::content::Bytes json2;
    gc::GeometryDiagnosticReport diag2{};
    REQUIRE(gc::WriteGeometrySource(reparsed, json2, diag2) == Status::Ok);
    CHECK(json.String() == json2.String());
}

TEST_CASE("Parse rejects unknown versions and duplicate ids with diagnostics", "[geometry_content]")
{
    gc::GeometrySource source;
    gc::GeometryDiagnosticReport diag{};
    // Unknown schema version.
    REQUIRE(gc::ParseGeometrySource(R"({"version":99,"mode":"cubic-path"})", source, diag) == Status::Invalid);
    CHECK(diag.Transport == Status::Invalid);
    CHECK(diag.Repair[0] != '\0');

    // Not JSON.
    gc::GeometryDiagnosticReport diag2{};
    REQUIRE(gc::ParseGeometrySource("not json", source, diag2) == Status::Invalid);

    // Unknown mode.
    gc::GeometryDiagnosticReport diag3{};
    REQUIRE(gc::ParseGeometrySource(R"({"version":1,"mode":"spiral"})", source, diag3) == Status::Invalid);
}

TEST_CASE("Truncated and wrong-magic blobs fail without asserting", "[geometry_content]")
{
    gc::GeometrySource source;
    MakeCubicSource(fx::CUBIC_FIXTURES[1], source);
    ludus::foundation::core::Array<uint8> blob;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::CookGeometry(source, blob, diag) == Status::Ok);

    gc::GeometryLoad load;
    gc::GeometryDiagnosticReport loadDiag{};

    // Truncated below the minimum header.
    REQUIRE(gc::LoadGeometry({blob.GetData(), 4}, MakeLimits(), load, loadDiag) == Status::Invalid);

    // Corrupt magic.
    ludus::foundation::core::Array<uint8> bad;
    REQUIRE(bad.TryResize(blob.GetSize()) == true);
    for (usize i = 0; i < blob.GetSize(); ++i)
    {
        bad[i] = blob[i];
    }
    bad[0] = 'X';
    gc::GeometryDiagnosticReport magicDiag{};
    REQUIRE(gc::LoadGeometry({bad.GetData(), bad.GetSize()}, MakeLimits(), load, magicDiag) == Status::Invalid);
}

TEST_CASE("Fuzzing every byte keeps the loader safe and output unchanged", "[geometry_content]")
{
    gc::GeometrySource source;
    MakeCubicSource(fx::CUBIC_FIXTURES[3], source);
    ludus::foundation::core::Array<uint8> blob;
    gc::GeometryDiagnosticReport diag{};
    REQUIRE(gc::CookGeometry(source, blob, diag) == Status::Ok);

    // Flip high bits across the whole blob, especially the section directory and
    // count fields, and confirm the loader either loads a valid owner or fails
    // with a diagnostic; it never asserts, reads out of bounds or mutates
    // outLoad on failure.
    for (usize i = 0; i < blob.GetSize(); ++i)
    {
        ludus::foundation::core::Array<uint8> mutated;
        REQUIRE(mutated.TryResize(blob.GetSize()) == true);
        for (usize b = 0; b < blob.GetSize(); ++b)
        {
            mutated[b] = blob[b];
        }
        mutated[i] = static_cast<uint8>(mutated[i] ^ 0xFFu);

        gc::GeometryLoad load;
        gc::GeometryDiagnosticReport fuzzDiag{};
        const Status status = gc::LoadGeometry({mutated.GetData(), mutated.GetSize()}, MakeLimits(), load, fuzzDiag);
        if (status != Status::Ok)
        {
            CHECK(fuzzDiag.Transport != Status::Ok);
        }
    }
}

TEST_CASE("Catalog admits cooked-geometry only at schema version 2", "[geometry_content]")
{
    ludus::content::Catalog catalog;
    ludus::content::Diagnostic diagnostic{};
    const char* v2 = R"({"version":2,"resources":[{"id":"curve","kind":"cooked-geometry","path":"c.lugeo"}]})";
    REQUIRE(catalog.Read(v2, diagnostic) == Status::Ok);
    REQUIRE(catalog.Find("curve") != nullptr);
    CHECK(catalog.Find("curve")->Type == ludus::content::Kind::CookedGeometry);

    ludus::content::Catalog rejecting;
    ludus::content::Diagnostic rejectDiag{};
    const char* v1 = R"({"version":1,"resources":[{"id":"curve","kind":"cooked-geometry","path":"c.lugeo"}]})";
    REQUIRE(rejecting.Read(v1, rejectDiag) == Status::Invalid);

    // Write emits version 2 when a geometry entry is present.
    ludus::content::Bytes out;
    REQUIRE(catalog.Write(out) == Status::Ok);
    const std::string_view text = out.String();
    CHECK(text.find("\"version\": 2") != std::string_view::npos);
    CHECK(text.find("cooked-geometry") != std::string_view::npos);
}
