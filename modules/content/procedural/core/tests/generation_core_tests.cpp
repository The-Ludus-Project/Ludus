#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/content/procedural/core/generation_trace.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::procedural;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

constexpr uint32 PRODUCT_SCHEMA = 7;
constexpr GenerationCacheLimits ROOMY_LIMITS{16, 4096};

[[nodiscard]] GenerationDigest MakeKey(uint32 value)
{
    GenerationByteWriter writer;
    writer.WriteUInt32(value);
    return writer.ComputeDigest();
}

[[nodiscard]] BorrowedView<const uint8> ViewOf(const Array<uint8>& bytes)
{
    return {bytes.GetData(), bytes.GetSize()};
}
} // namespace

TEST_CASE("generation bytes round-trip fixed-width little-endian fields", "[procedural][core]")
{
    GenerationByteWriter writer;
    writer.WriteUInt8(0xABU);
    writer.WriteUInt16(0x1234U);
    writer.WriteUInt32(0x89ABCDEFU);
    writer.WriteUInt64(0x0102030405060708ULL);
    writer.WriteInt16(-2);
    writer.WriteInt32(-70000);
    const GenerationDigest digest = MakeKey(3);
    writer.WriteDigest(digest);
    REQUIRE(writer.HasFailed() == false);
    const BorrowedView<const uint8> bytes = writer.GetBytes();
    REQUIRE(bytes.GetSize() == 1 + 2 + 4 + 8 + 2 + 4 + 32);
    REQUIRE(bytes[1] == 0x34U);
    REQUIRE(bytes[2] == 0x12U);
    REQUIRE(bytes[7] == 0x08U);

    GenerationByteReader reader(bytes);
    REQUIRE(reader.ReadUInt8() == 0xABU);
    REQUIRE(reader.ReadUInt16() == 0x1234U);
    REQUIRE(reader.ReadUInt32() == 0x89ABCDEFU);
    REQUIRE(reader.ReadUInt64() == 0x0102030405060708ULL);
    REQUIRE(reader.ReadInt16() == -2);
    REQUIRE(reader.ReadInt32() == -70000);
    REQUIRE(reader.ReadDigest() == digest);
    REQUIRE(reader.IsAtEnd() == true);
    REQUIRE(reader.ReadUInt8() == 0);
    REQUIRE(reader.HasFailed() == true);
    REQUIRE(reader.IsAtEnd() == false);
}

TEST_CASE("generation digests cover the written bytes exactly", "[procedural][core]")
{
    REQUIRE(MakeKey(1) == MakeKey(1));
    REQUIRE(MakeKey(1) != MakeKey(2));
    REQUIRE(ComputeGenerationDigest({}) == ludus::content::Hash({}));
    REQUIRE(GenerationDigestLess{}(MakeKey(1), MakeKey(1)) == false);
    REQUIRE(GenerationDigestLess{}(MakeKey(1), MakeKey(2)) != GenerationDigestLess{}(MakeKey(2), MakeKey(1)));
    const BorrowedView<const uint8> invalid{nullptr, 1};
    REQUIRE(ComputeGenerationDigest(invalid) == ComputeGenerationDigest({}));
}

TEST_CASE("the generation cache validates hits and names every miss", "[procedural][core][cache]")
{
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    const GenerationDigest key = MakeKey(10);
    Array<uint8> payload;
    for (uint8 i = 0; i < 40; ++i)
    {
        payload.Add(i);
    }
    Array<uint8> found;
    REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Absent);
    REQUIRE(TryStoreGenerationProduct(store, key, PRODUCT_SCHEMA, ViewOf(payload)) == true);
    REQUIRE(store.GetEntryCount() == 1);
    REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Hit);
    REQUIRE(found.GetSize() == payload.GetSize());
    REQUIRE(found[39] == 39);

    SECTION("a flipped payload byte is corrupt and removed")
    {
        BorrowedView<uint8> entry = store.FindEntryBytes(key);
        entry[entry.GetSize() - 1] ^= 1U;
        Array<uint8> untouched;
        untouched.Add(99);
        REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, untouched) == GenerationCacheOutcome::Corrupt);
        REQUIRE(untouched.GetSize() == 1);
        REQUIRE(store.GetEntryCount() == 0);
        REQUIRE(store.GetByteCount() == 0);
        REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Absent);
    }
    SECTION("a truncated entry is corrupt")
    {
        const BorrowedView<uint8> entry = store.FindEntryBytes(key);
        Array<uint8> truncated;
        truncated.AddRange({entry.GetDataOrNull(), entry.GetSize() - 1});
        REQUIRE(store.TryWrite(key, ViewOf(truncated)) == true);
        REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Corrupt);
    }
    SECTION("an entry stored under another key is corrupt")
    {
        const BorrowedView<uint8> entry = store.FindEntryBytes(key);
        Array<uint8> moved;
        moved.AddRange({entry.GetDataOrNull(), entry.GetSize()});
        const GenerationDigest other = MakeKey(11);
        REQUIRE(store.TryWrite(other, ViewOf(moved)) == true);
        REQUIRE(TryFindGenerationProduct(store, other, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Corrupt);
        REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA, found) == GenerationCacheOutcome::Hit);
    }
    SECTION("a different schema is a named miss")
    {
        REQUIRE(TryFindGenerationProduct(store, key, PRODUCT_SCHEMA + 1, found) ==
                GenerationCacheOutcome::SchemaMismatch);
        REQUIRE(store.GetEntryCount() == 0);
    }
}

TEST_CASE("the memory cache store refuses writes beyond its budgets", "[procedural][core][cache]")
{
    MemoryGenerationCacheStore store(GenerationCacheLimits{1, 100});
    Array<uint8> small;
    small.Resize(60, 1);
    Array<uint8> large;
    large.Resize(101, 2);
    REQUIRE(store.TryWrite(MakeKey(1), ViewOf(large)) == false);
    REQUIRE(store.TryWrite(MakeKey(1), ViewOf(small)) == true);
    REQUIRE(store.TryWrite(MakeKey(2), ViewOf(small)) == false);
    // Replacing an entry counts only the replacement's bytes.
    Array<uint8> replacement;
    replacement.Resize(100, 3);
    REQUIRE(store.TryWrite(MakeKey(1), ViewOf(replacement)) == true);
    REQUIRE(store.GetByteCount() == 100);
    REQUIRE(store.GetEntryCount() == 1);
    // A product too large for the store is not cached, which is reported but not an error for generation.
    REQUIRE(TryStoreGenerationProduct(store, MakeKey(3), PRODUCT_SCHEMA, ViewOf(small)) == false);
}

TEST_CASE("the generation trace is bounded and counts dropped records", "[procedural][core][trace]")
{
    GenerationTrace trace(2);
    GenerationStageRecord record;
    for (uint32 stage = 0; stage < 5; ++stage)
    {
        record.Stage = stage;
        trace.Record(record);
    }
    REQUIRE(trace.GetRecords().GetSize() == 2);
    REQUIRE(trace.GetRecords()[1].Stage == 1);
    REQUIRE(trace.GetDroppedCount() == 3);
    trace.Clear();
    REQUIRE(trace.GetRecords().IsEmpty() == true);
    REQUIRE(trace.GetDroppedCount() == 0);
}
