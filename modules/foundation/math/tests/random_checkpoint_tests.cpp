// Named sequential setup goldens, the explicit 20-byte PCG payload, and the random checkpoint format: golden bytes,
// event and identity metadata, compatibility rejection that changes nothing, atomic stream restore and replay across
// a reload. Contract: docs/architecture/randomness.md#replay-persistence-networking-and-entropy.

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/random_checkpoint.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string_view>

using namespace ludus::foundation::math;
using ludus::foundation::core::StaticArray;

namespace
{
constexpr usize GOLDEN_DRAWS = 4;
constexpr usize SETUP_GOLDEN_COUNT = 4;
constexpr usize GOLDEN_CHECKPOINT_BYTES = 124;
constexpr usize RECORD_CAPACITY = 4;
constexpr usize REPLAY_DRAWS = 64;
constexpr uint64 CONTENT_IDENTITY = 0x1122334455667788ULL;
constexpr uint64 ROOT_SEED = 0x0123456789abcdefULL;
constexpr uint32 LOOT_DOMAIN = 4;
constexpr uint64 LOOT_SCOPE = 0x0000000100020003ULL;
/// Byte offsets of header fields that the rejection cases patch.
constexpr usize FORMAT_OFFSET = 4;
constexpr usize TOTAL_BYTES_OFFSET = 8;
constexpr usize STREAM_SETUP_OFFSET = 24;
constexpr usize STREAM_RECORD_OFFSET = 60;
constexpr usize STREAM_PAYLOAD_LENGTH_OFFSET = STREAM_RECORD_OFFSET + 12;
constexpr usize STREAM_INCREMENT_OFFSET = STREAM_RECORD_OFFSET + 28;
constexpr usize STREAM_RECORD_BYTES = 36;
constexpr usize EVENT_RECORD_BYTES = 16;

using CheckpointBytes = StaticArray<uint8, GOLDEN_CHECKPOINT_BYTES>;

struct SetupGolden
{
    uint64 RootSeed = 0;
    RandomOwner Owner;
    uint64 State = 0;
    uint64 Increment = 0;
    StaticArray<uint32, GOLDEN_DRAWS> Draws{};
};

/// Independently computed from the documented formula (scratch Python model of Mix64, the domain key and PCG v1).
constexpr StaticArray<SetupGolden, SETUP_GOLDEN_COUNT> SETUP_GOLDENS{{
    {0, {1, 0}, 0xc69b8f81c9f49a38ULL, 0x3ULL, {{0x77c4e5d3u, 0xebaa8355u, 0xb1cfabcbu, 0xe8cecae1u}}},
    {0x4c75647573000001ULL,
     {LOOT_DOMAIN, LOOT_SCOPE},
     0xe61fd4e5d30298a3ULL,
     0x9ULL,
     {{0x3fdac44cu, 0xc2e444e8u, 0x7ef724cdu, 0x7a205280u}}},
    {0xffffffffffffffffULL,
     {0x00ffffffu, 0xffffffffffffffffULL},
     0x3b2ac8484d44b531ULL,
     0x1ffffffULL,
     {{0xbecab1a0u, 0x5b8d49aeu, 0xf75e1b8fu, 0xc6333fc2u}}},
    {42,
     {0x42414b45u, 7},
     0x59e0c508bf0f693eULL,
     0x8482968bULL,
     {{0xc227834du, 0xc13fda22u, 0xa033ab93u, 0xf8c274f9u}}},
}};

/// The checkpoint below, frozen byte for byte: header, one stream, one event counter and one identity allocator.
constexpr CheckpointBytes GOLDEN_CHECKPOINT{{
    0x4c, 0x52, 0x43, 0x50, 0x01, 0x00, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x88, 0x77, 0x66, 0x55,
    0x44, 0x33, 0x22, 0x11, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa3, 0x98, 0x02, 0xd3, 0xe5, 0xd4, 0x1f, 0xe6, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}};

constexpr RandomStreamRecord GOLDEN_STREAM{{LOOT_DOMAIN, LOOT_SCOPE}, {1, 0xe61fd4e5d30298a3ULL, 0x9ULL}};
constexpr RandomEventRecord GOLDEN_EVENT{{LOOT_DOMAIN, 7}, 3};
constexpr RandomIdentityRecord GOLDEN_IDENTITY{1, 0x10};

RandomCheckpoint MakeGoldenCheckpoint() noexcept
{
    return RandomCheckpoint
    {
        .RootSeed = ROOT_SEED,
        .ContentIdentity = CONTENT_IDENTITY,
        .Streams = {&GOLDEN_STREAM, 1},
        .Events = {&GOLDEN_EVENT, 1},
        .Identities = {&GOLDEN_IDENTITY, 1},
    };
}

/// Decoded-record storage pre-filled with sentinels, so a test can prove a rejected read wrote nothing.
struct Storage
{
    StaticArray<RandomStreamRecord, RECORD_CAPACITY> Streams{};
    StaticArray<RandomEventRecord, RECORD_CAPACITY> Events{};
    StaticArray<RandomIdentityRecord, RECORD_CAPACITY> Identities{};

