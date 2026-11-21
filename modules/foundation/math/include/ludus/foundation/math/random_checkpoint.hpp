#pragma once

// Explicit wire format for sequential random state and replay compatibility metadata.
//
// A random checkpoint records the root seed, the RNG contract versions, an owner-supplied content identity and,
// for each persistent owner, its PCG stream state, its addressed event counter or its identity allocator. Every
// field is written field by field in little-endian order; no structure is copied to or from bytes. Reading
// validates the whole checkpoint before writing any output, and TryRestoreRandomStreams commits a validated set of
// stream records to their live owners all at once. Ownership, compatibility and rollback rules:
// docs/architecture/randomness.md#replay-persistence-networking-and-entropy.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/status.hpp>

#include <span>

namespace ludus::foundation::math
{
using core::uint8;
using core::usize;

/// @brief Bytes of one encoded RandomState: Version (uint32), State (uint64), Increment (uint64), little-endian.
inline constexpr usize RANDOM_STATE_PAYLOAD_BYTES = sizeof(uint32) + sizeof(uint64) + sizeof(uint64);

/// @brief Encode a stream state as the explicit 20-byte little-endian PCG v1 payload.
/// @param state The state to encode; its Version must be RANDOM_STREAM_STATE_VERSION and its Increment odd.
/// @param destination Exactly RANDOM_STATE_PAYLOAD_BYTES bytes; written only on success.
/// @return Success; InvalidArgument for an unsupported version or an even increment; SizeMismatch when destination
///         is not exactly RANDOM_STATE_PAYLOAD_BYTES long.
[[nodiscard]] MathStatus TryEncodeRandomState(const RandomState& state, std::span<uint8> destination) noexcept;

/// @brief Decode the explicit 20-byte little-endian PCG v1 payload.
/// @param source Exactly RANDOM_STATE_PAYLOAD_BYTES bytes.
/// @param outState Receives the decoded state on success; unchanged on failure.
/// @return Success; InvalidArgument for an unsupported version or an even increment; SizeMismatch when source is
///         not exactly RANDOM_STATE_PAYLOAD_BYTES long.
[[nodiscard]] MathStatus TryDecodeRandomState(std::span<const uint8> source, RandomState& outState) noexcept;

/// @brief Leading word of a random checkpoint: the bytes `LRCP` in file order.
inline constexpr uint32 RANDOM_CHECKPOINT_MAGIC = 0x5043524cu;
/// @brief Version of the checkpoint layout described in docs/architecture/randomness.md.
inline constexpr uint32 RANDOM_CHECKPOINT_FORMAT_VERSION = 1;

/// @brief uint32 header fields: magic, format, total bytes, five contract versions and three record counts.
inline constexpr usize RANDOM_CHECKPOINT_HEADER_WORD_FIELDS = 11;
/// @brief uint64 header fields: content identity and root seed.
inline constexpr usize RANDOM_CHECKPOINT_HEADER_WIDE_FIELDS = 2;
/// @brief Encoded header bytes; the records follow it in stream, event, identity order.
inline constexpr usize RANDOM_CHECKPOINT_HEADER_BYTES =
    RANDOM_CHECKPOINT_HEADER_WORD_FIELDS * sizeof(uint32) + RANDOM_CHECKPOINT_HEADER_WIDE_FIELDS * sizeof(uint64);
/// @brief Encoded stream record bytes: domain uint32, scope uint64, payload length uint32 and the state payload.
inline constexpr usize RANDOM_CHECKPOINT_STREAM_RECORD_BYTES =
    sizeof(uint32) + sizeof(uint64) + sizeof(uint32) + RANDOM_STATE_PAYLOAD_BYTES;
/// @brief Encoded event record bytes: domain uint32, scope uint64 and next event uint32.
inline constexpr usize RANDOM_CHECKPOINT_EVENT_RECORD_BYTES = sizeof(uint32) + sizeof(uint64) + sizeof(uint32);
/// @brief Encoded identity record bytes: allocator uint32 and next identity uint64.
inline constexpr usize RANDOM_CHECKPOINT_IDENTITY_RECORD_BYTES = sizeof(uint32) + sizeof(uint64);

/// @brief The RNG algorithm and mapping versions a checkpoint depends on.
///
/// A reader accepts only the versions this build implements (GetRandomContract). The checkpoint's content identity
/// separately covers the owner's domain/dimension manifests, generation revisions and policy schemas.
struct RandomContract
{
    /// Domain-key derivation (TryMakeRandomKey).
    uint32 KeyDerivation = RandomKeyDerivationVersion;
    /// Addressed counter packing (RandomAddress).
    uint32 AddressLayout = RandomAddressLayoutVersion;
    /// Addressed bounded mapping (TrySampleBounded).
    uint32 BoundedMapping = RandomBoundedMappingVersion;
    /// Named sequential setup (TryMakeRandomStream).
    uint32 StreamSetup = RANDOM_STREAM_SETUP_VERSION;
    /// Sequential stream state (RandomState).
    uint32 StreamState = RANDOM_STREAM_STATE_VERSION;

