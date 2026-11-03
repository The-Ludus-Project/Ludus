#pragma once

// Editor-private authoring adapters for editable curves and surfaces (curves and
// surfaces architecture, phase G5, "Editing, cache invalidation and publication").
//
// These are Qt-free logic owners, testable headless, built on the existing
// editor owners rather than a parallel system: GeometryDocument embeds the
// generic DocumentHistory<T> undo owner (apps/editor/src/internal/document_history.h,
// ADR 0023), GeometryPrepareJob mirrors the staged Prepare() pump of
// PreviewGpuGeneration, and GeometryPreview mirrors RenderGraphPreview's
// transactional candidate publication with a retained stale view. The async
// identity and late-completion rejection mirror RenderGraphJob. Author IDs are
// stable; array indices are revision-local. GPU presentation is out of G5 scope.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/geometry/content/geometry_content.h>

#include "internal/document_history.h"

namespace ludus::editor::geometry
{
namespace fg = ludus::content::geometry_format;
namespace geom = ludus::foundation::geometry;
namespace gc = ludus::geometry::content;
using foundation::float64;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// @brief One authored control point of a curve document with its handle mode.
struct ControlHandle final
{
    uint64 AuthorId = 0;                        ///< Stable author ID, minted through the editor identity path.
    foundation::math::Vector3d Position{};      ///< Asset-space position.
    gc::HandleMode Mode = gc::HandleMode::Manual; ///< Automatic handles convert to Manual when moved directly.
};

/// @brief Editable curve-document state: the undo payload (before/after snapshots).
///
/// Equality-comparable and exception-free to copy, so DocumentHistory<State> can
/// store before/after snapshots. One span uses four handles.
struct CurveState final
{
    static constexpr usize MAX_HANDLES = 4096;
    uint64 AssetId = 0;                                   ///< Stable document identity.
    bool IsClosed = false;                                ///< Closed path boundary.
    foundation::core::Array<ControlHandle> Handles;       ///< Author-ordered control handles.

    CurveState() noexcept = default;
    CurveState(const CurveState& other) { copyFrom(other); }
    CurveState& operator=(const CurveState& other)
    {
        if (this != &other)
        {
            copyFrom(other);
        }
        return *this;
    }
    CurveState(CurveState&&) noexcept = default;
    CurveState& operator=(CurveState&&) noexcept = default;
    ~CurveState() = default;

    [[nodiscard]] bool operator==(const CurveState& other) const noexcept
    {
        if (AssetId != other.AssetId || IsClosed != other.IsClosed || Handles.GetSize() != other.Handles.GetSize())
        {
            return false;
        }
        for (usize i = 0; i < Handles.GetSize(); ++i)
        {
            const ControlHandle& a = Handles[i];
            const ControlHandle& b = other.Handles[i];
            const bool same = a.AuthorId == b.AuthorId && a.Mode == b.Mode && a.Position.X == b.Position.X &&
                              a.Position.Y == b.Position.Y && a.Position.Z == b.Position.Z;
            if (same == false)
            {
                return false;
            }
        }
        return true;
    }

private:
    void copyFrom(const CurveState& other)
    {
        AssetId = other.AssetId;
        IsClosed = other.IsClosed;
        Handles.Clear();
        (void)Handles.TryAddRange({other.Handles.GetData(), other.Handles.GetSize()});
    }
};

/// @brief Curve authoring document: validated edits recorded as undoable commands.
///
/// Every mutator validates the whole candidate before committing, so an invalid
/// edit leaves the draft, history and outputs unchanged (it may still be shown in
/// the UI as an invalid candidate). Undo/redo and the monotonic revision forward
/// to the embedded DocumentHistory. Mirrors WorldDocument's shape.
class CurveDocument final
{
public:
    /// Begin a new document identity; resets history and revision.
    void Reset(uint64 assetId) noexcept;

