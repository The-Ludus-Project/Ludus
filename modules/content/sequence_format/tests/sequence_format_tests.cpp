// Header self-sufficiency and layout contract for Ludus::SequenceFormat.

#include <ludus/content/sequence_format/luseq.h>

#include <catch2/catch_test_macros.hpp>

namespace format = ludus::content::sequence_format;

TEST_CASE("LUSEQ header offsets match the v1 wire format", "[sequence_format]")
{
    REQUIRE(format::MAGIC.size() == 8);
    REQUIRE(format::MAGIC.back() == '\0');
    REQUIRE(format::VERSION_OFFSET == 8);
    REQUIRE(format::RESERVED_OFFSET == 12);
    REQUIRE(format::SEQUENCE_ID_OFFSET == 16);
    REQUIRE(format::WORLD_DOCUMENT_OFFSET == 32);
    REQUIRE(format::WORLD_DIGEST_OFFSET == 48);
    REQUIRE(format::WORLD_FLAGS_OFFSET == 80);
    REQUIRE(format::TICKS_PER_SECOND_OFFSET == 84);
    REQUIRE(format::PLAYBACK_START_OFFSET == 88);
    REQUIRE(format::PLAYBACK_END_OFFSET == 96);
    REQUIRE(format::BINDING_COUNT_OFFSET == 104);
    REQUIRE(format::TRACK_COUNT_OFFSET == 108);
    REQUIRE(format::INTERVAL_COUNT_OFFSET == 112);
    REQUIRE(format::KEY_COUNT_OFFSET == 116);
    REQUIRE(format::HEADER_BYTES == 120);
}

TEST_CASE("LUSEQ record offsets match the v1 wire format", "[sequence_format]")
{
    REQUIRE(format::BINDING_KIND_FIELD == 8);
    REQUIRE(format::BINDING_LEGACY_KEY_LENGTH_FIELD == 12);
    REQUIRE(format::BINDING_OBJECT_FIELD == 16);
    REQUIRE(format::BINDING_SUBOBJECT_FIELD == 32);
    REQUIRE(format::BINDING_LEGACY_KEY_FIELD == 40);
    REQUIRE(format::BINDING_CAMERA_POSITION_FIELD == 104);
    REQUIRE(format::BINDING_CAMERA_ROTATION_FIELD == 128);
    REQUIRE(format::BINDING_CAMERA_LENS_FIELD == 144);
    REQUIRE(format::BINDING_CAMERA_OUTPUT_FIELD == 168);
    REQUIRE(format::BINDING_CAMERA_REVISION_FIELD == 176);
    REQUIRE(format::BINDING_RECORD_BYTES == 184);
    REQUIRE(format::TRACK_CHANNEL_FIELD == 16);
    REQUIRE(format::TRACK_KEY_COUNT_FIELD == 24);
    REQUIRE(format::TRACK_RECORD_BYTES == 32);
    REQUIRE(format::INTERVAL_CAMERA_FIELD == 16);
    REQUIRE(format::INTERVAL_RECORD_BYTES == 24);
    REQUIRE(format::KEY_VALUE_FIELD == 12);
    REQUIRE(format::LENS_FAR_FIELD == 20);
    REQUIRE(format::LENS_BYTES == 24);
    REQUIRE(format::VECTOR_KEY_BYTES == 24);
    REQUIRE(format::ROTATION_KEY_BYTES == 28);
    REQUIRE(format::POSITION_KEY_BYTES == 36);
    REQUIRE(format::VISIBILITY_KEY_BYTES == 12);
    REQUIRE(format::LENS_KEY_BYTES == 36);
    REQUIRE(format::MAX_KEY_RECORD_BYTES == 36);
}

TEST_CASE("LUSEQ tags are distinct and nonzero", "[sequence_format]")
{
    REQUIRE(format::BINDING_WORLD_OBJECT != 0);
    REQUIRE(format::BINDING_WORLD_OBJECT != format::BINDING_SEQUENCE_CAMERA);
    REQUIRE(format::BINDING_SEQUENCE_CAMERA != format::BINDING_LEGACY_WORLD_OBJECT);
    REQUIRE(format::CHANNEL_TRANSLATION != 0);
    REQUIRE(format::CHANNEL_CAMERA_LENS == 7);
    REQUIRE(format::INTERPOLATION_STEP != format::INTERPOLATION_LINEAR);
    REQUIRE(format::PROJECTION_PERSPECTIVE_FINITE != 0);
    REQUIRE((format::WORLD_FLAGS_V1 & format::WORLD_FLAG_CONTENT_DIGEST) != 0);
}
