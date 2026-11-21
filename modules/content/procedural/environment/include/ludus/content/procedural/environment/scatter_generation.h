#pragma once

// Staged scatter generation: the first consumer of the procedural generation
// executor contracts of slice P1. Each room is an independently keyed scope and
// each of its layers a pure, cached stage product. A request stages a candidate
// manifest; the coordinator publishes it only when it succeeded, satisfies every
// author lock and was staged against the current accepted revision. Comparison
// explains each removed placement by its first responsible stage and input.
// Contracts: docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/content/procedural/core/generation_trace.h>
#include <ludus/content/procedural/environment/scatter.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural
{
/// Persistent operator ID of the scatter layer stage; part of every scatter cache key.
inline constexpr uint32 SCATTER_LAYER_OPERATOR = 1;
/// Schema of a cached scatter layer product: a count, then fixed-width placement records.
inline constexpr uint32 SCATTER_LAYER_PRODUCT_SCHEMA = 1;
/// Version of the cache key and manifest digest encodings below.
inline constexpr uint32 SCATTER_GENERATION_VERSION = 1;
/// Most scopes in one request.
inline constexpr usize MAX_SCATTER_GENERATION_SCOPES = 64;
/// Most author locks in one scope.
inline constexpr usize MAX_SCATTER_SCOPE_LOCKS = 256;
/// Most candidates one layer may evaluate; bounds the scratch a layer stage reserves.
inline constexpr usize MAX_SCATTER_LAYER_CANDIDATES = 1U << 20U;

/// @brief One independently keyed scope: a room's recipe and its author locks.
struct ScatterScope final
{
    /// The room's recipe; its Room identifies the scope. Borrows its arrays from the caller.
    ScatterRecipe Recipe;
    /// @brief Placements the author locked, at most MAX_SCATTER_SCOPE_LOCKS, each in this scope's room.
    ///
    /// A candidate satisfies a lock when it contains a placement with the same identity and exactly these values.
    /// Locks do not change generation; an unsatisfied lock is a conflict that blocks publication. Borrowed.
    foundation::BorrowedView<const ScatterPlacement> Locks;
};

/// @brief A generation request over one or more scopes.
struct ScatterGenerationInput final
{
    /// The session's recorded root seed.
    uint64 WorldSeed = 0;
    /// Domain IDs from the game's random domain manifest.
    ScatterDomains Domains;
    /// Scopes in strictly ascending room order, 1 through MAX_SCATTER_GENERATION_SCOPES. Borrowed.
    foundation::BorrowedView<const ScatterScope> Scopes;
};

/// @brief One layer product of a manifest.
struct ScatterManifestLayer final
{
    /// The scope's room.
    uint16 Room = 0;
    /// The layer's authored ID.
    uint8 Layer = 0;
    /// Cache key: digest of every input the product depends on.
    GenerationDigest Key;
    /// Digest of the product's canonical bytes.
    GenerationDigest Output;
    /// Index of the layer's first placement in ScatterManifest::Placements.
    usize FirstPlacement = 0;
    /// Number of the layer's placements.
    usize PlacementCount = 0;
};

/// @brief The generated placements of every requested scope, with their provenance.
struct ScatterManifest final
{
    /// Layer products in scope order, then recipe layer order.
    foundation::Array<ScatterManifestLayer> Layers;
    /// Each layer's placements in canonical order, concatenated in Layers order.
    foundation::Array<ScatterPlacement> Placements;
    /// Digest over the layer rooms, IDs, keys and output digests; equal digests mean equal manifests.
    GenerationDigest Digest;

    /// @brief The placement with an identity, or null when the manifest has none. O(layers + log placements).
    [[nodiscard]] const ScatterPlacement* FindPlacement(ScatterIdentity identity) const noexcept;
};

/// @brief An author lock that a candidate does not satisfy.
struct ScatterLockConflict final
{
    /// The locked placement.
    ScatterPlacement Lock;
    /// Whether the candidate has no placement with the lock's identity.
    bool IsMissing = false;
    /// Why the candidate lacks the identity, when IsMissing; otherwise accepted.
    ScatterDecision Decision;
    /// The candidate's placement with the lock's identity, when not IsMissing.
    ScatterPlacement Generated;
};

/// @brief A staged generation result awaiting publication or rejection.
struct ScatterCandidate final
{
    /// Success when the candidate may be published; otherwise why staging failed.
    GenerationStatus Status = GenerationStatus::NotReady;
    /// The accepted revision the candidate was staged against.
    uint64 BaseRevision = 0;
    /// The generated manifest; empty unless Status is Success or ConstraintFailed.
    ScatterManifest Manifest;
    /// Unsatisfied author locks; nonempty exactly when Status is ConstraintFailed.
    foundation::Array<ScatterLockConflict> Conflicts;
};

/// @brief How a placement differs between two manifests.
enum class ScatterChangeKind : uint8
{
    /// The accepted manifest has the identity and the candidate does not.
    Removed,
    /// The candidate has the identity and the accepted manifest does not.
    Added,
    /// Both have the identity with different values.
    Changed,
};

/// @brief One difference between an accepted manifest and a candidate, by persistent placement identity.
struct ScatterChange final
{
    /// The kind of difference.
    ScatterChangeKind Kind = ScatterChangeKind::Removed;
    /// The placement's persistent identity.
    ScatterIdentity Identity;
    /// For Removed, the first stage and the input or rule that excluded the placement; otherwise accepted.
    ScatterDecision Decision;
};

/// @brief Stages scatter candidates through a validated product cache and owns the accepted manifest.
///
/// The accepted manifest changes only through TryPublish. Staging never changes it, whatever the outcome. The
/// coordinator borrows the cache store, which must outlive it. Single-owner; not thread-safe.
class ScatterGenerationCoordinator final
{
public:
    /// Create a coordinator with an empty accepted manifest at revision zero.
    explicit ScatterGenerationCoordinator(GenerationCacheStore& inoutCacheStore) noexcept;

    /// @brief Generate a candidate for every requested scope.
    ///
    /// Each layer product is read from the cache when its key matches a validated entry and generated otherwise;
    /// a corrupt entry is discarded and rebuilt. A failure to store a product never changes the result. Every layer
    /// evaluation is recorded in the trace.
    /// @param input The request; validated by this call.
    /// @param inoutTrace Receives one record per evaluated layer.
    /// @param outCandidate Replaced on every call; its Status equals the return value.
    /// @return Success; InvalidInput for an invalid request; CapacityExceeded when a layer exceeds
    ///         MAX_SCATTER_LAYER_CANDIDATES; OutOfMemory; SearchExhausted when a bounded draw is exhausted; or
    ///         ConstraintFailed when the manifest was generated but does not satisfy every lock.
    [[nodiscard]] GenerationStatus
    TryStage(const ScatterGenerationInput& input, GenerationTrace& inoutTrace, ScatterCandidate& outCandidate) noexcept;

    /// @brief Make a successful candidate the accepted manifest and advance the revision.
    /// @param inoutCandidate Emptied and marked NotReady on success; unchanged on failure.
    /// @return Success; NotReady when the candidate did not stage successfully; or StaleCandidate when it was staged
    ///         against an older revision. The accepted manifest is unchanged on failure.
    [[nodiscard]] GenerationStatus TryPublish(ScatterCandidate& inoutCandidate) noexcept;

    /// Discard a candidate without changing the accepted manifest; it becomes empty and NotReady.
    void Reject(ScatterCandidate& inoutCandidate) noexcept;

    /// The accepted manifest; valid until the next successful TryPublish.
    [[nodiscard]] LUDUS_INLINE const ScatterManifest& GetAccepted() const noexcept
    {
        return mAccepted;
    }
    /// Revision of the accepted manifest; zero before the first publication.
    [[nodiscard]] LUDUS_INLINE uint64 GetAcceptedRevision() const noexcept
    {
        return mAcceptedRevision;
    }

private:
    GenerationCacheStore& mCacheStore;
    ScatterManifest mAccepted;
    uint64 mAcceptedRevision = 0;
};

/// @brief Compare an accepted manifest with a candidate by persistent placement identity.
///
/// Removed and Changed entries follow the accepted manifest's order, then Added entries follow the candidate's.
/// A removed placement is explained against the candidate's input: a missing scope or layer, or the kernel's first
/// responsible stage and input (TryExplainScatterCandidate).
/// @param accepted The accepted manifest.
/// @param candidate The candidate manifest, generated from candidateInput.
/// @param candidateInput The request that produced the candidate; validated by this call.
/// @param outChanges Replaced on success; unchanged on failure.
/// @return Success, InvalidInput, OutOfMemory or SearchExhausted.
[[nodiscard]] GenerationStatus TryCompareScatterManifests(const ScatterManifest& accepted,
                                                          const ScatterManifest& candidate,
                                                          const ScatterGenerationInput& candidateInput,
                                                          foundation::Array<ScatterChange>& outChanges) noexcept;
} // namespace ludus::content::procedural
