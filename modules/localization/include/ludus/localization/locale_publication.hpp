#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/localization/formatter.hpp>

#include <string_view>

namespace ludus::localization
{
/// Most resident catalog domains in one context generation.
inline constexpr usize MAX_LOCALE_DOMAINS = 32;
/// Most text entries in one prepared presentation, including repeated uses of a message.
inline constexpr usize MAX_VISIBLE_MESSAGES = 256;

namespace detail
{
struct PublicationBudget;
struct CatalogGeneration;
struct LocalizedView;
} // namespace detail

/// Process-local identity for one request, unique across contexts and issued by RequestLocaleChange.
class LocaleRequest final
{
public:
    /// Makes an empty request identity.
    constexpr LocaleRequest() noexcept = default;
    /// Compares two request identities, including empty ones.
    friend constexpr bool operator==(const LocaleRequest& left, const LocaleRequest& right) noexcept = default;

private:
    uint64 mValue{};
    friend class LocalizationContext;
};

/// @brief The application-selected catalog set for a locale switch or reload.
///
/// Borrowed fields are copied by RequestLocaleChange. All labels are exact,
/// case-sensitive cooker labels. The acquiring owner chooses the effective locale
/// and verifies package membership and digests against its completion manifest.
struct LocaleSelection final
{
    std::string_view Locale;                                  ///< Effective UI locale to load in every domain.
    foundation::BorrowedView<const std::string_view> Domains; ///< Distinct required domains, in diagnostic order.
    uint64 ReleaseRevision{};                                 ///< Nonzero identity of the verified package set.
    uint64 PreferenceRevision{};                              ///< Nonzero revision of the application preferences.
};

/// One required text entry of the visible screen, in presentation order. Inputs are read only during preparation.
struct VisibleMessage final
{
    MessageDescriptor Descriptor;                            ///< Exact key and schema, usually a generated descriptor.
    foundation::BorrowedView<const ArgumentValue> Arguments; ///< Values for this occurrence, empty for a static entry.
};

/// Preparation and publication stages for the most recent accepted request.
enum class LocaleChangeStage : uint8
{
    LoadingCatalogs, ///< Waiting for verified package completions.
    WaitingForView,  ///< All required catalogs are loaded; visible text preparation can start.
    Ready,           ///< A complete candidate text view can be used to prepare application presentation.
    Published,       ///< The candidate and its view were committed together.
    Cancelled,       ///< The owner cancelled the request.
    Failed,          ///< A required catalog or visible preparation failed.
};

/// Poll result for the latest request, including terminal failures.
struct LocaleChangeProgress final
{
    LocaleChangeStage Stage{LocaleChangeStage::LoadingCatalogs}; ///< Current stage of the request.
    Diagnostic Failure; ///< Terminal failure status and cooked-byte offset, if available.
};

/// Current application revisions at the frame boundary where the prepared view would become visible.
struct LocaleCommitRevisions final
{
    uint64 View{};        ///< Current revision of the visible set, arguments and layout inputs.
    uint64 Preferences{}; ///< Current revision of the application's localization preferences.
};

/// @brief Immutable catalog-generation and prepared-text lease.
///
/// A copy extends the lifetime of every borrowed text and provider label. Copies
/// may be handed to other threads with caller synchronization. A lease can
/// outlive its context, but its upstream allocation domain must still be alive.
/// A default-constructed or moved-from lease has no generation.
class LocalizationSnapshot final
{
public:
    /// Makes an empty lease.
    LocalizationSnapshot() noexcept = default;
    /// Releases this lease and any bytes no longer retained by another owner.
    ~LocalizationSnapshot() noexcept = default;
    /// Shares immutable storage without allocating.
    LocalizationSnapshot(const LocalizationSnapshot&) noexcept = default;
    /// Takes the lease and empties other.
    LocalizationSnapshot(LocalizationSnapshot&& other) noexcept = default;
    /// Replaces the lease by copying or moving other. Copying does not allocate.
    LocalizationSnapshot& operator=(LocalizationSnapshot other) noexcept;