    Storage() noexcept
    {
        Streams.Fill({{99, 99}, {1, 99, 99}});
        Events.Fill({{99, 99}, 99});
        Identities.Fill({99, 99});
    }

    [[nodiscard]] RandomCheckpointStorage GetView() noexcept
    {
        return
        {
            .Streams = {Streams.data(), Streams.size()},
            .Events = {Events.data(), Events.size()},
            .Identities = {Identities.data(), Identities.size()},
        };
    }

    [[nodiscard]] bool IsUntouched() const noexcept
    {
        for (usize i = 0; i < RECORD_CAPACITY; ++i)
        {
            if (Streams[i].Owner.Domain != 99 || Streams[i].State.State != 99 || Events[i].NextEvent != 99 ||
                Identities[i].Allocator != 99)
            {
                return false;
            }
        }
        return true;
    }
};

const RandomCheckpointContents SENTINEL_CONTENTS{ .RootSeed = 5, .ByteCount = 5 };

RandomCheckpointStatus Read(const CheckpointBytes& bytes, Storage& inoutStorage, RandomCheckpointContents& outContents)
{
    return TryReadRandomCheckpoint({bytes.data(), bytes.size()}, CONTENT_IDENTITY, inoutStorage.GetView(), outContents);
}

/// Read `bytes`, expect `expected`, and require that nothing was written.
void RequireRejected(const CheckpointBytes& bytes, RandomCheckpointStatus expected)
{
    Storage storage;
    RandomCheckpointContents contents = SENTINEL_CONTENTS;
    REQUIRE(Read(bytes, storage, contents) == expected);
    REQUIRE(storage.IsUntouched() == true);
    REQUIRE(contents.RootSeed == SENTINEL_CONTENTS.RootSeed);
    REQUIRE(contents.ByteCount == SENTINEL_CONTENTS.ByteCount);
}
} // namespace

TEST_CASE("Named stream setup matches the documented domain/scope derivation", "[math][random][checkpoint]")
{
    for (const SetupGolden& golden : SETUP_GOLDENS)
    {
        RandomStream stream;
        REQUIRE(TryMakeRandomStream(golden.RootSeed, golden.Owner, stream) == MathStatus::Success);
        const RandomState state = stream.GetState();
        CHECK(state.State == golden.State);
        CHECK(state.Increment == golden.Increment);
        for (const uint32 draw : golden.Draws)
        {
            CHECK(stream.NextUInt32() == draw);
        }
    }
    // The setup equals an explicit reseed with the documented seed and the domain as selector.
    RandomStream named;
    RandomStream explicitSeed;
    REQUIRE(TryMakeRandomStream(0, {1, 0}, named) == MathStatus::Success);
    REQUIRE(explicitSeed.TryReseed(0xcd73fe3de975ac26ULL, 1) == true);
    CHECK(named.GetState().State == explicitSeed.GetState().State);
    CHECK(named.GetState().Increment == explicitSeed.GetState().Increment);
}

