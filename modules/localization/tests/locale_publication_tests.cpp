#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/localization/locale_publication.hpp>

#include "fixture.hpp"

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#if LUDUS_LOCALIZATION_TEST_HAS_ICU
#    include <ludus/foundation/unicode/unicode.hpp>
#endif

using namespace ludus::localization;
using ludus::foundation::AllocationDomain;
using ludus::foundation::BorrowedView;
using ludus::foundation::GetAllocationStatistics;
using ludus::foundation::GetSystemAllocationDomain;

namespace
{
namespace release = localization_release_fixture;
namespace corpus = localization_corpus;
namespace threading = ludus::foundation::threading;

constexpr usize STORAGE_BUDGET = usize{1024} * 1024;
constexpr std::string_view UI_DOMAIN = "game/ui";
constexpr std::string_view RELEASE_DOMAIN = "test/l1";
constexpr std::string_view UI_DOMAINS[] = {UI_DOMAIN};
constexpr std::string_view RELEASE_DOMAINS[] = {RELEASE_DOMAIN};
constexpr std::string_view SCREEN_DOMAINS[] = {UI_DOMAIN, RELEASE_DOMAIN};
constexpr VisibleMessage PLAY[] = {{corpus::kMenuPlay, {}}};
constexpr VisibleMessage START[] = {{release::kMenuStart, {}}};
constexpr VisibleMessage STATIC_SCREEN[] = {{corpus::kMenuPlay, {}}, {release::kMenuStart, {}}};

LocaleRequest Request(LocalizationContext& inoutContext,
                      std::string_view locale,
                      uint64 releaseRevision,
                      uint64 preferenceRevision,
                      BorrowedView<const std::string_view> domains = UI_DOMAINS)
{
    LocaleRequest request{};
    REQUIRE(inoutContext.RequestLocaleChange({locale, domains, releaseRevision, preferenceRevision}, request) ==
            Status::Ok);
    REQUIRE(request != LocaleRequest{});
    return request;
}

LocaleChangeStage GetStage(const LocalizationContext& context, LocaleRequest request, Status status = Status::Ok)
{
    LocaleChangeProgress progress;
    REQUIRE(context.PollLocaleChange(request, progress) == Status::Ok);
    REQUIRE(progress.Failure.Result == status);
    return progress.Stage;
}

ResolvedText GetText(const LocalizationSnapshot& view, usize index = 0)
{
    ResolvedText result;
    REQUIRE(view.GetText(index, result) == Status::Ok);
    return result;
}

LocalizationSnapshot Acquire(const LocalizationContext& context)
{
    LocalizationSnapshot view;
    REQUIRE(context.AcquireSnapshot(view) == Status::Ok);
    return view;
}

LocalizationSnapshot PrepareAndCommit(LocalizationContext& inoutContext,
                                      LocaleRequest request,
                                      BorrowedView<const VisibleMessage> messages,
                                      uint64 preferenceRevision)
{
    LocalizationSnapshot view;
    REQUIRE(inoutContext.PrepareCandidateView(request, 1, messages, view) == Status::Ok);
    REQUIRE(inoutContext.CommitLocaleChange(request, view, { .View = 1, .Preferences = preferenceRevision }) ==
            Status::Ok);
    return view;
}

struct RecordingHeap final
{
    bool ShouldReject{};
    AllocationDomain Domain{this, Allocate, Free};

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        return static_cast<RecordingHeap*>(context)->ShouldReject == true
                   ? nullptr
                   : GetSystemAllocationDomain().TryAllocate(bytes, alignment);
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AllocationDomain callback ABI.
    static void Free(void*, void* pointer, usize bytes, usize alignment) noexcept
    {
        GetSystemAllocationDomain().Free(pointer, bytes, alignment);
    }
};
} // namespace