    /// Returns the request that created these catalogs, or an empty request for an empty lease.
    [[nodiscard]] LocaleRequest GetGeneration() const noexcept;
    /// Returns the caller's visible-input revision, or zero for an empty lease.
    [[nodiscard]] uint64 GetViewRevision() const noexcept;
    /// Returns the verified release revision supplied at request time, or zero for an empty lease.
    [[nodiscard]] uint64 GetReleaseRevision() const noexcept;
    /// Returns the effective locale label, borrowed while this generation is retained.
    [[nodiscard]] std::string_view GetLocale() const& noexcept;
    /// Deleted because a temporary lease may be the last owner of the label.
    [[nodiscard]] std::string_view GetLocale() const&& = delete;
    /// Number of prepared entries; zero for an empty or text-free view.
    [[nodiscard]] usize GetMessageCount() const noexcept;
    /// Reads a prepared entry in input order without allocating or formatting. Invalid index preserves outText.
    [[nodiscard]] Status GetText(usize index, ResolvedText& outText) const& noexcept;
    /// Deleted because a temporary lease may be the last owner of the text.
    [[nodiscard]] Status GetText(usize index, ResolvedText& outText) const&& = delete;

private:
    // The budget outlives the controller and payload allocated through its domain.
    foundation::SharedPtr<detail::PublicationBudget> mBudget;
    foundation::SharedPtr<const detail::LocalizedView> mView;
    friend class LocalizationContext;
};

/// @brief Serial owner of one session or local player's localization publication.
///
/// Call every method on the owning thread, including acquisition and destruction.
/// Marshal asynchronous completions to that thread using their request and release
/// identities. The owner does no I/O or waiting. A new accepted request supersedes
/// any pending request. Readers retain their own immutable snapshots.
class LocalizationContext final
{
public:
    /// @brief Creates an empty context. No allocation occurs until the first request.
    /// @param maximumBytes Cap on requested bytes from this context's storage domain,
    /// including active, candidate, reader-retained generations and Ludus preparation
    /// scratch. The fixed budget control block, upstream allocator overhead, caller's
    /// acquisition/presentation bytes and ICU's private heap are outside the cap.
    /// @param domain Upstream allocator; it must outlive the context and every snapshot.
    explicit LocalizationContext(usize maximumBytes, const foundation::AllocationDomain& domain) noexcept;
    /// Releases the context's owners. Other leases remain valid.
    ~LocalizationContext() noexcept = default;
    /// A publication owner is unique to its session.
    LocalizationContext(const LocalizationContext&) = delete;
    /// A publication owner cannot be reassigned.
    LocalizationContext& operator=(const LocalizationContext&) = delete;

    /// @brief Starts loading a verified release/locale. On failure, preserves the current request and outRequest.
    /// @return Ok, Invalid for malformed labels, empty/duplicate domains or zero revisions,
    /// Limit for too many domains, TokenExhausted, BudgetExceeded or OutOfMemory.
    [[nodiscard]] Status RequestLocaleChange(const LocaleSelection& selection, LocaleRequest& outRequest) noexcept;
    /// @brief Validates and copies the completed bytes for one missing domain of the request.
    ///
    /// The owner calls this after verifying the package against releaseRevision's manifest.
    /// Bytes and domainName are borrowed only for this call. Stale request/revision or a
    /// duplicate/undeclared domain completion preserves transaction state. Malformed bytes,
    /// mismatch with the declared domain/locale, budget failure or a changed schema in a
    /// currently published domain fail the request and preserve the active snapshot.
    [[nodiscard]] Status SubmitCatalog(LocaleRequest request,
                                       uint64 releaseRevision,
                                       std::string_view domainName,
                                       foundation::BorrowedView<const uint8> bytes) noexcept;
    /// Reports acquisition failure for the current loading request, matching release and unfilled domain.
    /// Returns the supplied terminal failure once recorded. A stale or duplicate completion preserves the request.
    [[nodiscard]] Status FailCatalogAcquisition(LocaleRequest request,
                                                uint64 releaseRevision,
                                                std::string_view domainName,
                                                Status failure) noexcept;
    /// Copies the latest request's progress. StaleRequest preserves outProgress.
    [[nodiscard]] Status PollLocaleChange(LocaleRequest request, LocaleChangeProgress& outProgress) const noexcept;
    /// Cancels the current pending request and releases its owners. A published request returns StaleRequest.
    [[nodiscard]] Status CancelLocaleChange(LocaleRequest request) noexcept;

