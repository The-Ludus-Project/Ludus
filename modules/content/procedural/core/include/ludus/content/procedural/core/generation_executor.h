#pragma once

// Demand-driven execution of a plan of pure, cached generation stages, shared by
// every procedural consumer (scatter layers, modular assembly). A request names one
// scope; the coordinator evaluates exactly that scope's stages and their upstream
// closure, reading each validated product from the cache or running its kernel, and
// leaves every other stage pending. A candidate publishes only when every stage is
// ready, it succeeded and it was begun against the current accepted revision.
// Contracts: docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

// Thanks to William J. Schroeder, Kenneth M. Martin and William E. Lorensen, "The
// Visualization Toolkit: An Object-Oriented Approach to 3D Graphics", 4th ed.,
// section 4.4 "Executing the Pipeline" and Figure 4-6,
// https://book.vtk.org/en/latest/VTKBook/04Chapter4.html#executing-the-pipeline.
// Their demand-driven execution, where a request for one output updates only the
// upstream filters it depends on and leaves other branches untouched, is adapted
// here per scope. Ludus decides what is out of date from content digests of stage
// parameters and input products rather than from modification times.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/content/procedural/core/generation_trace.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural
{
/// Version of the stage cache key encoding below; part of every stage key.
inline constexpr uint32 GENERATION_STAGE_KEY_VERSION = 1;
/// Version of the manifest digest encoding below.
inline constexpr uint32 GENERATION_MANIFEST_VERSION = 1;
/// Most stages in one plan.
inline constexpr usize MAX_GENERATION_PLAN_STAGES = 1024;
/// Most upstream inputs of one stage.
inline constexpr usize MAX_GENERATION_STAGE_INPUTS = 8;
/// Most canonical parameter bytes of one stage.
inline constexpr usize MAX_GENERATION_STAGE_PARAMETER_BYTES = usize{1} << 20U;

/// The products of a stage's inputs, in the stage's declared input order. Each view borrows a ready product.
using GenerationInputProducts = foundation::BorrowedView<const foundation::BorrowedView<const uint8>>;

struct GenerationStage;

/// @brief The pure function behind one or more stages of a plan, implemented by a domain.
///
/// A kernel derives its output only from the stage's parameter bytes and its input products, the exact inputs of
/// the stage's cache key, so a cached product can never disagree with what the kernel would generate. It reads no
/// files, clock, world state or global random stream. A kernel may keep reusable scratch storage; the coordinator
/// calls it from one thread at a time.
class GenerationStageKernel
{
public:
    /// Destroy the kernel.
    virtual ~GenerationStageKernel() = default;

    /// @brief Generate the stage's canonical product bytes.
    /// @param stage The stage being evaluated; its Parameters are the kernel's local inputs.
    /// @param inputs The stage's input products in declared order.
    /// @param outProduct Cleared, then receives the product; the coordinator discards it on failure.
    /// @return Success or the stage's failure; any failure leaves the candidate failed.
    [[nodiscard]] virtual GenerationStatus TryExecute(const GenerationStage& stage,
                                                      GenerationInputProducts inputs,
                                                      GenerationByteWriter& outProduct) noexcept = 0;

    /// @brief Check a cached product's content against the stage, after the cache verified its framing and digest.
    /// @return False rejects the entry as corrupt; the coordinator removes it and runs TryExecute instead.
    [[nodiscard]] virtual bool IsValidProduct(const GenerationStage& stage,
                                              GenerationInputProducts inputs,
                                              foundation::BorrowedView<const uint8> product) noexcept = 0;

protected:
    /// Construct the interface.
    GenerationStageKernel() noexcept = default;
    /// Implementations decide whether they are copyable.
    GenerationStageKernel(const GenerationStageKernel&) noexcept = default;
    /// Implementations decide whether they are copyable.
    GenerationStageKernel& operator=(const GenerationStageKernel&) noexcept = default;
};

/// @brief One pure stage of a plan.
///
/// The stage key is the digest of GENERATION_STAGE_KEY_VERSION, the operator and its version, the product schema,
/// the scope and stage IDs, the parameter bytes, and the scope, stage and output digest of each input product. An
/// upstream edit that reproduces the same product bytes therefore leaves every downstream key unchanged.
struct GenerationStage final
{
    /// Owner-defined scope ID, for example a room ordinal or a shared scope; requests select stages by scope.
    uint32 Scope = 0;
    /// Persistent stage ID, unique within the scope.
    uint32 Stage = 0;
    /// Persistent operator ID of the kernel's function; never reused for a different function.
    uint32 Operator = 0;
    /// Version of the operator's algorithm and parameter encoding.
    uint32 OperatorVersion = 0;
    /// Schema of the product bytes, checked on every cache hit.
    uint32 Schema = 0;
    /// Canonical little-endian bytes of every local input, at most MAX_GENERATION_STAGE_PARAMETER_BYTES.
    foundation::Array<uint8> Parameters;
    /// Plan indices of the upstream stages whose products this stage reads, each lower than this stage's index.
    foundation::Array<usize> Inputs;
    /// The kernel; borrowed and must outlive every request using the plan. A null kernel makes the plan invalid.
    GenerationStageKernel* KernelOrNull = nullptr;
};

/// @brief Whether a candidate holds a stage's product.
enum class GenerationProductState : uint8
{
    /// The stage has not been requested since the candidate began.
    Pending,
    /// The product was read from the cache or generated.
    Ready,
    /// The stage ran and failed.
    Failed,
};

/// @brief A stage product held by a candidate or an accepted manifest.
struct GenerationProduct final
{
    /// The stage's scope.
    uint32 Scope = 0;
    /// The stage's ID within the scope.
    uint32 Stage = 0;
    /// Whether the product is available.
    GenerationProductState State = GenerationProductState::Pending;
    /// The stage's cache key; zero while pending.
    GenerationDigest Key;
    /// Digest of Bytes; zero unless ready.
    GenerationDigest Output;
    /// The product's canonical bytes; empty unless ready.
    foundation::Array<uint8> Bytes;
};

/// @brief A plan's evaluation in progress, begun against one accepted revision.
///
/// Requests fill its products on demand. It stays bound to the plan it was begun with: a request with an edited plan
/// is refused, and an edit begins a new candidate. Single-owner; not thread-safe.
struct GenerationCandidate final
{
    /// Success while every evaluated stage succeeded; otherwise the first failure, which latches.
    GenerationStatus Status = GenerationStatus::NotReady;
    /// The accepted revision the candidate was begun against.
    uint64 BaseRevision = 0;
    /// Digest of the plan's stage identities, parameters and input indices; requests must present the same plan.
    GenerationDigest Plan;
    /// One product per plan stage, in plan order.
    foundation::Array<GenerationProduct> Products;

    /// Whether every product is ready.
    [[nodiscard]] bool IsReady() const noexcept;
    /// Whether the scope has at least one stage and every stage of it is ready.
    [[nodiscard]] bool IsScopeReady(uint32 scope) const noexcept;
    /// The product of a stage, or null when the plan has no such stage.
    [[nodiscard]] const GenerationProduct* FindProduct(uint32 scope, uint32 stage) const noexcept;
};

/// @brief The accepted products of a published plan.
struct GenerationManifest final
{
    /// Every stage product in plan order; all ready.
    foundation::Array<GenerationProduct> Products;
    /// Digest over the scopes, stage IDs, keys and output digests; equal digests mean equal manifests.
    GenerationDigest Digest;

    /// The product of a stage, or null when the manifest has none.
    [[nodiscard]] const GenerationProduct* FindProduct(uint32 scope, uint32 stage) const noexcept;
};

/// @brief Check a plan's bounds, topological order, unique (scope, stage) pairs and kernels.
/// @return Success; InvalidInput for an empty or malformed plan; CapacityExceeded over a MAX_GENERATION_* bound.
[[nodiscard]] GenerationStatus ValidateGenerationPlan(foundation::BorrowedView<const GenerationStage> plan) noexcept;

/// @brief Digest a manifest over the scope, stage, key and output digest of each product, in order.
/// @param products Ready products in plan order.
/// @param outDigest Replaced on success; unchanged on failure.
/// @return False when encoding fails to allocate.
[[nodiscard]] bool TryComputeGenerationManifestDigest(foundation::BorrowedView<const GenerationProduct> products,
                                                      GenerationDigest& outDigest) noexcept;

/// @brief Evaluates plans on demand through a validated product cache and owns the accepted manifest.
///
/// The accepted manifest changes only through TryPublish; beginning, requesting and rejecting candidates never change
/// it, whatever the outcome. The coordinator borrows the cache store, which must outlive it. Single-owner; not
/// thread-safe.
class GenerationCoordinator final
{
public:
    /// Create a coordinator with an empty accepted manifest at revision zero.
    explicit GenerationCoordinator(GenerationCacheStore& inoutCacheStore) noexcept;

    /// @brief Begin a candidate for a plan with every stage pending.
    /// @param plan The plan; validated by this call.
    /// @param outCandidate Replaced on every call; its Status equals the return value.
    /// @return Success, a ValidateGenerationPlan failure, or OutOfMemory.
    [[nodiscard]] GenerationStatus TryBeginCandidate(foundation::BorrowedView<const GenerationStage> plan,
                                                     GenerationCandidate& outCandidate) noexcept;

    /// @brief Make one scope's products ready: evaluate its pending stages and their pending upstream closure.
    ///
    /// Stages outside the closure stay pending, and stages already ready are not evaluated again. Each evaluated
    /// stage reads a validated cache entry when its key matches one and otherwise runs its kernel and stores the
    /// product; a corrupt entry is discarded and rebuilt, and a refused store write never changes a product. Every
    /// evaluation is recorded in the trace.
    /// @param plan The plan the candidate was begun with.
    /// @param scope The requested scope.
    /// @param inoutTrace Receives one record per evaluated stage.
    /// @param inoutCandidate Its products are filled; when a stage fails it becomes Failed and Status latches the
    ///         failure. Products evaluated before the failure stay ready.
    /// @return Success; InvalidInput when the scope has no stage or the plan differs from the candidate's;
    ///         OutOfMemory before any evaluation, leaving the candidate unchanged; the latched status of a failed
    ///         candidate; or the failing stage's status.
    [[nodiscard]] GenerationStatus TryRequestScope(foundation::BorrowedView<const GenerationStage> plan,
                                                   uint32 scope,
                                                   GenerationTrace& inoutTrace,
                                                   GenerationCandidate& inoutCandidate) noexcept;

    /// @brief Make every product ready, evaluating each pending stage as TryRequestScope does.
    /// @param plan The plan the candidate was begun with.
    /// @param inoutTrace Receives one record per evaluated stage.
    /// @param inoutCandidate Its products are filled; when a stage fails it becomes Failed and Status latches the
    ///         failure. Products evaluated before the failure stay ready.
    /// @return Success; InvalidInput when the plan differs from the candidate's; OutOfMemory before any evaluation,
    ///         leaving the candidate unchanged; the latched status of a failed candidate; or the failing stage's
    ///         status.
    [[nodiscard]] GenerationStatus TryRequestAll(foundation::BorrowedView<const GenerationStage> plan,
                                                 GenerationTrace& inoutTrace,
                                                 GenerationCandidate& inoutCandidate) noexcept;

    /// @brief Make a ready, successful candidate the accepted manifest and advance the revision.
    /// @param inoutCandidate Emptied and marked NotReady on success; unchanged on failure.
    /// @return Success; NotReady when the candidate failed or a stage is still pending; StaleCandidate when it was
    ///         begun against an older revision; or OutOfMemory. The accepted manifest is unchanged on failure.
    [[nodiscard]] GenerationStatus TryPublish(GenerationCandidate& inoutCandidate) noexcept;

    /// Discard a candidate without changing the accepted manifest; it becomes empty and NotReady.
    void Reject(GenerationCandidate& inoutCandidate) noexcept;

    /// The accepted manifest; valid until the next successful TryPublish.
    [[nodiscard]] LUDUS_INLINE const GenerationManifest& GetAccepted() const noexcept
    {
        return mAccepted;
    }
    /// Revision of the accepted manifest; zero before the first publication.
    [[nodiscard]] LUDUS_INLINE uint64 GetAcceptedRevision() const noexcept
    {
        return mAcceptedRevision;
    }

private:
    [[nodiscard]] GenerationStatus requestStages(foundation::BorrowedView<const GenerationStage> plan,
                                                 foundation::BorrowedView<const bool> isRequested,
                                                 GenerationTrace& inoutTrace,
                                                 GenerationCandidate& inoutCandidate) noexcept;
    [[nodiscard]] GenerationStatus evaluateStage(const GenerationStage& stage,
                                                 usize index,
                                                 GenerationTrace& inoutTrace,
                                                 GenerationCandidate& inoutCandidate) noexcept;

    GenerationCacheStore& mCacheStore;
    GenerationByteWriter mWriter;
    GenerationManifest mAccepted;
    uint64 mAcceptedRevision = 0;
};
} // namespace ludus::content::procedural