TEST_CASE("Locale requests reject late completions and keep the active screen through cancellation and failure",
          "[localization][publication]")
{
    LocalizationContext context(STORAGE_BUDGET, GetSystemAllocationDomain());
    LocalizationSnapshot output;
    REQUIRE(context.AcquireSnapshot(output) == Status::NotReady);
    const LocaleRequest initial = Request(context, "en-US", 1, 1);
    REQUIRE(context.SubmitCatalog(initial, 1, UI_DOMAIN, kCorpusEnUs) == Status::Ok);
    const LocalizationSnapshot original = PrepareAndCommit(context, initial, PLAY, 1);
    const usize resident = context.GetResidentBytes();

    const LocaleRequest cancelled = Request(context, "fr-FR", 1, 2, SCREEN_DOMAINS);
    REQUIRE(context.SubmitCatalog(cancelled, 1, UI_DOMAIN, kCorpusFrFr) == Status::Ok);
    REQUIRE(context.FailCatalogAcquisition(cancelled, 1, UI_DOMAIN, Status::NotFound) == Status::Invalid);
    REQUIRE(GetStage(context, cancelled) == LocaleChangeStage::LoadingCatalogs);
    REQUIRE(context.PrepareCandidateView(cancelled, 2, STATIC_SCREEN, output) == Status::NotReady);
    REQUIRE(context.CancelLocaleChange(cancelled) == Status::Ok);
    REQUIRE(GetStage(context, cancelled, Status::Cancelled) == LocaleChangeStage::Cancelled);
    REQUIRE(context.SubmitCatalog(cancelled, 1, RELEASE_DOMAIN, kReleaseFrFr) == Status::Cancelled);
    REQUIRE(context.GetResidentBytes() == resident);
    REQUIRE(context.IsCurrent(original) == true);

    const LocaleRequest superseded = Request(context, "fr-FR", 1, 3);
    const LocaleRequest latest = Request(context, "fr-FR", 2, 4);
    REQUIRE(context.SubmitCatalog(superseded, 1, UI_DOMAIN, kCorpusFrFr) == Status::StaleRequest);
    REQUIRE(context.SubmitCatalog(latest, 1, UI_DOMAIN, kCorpusFrFr) == Status::StaleRequest);
    REQUIRE(context.FailCatalogAcquisition(latest, 1, UI_DOMAIN, Status::NotFound) == Status::StaleRequest);
    const uint8 malformed[] = {0};
    REQUIRE(context.SubmitCatalog(latest, 2, UI_DOMAIN, malformed) == Status::Invalid);
    REQUIRE(GetStage(context, latest, Status::Invalid) == LocaleChangeStage::Failed);
    REQUIRE(context.IsCurrent(original) == true);
    REQUIRE(context.GetResidentBytes() == resident);

    const LocaleRequest missing = Request(context, "fr-FR", 2, 4);
    REQUIRE(context.FailCatalogAcquisition(missing, 2, UI_DOMAIN, Status::NotFound) == Status::NotFound);
    REQUIRE(GetStage(context, missing, Status::NotFound) == LocaleChangeStage::Failed);
    const LocaleRequest wrongLocale = Request(context, "fr-FR", 2, 4);
    REQUIRE(context.SubmitCatalog(wrongLocale, 2, UI_DOMAIN, kCorpusEnUs) == Status::Invalid);
    REQUIRE(GetStage(context, wrongLocale, Status::Invalid) == LocaleChangeStage::Failed);
    REQUIRE(context.IsCurrent(original) == true);

    const LocaleRequest ready = Request(context, "fr-FR", 2, 4);
    REQUIRE(context.SubmitCatalog(ready, 2, UI_DOMAIN, kCorpusFrFr) == Status::Ok);
    REQUIRE(context.PrepareCandidateView(ready, 2, PLAY, output) == Status::Ok);
    REQUIRE(context.CancelLocaleChange(ready) == Status::Ok);
    REQUIRE(context.CommitLocaleChange(ready, output, { .View = 2, .Preferences = 4 }) == Status::Cancelled);
    REQUIRE(GetText(output).Text == "Jouer");
    output = {};
    REQUIRE(context.GetResidentBytes() == resident);
    REQUIRE(context.IsCurrent(original) == true);
}

