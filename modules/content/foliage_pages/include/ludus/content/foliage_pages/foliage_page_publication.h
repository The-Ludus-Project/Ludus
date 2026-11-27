#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>

#include <ludus/content/foliage_pages/foliage_pages.h>

/// @file
/// Generation publication for LUFOL foliage page sets (Ludus::FoliagePages).
///
/// A publication owns the accepted page set of one terrain domain. A replacement is staged as a
/// candidate, validated completely and published only when it is Ready and was staged against the
/// current accepted revision. A failed, discarded or stale replacement leaves the accepted page set,
/// its bytes and its views unchanged. Staging and publication never allocate: they transfer the
/// caller's bytes. The design is in docs/architecture/foliage.md, "Page set publication".
namespace ludus::content::foliage_pages
{
class FoliagePageSetPublication;

/// Result of publishing a candidate.
enum class FoliagePublicationStatus : uint8
{
    Ok,              ///< The candidate is the accepted page set and the revision advanced.
    InvalidArgument, ///< The candidate was staged by a different publication.
    NotReady,        ///< The candidate did not stage successfully, or was already published or cleared.
    StaleCandidate,  ///< Another candidate was published after this one was staged.
    LimitExceeded,   ///< The accepted revision cannot advance further.
};

/// Lifecycle of a candidate.
enum class FoliagePageSetCandidateState : uint8
{
    Empty,   ///< Holds nothing: default, cleared, or a successful publication with no previous page set.
    Ready,   ///< Holds a validated page set that may be published.
    Failed,  ///< Staging rejected the page set; the diagnostic names the rule to repair.
    Retired, ///< Holds the page set that a successful publication replaced, so its views stay valid.
};

/// @brief A staged replacement for a publication's accepted page set.
///
/// A candidate owns its bytes. A Ready candidate holds the replacement and a view of it; after a
/// successful publication it becomes Retired and holds the previous accepted page set instead,
/// so consumers can finish with views of the previous generation before the caller clears it.
/// Move-only; not thread-safe. Destroying or clearing a candidate invalidates views of its bytes.
class FoliagePageSetCandidate final
{
public:
    /// Constructs an Empty candidate without allocating.
    FoliagePageSetCandidate() noexcept = default;
    /// Transfers the bytes and state, leaving the source Empty. Views of the bytes stay valid.
    FoliagePageSetCandidate(FoliagePageSetCandidate&& other) noexcept;
    /// Releases this candidate's bytes and transfers the source's, leaving the source Empty.
    FoliagePageSetCandidate& operator=(FoliagePageSetCandidate&& other) noexcept;
    /// Copying is disabled: a candidate uniquely owns the bytes its view refers to.
    FoliagePageSetCandidate(const FoliagePageSetCandidate&) = delete;
    /// Copying is disabled: a candidate uniquely owns the bytes its view refers to.
    FoliagePageSetCandidate& operator=(const FoliagePageSetCandidate&) = delete;
    /// Releases the bytes.
    ~FoliagePageSetCandidate() noexcept = default;

    /// Current lifecycle state.
    [[nodiscard]] LUDUS_INLINE FoliagePageSetCandidateState GetState() const noexcept
    {
        return mState;
    }
    /// @brief View of the held page set.
    /// @return The validated replacement when Ready, the previous accepted page set when Retired, and an
    /// empty view otherwise. Valid while this candidate holds the bytes.
    [[nodiscard]] LUDUS_INLINE const FoliagePageSetView& GetView() const noexcept
    {
        return mView;
    }
    /// Outcome of the last staging: Ok unless the candidate is Failed.
    [[nodiscard]] LUDUS_INLINE const FoliagePageDiagnostic& GetDiagnostic() const noexcept
    {
        return mDiagnostic;
    }
    /// Accepted revision the candidate was staged against; zero unless Ready.
    [[nodiscard]] LUDUS_INLINE uint64 GetBaseRevision() const noexcept
    {
        return mBaseRevision;
    }
    /// Releases the bytes and returns to Empty, invalidating views of them; discards an unpublished candidate.
    void Clear() noexcept;

private:
    foundation::Array<uint8> mBytes;
    FoliagePageSetView mView;
    FoliagePageDiagnostic mDiagnostic;
    const FoliagePageSetPublication* mPublicationOrNull = nullptr;
    uint64 mBaseRevision = 0;
    FoliagePageSetCandidateState mState = FoliagePageSetCandidateState::Empty;