    /// @brief Resolves all required text from the candidate catalogs, using ICU for dynamic entries.
    ///
    /// Pass the complete visible screen as one ordered list. Use a nonzero revision
    /// that changes with the visible set or any of its arguments/layout inputs. A
    /// refresh replaces the previous preview. An empty list explicitly declares a
    /// text-free screen. No input is retained. On success outView can be used to
    /// prepare application-owned layout/resources. A preparation failure preserves
    /// outView and the active snapshot and fails the request.
    [[nodiscard]] Status PrepareCandidateView(LocaleRequest request,
                                              uint64 viewRevision,
                                              foundation::BorrowedView<const VisibleMessage> messages,
                                              LocalizationSnapshot& outView) noexcept;
    /// @brief Publishes the complete catalog set and exact prepared view at a frame boundary.
    ///
    /// Pass the latest candidate view and current input revisions only once every
    /// required application layout/resource using that view is ready. A changed
    /// preference revision requires a new request. A changed visible revision or
    /// superseded preview returns NotReady. All failures preserve the active lease.
    [[nodiscard]] Status CommitLocaleChange(LocaleRequest request,
                                            const LocalizationSnapshot& preparedView,
                                            LocaleCommitRevisions currentRevisions) noexcept;
    /// Acquires the catalog and visible-text generation together, without allocating.
    /// Returns NotReady and preserves outSnapshot before the first commit.
    [[nodiscard]] Status AcquireSnapshot(LocalizationSnapshot& outSnapshot) const noexcept;
    /// Prepares an additional view from the current catalogs, without changing the published view.
    /// An unopened view must prepare successfully and pass IsCurrent at the point it becomes visible.
    /// Inputs follow PrepareCandidateView's contract. Every failure preserves outView.
    [[nodiscard]] Status PrepareActiveView(uint64 viewRevision,
                                           foundation::BorrowedView<const VisibleMessage> messages,
                                           LocalizationSnapshot& outView) noexcept;
    /// True if the view was built from the currently published catalog generation in this context.
    /// Compare its own visible-input revision separately before presenting it.
    [[nodiscard]] bool IsCurrent(const LocalizationSnapshot& view) const noexcept;
    /// Samples charged requested bytes, including reader-retained data after replacement.
    [[nodiscard]] usize GetResidentBytes() const noexcept;

private:
    const foundation::AllocationDomain& mDomain;
    usize mMaximumBytes;
    foundation::SharedPtr<detail::PublicationBudget> mBudget;
    foundation::SharedPtr<detail::CatalogGeneration> mCandidate;
    LocalizationSnapshot mCandidateView;
    LocalizationSnapshot mPublished;
    LocaleRequest mRequest;
    LocaleChangeProgress mProgress;

    [[nodiscard]] Status checkPending(LocaleRequest request) const noexcept;
    [[nodiscard]] Status checkCatalogCompletion(LocaleRequest request,
                                                uint64 releaseRevision,
                                                std::string_view domainName,
                                                usize& outIndex) const noexcept;
    void discardCandidate() noexcept;
    Status failCandidate(Status failure, usize offset = 0) noexcept;
    [[nodiscard]] Status prepareView(const foundation::SharedPtr<const detail::CatalogGeneration>& generation,
                                     uint64 viewRevision,
                                     foundation::BorrowedView<const VisibleMessage> messages,
                                     LocalizationSnapshot& outView) noexcept;
};
} // namespace ludus::localization