TEST_CASE("Locale publication admits old, candidate and retained presentation bytes within one budget",
          "[localization][publication]")
{
    usize exactOverlap{};
    {
        LocalizationContext measurement(STORAGE_BUDGET, GetSystemAllocationDomain());
        const LocaleRequest initial = Request(measurement, "en-US", 1, 1);
        REQUIRE(measurement.SubmitCatalog(initial, 1, UI_DOMAIN, kCorpusEnUs) == Status::Ok);
        static_cast<void>(PrepareAndCommit(measurement, initial, PLAY, 1));
        const LocaleRequest replacement = Request(measurement, "fr-FR", 1, 2);
        REQUIRE(measurement.SubmitCatalog(replacement, 1, UI_DOMAIN, kCorpusFrFr) == Status::Ok);
        LocalizationSnapshot preview;
        REQUIRE(measurement.PrepareCandidateView(replacement, 2, PLAY, preview) == Status::Ok);
        exactOverlap = measurement.GetResidentBytes();
    }
    for (const bool isExact : {false, true})
    {
        RecordingHeap heap;
        LocalizationSnapshot oldest;
        {
            const usize budget = isExact == true ? exactOverlap : exactOverlap - 1;
            LocalizationContext context(budget, heap.Domain);
            const LocaleRequest initial = Request(context, "en-US", 1, 1);
            REQUIRE(context.SubmitCatalog(initial, 1, UI_DOMAIN, kCorpusEnUs) == Status::Ok);
            oldest = PrepareAndCommit(context, initial, PLAY, 1);
            const LocaleRequest replacement = Request(context, "fr-FR", 1, 2);
            REQUIRE(context.SubmitCatalog(replacement, 1, UI_DOMAIN, kCorpusFrFr) == Status::Ok);
            const usize originalPlusLoaded = context.GetResidentBytes();
            REQUIRE(originalPlusLoaded < exactOverlap);
            LocalizationSnapshot preview = oldest;
            const Status prepared = context.PrepareCandidateView(replacement, 2, PLAY, preview);
            REQUIRE(prepared == (isExact == true ? Status::Ok : Status::BudgetExceeded));
            if (isExact == true)
            {
                REQUIRE(context.GetResidentBytes() == exactOverlap);
                REQUIRE(context.CommitLocaleChange(replacement,
                                                   preview,
                                                   {
                                                       .View = 2,
                                                       .Preferences = 2,
                                                   }) == Status::Ok);
                // The active and reader-retained generation consume the full budget.
                REQUIRE(context.GetResidentBytes() == exactOverlap);
                LocaleRequest rejected = replacement;
                REQUIRE(context.RequestLocaleChange({"en-US", UI_DOMAINS, 1, 3}, rejected) == Status::BudgetExceeded);
                REQUIRE(rejected == replacement);
                REQUIRE(context.IsCurrent(preview) == true);
                oldest = {};
                const LocaleRequest retry = Request(context, "en-US", 1, 3);
                REQUIRE(context.SubmitCatalog(retry, 1, UI_DOMAIN, kCorpusEnUs) == Status::Ok);
                oldest = PrepareAndCommit(context, retry, PLAY, 3);
            }
            else
            {
                REQUIRE(GetStage(context, replacement, Status::BudgetExceeded) == LocaleChangeStage::Failed);
                REQUIRE(context.IsCurrent(preview) == true);
                REQUIRE(context.GetResidentBytes() < originalPlusLoaded);
                heap.ShouldReject = true;
                LocaleRequest rejected = replacement;
                REQUIRE(context.RequestLocaleChange({"fr-FR", UI_DOMAINS, 1, 3}, rejected) == Status::OutOfMemory);
                REQUIRE(rejected == replacement);
            }
        }
        REQUIRE(GetAllocationStatistics(heap.Domain).LiveBlocks > 0);
        // Release the last lease on a reader thread after the owner has gone away.
        bool hasReadRetainedText = false;
        threading::Thread reader;
        REQUIRE(reader.Start([&oldest, &hasReadRetainedText]() noexcept {
            ResolvedText retained;
            hasReadRetainedText = oldest.GetText(0, retained) == Status::Ok && retained.Text == "Play";
            oldest = {};
        }) == threading::ThreadStatus::Ok);
        REQUIRE(reader.Join() == threading::ThreadStatus::Ok);
        REQUIRE(hasReadRetainedText == true);
        REQUIRE(GetAllocationStatistics(heap.Domain).LiveBlocks == 0);
    }
}

