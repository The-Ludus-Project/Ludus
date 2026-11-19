#pragma once

// Execution status, stage records and a bounded trace for procedural generation.
// The trace records which stages ran and which were served from the cache; it
// never changes a result, and saturation counts dropped records. Contract:
// docs/architecture/procedural-content-generation.md#authoring-debugging-and-external-tools.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural
{
/// @brief Outcome of a generation request, staging or publication.
enum class GenerationStatus : uint8
{
    /// The operation succeeded.
    Success,
    /// An input failed validation; nothing was generated or published.
    InvalidInput,
    /// A declared count or byte budget would be exceeded.
    CapacityExceeded,
    /// Storage could not be allocated.
    OutOfMemory,
    /// A hard constraint or author override is not satisfied by the candidate.
    ConstraintFailed,
    /// A bounded search or sampler exhausted its attempts.
    SearchExhausted,
    /// The candidate was staged against an accepted revision that has since changed.
    StaleCandidate,
    /// The candidate has no successful staged result to publish.
    NotReady,
};

/// @brief How a stage produced its output.
enum class GenerationStageOutcome : uint8
{
    /// The output came from a validated cache entry.
    CacheHit,
    /// The stage ran.
    Executed,
    /// The stage ran and failed; its partial output was discarded.
    Failed,
};

/// @brief One stage evaluation of one scope.
struct GenerationStageRecord final
{
    /// Owner-defined scope identifier, for example a room ordinal.
    uint32 Scope = 0;
    /// Persistent stage identifier within the scope's recipe.
    uint32 Stage = 0;
    /// Cache key of the stage product.
    GenerationDigest Key;
    /// Digest of the product's canonical bytes; zero when the stage failed.
    GenerationDigest Output;
    /// How the output was produced.
    GenerationStageOutcome Outcome = GenerationStageOutcome::Executed;
    /// The cache lookup result, which names the reason for every miss.
    GenerationCacheOutcome Cache = GenerationCacheOutcome::Absent;
    /// Whether an executed product was stored in the cache.
    bool IsStored = false;
};

/// @brief A bounded, append-only record of stage evaluations.
///
/// Records beyond the capacity, or that fail to allocate, are counted as dropped. Single-owner; not thread-safe.
class GenerationTrace final
{
public:
    /// Create an empty trace that holds at most `capacity` records.
    explicit GenerationTrace(usize capacity) noexcept;

    /// Append a record, or count it as dropped.
    void Record(const GenerationStageRecord& record) noexcept;
    /// Remove every record and reset the dropped count.
    void Clear() noexcept;

    /// The recorded evaluations in execution order; valid until the next Record or Clear.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const GenerationStageRecord> GetRecords() const noexcept
    {
        return {mRecords.GetData(), mRecords.GetSize()};
    }
    /// Records that did not fit.
    [[nodiscard]] LUDUS_INLINE usize GetDroppedCount() const noexcept
    {
        return mDroppedCount;
    }

private:
    foundation::Array<GenerationStageRecord> mRecords;
    usize mCapacity = 0;
    usize mDroppedCount = 0;
};
} // namespace ludus::content::procedural
