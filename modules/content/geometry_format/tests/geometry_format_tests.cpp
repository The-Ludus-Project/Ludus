// Tests for the LUGEO cooked geometry format contract (Ludus::GeometryFormat).
// The format is header-only, so these check the frozen record widths, the magic
// and version constants, the enum-tag predicates and the section-directory
// arithmetic that readers in Ludus::GeometryContent depend on.

#include <ludus/content/geometry_format/geometry_format.h>
#include <ludus/content/geometry_format/limits.h>

#include <catch2/catch_test_macros.hpp>

namespace gf = ludus::content::geometry_format;

TEST_CASE("Magic and versions are frozen", "[geometry_format]")
{
    REQUIRE(gf::MAGIC.size() == 8);
    REQUIRE(gf::MAGIC[7] == '\0');
    REQUIRE(gf::FORMAT_VERSION == gf::FORMAT_VERSION_1);
    REQUIRE(gf::IsSupportedVersion(gf::FORMAT_VERSION) == true);
    REQUIRE(gf::IsSupportedVersion(0) == false);
    REQUIRE(gf::IsSupportedVersion(gf::FORMAT_VERSION + 1) == false);
    REQUIRE(gf::DIGEST_BYTES == 32);
}

TEST_CASE("Record widths match their field schema with no host padding", "[geometry_format]")
{
    REQUIRE(sizeof(gf::SectionEntry) == gf::SECTION_ENTRY_BYTES);
    REQUIRE(sizeof(gf::PathSpanRecord) == gf::PATH_SPAN_RECORD_BYTES);
    REQUIRE(sizeof(gf::JointRecord) == gf::JOINT_RECORD_BYTES);
    REQUIRE(sizeof(gf::TrackKeyRecord) == gf::TRACK_KEY_RECORD_BYTES);
    REQUIRE(sizeof(gf::PatchRecord) == gf::PATCH_RECORD_BYTES);
    REQUIRE(sizeof(gf::AdjacencyRecord) == gf::ADJACENCY_RECORD_BYTES);
    REQUIRE(sizeof(gf::SourceIdentityRecord) == gf::SOURCE_IDENTITY_RECORD_BYTES);
    // Every serialized width is a multiple of four so sections can be four-byte aligned.
    REQUIRE(gf::PATH_SPAN_RECORD_BYTES % 4 == 0);
    REQUIRE(gf::JOINT_RECORD_BYTES % 4 == 0);
    REQUIRE(gf::TRACK_KEY_RECORD_BYTES % 4 == 0);
    REQUIRE(gf::PATCH_RECORD_BYTES % 4 == 0);
    REQUIRE(gf::ADJACENCY_RECORD_BYTES % 4 == 0);
    REQUIRE(gf::SOURCE_IDENTITY_RECORD_BYTES % 4 == 0);
}

TEST_CASE("Header byte computation includes the directory and digests", "[geometry_format]")
{
    REQUIRE(gf::GetHeaderBytes(0) == gf::HEADER_MIN_BYTES);
    REQUIRE(gf::GetHeaderBytes(2) == gf::HEADER_FIXED_BYTES + 2 * gf::SECTION_ENTRY_BYTES + gf::HEADER_DIGEST_BYTES);
    REQUIRE(gf::GetHeaderBytes(2) > gf::GetHeaderBytes(1));
}

TEST_CASE("Enum tag predicates accept known values and reject the rest", "[geometry_format]")
{
    REQUIRE(gf::IsKnownKind(static_cast<gf::uint32>(gf::GeometryKindTag::Path)) == true);
    REQUIRE(gf::IsKnownKind(static_cast<gf::uint32>(gf::GeometryKindTag::Surface)) == true);
    REQUIRE(gf::IsKnownKind(3) == false);

    REQUIRE(gf::IsKnownSpanKind(static_cast<gf::uint32>(gf::SpanKindTag::Line)) == true);
    REQUIRE(gf::IsKnownSpanKind(2) == false);

    REQUIRE(gf::IsKnownTrackInterpolation(static_cast<gf::uint32>(gf::TrackInterpolationTag::Hermite)) == true);
    REQUIRE(gf::IsKnownTrackInterpolation(3) == false);

    REQUIRE(gf::IsKnownPatchSide(static_cast<gf::uint32>(gf::PatchSideTag::V1)) == true);
    REQUIRE(gf::IsKnownPatchSide(4) == false);

    REQUIRE(gf::IsKnownPrecision(static_cast<gf::uint32>(gf::PrecisionProfileTag::Strict)) == true);
    REQUIRE(gf::IsKnownPrecision(2) == false);
}

TEST_CASE("Section element widths agree with the record widths", "[geometry_format]")
{
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::PathSpans) == gf::PATH_SPAN_RECORD_BYTES);
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::PathJoints) == gf::JOINT_RECORD_BYTES);
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::TrackKeys) == gf::TRACK_KEY_RECORD_BYTES);
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::SurfacePatches) == gf::PATCH_RECORD_BYTES);
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::SurfaceAdjacency) == gf::ADJACENCY_RECORD_BYTES);
    REQUIRE(gf::GetSectionElementBytes(gf::SectionId::SourceIdentities) == gf::SOURCE_IDENTITY_RECORD_BYTES);
}

TEST_CASE("Admission limits are nonzero and ordered sensibly", "[geometry_format]")
{
    REQUIRE(gf::MAX_BLOB_BYTES > gf::MAX_SOURCE_BYTES);
    REQUIRE(gf::MAX_SPANS > 0);
    REQUIRE(gf::MAX_PATCHES > 0);
    REQUIRE(gf::MAX_SECTIONS >= 5);
}
