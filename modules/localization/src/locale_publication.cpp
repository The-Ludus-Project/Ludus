#include <ludus/localization/locale_publication.hpp>

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/strings/status.hpp>

#include "internal/catalog_access.hpp"
#include "internal/localized_view.hpp"

#include <atomic>
#include <span>

namespace ludus::localization::detail
{
// Snapshot copies can release budgeted storage from another thread. The owner is
// the only allocator; reader retirement can only reduce the requested-byte total.
struct PublicationBudget final
{
    PublicationBudget(const foundation::AllocationDomain& upstream, usize maximumBytes) noexcept
        : Upstream(upstream), MaximumBytes(maximumBytes), Domain(this, allocate, free)
    {
    }

    [[nodiscard]] Status TranslateFailure(Status status) noexcept
    {
        const bool hasDeniedAllocation = HasDeniedAllocation;
        HasDeniedAllocation = false;
        return status == Status::OutOfMemory && hasDeniedAllocation == true ? Status::BudgetExceeded : status;
    }

    const foundation::AllocationDomain& Upstream;
    const usize MaximumBytes;
    std::atomic<usize> ResidentBytes{0};
    bool HasDeniedAllocation{};
    foundation::AllocationDomain Domain;

private:
    static void* allocate(void* context, usize bytes, usize alignment) noexcept
    {
        PublicationBudget& budget = *static_cast<PublicationBudget*>(context);
        const usize residentBytes = budget.ResidentBytes.load(std::memory_order_relaxed);
        if (bytes > budget.MaximumBytes - residentBytes)
        {
            budget.HasDeniedAllocation = true;
            return nullptr;
        }
        budget.ResidentBytes.fetch_add(bytes, std::memory_order_relaxed);
        void* storageOrNull = budget.Upstream.TryAllocate(bytes, alignment);
        if (storageOrNull == nullptr)
        {
            budget.ResidentBytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
        return storageOrNull;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AllocationDomain callback ABI.
    static void free(void* context, void* pointer, usize bytes, usize alignment) noexcept
    {
        PublicationBudget& budget = *static_cast<PublicationBudget*>(context);
        budget.Upstream.Free(pointer, bytes, alignment);
        budget.ResidentBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
};
} // namespace ludus::localization::detail

namespace ludus::localization
{
namespace
{
using detail::CatalogAccess;
using detail::CatalogGeneration;
using detail::DomainCatalog;
using detail::PublicationBudget;
using foundation::OwnershipStatus;
using foundation::TryMakeShared;

std::atomic<uint64> gNextRequest{1};

Status MakeRequest(uint64& outRequest) noexcept
{
    uint64 request = gNextRequest.load(std::memory_order_relaxed);
    while (request != ~uint64{0})
    {
        if (gNextRequest.compare_exchange_weak(request, request + 1, std::memory_order_relaxed) == true)
        {
            outRequest = request;
            return Status::Ok;
        }
    }
    return Status::TokenExhausted;
}

Status ValidateSelection(const LocaleSelection& selection) noexcept
{
    if (selection.Domains.GetSize() > MAX_LOCALE_DOMAINS)
    {
        return Status::Limit;
    }
    if (selection.Domains.IsValid() == false || selection.Domains.IsEmpty() == true || selection.ReleaseRevision == 0 ||
        selection.PreferenceRevision == 0 || CatalogAccess::IsValidLocaleLabel(selection.Locale) == false)
    {
        return Status::Invalid;
    }
    for (usize i = 0; i < selection.Domains.GetSize(); ++i)
    {
        const std::string_view name = selection.Domains[i];
        if (CatalogAccess::IsValidDomain(name) == false)
        {
            return Status::Invalid;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (name == selection.Domains[j])
            {
                return Status::Invalid;
            }
        }
    }
    return Status::Ok;
}
} // namespace

LocalizationSnapshot& LocalizationSnapshot::operator=(LocalizationSnapshot other) noexcept
{
    mBudget.Swap(other.mBudget);
    mView.Swap(other.mView);
    return *this;
}

LocaleRequest LocalizationSnapshot::GetGeneration() const noexcept
{
    return mView.Get() == nullptr ? LocaleRequest{} : mView->Generation->Request;
}

uint64 LocalizationSnapshot::GetViewRevision() const noexcept
{
    return mView.Get() == nullptr ? 0 : mView->Revision;
}

uint64 LocalizationSnapshot::GetReleaseRevision() const noexcept
{
    return mView.Get() == nullptr ? 0 : mView->Generation->ReleaseRevision;
}

std::string_view LocalizationSnapshot::GetLocale() const& noexcept
{
    return mView.Get() == nullptr ? std::string_view{} : mView->Generation->Locale.GetView();
}

usize LocalizationSnapshot::GetMessageCount() const noexcept
{
    return mView.Get() == nullptr ? 0 : mView->MessageCount;
}

Status LocalizationSnapshot::GetText(usize index, ResolvedText& outText) const& noexcept
{
    if (index >= GetMessageCount())
    {
        return Status::NotFound;
    }
    outText = mView->Entries[index].Resolved;
    return Status::Ok;
}

LocalizationContext::LocalizationContext(usize maximumBytes, const foundation::AllocationDomain& domain) noexcept
    : mDomain(domain), mMaximumBytes(maximumBytes)
{
}

Status LocalizationContext::RequestLocaleChange(const LocaleSelection& selection, LocaleRequest& outRequest) noexcept
{
    const Status validation = ValidateSelection(selection);
    if (validation != Status::Ok)
    {
        return validation;
    }
    if (mBudget.Get() == nullptr && TryMakeShared(mDomain, mBudget, mDomain, mMaximumBytes) != OwnershipStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    foundation::SharedPtr<CatalogGeneration> candidate;
    if (TryMakeShared(mBudget->Domain, candidate) != OwnershipStatus::Ok)
    {
        return mBudget->TranslateFailure(Status::OutOfMemory);
    }
    const Status token = MakeRequest(candidate->Request.mValue);
    if (token != Status::Ok)
    {
        return token;
    }
    candidate->ReleaseRevision = selection.ReleaseRevision;
    candidate->PreferenceRevision = selection.PreferenceRevision;
    candidate->MissingCatalogCount = selection.Domains.GetSize();
    static_cast<void>(candidate->Locale.TryAssign(selection.Locale)); // The validated label fits.
    for (const std::string_view name : selection.Domains)
    {
        DomainCatalog domainCatalog;
        static_cast<void>(domainCatalog.Name.TryAssign(name));
        static_cast<void>(candidate->Catalogs.TryAdd(foundation::Move(domainCatalog))); // The validated count fits.
    }
    discardCandidate();
    mCandidate = foundation::Move(candidate);
    mRequest = mCandidate->Request;
    mProgress = {};
    outRequest = mRequest;
    return Status::Ok;
}

Status LocalizationContext::checkPending(LocaleRequest request) const noexcept
{
    if (request == LocaleRequest{} || request != mRequest)
    {
        return Status::StaleRequest;
    }
    switch (mProgress.Stage)
    {
        case LocaleChangeStage::LoadingCatalogs:
        case LocaleChangeStage::WaitingForView:
        case LocaleChangeStage::Ready:
            return Status::Ok;
        case LocaleChangeStage::Cancelled:
        case LocaleChangeStage::Failed:
            return mProgress.Failure.Result;
        case LocaleChangeStage::Published:
            return Status::StaleRequest;
    }
    return Status::Invalid;
}

Status LocalizationContext::checkCatalogCompletion(LocaleRequest request,
                                                   uint64 releaseRevision,
                                                   std::string_view domainName,
                                                   usize& outIndex) const noexcept
{
    const Status pending = checkPending(request);
    if (pending != Status::Ok)
    {
        return pending;
    }
    if (releaseRevision != mCandidate->ReleaseRevision || mProgress.Stage != LocaleChangeStage::LoadingCatalogs)
    {
        return Status::StaleRequest;
    }
    for (usize i = 0; i < mCandidate->Catalogs.GetSize(); ++i)
    {
        const DomainCatalog& domainCatalog = mCandidate->Catalogs[i];
        if (domainName == domainCatalog.Name.GetView() && domainCatalog.Package.IsValid() == false)
        {
            outIndex = i;
            return Status::Ok;
        }
    }
    return Status::Invalid;
}

Status LocalizationContext::SubmitCatalog(LocaleRequest request,
                                          uint64 releaseRevision,
                                          std::string_view domainName,
                                          foundation::BorrowedView<const uint8> bytes) noexcept
{
    usize index{};
    const Status pending = checkCatalogCompletion(request, releaseRevision, domainName, index);
    if (pending != Status::Ok)
    {
        return pending;
    }
    if (bytes.IsValid() == false)
    {
        return failCandidate(Status::Invalid);
    }
    Catalog catalog;
    Diagnostic diagnostic;
    const Status prepared = mBudget->TranslateFailure(
        PrepareCatalog({bytes.GetDataOrNull(), bytes.GetSize()}, mBudget->Domain, catalog, diagnostic));
    if (prepared != Status::Ok)
    {
        return failCandidate(prepared, diagnostic.Offset);
    }
    if (catalog.GetDomain() != domainName || catalog.GetLocale() != mCandidate->Locale.GetView())
    {
        return failCandidate(Status::Invalid);
    }
    if (mPublished.mView.Get() != nullptr)
    {
        const Catalog* previousOrNull = mPublished.mView->Generation->FindCatalogOrNull(domainName);
        if (previousOrNull != nullptr && CatalogAccess::HasSameSchema(*previousOrNull, catalog) == false)
        {
            return failCandidate(Status::SchemaMismatch);
        }
    }
    mCandidate->Catalogs[index].Package = foundation::Move(catalog);
    --mCandidate->MissingCatalogCount;
    if (mCandidate->MissingCatalogCount == 0)
    {
        mProgress.Stage = LocaleChangeStage::WaitingForView;
    }
    return Status::Ok;
}

Status LocalizationContext::FailCatalogAcquisition(LocaleRequest request,
                                                   uint64 releaseRevision,
                                                   std::string_view domainName,
                                                   Status failure) noexcept
{
    usize index{};
    const Status pending = checkCatalogCompletion(request, releaseRevision, domainName, index);
    if (pending != Status::Ok)
    {
        return pending;
    }
    if (failure == Status::Ok || failure == Status::NotReady || failure == Status::Cancelled ||
        failure == Status::StaleRequest)
    {
        return Status::Invalid;
    }
    return failCandidate(failure);
}

Status LocalizationContext::PollLocaleChange(LocaleRequest request, LocaleChangeProgress& outProgress) const noexcept
{
    if (request == LocaleRequest{} || request != mRequest)
    {
        return Status::StaleRequest;
    }
    outProgress = mProgress;
    return Status::Ok;
}

void LocalizationContext::discardCandidate() noexcept
{
    mCandidateView = {};
    mCandidate.Reset();
}

Status LocalizationContext::failCandidate(Status failure, usize offset) noexcept
{
    discardCandidate();
    mProgress = { .Stage = LocaleChangeStage::Failed, .Failure = {failure, offset} };
    return failure;
}

Status LocalizationContext::CancelLocaleChange(LocaleRequest request) noexcept
{
    const Status pending = checkPending(request);
    if (pending != Status::Ok)
    {
        return pending;
    }
    discardCandidate();
    mProgress = { .Stage = LocaleChangeStage::Cancelled, .Failure = {Status::Cancelled, 0} };
    return Status::Ok;
}

Status LocalizationContext::prepareView(const foundation::SharedPtr<const CatalogGeneration>& generation,
                                        uint64 viewRevision,
                                        foundation::BorrowedView<const VisibleMessage> messages,
                                        LocalizationSnapshot& outView) noexcept
{
    LocalizationSnapshot view;
    view.mBudget = mBudget;
    const Status prepared = mBudget->TranslateFailure(
        detail::PrepareLocalizedView(generation, viewRevision, messages, mBudget->Domain, view.mView));
    if (prepared == Status::Ok)
    {
        outView = foundation::Move(view);
    }
    return prepared;
}

Status LocalizationContext::PrepareCandidateView(LocaleRequest request,
                                                 uint64 viewRevision,
                                                 foundation::BorrowedView<const VisibleMessage> messages,
                                                 LocalizationSnapshot& outView) noexcept
{
    const Status pending = checkPending(request);
    if (pending != Status::Ok)
    {
        return pending;
    }
    if (mProgress.Stage == LocaleChangeStage::LoadingCatalogs)
    {
        return Status::NotReady;
    }
    mCandidateView = {};
    const Status prepared = prepareView(mCandidate, viewRevision, messages, mCandidateView);
    if (prepared != Status::Ok)
    {
        return failCandidate(prepared);
    }
    mProgress.Stage = LocaleChangeStage::Ready;
    outView = mCandidateView;
    return Status::Ok;
}

Status LocalizationContext::CommitLocaleChange(LocaleRequest request,
                                               const LocalizationSnapshot& preparedView,
                                               LocaleCommitRevisions currentRevisions) noexcept
{
    const Status pending = checkPending(request);
    if (pending != Status::Ok)
    {
        return pending;
    }
    if (currentRevisions.Preferences != mCandidate->PreferenceRevision)
    {
        return Status::StaleRequest;
    }
    if (mProgress.Stage != LocaleChangeStage::Ready || preparedView.mView.Get() != mCandidateView.mView.Get() ||
        currentRevisions.View != mCandidateView.GetViewRevision())
    {
        return Status::NotReady;
    }
    mPublished = foundation::Move(mCandidateView);
    mCandidate.Reset();
    mProgress.Stage = LocaleChangeStage::Published;
    return Status::Ok;
}

Status LocalizationContext::AcquireSnapshot(LocalizationSnapshot& outSnapshot) const noexcept
{
    if (mPublished.mView.Get() == nullptr)
    {
        return Status::NotReady;
    }
    outSnapshot = mPublished;
    return Status::Ok;
}

Status LocalizationContext::PrepareActiveView(uint64 viewRevision,
                                              foundation::BorrowedView<const VisibleMessage> messages,
                                              LocalizationSnapshot& outView) noexcept
{
    if (mPublished.mView.Get() == nullptr)
    {
        return Status::NotReady;
    }
    return prepareView(mPublished.mView->Generation, viewRevision, messages, outView);
}

bool LocalizationContext::IsCurrent(const LocalizationSnapshot& view) const noexcept
{
    const LocaleRequest generation = view.GetGeneration();
    return generation != LocaleRequest{} && generation == mPublished.GetGeneration();
}

usize LocalizationContext::GetResidentBytes() const noexcept
{
    return mBudget.Get() == nullptr ? 0 : mBudget->ResidentBytes.load(std::memory_order_relaxed);
}
} // namespace ludus::localization