TEST_CASE("Named stream setup rejects domain zero and separates scopes and domains", "[math][random][checkpoint]")
{
    RandomStream stream;
    REQUIRE(stream.TryReseed(42, 54) == true);
    const RandomState before = stream.GetState();
    REQUIRE(TryMakeRandomStream(ROOT_SEED, {0, 1}, stream) == MathStatus::InvalidArgument);
    CHECK(stream.GetState().State == before.State);
    CHECK(stream.GetState().Increment == before.Increment);

    RandomStream first;
    RandomStream secondScope;
    RandomStream secondDomain;
    REQUIRE(TryMakeRandomStream(ROOT_SEED, {LOOT_DOMAIN, 1}, first) == MathStatus::Success);
    REQUIRE(TryMakeRandomStream(ROOT_SEED, {LOOT_DOMAIN, 2}, secondScope) == MathStatus::Success);
    REQUIRE(TryMakeRandomStream(ROOT_SEED, {LOOT_DOMAIN + 1, 1}, secondDomain) == MathStatus::Success);
    // Scopes in one domain share the increment and start at different phases; domains differ in increment.
    CHECK(first.GetState().Increment == secondScope.GetState().Increment);
    CHECK(first.GetState().State != secondScope.GetState().State);
    CHECK(first.GetState().Increment != secondDomain.GetState().Increment);
}

TEST_CASE("The PCG state payload is 20 explicit little-endian bytes", "[math][random][checkpoint]")
{
    const RandomState state{1, 0x0123456789abcdefULL, 0xfedcba9876543211ULL};
    const StaticArray<uint8, RANDOM_STATE_PAYLOAD_BYTES> expected{{0x01, 0x00, 0x00, 0x00, 0xef, 0xcd, 0xab,
                                                                   0x89, 0x67, 0x45, 0x23, 0x01, 0x11, 0x32,
                                                                   0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe}};
    StaticArray<uint8, RANDOM_STATE_PAYLOAD_BYTES> bytes{};
    REQUIRE(TryEncodeRandomState(state, {bytes.data(), bytes.size()}) == MathStatus::Success);
    CHECK(bytes == expected);

    RandomState decoded;
    REQUIRE(TryDecodeRandomState({bytes.data(), bytes.size()}, decoded) == MathStatus::Success);
    CHECK(decoded.Version == state.Version);
    CHECK(decoded.State == state.State);
    CHECK(decoded.Increment == state.Increment);
}

TEST_CASE("The PCG state payload rejects unsupported states and lengths without writing", "[math][random][checkpoint]")
{
    StaticArray<uint8, RANDOM_STATE_PAYLOAD_BYTES + 1> bytes{};
    bytes.Fill(0xaa);
    const std::span<uint8> exact(bytes.data(), RANDOM_STATE_PAYLOAD_BYTES);
    CHECK(TryEncodeRandomState({2, 1, 1}, exact) == MathStatus::InvalidArgument);
    CHECK(TryEncodeRandomState({1, 1, 2}, exact) == MathStatus::InvalidArgument);
    CHECK(TryEncodeRandomState({1, 1, 1}, exact.first(RANDOM_STATE_PAYLOAD_BYTES - 1)) == MathStatus::SizeMismatch);
    CHECK(TryEncodeRandomState({1, 1, 1}, {bytes.data(), bytes.size()}) == MathStatus::SizeMismatch);
    for (const uint8 value : bytes)
    {
        CHECK(value == 0xaa);
    }

    const RandomState sentinel{1, 7, 9};
    RandomState decoded = sentinel;
    REQUIRE(TryEncodeRandomState({1, 1, 1}, exact) == MathStatus::Success);
    CHECK(TryDecodeRandomState({bytes.data(), bytes.size()}, decoded) == MathStatus::SizeMismatch);
    exact[0] = 2; // version 2
    CHECK(TryDecodeRandomState(exact, decoded) == MathStatus::InvalidArgument);
    exact[0] = 1;
    exact[RANDOM_STATE_PAYLOAD_BYTES - sizeof(uint64)] = 2; // even increment
    CHECK(TryDecodeRandomState(exact, decoded) == MathStatus::InvalidArgument);
    CHECK(decoded.State == sentinel.State);
    CHECK(decoded.Increment == sentinel.Increment);
}