TEST_CASE("A complete visible screen and its locale publish together across multiple domains",
          "[localization][publication]")
{
    LocalizationContext context(STORAGE_BUDGET, GetSystemAllocationDomain());
    const LocaleRequest initial = Request(context, "en-US", 1, 1, SCREEN_DOMAINS);
    REQUIRE(context.SubmitCatalog(initial, 1, UI_DOMAIN, kCorpusEnUs) == Status::Ok);
    REQUIRE(context.SubmitCatalog(initial, 1, RELEASE_DOMAIN, kReleaseEnUs) == Status::Ok);
    const LocalizationSnapshot original = PrepareAndCommit(context, initial, STATIC_SCREEN, 1);
    CHECK(original.GetLocale() == "en-US");
    CHECK(GetText(original, 0).Text == "Play");
    CHECK(GetText(original, 1).Text == "Start");

    LocalizationSnapshot hidden;
    REQUIRE(context.PrepareActiveView(2, START, hidden) == Status::Ok);
    REQUIRE(context.IsCurrent(hidden) == true);
    const LocaleRequest change = Request(context, "fr-FR", 1, 2, SCREEN_DOMAINS);
    REQUIRE(context.SubmitCatalog(change, 1, RELEASE_DOMAIN, kReleaseFrFr) == Status::Ok);
    REQUIRE(context.CommitLocaleChange(change, original, { .View = 2, .Preferences = 2 }) == Status::NotReady);
    REQUIRE(context.SubmitCatalog(change, 1, UI_DOMAIN, kCorpusFrFr) == Status::Ok);
    REQUIRE(GetStage(context, change) == LocaleChangeStage::WaitingForView);
    REQUIRE(context.CommitLocaleChange(change, original, { .View = 2, .Preferences = 2 }) == Status::NotReady);

    LocalizationSnapshot firstPreview;
    REQUIRE(context.PrepareCandidateView(change, 2, STATIC_SCREEN, firstPreview) == Status::Ok);
    REQUIRE(GetStage(context, change) == LocaleChangeStage::Ready);
    REQUIRE(context.CommitLocaleChange(change, firstPreview, { .View = 3, .Preferences = 2 }) == Status::NotReady);
    REQUIRE(context.CommitLocaleChange(change, firstPreview, { .View = 2, .Preferences = 3 }) == Status::StaleRequest);
    LocalizationSnapshot preview;
    REQUIRE(context.PrepareCandidateView(change, 3, STATIC_SCREEN, preview) == Status::Ok);
    REQUIRE(context.CommitLocaleChange(change, firstPreview, { .View = 3, .Preferences = 2 }) == Status::NotReady);
    CHECK(context.IsCurrent(Acquire(context)) == true);
    CHECK(Acquire(context).GetGeneration() == initial);
    CHECK(context.IsCurrent(preview) == false);
    REQUIRE(context.CommitLocaleChange(change, preview, { .View = 3, .Preferences = 2 }) == Status::Ok);

    const LocalizationSnapshot published = Acquire(context);
    CHECK(published.GetGeneration() == change);
    CHECK(published.GetViewRevision() == 3);
    CHECK(published.GetLocale() == "fr-FR");
    CHECK(published.GetMessageCount() == 2);
    ResolvedText untouched{"retained", Origin::Source, "en-US"};
    REQUIRE(published.GetText(published.GetMessageCount(), untouched) == Status::NotFound);
    CHECK(untouched.Text == "retained");
    CHECK(GetText(published, 0).Text == "Jouer");
    CHECK(GetText(published, 1).Text == "Démarrer");
    CHECK(GetText(original, 0).Text == "Play");
    CHECK(context.IsCurrent(hidden) == false);
    REQUIRE(context.PrepareActiveView(4, START, hidden) == Status::Ok);
    REQUIRE(context.IsCurrent(hidden) == true);
    CHECK(GetText(hidden).Text == "Démarrer");
    CHECK(GetStage(context, change) == LocaleChangeStage::Published);
}