    /// @brief Move a control handle's position as one undoable edit.
    /// @param authorId Stable handle ID.
    /// @param position New asset-space position.
    /// @param merge Coalesce with the previous edit of the same handle (a drag).
    /// @param outDiagnostic Set when the edit is rejected.
    /// @return True when the validated edit was committed.
    [[nodiscard]] bool MoveControl(uint64 authorId,
                                   const foundation::math::Vector3d& position,
                                   bool merge,
                                   gc::GeometryDiagnosticReport& outDiagnostic) noexcept;

    /// @brief Convert a handle to Manual mode as one undoable edit (visible conversion).
    [[nodiscard]] bool SetHandleManual(uint64 authorId, gc::GeometryDiagnosticReport& outDiagnostic) noexcept;

    /// @brief Append a control handle as one undoable edit; its ID must be unique.
    [[nodiscard]] bool AddControl(const ControlHandle& handle, gc::GeometryDiagnosticReport& outDiagnostic) noexcept;

    /// End a coalescing group so the next edit starts a new undo step.
    void BreakEditGroup() noexcept;

    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;
    [[nodiscard]] bool Undo() noexcept;
    [[nodiscard]] bool Redo() noexcept;
    [[nodiscard]] uint64 Revision() const noexcept;

    /// Read-only access to the current draft.
    [[nodiscard]] const CurveState& Draft() const noexcept
    {
        return mDraft;
    }

    /// @brief Build a cookable GeometrySource from the current draft (CubicPath mode).
    /// @return True on success; false (with a diagnostic) when the draft cannot form a source.
    [[nodiscard]] bool BuildSource(const geom::ToleranceProfile& tolerance,
                                   const geom::WorkLimits& limits,
                                   gc::GeometrySource& outSource,
                                   gc::GeometryDiagnosticReport& outDiagnostic) const noexcept;

private:
    [[nodiscard]] bool commit(const CurveState& candidate, bool merge, gc::GeometryDiagnosticReport& outDiagnostic) noexcept;
    [[nodiscard]] const ControlHandle* findHandleOrNull(uint64 authorId) const noexcept;

    CurveState mDraft;
    DocumentHistory<CurveState, 64> mHistory;
};

/// @brief Staged outcome of an async geometry prepare, mirroring PreviewPreparationStatus.
enum class GeometryPrepareStatus : uint8
{
    Pending, ///< More bounded work remains; poll Prepare again.
    Ready,   ///< The prepared owner is available.
    Failed,  ///< Terminal failure for this generation; the caller keeps its previous output.
};

/// @brief Identity carried by an async prepare request so late completions can be rejected.
struct GeometryPrepareRequest final
{
    uint64 Request = 0;    ///< Monotonic request serial.
    uint64 DocumentId = 0; ///< Session/document identity.
    uint64 Revision = 0;   ///< Document revision at dispatch.
    uint64 Generation = 0; ///< Activation serial.
    geom::PrecisionProfile Profile = geom::PrecisionProfile::Visual; ///< Preparation profile.
    content::Digest InputIdentity{}; ///< Semantic source+profile digest.
};

/// @brief Async geometry prepare over an immutable cooked snapshot.
///
/// Frozen to one source snapshot and request identity. Prepare advances at most
/// one bounded stage per call and never reads mutable document state; the
/// exclusive output (the GeometryLoad) is retained through completion. Failed is
/// terminal for this generation and the caller keeps its previous display. A
/// requested cancellation yields Failed with a Cancelled transport. Mirrors
/// PreviewGpuGeneration.
class GeometryPrepareJob final
{
public:
    /// @brief Begin a job over an immutable cooked snapshot.
    /// @param request Carried identity (revision, generation, profile, digest).
    /// @param blob Cooked LUGEO bytes; copied into the job as its immutable input.
    /// @param limits Preparation work limits.
    /// @return False only when the snapshot copy fails to allocate (then status is Failed).
    [[nodiscard]] bool Begin(const GeometryPrepareRequest& request, std::span<const uint8> blob, const geom::WorkLimits& limits) noexcept;

    /// @brief Request cooperative cancellation; the next Prepare reports Failed/Cancelled.
    void Cancel() noexcept;

    /// @brief Advance at most one bounded stage.
    /// @return Pending, Ready or Failed.
    [[nodiscard]] GeometryPrepareStatus Prepare() noexcept;