TEST_CASE("A random checkpoint has a frozen byte layout with event and identity metadata", "[math][random][checkpoint]")
{
    const RandomCheckpoint checkpoint = MakeGoldenCheckpoint();
    usize measured = 0;
    REQUIRE(TryMeasureRandomCheckpoint(checkpoint, measured) == RandomCheckpointStatus::Success);
    REQUIRE(measured == GOLDEN_CHECKPOINT_BYTES);
    CheckpointBytes bytes{};
    usize written = 0;
    REQUIRE(TryWriteRandomCheckpoint(checkpoint, {bytes.data(), bytes.size()}, written) ==
            RandomCheckpointStatus::Success);
    CHECK(written == GOLDEN_CHECKPOINT_BYTES);
    CHECK(bytes == GOLDEN_CHECKPOINT);

    Storage storage;
    RandomCheckpointContents contents;
    REQUIRE(Read(GOLDEN_CHECKPOINT, storage, contents) == RandomCheckpointStatus::Success);
    CHECK(contents.RootSeed == ROOT_SEED);
    CHECK(contents.ContentIdentity == CONTENT_IDENTITY);
    CHECK(contents.ByteCount == GOLDEN_CHECKPOINT_BYTES);
    REQUIRE(contents.StreamCount == 1);
    REQUIRE(contents.EventCount == 1);
    REQUIRE(contents.IdentityCount == 1);
    CHECK(storage.Streams[0].Owner == GOLDEN_STREAM.Owner);
    CHECK(storage.Streams[0].State.State == GOLDEN_STREAM.State.State);
    CHECK(storage.Streams[0].State.Increment == GOLDEN_STREAM.State.Increment);
    CHECK(storage.Events[0].Owner == GOLDEN_EVENT.Owner);
    CHECK(storage.Events[0].NextEvent == GOLDEN_EVENT.NextEvent);
    CHECK(storage.Identities[0].Allocator == GOLDEN_IDENTITY.Allocator);
    CHECK(storage.Identities[0].NextIdentity == GOLDEN_IDENTITY.NextIdentity);
    // Records past the decoded counts are untouched.
    CHECK(storage.Streams[1].Owner.Domain == 99);
}

TEST_CASE("A random checkpoint embedded in a larger buffer reports its own length", "[math][random][checkpoint]")
{
    StaticArray<uint8, GOLDEN_CHECKPOINT_BYTES + 8> bytes{};
    bytes.Fill(0xee);
    usize written = 0;
    REQUIRE(TryWriteRandomCheckpoint(MakeGoldenCheckpoint(), {bytes.data(), bytes.size()}, written) ==
            RandomCheckpointStatus::Success);
    CHECK(bytes[GOLDEN_CHECKPOINT_BYTES] == 0xee); // trailing bytes untouched
    Storage storage;
    RandomCheckpointContents contents;
    REQUIRE(TryReadRandomCheckpoint({bytes.data(), bytes.size()}, CONTENT_IDENTITY, storage.GetView(), contents) ==
            RandomCheckpointStatus::Success);
    CHECK(contents.ByteCount == GOLDEN_CHECKPOINT_BYTES);
}