    /// Memberwise equality.
    [[nodiscard]] constexpr bool operator==(const RandomContract&) const noexcept = default;
};

/// @brief The contract versions this build implements and writes.
[[nodiscard]] constexpr RandomContract GetRandomContract() noexcept
{
    return RandomContract{};
}

/// @brief Saved state of one sequential stream, keyed by its persistent owner.
struct RandomStreamRecord
{
    /// The stream's persistent owner; unique among a checkpoint's stream records.
    RandomOwner Owner;
    /// The stream's state, as RandomStream::GetState returns it.
    RandomState State;
};

/// @brief Next addressed event ordinal of one owner: the Event its next committed event will use.
///
/// The owner advances it exactly once per committed logical event (docs/architecture/randomness.md, addressed
/// randomness); a value at the end of the 32-bit event range stays there and the owner reports exhaustion.
struct RandomEventRecord
{
    /// The counter's persistent owner; unique among a checkpoint's event records.
    RandomOwner Owner;
    /// Next event ordinal.
    uint32 NextEvent = 0;
};

/// @brief Next identity of one stable identity allocator, so restored IDs are never reissued.
struct RandomIdentityRecord
{
    /// Nonzero owner-assigned allocator ID; unique among a checkpoint's identity records.
    uint32 Allocator = 0;
    /// Next identity the allocator issues.
    uint64 NextIdentity = 0;
};

/// @brief Borrowed contents to write; every span must stay valid for the call.
struct RandomCheckpoint
{
    /// The world's recorded root seed.
    uint64 RootSeed = 0;
    /// Owner-defined identity of the content, manifests and policy schemas the state belongs to.
    uint64 ContentIdentity = 0;
    /// Stream states in the owner's stable order.
    std::span<const RandomStreamRecord> Streams;
    /// Event counters in the owner's stable order.
    std::span<const RandomEventRecord> Events;
    /// Identity allocators in the owner's stable order.
    std::span<const RandomIdentityRecord> Identities;
};

/// @brief Outcome of writing, reading or restoring a random checkpoint. Every failure leaves all outputs unchanged.
enum class RandomCheckpointStatus : uint8
{
    /// The operation completed and its outputs are written.
    Success,
    /// The bytes are truncated, have the wrong magic or a length that disagrees with their records.
    Malformed,
    /// The checkpoint uses a layout version this build does not read.
    UnsupportedFormat,
    /// The checkpoint was written with RNG algorithm or mapping versions this build does not implement.
    UnsupportedContract,
    /// The checkpoint belongs to different content, manifests or policy schemas than the reader expects.
    IncompatibleContent,
    /// A record has a zero domain or allocator, an unsupported state version, an even increment or a payload of the
    /// wrong length.
    InvalidRecord,
    /// Two records of one kind name the same owner or allocator.
    DuplicateOwner,
    /// The destination buffer or record storage is too small, or a count exceeds the 32-bit format fields.
    CapacityExceeded,
    /// The records and the live owners do not correspond one to one.
    OwnerMismatch,
};

/// @brief Stable, human-readable name of `status`; never null.
[[nodiscard]] const char* GetRandomCheckpointStatusName(RandomCheckpointStatus status) noexcept;

/// @brief Validate `checkpoint` and compute its encoded size.
/// @param checkpoint The contents to measure.
/// @param outBytes Receives the encoded size in bytes on success; unchanged on failure.
/// @return Success, InvalidRecord, DuplicateOwner or CapacityExceeded.
[[nodiscard]] RandomCheckpointStatus TryMeasureRandomCheckpoint(const RandomCheckpoint& checkpoint,
                                                                usize& outBytes) noexcept;

/// @brief Validate and encode `checkpoint` with the current contract (GetRandomContract).
///
/// Duplicate detection is quadratic in the record count, which suits per-world checkpoints of a few hundred owners.
/// @param checkpoint The contents to write.
/// @param destination Receives the encoding in its first bytes; untouched on failure. Trailing bytes stay untouched.
/// @param outWritten Receives the number of bytes written on success; unchanged on failure.
/// @return Success, InvalidRecord, DuplicateOwner, or CapacityExceeded when destination is shorter than the
///         encoding or a count exceeds the format's 32-bit fields.
[[nodiscard]] RandomCheckpointStatus
TryWriteRandomCheckpoint(const RandomCheckpoint& checkpoint, std::span<uint8> destination, usize& outWritten) noexcept;

/// @brief Caller-owned storage that receives decoded records.
struct RandomCheckpointStorage
{
    /// Receives the stream records; must hold at least the checkpoint's stream count.
    std::span<RandomStreamRecord> Streams;
    /// Receives the event records; must hold at least the checkpoint's event count.
    std::span<RandomEventRecord> Events;
    /// Receives the identity records; must hold at least the checkpoint's identity count.
    std::span<RandomIdentityRecord> Identities;
};

/// @brief Header fields and record counts of a successfully read checkpoint.
struct RandomCheckpointContents
{
    /// The recorded root seed.
    uint64 RootSeed = 0;
    /// The recorded content identity, equal to the reader's expectation.
    uint64 ContentIdentity = 0;
    /// Stream records written to the front of RandomCheckpointStorage::Streams.
    usize StreamCount = 0;
    /// Event records written to the front of RandomCheckpointStorage::Events.
    usize EventCount = 0;
    /// Identity records written to the front of RandomCheckpointStorage::Identities.
    usize IdentityCount = 0;
    /// Bytes the checkpoint occupies at the start of the source; an enclosing format continues after them.
    usize ByteCount = 0;
};

/// @brief Validate and decode a checkpoint written by TryWriteRandomCheckpoint.
///
/// The whole checkpoint is validated (magic, format, contract, content identity, record lengths, states and owner
/// uniqueness) before any storage element or `outContents` is written, so a rejected checkpoint changes nothing.
/// @param source Bytes beginning with a checkpoint; bytes after its recorded length are ignored.
/// @param expectedContentIdentity The content identity of the world that will receive the state.
/// @param storage Storage for the decoded records; only the first counts of each span are written, on success.
/// @param outContents Receives the header fields and counts on success; unchanged on failure.
/// @return Success or the first rejection found: Malformed, UnsupportedFormat, UnsupportedContract,
///         IncompatibleContent, InvalidRecord, DuplicateOwner, or CapacityExceeded when storage is too small.
[[nodiscard]] RandomCheckpointStatus TryReadRandomCheckpoint(std::span<const uint8> source,
                                                             uint64 expectedContentIdentity,
                                                             const RandomCheckpointStorage& storage,
                                                             RandomCheckpointContents& outContents) noexcept;

/// @brief Restore every live stream from its record at once, or none of them.
///
/// `owners[i]` names the owner of `streams[i]`. Each owner must be unique and match exactly one record, and every
/// record must match an owner; then each stream is restored from its record. Any mismatch or invalid state leaves
/// every stream unchanged. Matching is quadratic in the record count.
/// @param records Decoded stream records, such as the front of RandomCheckpointStorage::Streams after a read.
/// @param owners The persistent owner of each live stream.
/// @param streams The live streams, parallel to `owners`; restored only on success.
/// @return Success; OwnerMismatch when the counts differ or an owner has no record; DuplicateOwner for a repeated
///         owner or record; InvalidRecord for a state RandomStream::TryRestore rejects.
[[nodiscard]] RandomCheckpointStatus TryRestoreRandomStreams(std::span<const RandomStreamRecord> records,
                                                             std::span<const RandomOwner> owners,
                                                             std::span<RandomStream> streams) noexcept;
} // namespace ludus::foundation::math
