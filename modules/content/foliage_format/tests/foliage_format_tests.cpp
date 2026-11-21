// Header self-sufficiency and layout contract for Ludus::FoliageFormat.

#include <ludus/content/foliage_format/lufol.h>

#include <catch2/catch_test_macros.hpp>

namespace format = ludus::content::foliage_format;

TEST_CASE("LUFOL header offsets match the v1 wire format", "[foliage_format]")
{
    REQUIRE(format::MAGIC.size() == 8);
    REQUIRE(format::MAGIC.back() == '\0');
    REQUIRE(format::FORMAT_VERSION_OFFSET == 8);
    REQUIRE(format::GENERATOR_VERSION_OFFSET == 12);
    REQUIRE(format::DOMAIN_SCHEMA_OFFSET == 16);
    REQUIRE(format::PAGE_SIZE_OFFSET == 20);
    REQUIRE(format::DOMAIN_ID_OFFSET == 24);
    REQUIRE(format::ORIGIN_X_OFFSET == 32);
    REQUIRE(format::ORIGIN_Z_OFFSET == 40);
    REQUIRE(format::TERRAIN_REVISION_OFFSET == 48);
    REQUIRE(format::LAYER_COUNT_OFFSET == 56);
    REQUIRE(format::SPECIES_COUNT_OFFSET == 60);
    REQUIRE(format::PAGE_COUNT_OFFSET == 64);
    REQUIRE(format::PLACEMENT_COUNT_OFFSET == 68);
    REQUIRE(format::INPUT_DIGEST_OFFSET == 72);
    REQUIRE(format::HEADER_BYTES == 104);
}

TEST_CASE("LUFOL record fields match the v1 wire format", "[foliage_format]")
{
    REQUIRE(format::LAYER_CELL_SIZE_FIELD == 4);
    REQUIRE(format::LAYER_RECORD_BYTES == 8);
    REQUIRE(format::SPECIES_VARIANT_COUNT_FIELD == 16);
    REQUIRE(format::SPECIES_RECORD_BYTES == 20);
    REQUIRE(format::PAGE_MINIMUM_X_FIELD == 16);
    REQUIRE(format::PAGE_MINIMUM_HEIGHT_FIELD == 48);
    REQUIRE(format::PAGE_RECORD_BYTES == 56);
    REQUIRE(format::PLACEMENT_CELL_X_FIELD == 4);
    REQUIRE(format::PLACEMENT_OFFSET_X_FIELD == 12);
    REQUIRE(format::PLACEMENT_HEIGHT_FIELD == 16);
    REQUIRE(format::PLACEMENT_YAW_FIELD == 20);
    REQUIRE(format::PLACEMENT_NORMAL_X_FIELD == 24);
    REQUIRE(format::PLACEMENT_SPECIES_FIELD == 28);
    REQUIRE(format::PLACEMENT_RECORD_BYTES == 32);
}

TEST_CASE("LUFOL limits keep references and offsets in their stored widths", "[foliage_format]")
{
    // A full-size cell offset fits uint16 and a whole page set fits a 32-bit address space.
    REQUIRE(format::MAX_CELL_SIZE - 1 == 0xffffU);
    REQUIRE(format::MAX_SPECIES - 1 <= 0xffffU);
    REQUIRE(format::MAX_PAGE_SET_BYTES < format::MAX_ADDRESSABLE_BYTES);
    REQUIRE(format::MAX_ADDRESSABLE_BYTES == (format::usize{1} << 31));
    REQUIRE(format::MAX_PAGE_SIZE % format::MAX_CELL_SIZE == 0);
}