TEST_CASE("Reading rejects incompatible or corrupt checkpoints without writing anything", "[math][random][checkpoint]")
{
    SECTION("bad magic")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[0] = 0;
        RequireRejected(bytes, RandomCheckpointStatus::Malformed);
    }
    SECTION("unsupported format version")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[FORMAT_OFFSET] = 2;
        RequireRejected(bytes, RandomCheckpointStatus::UnsupportedFormat);
    }
    SECTION("unsupported setup contract")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[STREAM_SETUP_OFFSET] = 2;
        RequireRejected(bytes, RandomCheckpointStatus::UnsupportedContract);
    }
    SECTION("incompatible content identity")
    {
        Storage storage;
        RandomCheckpointContents contents = SENTINEL_CONTENTS;
        REQUIRE(TryReadRandomCheckpoint({GOLDEN_CHECKPOINT.data(), GOLDEN_CHECKPOINT.size()},
                                        CONTENT_IDENTITY + 1,
                                        storage.GetView(),
                                        contents) == RandomCheckpointStatus::IncompatibleContent);
        CHECK(storage.IsUntouched() == true);
        CHECK(contents.RootSeed == SENTINEL_CONTENTS.RootSeed);
    }
    SECTION("recorded length disagrees with the records")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[TOTAL_BYTES_OFFSET] = static_cast<uint8>(GOLDEN_CHECKPOINT_BYTES - 1);
        RequireRejected(bytes, RandomCheckpointStatus::Malformed);
    }
    SECTION("even stream increment")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[STREAM_INCREMENT_OFFSET] = 0x08;
        RequireRejected(bytes, RandomCheckpointStatus::InvalidRecord);
    }
    SECTION("wrong stream payload length")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[STREAM_PAYLOAD_LENGTH_OFFSET] = static_cast<uint8>(RANDOM_STATE_PAYLOAD_BYTES + 1);
        RequireRejected(bytes, RandomCheckpointStatus::InvalidRecord);
    }
    SECTION("zero stream domain")
    {
        CheckpointBytes bytes = GOLDEN_CHECKPOINT;
        bytes[STREAM_RECORD_OFFSET] = 0;
        RequireRejected(bytes, RandomCheckpointStatus::InvalidRecord);
    }
    SECTION("insufficient record storage")
    {
        Storage storage;
        RandomCheckpointStorage view = storage.GetView();
        view.Events = {};
        RandomCheckpointContents contents = SENTINEL_CONTENTS;
        REQUIRE(TryReadRandomCheckpoint({GOLDEN_CHECKPOINT.data(), GOLDEN_CHECKPOINT.size()},
                                        CONTENT_IDENTITY,
                                        view,
                                        contents) == RandomCheckpointStatus::CapacityExceeded);
        CHECK(storage.IsUntouched() == true);
    }
    SECTION("every truncation")
    {
        for (usize length = 0; length < GOLDEN_CHECKPOINT_BYTES; ++length)
        {
            Storage storage;
            RandomCheckpointContents contents = SENTINEL_CONTENTS;
            REQUIRE(TryReadRandomCheckpoint({GOLDEN_CHECKPOINT.data(), length},
                                            CONTENT_IDENTITY,
                                            storage.GetView(),
                                            contents) == RandomCheckpointStatus::Malformed);
            REQUIRE(storage.IsUntouched() == true);
        }
    }
}

TEST_CASE("Reading rejects duplicate owners of every record kind", "[math][random][checkpoint]")
{
    const StaticArray<RandomStreamRecord, 2> streams{{GOLDEN_STREAM, {{LOOT_DOMAIN, LOOT_SCOPE + 1}, {1, 5, 7}}}};
    const StaticArray<RandomEventRecord, 2> events{{GOLDEN_EVENT, {{LOOT_DOMAIN, 8}, 1}}};
    const StaticArray<RandomIdentityRecord, 2> identities{{GOLDEN_IDENTITY, {2, 1}}};
    const RandomCheckpoint checkpoint
    {
        .RootSeed = ROOT_SEED,
        .ContentIdentity = CONTENT_IDENTITY,
        .Streams = {streams.data(), streams.size()},
        .Events = {events.data(), events.size()},
        .Identities = {identities.data(), identities.size()},
    };
    StaticArray<uint8, GOLDEN_CHECKPOINT_BYTES * 2> bytes{};
    usize written = 0;
    REQUIRE(TryWriteRandomCheckpoint(checkpoint, {bytes.data(), bytes.size()}, written) ==
            RandomCheckpointStatus::Success);
    const usize eventOffset = STREAM_RECORD_OFFSET + 2 * STREAM_RECORD_BYTES;
    const usize identityOffset = eventOffset + 2 * EVENT_RECORD_BYTES;
    // Patch the second record of one table so that it names the first record's owner.
    const auto requireDuplicate = [&](usize offset) {
        StaticArray<uint8, GOLDEN_CHECKPOINT_BYTES * 2> patched = bytes;
        patched[offset] = static_cast<uint8>(patched[offset] - 1);
        Storage storage;
        RandomCheckpointContents contents = SENTINEL_CONTENTS;
        REQUIRE(TryReadRandomCheckpoint({patched.data(), written}, CONTENT_IDENTITY, storage.GetView(), contents) ==
                RandomCheckpointStatus::DuplicateOwner);
        REQUIRE(storage.IsUntouched() == true);
    };
    // Scope low bytes of the second stream and event records; the allocator ID of the second identity record.
    requireDuplicate(STREAM_RECORD_OFFSET + STREAM_RECORD_BYTES + sizeof(uint32));
    requireDuplicate(eventOffset + EVENT_RECORD_BYTES + sizeof(uint32));
    requireDuplicate(identityOffset + sizeof(uint32) + sizeof(uint64));
}

