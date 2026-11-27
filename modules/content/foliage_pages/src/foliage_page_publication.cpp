#include <ludus/content/foliage_pages/foliage_page_publication.h>

#include <ludus/foundation/base/core.h>

// The stage/publish protocol follows ProceduralEnvironment's ScatterGenerationCoordinator
// (modules/content/procedural/environment): the accepted product changes only through a publication of a
// successfully staged candidate against the current revision. Original implementation; see
// docs/architecture/foliage.md, "Page set publication".

namespace ludus::content::foliage_pages
{
namespace
{
constexpr uint64 MAXIMUM_REVISION = ~uint64{0};
} // namespace

FoliagePageSetCandidate::FoliagePageSetCandidate(FoliagePageSetCandidate&& other) noexcept
    : mBytes(foundation::Move(other.mBytes)), mView(other.mView), mDiagnostic(other.mDiagnostic),
      mPublicationOrNull(other.mPublicationOrNull), mBaseRevision(other.mBaseRevision), mState(other.mState)
{
    // Moving an Array keeps its storage, so the view still refers to the transferred bytes.
    other.Clear();
}

FoliagePageSetCandidate& FoliagePageSetCandidate::operator=(FoliagePageSetCandidate&& other) noexcept
{
    if (this != &other)
    {
        mBytes = foundation::Move(other.mBytes);
        mView = other.mView;
        mDiagnostic = other.mDiagnostic;
        mPublicationOrNull = other.mPublicationOrNull;
        mBaseRevision = other.mBaseRevision;
        mState = other.mState;
        other.Clear();
    }
    return *this;
}

void FoliagePageSetCandidate::Clear() noexcept
{
    mBytes = foundation::Array<uint8>();
    mView = FoliagePageSetView();
    mDiagnostic = FoliagePageDiagnostic();
    mPublicationOrNull = nullptr;
    mBaseRevision = 0;
    mState = FoliagePageSetCandidateState::Empty;
}

FoliagePageSetPublication::FoliagePageSetPublication(const FoliagePageLimits& limits) noexcept : mLimits(limits) {}

FoliagePageStatus FoliagePageSetPublication::TryStage(foundation::Array<uint8>& inoutBytes,
                                                      const FoliagePageBinding& binding,
                                                      FoliagePageSetCandidate& outCandidate,
                                                      FoliagePageDiagnostic& outDiagnostic) const noexcept
{
    outCandidate.Clear();
    FoliagePageSetView view;
    const FoliagePageStatus status =
        ReadFoliagePageSet({inoutBytes.GetData(), inoutBytes.GetSize()}, binding, mLimits, view, outDiagnostic);
    outCandidate.mDiagnostic = outDiagnostic;
    if (status != FoliagePageStatus::Ok)
    {
        outCandidate.mState = FoliagePageSetCandidateState::Failed;
        return status;
    }
    // The view refers to the Array's storage, which the move transfers unchanged.
    outCandidate.mBytes = foundation::Move(inoutBytes);
    outCandidate.mView = view;
    outCandidate.mPublicationOrNull = this;
    outCandidate.mBaseRevision = mAcceptedRevision;
    outCandidate.mState = FoliagePageSetCandidateState::Ready;
    return FoliagePageStatus::Ok;
}

FoliagePublicationStatus FoliagePageSetPublication::TryPublish(FoliagePageSetCandidate& inoutCandidate) noexcept
{
    if (inoutCandidate.mState != FoliagePageSetCandidateState::Ready)
    {
        return FoliagePublicationStatus::NotReady;
    }
    if (inoutCandidate.mPublicationOrNull != this)
    {
        return FoliagePublicationStatus::InvalidArgument;
    }
    if (inoutCandidate.mBaseRevision != mAcceptedRevision)
    {
        return FoliagePublicationStatus::StaleCandidate;
    }
    if (mAcceptedRevision == MAXIMUM_REVISION)
    {
        return FoliagePublicationStatus::LimitExceeded;
    }

    // Exchange storage: the candidate keeps the previous generation's bytes, so its views stay valid.
    FoliagePageSetCandidate previous = foundation::Move(mAccepted);
    mAccepted = foundation::Move(inoutCandidate);
    mAccepted.mPublicationOrNull = nullptr;
    mAccepted.mBaseRevision = 0;
    mAcceptedRevision += 1;

    const bool hasPrevious = previous.mView.IsValid() == true;
    inoutCandidate = foundation::Move(previous);
    inoutCandidate.mDiagnostic = FoliagePageDiagnostic();
    inoutCandidate.mState =
        hasPrevious == true ? FoliagePageSetCandidateState::Retired : FoliagePageSetCandidateState::Empty;
    return FoliagePublicationStatus::Ok;
}
} // namespace ludus::content::foliage_pages