    friend class FoliagePageSetPublication;
};

/// @brief Owns the accepted page set of one terrain domain and replaces it only with validated candidates.
///
/// The accepted page set changes only through TryPublish; staging and clearing candidates never change it,
/// whatever their outcome. Candidates carry the revision they were staged against, so of several
/// candidates staged from one revision only the first published one is accepted. That orders
/// asynchronous cooks that complete out of order. The publication applies the same admission
/// limits to every candidate. Single-owner and not thread-safe; the accepted view may be read
/// concurrently while no method is running. Not movable, because candidates refer to it.
class FoliagePageSetPublication final
{
public:
    /// @brief Creates a publication with no accepted page set at revision zero.
    /// @param limits Admission limits for every candidate, at most the format limits. Limits above them
    /// make every staging fail with InvalidArgument.
    explicit FoliagePageSetPublication(const FoliagePageLimits& limits = {}) noexcept;
    /// Copying is disabled: the publication uniquely owns the accepted bytes.
    FoliagePageSetPublication(const FoliagePageSetPublication&) = delete;
    /// Copying is disabled: the publication uniquely owns the accepted bytes.
    FoliagePageSetPublication& operator=(const FoliagePageSetPublication&) = delete;
    /// Moving is disabled: staged candidates identify their publication by address.
    FoliagePageSetPublication(FoliagePageSetPublication&&) = delete;
    /// Moving is disabled: staged candidates identify their publication by address.
    FoliagePageSetPublication& operator=(FoliagePageSetPublication&&) = delete;
    /// Releases the accepted bytes, invalidating views of them.
    ~FoliagePageSetPublication() noexcept = default;

    /// @brief Validates a complete page set as a replacement for the accepted one.
    ///
    /// The bytes are read with ReadFoliagePageSet against the binding and this publication's limits.
    /// On success the candidate takes the bytes and becomes Ready, staged against the current
    /// accepted revision. On failure it becomes Failed with the read diagnostic. The accepted page
    /// set never changes.
    /// @param inoutBytes The page set. Transferred to the candidate on success, leaving it empty; unchanged on
    /// failure, so the caller can inspect the offending bytes.
    /// @param binding Terrain domain and revision the replacement must belong to.
    /// @param outCandidate Replaced on every call, releasing what it held; its diagnostic equals outDiagnostic.
    /// @param outDiagnostic Receives the status and, on failure, the violated rule, record index and byte offset.
    /// @return Ok, InvalidArgument, Malformed, UnsupportedVersion, RevisionMismatch or LimitExceeded.
    [[nodiscard]] FoliagePageStatus TryStage(foundation::Array<uint8>& inoutBytes,
                                             const FoliagePageBinding& binding,
                                             FoliagePageSetCandidate& outCandidate,
                                             FoliagePageDiagnostic& outDiagnostic) const noexcept;

    /// @brief Makes a Ready candidate the accepted page set and advances the revision.
    ///
    /// On success the candidate becomes Retired and holds the previous accepted page set, or becomes
    /// Empty when there was none, so views of the previous generation remain valid until the caller
    /// clears it. Never allocates.
    /// @param inoutCandidate The candidate; unchanged on failure.
    /// @return Ok, InvalidArgument, NotReady, StaleCandidate or LimitExceeded. The accepted page set is unchanged
    /// on failure.
    [[nodiscard]] FoliagePublicationStatus TryPublish(FoliagePageSetCandidate& inoutCandidate) noexcept;

    /// View of the accepted page set; empty before the first publication. Valid until the next successful
    /// TryPublish, after which the retired candidate keeps its bytes alive.
    [[nodiscard]] LUDUS_INLINE const FoliagePageSetView& GetAccepted() const noexcept
    {
        return mAccepted.mView;
    }
    /// Encoded bytes of the accepted page set; empty before the first publication.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetAcceptedBytes() const noexcept
    {
        return {mAccepted.mBytes.GetData(), mAccepted.mBytes.GetSize()};
    }
    /// Revision of the accepted page set; zero before the first publication, plus one per publication.
    [[nodiscard]] LUDUS_INLINE uint64 GetAcceptedRevision() const noexcept
    {
        return mAcceptedRevision;
    }
    /// Admission limits applied to every candidate.
    [[nodiscard]] LUDUS_INLINE const FoliagePageLimits& GetLimits() const noexcept
    {
        return mLimits;
    }

private:
    FoliagePageSetCandidate mAccepted;
    FoliagePageLimits mLimits;
    uint64 mAcceptedRevision = 0;
};
} // namespace ludus::content::foliage_pages