TEST_CASE("Writing validates records and never writes a partial checkpoint", "[math][random][checkpoint]")
{
    CheckpointBytes bytes{};
    bytes.Fill(0xcc);
    usize written = 7;
    const auto requireRejected = [&](const RandomCheckpoint& checkpoint, RandomCheckpointStatus expected) {
        REQUIRE(TryWriteRandomCheckpoint(checkpoint, {bytes.data(), bytes.size()}, written) == expected);
        REQUIRE(written == 7);
        for (const uint8 value : bytes)
        {
            REQUIRE(value == 0xcc);
        }
    };
    const StaticArray<RandomStreamRecord, 2> duplicateStreams{{GOLDEN_STREAM, GOLDEN_STREAM}};
    RandomCheckpoint checkpoint = MakeGoldenCheckpoint();
    checkpoint.Streams = {duplicateStreams.data(), duplicateStreams.size()};
    requireRejected(checkpoint, RandomCheckpointStatus::DuplicateOwner);

    const RandomStreamRecord evenIncrement{{LOOT_DOMAIN, 1}, {1, 1, 2}};
    checkpoint = MakeGoldenCheckpoint();
    checkpoint.Streams = {&evenIncrement, 1};
    requireRejected(checkpoint, RandomCheckpointStatus::InvalidRecord);

    const RandomEventRecord zeroDomain{{0, 1}, 1};
    checkpoint = MakeGoldenCheckpoint();
    checkpoint.Events = {&zeroDomain, 1};
    requireRejected(checkpoint, RandomCheckpointStatus::InvalidRecord);

    const RandomIdentityRecord zeroAllocator{0, 1};
    checkpoint = MakeGoldenCheckpoint();
    checkpoint.Identities = {&zeroAllocator, 1};
    requireRejected(checkpoint, RandomCheckpointStatus::InvalidRecord);

    REQUIRE(TryWriteRandomCheckpoint(MakeGoldenCheckpoint(), {bytes.data(), GOLDEN_CHECKPOINT_BYTES - 1}, written) ==
            RandomCheckpointStatus::CapacityExceeded);
    REQUIRE(written == 7);
    CHECK(bytes[0] == 0xcc);
}

TEST_CASE("Stream restore is all or nothing", "[math][random][checkpoint]")
{
    const StaticArray<RandomOwner, 2> owners{{{LOOT_DOMAIN, 1}, {LOOT_DOMAIN, 2}}};
    StaticArray<RandomStream, 2> streams{};
    for (usize i = 0; i < owners.size(); ++i)
    {
        REQUIRE(TryMakeRandomStream(ROOT_SEED, owners[i], streams[i]) == MathStatus::Success);
    }
    const StaticArray<RandomState, 2> before{{streams[0].GetState(), streams[1].GetState()}};
    const auto requireUnchanged = [&]() {
        for (usize i = 0; i < streams.size(); ++i)
        {
            REQUIRE(streams[i].GetState().State == before[i].State);
            REQUIRE(streams[i].GetState().Increment == before[i].Increment);
        }
    };
    const std::span<const RandomOwner> ownerView(owners.data(), owners.size());
    const std::span<RandomStream> streamView(streams.data(), streams.size());

    // Records in a different order from the owners still match by identity.
    const StaticArray<RandomStreamRecord, 2> records{{{owners[1], {1, 11, 13}}, {owners[0], {1, 21, 23}}}};
    const StaticArray<RandomStreamRecord, 2> unknownOwner{{records[0], {{LOOT_DOMAIN, 3}, {1, 21, 23}}}};
    const StaticArray<RandomStreamRecord, 2> invalidState{{records[0], {owners[0], {1, 21, 22}}}};
    const StaticArray<RandomStreamRecord, 2> duplicate{{records[0], records[0]}};
    CHECK(TryRestoreRandomStreams({records.data(), 1}, ownerView, streamView) == RandomCheckpointStatus::OwnerMismatch);
    requireUnchanged();
    CHECK(TryRestoreRandomStreams({unknownOwner.data(), unknownOwner.size()}, ownerView, streamView) ==
          RandomCheckpointStatus::OwnerMismatch);
    requireUnchanged();
    CHECK(TryRestoreRandomStreams({invalidState.data(), invalidState.size()}, ownerView, streamView) ==
          RandomCheckpointStatus::InvalidRecord);
    requireUnchanged();
    CHECK(TryRestoreRandomStreams({duplicate.data(), duplicate.size()}, ownerView, streamView) ==
          RandomCheckpointStatus::DuplicateOwner);
    requireUnchanged();

    REQUIRE(TryRestoreRandomStreams({records.data(), records.size()}, ownerView, streamView) ==
            RandomCheckpointStatus::Success);
    CHECK(streams[0].GetState().State == 21);
    CHECK(streams[1].GetState().State == 11);
}