    [[nodiscard]] GeometryPrepareStatus Status() const noexcept
    {
        return mStatus;
    }
    [[nodiscard]] const GeometryPrepareRequest& Request() const noexcept
    {
        return mRequest;
    }
    /// @brief The transport status of a completed prepare (Ok on Ready).
    [[nodiscard]] content::Status Transport() const noexcept
    {
        return mDiagnostic.Transport;
    }
    [[nodiscard]] const gc::GeometryDiagnosticReport& Diagnostic() const noexcept
    {
        return mDiagnostic;
    }
    /// @brief Take the Ready output; leaves the job empty. @pre Status() == Ready.
    [[nodiscard]] gc::GeometryLoad TakeLoad() noexcept;

private:
    enum class Stage : uint8
    {
        Decode, ///< Validate and load the blob into a prepared owner.
        Done,   ///< Ready or Failed.
    };
    GeometryPrepareRequest mRequest{};
    foundation::core::Array<uint8> mBlob;
    geom::WorkLimits mLimits{};
    gc::GeometryLoad mLoad;
    gc::GeometryDiagnosticReport mDiagnostic{};
    GeometryPrepareStatus mStatus = GeometryPrepareStatus::Pending;
    Stage mStage = Stage::Decode;
    bool mIsCancelled = false;
};

/// @brief Whether the preview is showing the current revision or a stale fallback.
enum class GeometryDisplayStatus : uint8
{
    Empty,        ///< No valid load has ever been published.
    Active,       ///< The published load matches the latest edit.
    StalePending, ///< A newer edit is preparing; the last valid load stays active.
    StaleFailed,  ///< The newest prepare failed; the last valid load stays active.
};

/// @brief Transactional candidate publication with a retained stale view.
///
/// Owns the active published load and at most one candidate. A Ready candidate
/// publishes at a declared frame/tick boundary (Tick). A Pending, Failed or
/// identity-mismatched candidate keeps the active load and raises a stale flag;
/// a failed replacement never erases the active view. Peak storage is active plus
/// one candidate. Mirrors RenderGraphPreview. Owner-thread only.
class GeometryPreview final
{
public:
    /// @brief Note the document identity the next publication must match.
    void SetExpected(uint64 documentId, uint64 revision, uint64 generation) noexcept;

    /// @brief Offer a completed prepare as the next candidate.
    ///
    /// A late completion (its request's generation/revision no longer match the
    /// expected identity) is rejected and does not become the candidate. A Ready
    /// matching result is held as the pending candidate; a Failed result raises
    /// the stale-failed flag without replacing the active load.
    /// @return True when the result was accepted as the pending candidate.
    [[nodiscard]] bool OfferCompletion(const GeometryPrepareRequest& request, GeometryPrepareStatus status, gc::GeometryLoad&& load) noexcept;

    /// @brief Publish the pending candidate at a frame/tick boundary, if any.
    /// @return True when a candidate was published this tick.
    [[nodiscard]] bool Tick() noexcept;

