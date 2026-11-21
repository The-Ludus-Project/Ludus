// Random checkpoint wire format v1. Every field is little-endian and written field by field:
//
//   header   magic, format version, total bytes                       3 x uint32
//            key derivation, address layout, bounded mapping,
//            stream setup, stream state versions                      5 x uint32
//            content identity, root seed                              2 x uint64
//            stream, event and identity record counts                 3 x uint32
//   streams  domain uint32, scope uint64, payload bytes uint32, 20-byte RandomState payload
//   events   domain uint32, scope uint64, next event uint32
//   ids      allocator uint32, next identity uint64
//
// Contract and ownership rules: docs/architecture/randomness.md#replay-persistence-networking-and-entropy.

#include <ludus/foundation/math/random_checkpoint.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::foundation::math
{
namespace
{
constexpr usize HEADER_BYTES = RANDOM_CHECKPOINT_HEADER_BYTES;
constexpr usize STREAM_RECORD_BYTES = RANDOM_CHECKPOINT_STREAM_RECORD_BYTES;
constexpr usize EVENT_RECORD_BYTES = RANDOM_CHECKPOINT_EVENT_RECORD_BYTES;
constexpr usize IDENTITY_RECORD_BYTES = RANDOM_CHECKPOINT_IDENTITY_RECORD_BYTES;

/// Sequential little-endian reader over a bounded byte range; a failed read consumes nothing.
class ByteReader final
{
public:
    explicit ByteReader(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}

    template <core::UnsignedInteger ValueType>
    [[nodiscard]] bool TryRead(ValueType& outValue) noexcept
    {
        if (core::TryReadLittleEndian({mBytes.data() + mOffset, mBytes.size() - mOffset}, outValue) == false)
        {
            return false;
        }
        mOffset += sizeof(ValueType);
        return true;
    }

    [[nodiscard]] bool TryReadBytes(usize count, std::span<const uint8>& outBytes) noexcept
    {
        if (mBytes.size() - mOffset < count)
        {
            return false;
        }
        outBytes = mBytes.subspan(mOffset, count);
        mOffset += count;
        return true;
    }

private:
    std::span<const uint8> mBytes;
    usize mOffset = 0;
};

/// Sequential little-endian writer; the caller sizes the destination before writing.
class ByteWriter final
{
public:
    explicit ByteWriter(std::span<uint8> bytes) noexcept : mBytes(bytes) {}

    template <core::UnsignedInteger ValueType>
    void Write(ValueType value) noexcept
    {
        const bool isWritten = core::TryWriteLittleEndian(value, {mBytes.data() + mOffset, mBytes.size() - mOffset});
        LUDUS_ASSERT(isWritten == true);
        static_cast<void>(isWritten);
        mOffset += sizeof(ValueType);
    }

    [[nodiscard]] std::span<uint8> Take(usize count) noexcept
    {
        LUDUS_ASSERT(mBytes.size() - mOffset >= count);
        const std::span<uint8> taken = mBytes.subspan(mOffset, count);
        mOffset += count;
        return taken;
    }

    [[nodiscard]] usize GetOffset() const noexcept
    {
        return mOffset;
    }

private:
    std::span<uint8> mBytes;
    usize mOffset = 0;
};

[[nodiscard]] bool IsSupportedState(const RandomState& state) noexcept
{
    return state.Version == RANDOM_STREAM_STATE_VERSION && (state.Increment & 1u) != 0u;
}

/// Encoded size of a checkpoint with these counts, or false when it does not fit the 32-bit format fields.
[[nodiscard]] bool
TryComputeCheckpointBytes(usize streamCount, usize eventCount, usize identityCount, usize& outBytes) noexcept
{
    usize streamBytes = 0;
    usize eventBytes = 0;
    usize identityBytes = 0;
    usize total = HEADER_BYTES;
    uint32 narrowed = 0;
    const bool isRepresentable =
        core::TryMultiply(streamCount, STREAM_RECORD_BYTES, streamBytes) == true &&
        core::TryMultiply(eventCount, EVENT_RECORD_BYTES, eventBytes) == true &&
        core::TryMultiply(identityCount, IDENTITY_RECORD_BYTES, identityBytes) == true &&
        core::TryAdd(total, streamBytes, total) == true && core::TryAdd(total, eventBytes, total) == true &&
        core::TryAdd(total, identityBytes, total) == true && core::TryIntegerCast(total, narrowed) == true;
    if (isRepresentable == false)
    {
        return false;
    }
    outBytes = total;
    return true;
}

[[nodiscard]] RandomCheckpointStatus ValidateCheckpoint(const RandomCheckpoint& checkpoint) noexcept
{
    for (usize i = 0; i < checkpoint.Streams.size(); ++i)
    {
        const RandomStreamRecord& record = checkpoint.Streams[i];
        if (record.Owner.Domain == 0u || IsSupportedState(record.State) == false)
        {
            return RandomCheckpointStatus::InvalidRecord;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (checkpoint.Streams[j].Owner == record.Owner)
            {
                return RandomCheckpointStatus::DuplicateOwner;
            }
        }
    }
    for (usize i = 0; i < checkpoint.Events.size(); ++i)
    {
        if (checkpoint.Events[i].Owner.Domain == 0u)
        {
            return RandomCheckpointStatus::InvalidRecord;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (checkpoint.Events[j].Owner == checkpoint.Events[i].Owner)
            {
                return RandomCheckpointStatus::DuplicateOwner;
            }
        }
    }
    for (usize i = 0; i < checkpoint.Identities.size(); ++i)
    {
        if (checkpoint.Identities[i].Allocator == 0u)
        {
            return RandomCheckpointStatus::InvalidRecord;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (checkpoint.Identities[j].Allocator == checkpoint.Identities[i].Allocator)
            {
                return RandomCheckpointStatus::DuplicateOwner;
            }
        }
    }
    return RandomCheckpointStatus::Success;
}

/// The bytes of one table of fixed-width records inside a checkpoint whose length has been validated.
struct RecordTable final
{
    std::span<const uint8> Bytes;
    usize RecordBytes = 1;

    [[nodiscard]] usize GetCount() const noexcept
    {
        return Bytes.size() / RecordBytes;
    }
    [[nodiscard]] std::span<const uint8> GetRecord(usize index) const noexcept
    {
        return Bytes.subspan(index * RecordBytes, RecordBytes);
    }
};

[[nodiscard]] RandomCheckpointStatus DecodeStreamRecord(std::span<const uint8> bytes,
                                                        RandomStreamRecord& outRecord) noexcept
{
    ByteReader reader(bytes);
    RandomStreamRecord record;
    uint32 payloadBytes = 0;
    std::span<const uint8> payload;
    if (reader.TryRead(record.Owner.Domain) == false || reader.TryRead(record.Owner.Scope) == false ||
        reader.TryRead(payloadBytes) == false || payloadBytes != RANDOM_STATE_PAYLOAD_BYTES ||
        reader.TryReadBytes(RANDOM_STATE_PAYLOAD_BYTES, payload) == false ||
        TryDecodeRandomState(payload, record.State) != MathStatus::Success || record.Owner.Domain == 0u)
    {
        return RandomCheckpointStatus::InvalidRecord;
    }
    outRecord = record;
    return RandomCheckpointStatus::Success;
}

[[nodiscard]] RandomCheckpointStatus DecodeEventRecord(std::span<const uint8> bytes,
                                                       RandomEventRecord& outRecord) noexcept
{
    ByteReader reader(bytes);
    RandomEventRecord record;
    if (reader.TryRead(record.Owner.Domain) == false || reader.TryRead(record.Owner.Scope) == false ||
        reader.TryRead(record.NextEvent) == false || record.Owner.Domain == 0u)
    {
        return RandomCheckpointStatus::InvalidRecord;
    }
    outRecord = record;
    return RandomCheckpointStatus::Success;
}

[[nodiscard]] RandomCheckpointStatus DecodeIdentityRecord(std::span<const uint8> bytes,
                                                          RandomIdentityRecord& outRecord) noexcept
{
    ByteReader reader(bytes);
    RandomIdentityRecord record;
    if (reader.TryRead(record.Allocator) == false || reader.TryRead(record.NextIdentity) == false ||
        record.Allocator == 0u)
    {
        return RandomCheckpointStatus::InvalidRecord;
    }
    outRecord = record;
    return RandomCheckpointStatus::Success;
}

/// Decode every record of one table, rejecting invalid records and repeated keys; writes nothing.
template <typename Record, typename Decode, typename IsSameOwner>
[[nodiscard]] RandomCheckpointStatus ValidateTable(RecordTable table, Decode decode, IsSameOwner isSameOwner) noexcept
{
    for (usize i = 0; i < table.GetCount(); ++i)
    {
        Record record;
        const RandomCheckpointStatus status = decode(table.GetRecord(i), record);
        if (status != RandomCheckpointStatus::Success)
        {
            return status;
        }
        for (usize j = 0; j < i; ++j)
        {
            Record earlier;
            // An earlier record already decoded successfully in this loop.
            const RandomCheckpointStatus earlierStatus = decode(table.GetRecord(j), earlier);
            LUDUS_ASSERT(earlierStatus == RandomCheckpointStatus::Success);
            static_cast<void>(earlierStatus);
            if (isSameOwner(earlier, record) == true)
            {
                return RandomCheckpointStatus::DuplicateOwner;
            }
        }
    }
    return RandomCheckpointStatus::Success;
}

/// Decode one validated table into storage; validation guarantees every record decodes.
template <typename Record, typename Decode>
void DecodeTable(RecordTable table, std::span<Record> destination, Decode decode) noexcept
{
    LUDUS_ASSERT(destination.size() == table.GetCount());
    for (usize i = 0; i < destination.size(); ++i)
    {
        const RandomCheckpointStatus status = decode(table.GetRecord(i), destination[i]);
        LUDUS_ASSERT(status == RandomCheckpointStatus::Success);
        static_cast<void>(status);
    }
}
} // namespace

MathStatus TryEncodeRandomState(const RandomState& state, std::span<uint8> destination) noexcept
{
    if (IsSupportedState(state) == false)
    {
        return MathStatus::InvalidArgument;
    }
    if (destination.size() != RANDOM_STATE_PAYLOAD_BYTES)
    {
        return MathStatus::SizeMismatch;
    }
    ByteWriter writer(destination);
    writer.Write(state.Version);
    writer.Write(state.State);
    writer.Write(state.Increment);
    return MathStatus::Success;
}

MathStatus TryDecodeRandomState(std::span<const uint8> source, RandomState& outState) noexcept
{
    if (source.size() != RANDOM_STATE_PAYLOAD_BYTES)
    {
        return MathStatus::SizeMismatch;
    }
    ByteReader reader(source);
    RandomState state;
    if (reader.TryRead(state.Version) == false || reader.TryRead(state.State) == false ||
        reader.TryRead(state.Increment) == false)
    {
        return MathStatus::SizeMismatch;
    }
    if (IsSupportedState(state) == false)
    {
        return MathStatus::InvalidArgument;
    }
    outState = state;
    return MathStatus::Success;
}

const char* GetRandomCheckpointStatusName(RandomCheckpointStatus status) noexcept
{
    switch (status)
    {
        case RandomCheckpointStatus::Success:
            return "Success";
        case RandomCheckpointStatus::Malformed:
            return "Malformed";
        case RandomCheckpointStatus::UnsupportedFormat:
            return "UnsupportedFormat";
        case RandomCheckpointStatus::UnsupportedContract:
            return "UnsupportedContract";
        case RandomCheckpointStatus::IncompatibleContent:
            return "IncompatibleContent";
        case RandomCheckpointStatus::InvalidRecord:
            return "InvalidRecord";
        case RandomCheckpointStatus::DuplicateOwner:
            return "DuplicateOwner";
        case RandomCheckpointStatus::CapacityExceeded:
            return "CapacityExceeded";
        case RandomCheckpointStatus::OwnerMismatch:
            return "OwnerMismatch";
    }
    return "Unknown";
}

RandomCheckpointStatus TryMeasureRandomCheckpoint(const RandomCheckpoint& checkpoint, usize& outBytes) noexcept
{
    const RandomCheckpointStatus status = ValidateCheckpoint(checkpoint);
    if (status != RandomCheckpointStatus::Success)
    {
        return status;
    }
    usize bytes = 0;
    if (TryComputeCheckpointBytes(checkpoint.Streams.size(),
                                  checkpoint.Events.size(),
                                  checkpoint.Identities.size(),
                                  bytes) == false)
    {
        return RandomCheckpointStatus::CapacityExceeded;
    }
    outBytes = bytes;
    return RandomCheckpointStatus::Success;
}

RandomCheckpointStatus
TryWriteRandomCheckpoint(const RandomCheckpoint& checkpoint, std::span<uint8> destination, usize& outWritten) noexcept
{
    usize bytes = 0;
    const RandomCheckpointStatus status = TryMeasureRandomCheckpoint(checkpoint, bytes);
    if (status != RandomCheckpointStatus::Success)
    {
        return status;
    }
    if (destination.size() < bytes)
    {
        return RandomCheckpointStatus::CapacityExceeded;
    }
    // Measurement proved that the total and every count fit their uint32 fields.
    const RandomContract contract = GetRandomContract();
    ByteWriter writer(destination.first(bytes));
    writer.Write(RANDOM_CHECKPOINT_MAGIC);
    writer.Write(RANDOM_CHECKPOINT_FORMAT_VERSION);
    writer.Write(static_cast<uint32>(bytes));
    writer.Write(contract.KeyDerivation);
    writer.Write(contract.AddressLayout);
    writer.Write(contract.BoundedMapping);
    writer.Write(contract.StreamSetup);
    writer.Write(contract.StreamState);
    writer.Write(checkpoint.ContentIdentity);
    writer.Write(checkpoint.RootSeed);
    writer.Write(static_cast<uint32>(checkpoint.Streams.size()));
    writer.Write(static_cast<uint32>(checkpoint.Events.size()));
    writer.Write(static_cast<uint32>(checkpoint.Identities.size()));
    for (const RandomStreamRecord& record : checkpoint.Streams)
    {
        writer.Write(record.Owner.Domain);
        writer.Write(record.Owner.Scope);
        writer.Write(static_cast<uint32>(RANDOM_STATE_PAYLOAD_BYTES));
        const MathStatus encoded = TryEncodeRandomState(record.State, writer.Take(RANDOM_STATE_PAYLOAD_BYTES));
        LUDUS_ASSERT(encoded == MathStatus::Success);
        static_cast<void>(encoded);
    }
    for (const RandomEventRecord& record : checkpoint.Events)
    {
        writer.Write(record.Owner.Domain);
        writer.Write(record.Owner.Scope);
        writer.Write(record.NextEvent);
    }
    for (const RandomIdentityRecord& record : checkpoint.Identities)
    {
        writer.Write(record.Allocator);
        writer.Write(record.NextIdentity);
    }
    LUDUS_ASSERT(writer.GetOffset() == bytes);
    outWritten = bytes;
    return RandomCheckpointStatus::Success;
}

RandomCheckpointStatus TryReadRandomCheckpoint(std::span<const uint8> source,
                                               uint64 expectedContentIdentity,
                                               const RandomCheckpointStorage& storage,
                                               RandomCheckpointContents& outContents) noexcept
{
    ByteReader reader(source);
    uint32 magic = 0;
    uint32 format = 0;
    uint32 totalBytes = 0;
    if (reader.TryRead(magic) == false || magic != RANDOM_CHECKPOINT_MAGIC)
    {
        return RandomCheckpointStatus::Malformed;
    }
    if (reader.TryRead(format) == false)
    {
        return RandomCheckpointStatus::Malformed;
    }
    if (format != RANDOM_CHECKPOINT_FORMAT_VERSION)
    {
        return RandomCheckpointStatus::UnsupportedFormat;
    }
    RandomContract contract;
    RandomCheckpointContents contents;
    uint32 streamCount = 0;
    uint32 eventCount = 0;
    uint32 identityCount = 0;
    if (reader.TryRead(totalBytes) == false || totalBytes < HEADER_BYTES || totalBytes > source.size() ||
        reader.TryRead(contract.KeyDerivation) == false || reader.TryRead(contract.AddressLayout) == false ||
        reader.TryRead(contract.BoundedMapping) == false || reader.TryRead(contract.StreamSetup) == false ||
        reader.TryRead(contract.StreamState) == false || reader.TryRead(contents.ContentIdentity) == false ||
        reader.TryRead(contents.RootSeed) == false || reader.TryRead(streamCount) == false ||
        reader.TryRead(eventCount) == false || reader.TryRead(identityCount) == false)
    {
        return RandomCheckpointStatus::Malformed;
    }
    usize expectedBytes = 0;
    if (TryComputeCheckpointBytes(streamCount, eventCount, identityCount, expectedBytes) == false ||
        expectedBytes != totalBytes)
    {
        return RandomCheckpointStatus::Malformed;
    }
    if (contract != GetRandomContract())
    {
        return RandomCheckpointStatus::UnsupportedContract;
    }
    if (contents.ContentIdentity != expectedContentIdentity)
    {
        return RandomCheckpointStatus::IncompatibleContent;
    }
    if (streamCount > storage.Streams.size() || eventCount > storage.Events.size() ||
        identityCount > storage.Identities.size())
    {
        return RandomCheckpointStatus::CapacityExceeded;
    }
    const std::span<const uint8> checkpoint = source.first(totalBytes);
    const RecordTable streams
    {
        .Bytes = checkpoint.subspan(HEADER_BYTES, streamCount * STREAM_RECORD_BYTES),
        .RecordBytes = STREAM_RECORD_BYTES,
    };
    const RecordTable events
    {
        .Bytes = checkpoint.subspan(HEADER_BYTES + streams.Bytes.size(), eventCount * EVENT_RECORD_BYTES),
        .RecordBytes = EVENT_RECORD_BYTES,
    };
    const RecordTable identities
    {
        .Bytes = checkpoint.subspan(HEADER_BYTES + streams.Bytes.size() + events.Bytes.size()),
        .RecordBytes = IDENTITY_RECORD_BYTES,
    };
    RandomCheckpointStatus status =
        ValidateTable<RandomStreamRecord>(streams,
                                          DecodeStreamRecord,
                                          [](const RandomStreamRecord& left, const RandomStreamRecord& right) noexcept {
                                              return left.Owner == right.Owner;
                                          });
    if (status == RandomCheckpointStatus::Success)
    {
        status = ValidateTable<RandomEventRecord>(
            events,
            DecodeEventRecord,
            [](const RandomEventRecord& left, const RandomEventRecord& right) noexcept {
                return left.Owner == right.Owner;
            });
    }
    if (status == RandomCheckpointStatus::Success)
    {
        status = ValidateTable<RandomIdentityRecord>(
            identities,
            DecodeIdentityRecord,
            [](const RandomIdentityRecord& left, const RandomIdentityRecord& right) noexcept {
                return left.Allocator == right.Allocator;
            });
    }
    if (status != RandomCheckpointStatus::Success)
    {
        return status;
    }
    // Everything is validated: publish the records and the header together.
    DecodeTable(streams, storage.Streams.first(streamCount), DecodeStreamRecord);
    DecodeTable(events, storage.Events.first(eventCount), DecodeEventRecord);
    DecodeTable(identities, storage.Identities.first(identityCount), DecodeIdentityRecord);
    contents.StreamCount = streamCount;
    contents.EventCount = eventCount;
    contents.IdentityCount = identityCount;
    contents.ByteCount = totalBytes;
    outContents = contents;
    return RandomCheckpointStatus::Success;
}

RandomCheckpointStatus TryRestoreRandomStreams(std::span<const RandomStreamRecord> records,
                                               std::span<const RandomOwner> owners,
                                               std::span<RandomStream> streams) noexcept
{
    if (records.size() != owners.size() || owners.size() != streams.size())
    {
        return RandomCheckpointStatus::OwnerMismatch;
    }
    for (usize i = 0; i < owners.size(); ++i)
    {
        if (IsSupportedState(records[i].State) == false)
        {
            return RandomCheckpointStatus::InvalidRecord;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (owners[j] == owners[i] || records[j].Owner == records[i].Owner)
            {
                return RandomCheckpointStatus::DuplicateOwner;
            }
        }
    }
    // With equal counts and unique keys on both sides, every owner having a record makes the match one to one.
    for (const RandomOwner& owner : owners)
    {
        bool isMatched = false;
        for (const RandomStreamRecord& record : records)
        {
            isMatched = isMatched == true || record.Owner == owner;
        }
        if (isMatched == false)
        {
            return RandomCheckpointStatus::OwnerMismatch;
        }
    }
    for (usize i = 0; i < owners.size(); ++i)
    {
        for (const RandomStreamRecord& record : records)
        {
            if (record.Owner == owners[i])
            {
                const MathStatus restored = streams[i].TryRestore(record.State);
                LUDUS_ASSERT(restored == MathStatus::Success);
                static_cast<void>(restored);
            }
        }
    }
    return RandomCheckpointStatus::Success;
}
} // namespace ludus::foundation::math