TEST_CASE("Same-locale reload rebinds the cooked release and preserves other contexts and retained leases",
          "[localization][publication]")
{
    LocalizationContext first(STORAGE_BUDGET, GetSystemAllocationDomain());
    LocalizationContext second(STORAGE_BUDGET, GetSystemAllocationDomain());
    const LocaleRequest french = Request(first, "fr-FR", 1, 1, RELEASE_DOMAINS);
    const LocaleRequest english = Request(second, "en-US", 1, 1, RELEASE_DOMAINS);
    REQUIRE(french != english);
    REQUIRE(second.SubmitCatalog(french, 1, RELEASE_DOMAIN, kReleaseFrFr) == Status::StaleRequest);
    REQUIRE(first.SubmitCatalog(french, 1, RELEASE_DOMAIN, kReleaseFrFr) == Status::Ok);
    REQUIRE(second.SubmitCatalog(english, 1, RELEASE_DOMAIN, kReleaseEnUs) == Status::Ok);
    const LocalizationSnapshot oldFrench = PrepareAndCommit(first, french, START, 1);
    const LocalizationSnapshot steadyEnglish = PrepareAndCommit(second, english, START, 1);
    REQUIRE(first.IsCurrent(steadyEnglish) == false);

    const LocaleRequest reload = Request(first, "fr-FR", 2, 1, RELEASE_DOMAINS);
    REQUIRE(first.SubmitCatalog(reload, 2, RELEASE_DOMAIN, kReloadedReleaseFrFr) == Status::Ok);
    const LocalizationSnapshot reloaded = PrepareAndCommit(first, reload, START, 1);
    CHECK(reloaded.GetReleaseRevision() == 2);
    CHECK(reloaded.GetLocale() == "fr-FR");
    CHECK(GetText(reloaded).Text == "Crédits");
    CHECK(GetText(oldFrench).Text == "Démarrer");
    CHECK(GetText(steadyEnglish).Text == "Start");
    CHECK(second.IsCurrent(steadyEnglish) == true);

    // The changed schema is on a message outside the visible view.
    const LocaleRequest changedSchema = Request(first, "fr-FR", 3, 1, RELEASE_DOMAINS);
    REQUIRE(first.SubmitCatalog(changedSchema, 3, RELEASE_DOMAIN, kIncompatibleReleaseFrFr) == Status::SchemaMismatch);
    CHECK(GetStage(first, changedSchema, Status::SchemaMismatch) == LocaleChangeStage::Failed);
    CHECK(first.IsCurrent(reloaded) == true);
}