    [[nodiscard]] GeometryDisplayStatus DisplayStatus() const noexcept
    {
        return mDisplay;
    }
    [[nodiscard]] bool HasActive() const noexcept
    {
        return mHasActive;
    }
    [[nodiscard]] uint64 PresentedRevision() const noexcept
    {
        return mPresentedRevision;
    }
    [[nodiscard]] uint64 PresentedGeneration() const noexcept
    {
        return mPresentedGeneration;
    }
    [[nodiscard]] const gc::GeometryLoad& Active() const noexcept
    {
        return mActive;
    }
    /// @brief Live owner count: 1 for the active load plus 1 when a candidate is pending.
    [[nodiscard]] usize LiveLoadCount() const noexcept
    {
        return (mHasActive ? 1u : 0u) + (mHasCandidate ? 1u : 0u);
    }

private:
    gc::GeometryLoad mActive;
    gc::GeometryLoad mCandidate;
    bool mHasActive = false;
    bool mHasCandidate = false;
    uint64 mExpectedDocumentId = 0;
    uint64 mExpectedRevision = 0;
    uint64 mExpectedGeneration = 0;
    uint64 mCandidateRevision = 0;
    uint64 mCandidateGeneration = 0;
    uint64 mPresentedRevision = 0;
    uint64 mPresentedGeneration = 0;
    GeometryDisplayStatus mDisplay = GeometryDisplayStatus::Empty;
};

/// @brief Debounce/supersession owner for live geometry preview.
///
/// Owns no document; records the current and last-requested semantic identity so
/// a newer edit supersedes a stale pending request. Mirrors RenderGraphLiveBuild
/// without the Qt timer (the caller drives the debounce clock).
class GeometryLiveBuild final
{
public:
    void SetEnabled(bool isEnabled) noexcept
    {
        mIsEnabled = isEnabled;
    }
    void SetBusy(bool isBusy) noexcept
    {
        mIsBusy = isBusy;
    }
    /// @brief Schedule a build for a semantic identity (supersedes a stale one).
    void Schedule(const content::Digest& identity) noexcept;
    /// @brief Record that the current identity has been dispatched.
    void MarkRequested() noexcept;
    /// @brief Whether a build should be dispatched now (enabled, not busy, pending).
    [[nodiscard]] bool IsBuildRequested() const noexcept;
    void Reset() noexcept;
    [[nodiscard]] const content::Digest& CurrentIdentity() const noexcept
    {
        return mCurrentIdentity;
    }

private:
    content::Digest mCurrentIdentity{};
    content::Digest mRequestedIdentity{};
    bool mHasPending = false;
    bool mIsEnabled = true;
    bool mIsBusy = false;
};

/// @brief Stable semantic build identity of a curve document and profile.
///
/// Excludes cosmetic/layout-only state, so a purely visual move does not trigger
/// a rebuild. Returns the SHA-256 of the author-visible geometry and profile.
[[nodiscard]] content::Digest ComputeGeometryBuildIdentity(const CurveState& state, geom::PrecisionProfile profile) noexcept;

/// @brief How a cursor is placed on a newly published path revision.
///
/// The consumer chooses this explicitly; it is never implicit, because each
/// policy yields different motion (curves and surfaces architecture).
enum class CursorReloadPolicy : uint8
{
    RetainParameter,          ///< Keep the span index and u, clamped into the new span range.
    RetainAbsoluteDistance,   ///< Keep the absolute arc distance (requires a distance map).
    RetainNormalizedDistance, ///< Keep the distance as a fraction of total length (requires a distance map).
    ProjectWorldPosition,     ///< Re-project the previous world position (requires a projection query).
};

/// @brief A traversal cursor carrying its snapshot revision.
struct GeometryCursor final
{
    uint64 Revision = 0;                   ///< Revision of the path the cursor was placed on.
    geom::SegmentParameter Parameter{};    ///< Current span index and u.
    geom::PathDistance Distance{};         ///< Current absolute arc distance.
    float64 NormalizedDistance = 0.0;      ///< Distance / total length, in [0,1].
    foundation::math::Vector3 WorldPosition{}; ///< Last evaluated chunk-relative position.
};

/// @brief Reload a cursor onto a newly published path under an explicit policy.
///
/// RetainParameter clamps the previous span index and u into the new path's span
/// range. The distance and projection policies require their caches and are not
/// resolved from the view alone; this G5 entry point implements RetainParameter
/// directly and reports MissingCache for the policies whose cache the caller did
/// not supply, so no policy is ever chosen implicitly.
/// @param newPath The newly published path view.
/// @param policy The consumer-chosen reload policy.
/// @param previous The cursor from the previous revision.
/// @param outCursor Written only on Success.
/// @return Success; StaleRevision if previous.Revision equals newPath.Revision is not required but a mismatch with
/// a zero previous revision is reported; MissingCache for a distance/projection policy without its cache;
/// InvalidDomain for an empty path.
[[nodiscard]] geom::GeometryStatus ReloadCursor(const geom::PathView& newPath,
                                                CursorReloadPolicy policy,
                                                const GeometryCursor& previous,
                                                GeometryCursor& outCursor) noexcept;
} // namespace ludus::editor::geometry
