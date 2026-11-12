// Editor-private curve/surface authoring adapters (curves and surfaces G5).
// See internal/geometry_document.h for the contracts and the owners reused.

#include "internal/geometry_document.h"

#include <ludus/content/content.h>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/geometry/path.hpp>

namespace ludus::editor::geometry
{
using geom::GeometryStatus;

namespace
{
void FailReport(gc::GeometryDiagnosticReport& outReport,
                content::Status transport,
                GeometryStatus code,
                const char* property,
                const char* repair) noexcept
{
    outReport.Transport = transport;
    outReport.Detail = geom::GeometryDiagnostic{};
    outReport.Detail.Code = code;
    outReport.Detail.Property = property;
    outReport.Repair = repair;
    (void)outReport.Message.Set("");
}

void SucceedReport(gc::GeometryDiagnosticReport& outReport) noexcept
{
    outReport.Transport = content::Status::Ok;
    outReport.Detail = geom::GeometryDiagnostic{};
    outReport.Repair = "";
    (void)outReport.Message.Set("");
}
} // namespace

void CurveDocument::Reset(uint64 assetId) noexcept
{
    mDraft = CurveState{};
    mDraft.AssetId = assetId;
    mHistory.Reset();
}

const ControlHandle* CurveDocument::findHandleOrNull(uint64 authorId) const noexcept
{
    for (usize i = 0; i < mDraft.Handles.GetSize(); ++i)
    {
        if (mDraft.Handles[i].AuthorId == authorId)
        {
            return &mDraft.Handles[i];
        }
    }
    return nullptr;
}

bool CurveDocument::commit(const CurveState& candidate,
                           bool shouldMerge,
                           gc::GeometryDiagnosticReport& outDiagnostic) noexcept
{
    // Validate the whole candidate: unique author IDs and a representable count.
    if (candidate.Handles.GetSize() > CurveState::MAX_HANDLES)
    {
        FailReport(outDiagnostic,
                   content::Status::Limit,
                   GeometryStatus::BudgetExceeded,
                   "Handles",
                   "Reduce the number of control handles.");
        return false;
    }
    for (usize i = 0; i < candidate.Handles.GetSize(); ++i)
    {
        for (usize j = i + 1; j < candidate.Handles.GetSize(); ++j)
        {
            if (candidate.Handles[i].AuthorId == candidate.Handles[j].AuthorId)
            {
                FailReport(outDiagnostic,
                           content::Status::Invalid,
                           GeometryStatus::InvalidArgument,
                           "Handles.AuthorId",
                           "Control handle IDs must be unique.");
                return false;
            }
        }
    }
    if (mHistory.Commit(mDraft, candidate, shouldMerge) == false)
    {
        // A no-op edit (candidate equals the draft) is not an error, just nothing to record.
        if (candidate == mDraft)
        {
            SucceedReport(outDiagnostic);
            return true;
        }
        FailReport(outDiagnostic,
                   content::Status::Conflict,
                   GeometryStatus::StaleRevision,
                   "History",
                   "The edit could not be recorded; retry after refreshing the document.");
        return false;
    }
    mDraft = candidate;
    SucceedReport(outDiagnostic);
    return true;
}

bool CurveDocument::MoveControl(uint64 authorId,
                                const foundation::math::Vector3d& position,
                                bool shouldMerge,
                                gc::GeometryDiagnosticReport& outDiagnostic) noexcept
{
    if (findHandleOrNull(authorId) == nullptr)
    {
        FailReport(outDiagnostic,
                   content::Status::NotFound,
                   GeometryStatus::InvalidArgument,
                   "AuthorId",
                   "Add the control before moving it.");
        return false;
    }
    CurveState candidate = mDraft;
    for (usize i = 0; i < candidate.Handles.GetSize(); ++i)
    {
        if (candidate.Handles[i].AuthorId == authorId)
        {
            candidate.Handles[i].Position = position;
            // Moving an automatic handle visibly converts it to manual.
            if (candidate.Handles[i].Mode == gc::HandleMode::Automatic)
            {
                candidate.Handles[i].Mode = gc::HandleMode::Manual;
            }
        }
    }
    return commit(candidate, shouldMerge, outDiagnostic);
}

bool CurveDocument::SetHandleManual(uint64 authorId, gc::GeometryDiagnosticReport& outDiagnostic) noexcept
{
    if (findHandleOrNull(authorId) == nullptr)
    {
        FailReport(outDiagnostic,
                   content::Status::NotFound,
                   GeometryStatus::InvalidArgument,
                   "AuthorId",
                   "Add the control before changing its mode.");
        return false;
    }
    CurveState candidate = mDraft;
    for (usize i = 0; i < candidate.Handles.GetSize(); ++i)
    {
        if (candidate.Handles[i].AuthorId == authorId)
        {
            candidate.Handles[i].Mode = gc::HandleMode::Manual;
        }
    }
    return commit(candidate, false, outDiagnostic);
}

bool CurveDocument::AddControl(const ControlHandle& handle, gc::GeometryDiagnosticReport& outDiagnostic) noexcept
{
    if (findHandleOrNull(handle.AuthorId) != nullptr)
    {
        FailReport(outDiagnostic,
                   content::Status::Conflict,
                   GeometryStatus::InvalidArgument,
                   "AuthorId",
                   "Choose a unique control handle ID.");
        return false;
    }
    CurveState candidate = mDraft;
    if (candidate.Handles.TryAdd(handle) == false)
    {
        FailReport(outDiagnostic,
                   content::Status::OutOfMemory,
                   GeometryStatus::OutOfMemory,
                   "Handles",
                   "Reduce the number of control handles.");
        return false;
    }
    return commit(candidate, false, outDiagnostic);
}

void CurveDocument::BreakEditGroup() noexcept
{
    mHistory.BreakGroup();
}

bool CurveDocument::CanUndo() const noexcept
{
    return mHistory.CanUndo();
}

bool CurveDocument::CanRedo() const noexcept
{
    return mHistory.CanRedo();
}

bool CurveDocument::Undo() noexcept
{
    return mHistory.Undo(mDraft);
}

bool CurveDocument::Redo() noexcept
{
    return mHistory.Redo(mDraft);
}

uint64 CurveDocument::Revision() const noexcept
{
    return mHistory.Revision();
}

bool CurveDocument::BuildSource(const geom::ToleranceProfile& tolerance,
                                const geom::WorkLimits& limits,
                                gc::GeometrySource& outSource,
                                gc::GeometryDiagnosticReport& outDiagnostic) const noexcept
{
    // A cubic path needs CUBIC_SPAN_STRIDE * n + 1 control handles for n spans.
    const usize handleCount = mDraft.Handles.GetSize();
    if (handleCount < CUBIC_MIN_HANDLES || ((handleCount - 1) % CUBIC_SPAN_STRIDE) != 0)
    {
        FailReport(outDiagnostic,
                   content::Status::Invalid,
                   GeometryStatus::InvalidArgument,
                   "Handles",
                   "A cubic path needs 3n+1 control handles.");
        return false;
    }
    gc::GeometrySource source;
    source.Mode = gc::GeometrySourceMode::CubicPath;
    source.Revision = Revision();
    source.AssetScale = 1.0;
    source.IsClosed = mDraft.IsClosed;
    source.Tolerance = tolerance;
    source.Limits = limits;
    source.Precision = geom::PrecisionProfile::Visual;
    const usize spanCount = (handleCount - 1) / CUBIC_SPAN_STRIDE;
    for (usize s = 0; s < spanCount; ++s)
    {
        gc::AuthoredSpan span{};
        span.AuthorId = mDraft.Handles[s * CUBIC_SPAN_STRIDE].AuthorId;
        span.Kind = fg::SpanKindTag::Cubic;
        span.DomainWidth = 1.0;
        for (usize c = 0; c < fg::SPAN_CONTROL_COUNT; ++c)
        {
            span.Control[c] = mDraft.Handles[s * CUBIC_SPAN_STRIDE + c].Position;
        }
        if (source.Spans.TryAdd(span) == false)
        {
            FailReport(outDiagnostic,
                       content::Status::OutOfMemory,
                       GeometryStatus::OutOfMemory,
                       "Spans",
                       "Reduce the number of spans.");
            return false;
        }
    }
    outSource = static_cast<gc::GeometrySource&&>(source);
    SucceedReport(outDiagnostic);
    return true;
}

// --- GeometryPrepareJob -----------------------------------------------------

bool GeometryPrepareJob::Begin(const GeometryPrepareRequest& request,
                               std::span<const uint8> blob,
                               const geom::WorkLimits& limits) noexcept
{
    mRequest = request;
    mLimits = limits;
    mStatus = GeometryPrepareStatus::Pending;
    mStage = Stage::Decode;
    mIsCancelled = false;
    mDiagnostic = gc::GeometryDiagnosticReport{};
    mBlob.Clear();
    if (mBlob.TryAddRange(foundation::MakeBorrowedView(blob)) == false)
    {
        mStatus = GeometryPrepareStatus::Failed;
        mStage = Stage::Done;
        mDiagnostic.Transport = content::Status::OutOfMemory;
        return false;
    }
    return true;
}

void GeometryPrepareJob::Cancel() noexcept
{
    mIsCancelled = true;
}

GeometryPrepareStatus GeometryPrepareJob::Prepare() noexcept
{
    if (mStatus != GeometryPrepareStatus::Pending)
    {
        return mStatus;
    }
    if (mIsCancelled)
    {
        mDiagnostic.Transport = content::Status::Cancelled;
        mDiagnostic.Detail.Code = GeometryStatus::Cancelled;
        mStatus = GeometryPrepareStatus::Failed;
        mStage = Stage::Done;
        return mStatus;
    }
    // One bounded stage: validate and load the immutable snapshot.
    gc::GeometryLoad load;
    const content::Status status = gc::LoadGeometry({mBlob.GetData(), mBlob.GetSize()}, mLimits, load, mDiagnostic);
    mStage = Stage::Done;
    if (status != content::Status::Ok)
    {
        mStatus = GeometryPrepareStatus::Failed;
        return mStatus;
    }
    mLoad = static_cast<gc::GeometryLoad&&>(load);
    mStatus = GeometryPrepareStatus::Ready;
    return mStatus;
}

gc::GeometryLoad GeometryPrepareJob::TakeLoad() noexcept
{
    return static_cast<gc::GeometryLoad&&>(mLoad);
}

// --- GeometryPreview --------------------------------------------------------

void GeometryPreview::SetExpected(uint64 documentId, uint64 revision, uint64 generation) noexcept
{
    mExpectedDocumentId = documentId;
    mExpectedRevision = revision;
    mExpectedGeneration = generation;
    if (mHasActive && (mPresentedRevision != revision || mPresentedGeneration != generation))
    {
        mDisplay = GeometryDisplayStatus::StalePending;
    }
}

bool GeometryPreview::OfferCompletion(const GeometryPrepareRequest& request,
                                      GeometryPrepareStatus status,
                                      gc::GeometryLoad&& load) noexcept
{
    // Reject a late completion: its identity no longer matches the expected one.
    const bool isIdentityMatch = request.DocumentId == mExpectedDocumentId && request.Revision == mExpectedRevision &&
                                 request.Generation == mExpectedGeneration;
    if (isIdentityMatch == false)
    {
        // Late or superseded; keep the active load and its stale flag unchanged.
        return false;
    }
    if (status == GeometryPrepareStatus::Failed)
    {
        if (mHasActive)
        {
            mDisplay = GeometryDisplayStatus::StaleFailed;
        }
        return false;
    }
    if (status != GeometryPrepareStatus::Ready)
    {
        if (mHasActive)
        {
            mDisplay = GeometryDisplayStatus::StalePending;
        }
        return false;
    }
    mCandidate = static_cast<gc::GeometryLoad&&>(load);
    mHasCandidate = true;
    mCandidateRevision = request.Revision;
    mCandidateGeneration = request.Generation;
    return true;
}

bool GeometryPreview::Tick() noexcept
{
    if (mHasCandidate == false)
    {
        return false;
    }
    mActive = static_cast<gc::GeometryLoad&&>(mCandidate);
    mHasActive = true;
    mHasCandidate = false;
    mPresentedRevision = mCandidateRevision;
    mPresentedGeneration = mCandidateGeneration;
    mDisplay = GeometryDisplayStatus::Active;
    return true;
}

// --- GeometryLiveBuild ------------------------------------------------------

void GeometryLiveBuild::Schedule(const content::Digest& identity) noexcept
{
    mCurrentIdentity = identity;
    mHasPending = (mCurrentIdentity == mRequestedIdentity) == false;
}

void GeometryLiveBuild::MarkRequested() noexcept
{
    mRequestedIdentity = mCurrentIdentity;
    mHasPending = false;
}

bool GeometryLiveBuild::IsBuildRequested() const noexcept
{
    return mIsEnabled && mIsBusy == false && mHasPending;
}

void GeometryLiveBuild::Reset() noexcept
{
    mCurrentIdentity = content::Digest{};
    mRequestedIdentity = content::Digest{};
    mHasPending = false;
}

content::Digest ComputeGeometryBuildIdentity(const CurveState& state, geom::PrecisionProfile profile) noexcept
{
    // Hash the author-visible geometry and the profile; exclude cosmetic state.
    content::Hasher hasher;
    const uint8 profileByte = static_cast<uint8>(profile);
    const uint8 closedByte = state.IsClosed ? 1u : 0u;
    (void)hasher.Add(std::span<const uint8>(&profileByte, 1));
    (void)hasher.Add(std::span<const uint8>(&closedByte, 1));
    for (usize i = 0; i < state.Handles.GetSize(); ++i)
    {
        const ControlHandle& handle = state.Handles[i];
        const uint8* idBytes = reinterpret_cast<const uint8*>(&handle.AuthorId);
        (void)hasher.Add(std::span<const uint8>(idBytes, sizeof(handle.AuthorId)));
        const foundation::float64 components[fg::CONTROL_COMPONENTS] = {handle.Position.X,
                                                                        handle.Position.Y,
                                                                        handle.Position.Z};
        (void)hasher.Add(std::span<const uint8>(reinterpret_cast<const uint8*>(components), sizeof(components)));
        const uint8 modeByte = static_cast<uint8>(handle.Mode);
        (void)hasher.Add(std::span<const uint8>(&modeByte, 1));
    }
    return hasher.Finish();
}

// --- ReloadCursor -----------------------------------------------------------

geom::GeometryStatus ReloadCursor(const geom::PathView& newPath,
                                  CursorReloadPolicy policy,
                                  const GeometryCursor& previous,
                                  GeometryCursor& outCursor) noexcept
{
    if (newPath.Spans.GetSize() == 0)
    {
        return GeometryStatus::InvalidDomain;
    }
    switch (policy)
    {
        case CursorReloadPolicy::RetainParameter: {
            GeometryCursor result{};
            result.Revision = newPath.Revision;
            const usize lastSpan = newPath.Spans.GetSize() - 1;
            const usize spanIndex = previous.Parameter.SpanIndex <= lastSpan ? previous.Parameter.SpanIndex : lastSpan;
            float64 u = previous.Parameter.U;
            if ((u >= 0.0) == false)
            {
                u = 0.0;
            }
            else if (u > 1.0)
            {
                u = 1.0;
            }
            result.Parameter = geom::SegmentParameter{spanIndex, u};
            foundation::math::Vector3 position{};
            const geom::GeometryStatus status = geom::TryEvaluatePath(newPath, result.Parameter, position);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
            result.WorldPosition = position;
            outCursor = result;
            return GeometryStatus::Success;
        }
        case CursorReloadPolicy::RetainAbsoluteDistance:
        case CursorReloadPolicy::RetainNormalizedDistance:
        case CursorReloadPolicy::ProjectWorldPosition:
            // These policies need a distance map or a projection query that the caller
            // must supply; this entry point does not build a cache on its own
            // (sampling never builds a cache, per ADR 0024), so it reports MissingCache
            // rather than silently falling back to another policy.
            return GeometryStatus::MissingCache;
    }
    return GeometryStatus::InvalidArgument;
}
} // namespace ludus::editor::geometry