TEST_CASE("Required dynamic text is formatted from the prepared revision before publication",
          "[localization][publication]")
{
    LocalizationContext context(STORAGE_BUDGET, GetSystemAllocationDomain());
    const LocaleRequest initial = Request(context, "en-US", 1, 1, RELEASE_DOMAINS);
    REQUIRE(context.SubmitCatalog(initial, 1, RELEASE_DOMAIN, kReleaseEnUs) == Status::Ok);
    const LocalizationSnapshot original = PrepareAndCommit(context, initial, START, 1);
    const LocaleRequest french = Request(context, "fr-CA", 1, 2, RELEASE_DOMAINS);
    REQUIRE(context.SubmitCatalog(french, 1, RELEASE_DOMAIN, kReleaseFrCa) == Status::Ok);
    const ArgumentValue lives[] = {MakeCountArgument("lives", 1)};
    const VisibleMessage messages[] = {{release::kMenuStart, {}}, {release::kHudLives, lives}};
    LocalizationSnapshot preview = original;
#if LUDUS_LOCALIZATION_TEST_HAS_ICU
    using namespace ludus::foundation::unicode;
    if (IsUnicodeHostInitialized() == false)
    {
        REQUIRE(InitializeUnicodeHost(GetSystemAllocationDomain()) == UnicodeStatus::Ok);
    }
    REQUIRE(context.PrepareCandidateView(french, 2, messages, preview) == Status::Ok);
    CHECK(GetText(preview, 1).Text == "1 vie");
    const ArgumentValue changedLives[] = {MakeCountArgument("lives", 2)};
    const VisibleMessage changed[] = {{release::kMenuStart, {}}, {release::kHudLives, changedLives}};
    REQUIRE(context.CommitLocaleChange(french, preview, { .View = 3, .Preferences = 2 }) == Status::NotReady);
    REQUIRE(context.PrepareCandidateView(french, 3, changed, preview) == Status::Ok);
    REQUIRE(context.CommitLocaleChange(french, preview, { .View = 3, .Preferences = 2 }) == Status::Ok);
    const LocalizationSnapshot published = Acquire(context);
    CHECK(published.GetLocale() == "fr-CA");
    const ResolvedText text = GetText(published, 1);
    CHECK(text.Text == "2 vies");
    CHECK(text.ProviderLocale == "fr-FR");
    CHECK(text.Provenance == Origin::Fallback);

    const LocaleRequest corruptInput = Request(context, "fr-CA", 1, 3, RELEASE_DOMAINS);
    REQUIRE(context.SubmitCatalog(corruptInput, 1, RELEASE_DOMAIN, kReleaseFrCa) == Status::Ok);
    const VisibleMessage invalid[] = {{release::kMenuStart, {}}, {release::kHudLives, {}}};
    REQUIRE(context.PrepareCandidateView(corruptInput, 4, invalid, preview) == Status::InvalidArguments);
    REQUIRE(GetStage(context, corruptInput, Status::InvalidArguments) == LocaleChangeStage::Failed);
    CHECK(preview.GetGeneration() == french);
    CHECK(context.IsCurrent(published) == true);
#else
    REQUIRE(context.PrepareCandidateView(french, 2, messages, preview) == Status::BackendUnavailable);
    REQUIRE(GetStage(context, french, Status::BackendUnavailable) == LocaleChangeStage::Failed);
    CHECK(preview.GetGeneration() == initial);
    CHECK(context.IsCurrent(original) == true);
#endif

    // Both catalog sets fit, but this budget cannot also admit dynamic output scratch.
    LocalizationContext tight(MAX_FORMATTED_BYTES, GetSystemAllocationDomain());
    const LocaleRequest tightInitial = Request(tight, "en-US", 1, 1, RELEASE_DOMAINS);
    REQUIRE(tight.SubmitCatalog(tightInitial, 1, RELEASE_DOMAIN, kReleaseEnUs) == Status::Ok);
    const LocalizationSnapshot steady = PrepareAndCommit(tight, tightInitial, START, 1);
    const LocaleRequest tightChange = Request(tight, "fr-CA", 1, 2, RELEASE_DOMAINS);
    REQUIRE(tight.SubmitCatalog(tightChange, 1, RELEASE_DOMAIN, kReleaseFrCa) == Status::Ok);
    REQUIRE(tight.PrepareCandidateView(tightChange, 2, messages, preview) == Status::BudgetExceeded);
    CHECK(tight.IsCurrent(steady) == true);
}