TEST_CASE("A checkpoint replays streams, event counters and identity allocators across a reload",
          "[math][random][checkpoint]")
{
    // A world with two loot streams, one event counter and one identity allocator, part way through a session.
    const StaticArray<RandomOwner, 2> owners{{{LOOT_DOMAIN, 10}, {LOOT_DOMAIN + 1, 10}}};
    StaticArray<RandomStream, 2> live{};
    for (usize i = 0; i < owners.size(); ++i)
    {
        REQUIRE(TryMakeRandomStream(ROOT_SEED, owners[i], live[i]) == MathStatus::Success);
        for (usize draw = 0; draw <= i * 3; ++draw)
        {
            static_cast<void>(live[i].NextUInt32());
        }
    }
    const RandomEventRecord event{{LOOT_DOMAIN, 10}, 17};
    const RandomIdentityRecord identity{1, 1234};

    const StaticArray<RandomStreamRecord, 2> saved{{{owners[0], live[0].GetState()}, {owners[1], live[1].GetState()}}};
    const RandomCheckpoint checkpoint
    {
        .RootSeed = ROOT_SEED,
        .ContentIdentity = CONTENT_IDENTITY,
        .Streams = {saved.data(), saved.size()},
        .Events = {&event, 1},
        .Identities = {&identity, 1},
    };
    StaticArray<uint8, GOLDEN_CHECKPOINT_BYTES * 2> bytes{};
    usize written = 0;
    REQUIRE(TryWriteRandomCheckpoint(checkpoint, {bytes.data(), bytes.size()}, written) ==
            RandomCheckpointStatus::Success);

    // The uninterrupted session continues.
    StaticArray<uint32, REPLAY_DRAWS> expected{};
    for (usize i = 0; i < REPLAY_DRAWS; ++i)
    {
        expected[i] = live[i % 2].NextUInt32();
    }

    // A reloaded world sets its streams up fresh, then restores the checkpoint over them.
    Storage storage;
    RandomCheckpointContents contents;
    REQUIRE(TryReadRandomCheckpoint({bytes.data(), written}, CONTENT_IDENTITY, storage.GetView(), contents) ==
            RandomCheckpointStatus::Success);
    StaticArray<RandomStream, 2> reloaded{};
    for (usize i = 0; i < owners.size(); ++i)
    {
        REQUIRE(TryMakeRandomStream(contents.RootSeed, owners[i], reloaded[i]) == MathStatus::Success);
    }
    REQUIRE(TryRestoreRandomStreams({storage.Streams.data(), contents.StreamCount},
                                    {owners.data(), owners.size()},
                                    {reloaded.data(), reloaded.size()}) == RandomCheckpointStatus::Success);
    for (usize i = 0; i < REPLAY_DRAWS; ++i)
    {
        REQUIRE(reloaded[i % 2].NextUInt32() == expected[i]);
    }
    REQUIRE(contents.EventCount == 1);
    CHECK(storage.Events[0].Owner == event.Owner);
    CHECK(storage.Events[0].NextEvent == event.NextEvent);
    REQUIRE(contents.IdentityCount == 1);
    CHECK(storage.Identities[0].NextIdentity == identity.NextIdentity);
}

TEST_CASE("Checkpoint status names are stable", "[math][random][checkpoint]")
{
    CHECK(std::string_view(GetRandomCheckpointStatusName(RandomCheckpointStatus::Success)) == "Success");
    CHECK(std::string_view(GetRandomCheckpointStatusName(RandomCheckpointStatus::IncompatibleContent)) ==
          "IncompatibleContent");
    CHECK(std::string_view(GetRandomCheckpointStatusName(RandomCheckpointStatus::OwnerMismatch)) == "OwnerMismatch");
}
