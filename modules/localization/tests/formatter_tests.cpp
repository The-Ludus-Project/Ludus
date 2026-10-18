#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/localization/catalog.hpp>
#include <ludus/localization/formatter.hpp>

#include "fixture.hpp"

#include <limits>
#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#if LUDUS_LOCALIZATION_TEST_HAS_ICU
#    include <ludus/foundation/unicode/unicode.hpp>
#endif

// L2 formatter contract. Fixtures are the production release cooker's packages
// (tests/generate_fixture.py); the corpus cases are modules/localization/corpus/expectations.json.

using namespace ludus::localization;
using ludus::foundation::AllocationDomain;
using ludus::foundation::Array;
using ludus::foundation::float64;
using ludus::foundation::GetSystemAllocationDomain;

namespace
{
namespace corpus = localization_corpus;
namespace cases = localization_corpus_cases;
namespace release = localization_release_fixture;

constexpr usize OUTPUT_BYTES = 512;
constexpr std::string_view SENTINEL = "sentinel";

Catalog Load(std::span<const uint8> bytes)
{
    Catalog result;
    Diagnostic diagnostic;
    REQUIRE(PrepareCatalog(bytes, GetSystemAllocationDomain(), result, diagnostic) == Status::Ok);
    return result;
}

MessageBinding Bind(const Catalog& catalog, const MessageDescriptor& descriptor)
{
    MessageBinding binding;
    REQUIRE(catalog.BindMessage(descriptor, binding) == Status::Ok);
    return binding;
}

/// Output buffer pre-filled with a sentinel so failures can prove it is unchanged.
struct Output final
{
    char Bytes[OUTPUT_BYTES]{};

    Output() noexcept
    {
        Reset();
    }
    void Reset() noexcept
    {
        for (usize i = 0; i < OUTPUT_BYTES; ++i)
        {
            Bytes[i] = SENTINEL[i % SENTINEL.size()];
        }
    }
    [[nodiscard]] bool IsUnchanged() const noexcept
    {
        for (usize i = 0; i < OUTPUT_BYTES; ++i)
        {
            if (Bytes[i] != SENTINEL[i % SENTINEL.size()])
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] std::span<char> GetSpan(usize size = OUTPUT_BYTES) noexcept
    {
        return {Bytes, size};
    }
    [[nodiscard]] std::string_view GetText(const FormatResult& result) const noexcept
    {
        return {Bytes, result.Written};
    }
};

struct FailingHeap final
{
    usize Attempts{};
    AllocationDomain Domain{this, Allocate, Free};

    static void* Allocate(void* context, usize /*bytes*/, usize /*alignment*/) noexcept
    {
        ++static_cast<FailingHeap*>(context)->Attempts;
        return nullptr;
    }
    // AllocationDomain callback ABI defines the adjacent context/pointer arguments.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void Free(void* /*context*/, void* /*pointer*/, usize /*bytes*/, usize /*alignment*/) noexcept {}
};
} // namespace

#if LUDUS_LOCALIZATION_TEST_HAS_ICU
namespace
{
using namespace ludus::foundation::unicode;

std::span<const uint8> FindPackage(std::string_view locale)
{
    for (const cases::CorpusPackage& package : cases::kCorpusPackages)
    {
        if (package.Locale == locale)
        {
            return package.Bytes;
        }
    }
    FAIL("no corpus package for " << locale);
    return {};
}

/// Starts the process's Unicode host once; tests never shut it down while contexts live.
void EnsureHost()
{
    if (IsUnicodeHostInitialized() == false)
    {
        REQUIRE(InitializeUnicodeHost(GetSystemAllocationDomain()) == UnicodeStatus::Ok);
    }
}

FormatterContext Prepare(const Catalog& catalog, std::span<const MessageBinding> bindings)
{
    EnsureHost();
    FormatterContext context;
    REQUIRE(PrepareFormatterContext(catalog, bindings, GetSystemAllocationDomain(), context) == Status::Ok);
    REQUIRE(context.IsValid() == true);
    return context;
}
} // namespace

TEST_CASE("Every corpus expectation formats from its cooked package version 2")
{
    EnsureHost();
    usize formatted = 0;
    usize rejected = 0;
    for (const cases::FormatCase& testCase : cases::kFormatCases)
    {
        const Catalog catalog = Load(FindPackage(testCase.Locale));
        const MessageBinding binding = Bind(catalog, *testCase.Descriptor);
        const MessageBinding prepared[] = {binding};
        FormatterContext context = Prepare(catalog, prepared);
        Output output;
        FormatResult result;
        const Status status = Format(context, binding, testCase.Arguments, output.GetSpan(), result);
        INFO(testCase.Locale << " " << testCase.Descriptor->Key.Key);
        if (testCase.IsFormatted == true)
        {
            REQUIRE(status == Status::Ok);
            CHECK(output.GetText(result) == testCase.Expected);
            CHECK(result.RequiredBytes == testCase.Expected.size());
            ++formatted;
        }
        else
        {
            REQUIRE(status == Status::InvalidArguments);
            CHECK(output.IsUnchanged() == true);
            CHECK(result.Written == 0);
            CHECK(result.RequiredBytes == 0);
            ++rejected;
        }
    }
    CHECK(formatted == 109);
    CHECK(rejected == 16);
}

TEST_CASE("Typed helpers erase arguments to Format with the generated schema")
{
    const Catalog french = Load(kCorpusFrFr);
    const MessageBinding count = Bind(french, corpus::kInventoryItemCount);
    const MessageBinding loot = Bind(french, corpus::kLootPickedUp);
    const MessageBinding distance = Bind(french, corpus::kHudDistance);
    const MessageBinding bindings[] = {count, loot, distance};
    FormatterContext context = Prepare(french, bindings);
    CHECK(context.GetPreparedCount() == 3);
    Output output;
    FormatResult result;
    REQUIRE(corpus::FormatInventoryItemCount(context, count, 1000000, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Vous avez 1\u202f000\u202f000 d\u2019objets.");
    CHECK(result.Provenance == Origin::Translation);
    CHECK(result.ProviderLocale == "fr-FR");
    REQUIRE(corpus::FormatLootPickedUp(context, loot, "feminine", 1, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Elle a ramass\u00e9 1 pi\u00e8ce.");
    REQUIRE(corpus::FormatHudDistance(context, distance, 0.25, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Distance\u00a0: 0,2\u00a0km"); // Half-even display rounding.
    output.Reset();
    CHECK(corpus::FormatLootPickedUp(context, loot, "robot", 1, output.GetSpan(), result) == Status::InvalidArguments);
    CHECK(output.IsUnchanged() == true);
}

TEST_CASE("Format copies complete output or leaves caller bytes unchanged")
{
    const Catalog english = Load(kCorpusEnUs);
    const MessageBinding count = Bind(english, corpus::kInventoryItemCount);
    const MessageBinding bindings[] = {count};
    FormatterContext context = Prepare(english, bindings);
    const ArgumentValue arguments[] = {MakeCountArgument("count", 2)};
    constexpr std::string_view EXPECTED = "You have 2 items.";
    Output output;
    FormatResult result;
    REQUIRE(Format(context, count, arguments, output.GetSpan(EXPECTED.size() - 1), result) == Status::BufferTooSmall);
    CHECK(result.RequiredBytes == EXPECTED.size());
    CHECK(result.Written == 0);
    CHECK(output.IsUnchanged() == true);
    REQUIRE(Format(context, count, arguments, output.GetSpan(0), result) == Status::BufferTooSmall);
    CHECK(output.IsUnchanged() == true);
    REQUIRE(Format(context, count, arguments, output.GetSpan(EXPECTED.size()), result) == Status::Ok);
    CHECK(output.GetText(result) == EXPECTED);
    CHECK(output.Bytes[EXPECTED.size()] == SENTINEL[EXPECTED.size() % SENTINEL.size()]); // Never NUL-terminated.
    // A failure after success resets every result field.
    const ArgumentValue missing[] = {MakeCountArgument("other_count", 2)};
    REQUIRE(Format(context, count, missing, output.GetSpan(), result) == Status::InvalidArguments);
    CHECK(result.Written == 0);
    CHECK(result.RequiredBytes == 0);
    CHECK(result.ProviderLocale.empty() == true);

    // Static records copy their cooked text; an approved empty translation is present text.
    const Catalog japanese = Load(kCorpusJaJp);
    const MessageBinding subtitle = Bind(japanese, corpus::kMenuSubtitle);
    const MessageBinding play = Bind(japanese, corpus::kMenuPlay);
    FormatterContext staticContext = Prepare(japanese, {});
    CHECK(staticContext.GetPreparedCount() == 0);
    output.Reset();
    REQUIRE(Format(staticContext, subtitle, {}, output.GetSpan(0), result) == Status::Ok);
    CHECK(result.Written == 0);
    CHECK(output.IsUnchanged() == true);
    REQUIRE(Format(staticContext, play, {}, output.GetSpan(2), result) == Status::BufferTooSmall);
    CHECK(result.RequiredBytes == std::string_view("\u30d7\u30ec\u30a4").size());
    CHECK(Format(staticContext, play, arguments, output.GetSpan(), result) == Status::InvalidArguments);
    CHECK(output.IsUnchanged() == true);
}

TEST_CASE("Arguments are validated against the full schema before ICU")
{
    const Catalog english = Load(kCorpusEnUs);
    const MessageBinding chat = Bind(english, corpus::kChatPlayerSaid);
    const MessageBinding distance = Bind(english, corpus::kHudDistance);
    const MessageBinding companion = Bind(english, corpus::kQuestCompanionJoined);
    const MessageBinding party = Bind(english, corpus::kPartyOnline);
    const MessageBinding bindings[] = {chat, distance, companion, party};
    FormatterContext context = Prepare(english, bindings);
    Output output;
    FormatResult result;
    const auto expectRejected = [&](MessageBinding binding, std::span<const ArgumentValue> arguments) {
        output.Reset();
        CHECK(Format(context, binding, arguments, output.GetSpan(), result) == Status::InvalidArguments);
        CHECK(output.IsUnchanged() == true);
    };

    constexpr float64 NOT_A_NUMBER = std::numeric_limits<float64>::quiet_NaN();
    constexpr float64 INFINITY_VALUE = std::numeric_limits<float64>::infinity();
    const ArgumentValue notANumber[] = {MakeNumberArgument("distance", NOT_A_NUMBER)};
    const ArgumentValue infinity[] = {MakeNumberArgument("distance", INFINITY_VALUE)};
    const ArgumentValue negativeInfinity[] = {MakeNumberArgument("distance", -INFINITY_VALUE)};
    const ArgumentValue belowMinimum[] = {MakeNumberArgument("distance", -0.5)};
    expectRejected(distance, notANumber);
    expectRejected(distance, infinity);
    expectRejected(distance, negativeInfinity);
    expectRejected(distance, belowMinimum);

    const ArgumentValue invalidUtf8[] = {MakeTextArgument("player", "a\xff"), MakeTextArgument("message", "hi")};
    const ArgumentValue embeddedNul[] = {MakeTextArgument("player", std::string_view("a\0b", 3)),
                                         MakeTextArgument("message", "hi")};
    const ArgumentValue isolate[] = {MakeTextArgument("player", "a\u2066b"), MakeTextArgument("message", "hi")};
    const ArgumentValue c1Control[] = {MakeTextArgument("player", "a\u0085b"), MakeTextArgument("message", "hi")};
    const ArgumentValue duplicate[] = {MakeTextArgument("player", "a"), MakeTextArgument("player", "b")};
    const ArgumentValue wrongKind[] = {MakeTextArgument("player", "a"), MakeCountArgument("message", 1)};
    expectRejected(chat, invalidUtf8);
    expectRejected(chat, embeddedNul);
    expectRejected(chat, isolate);
    expectRejected(chat, c1Control);
    expectRejected(chat, duplicate);
    expectRejected(chat, wrongKind);
    expectRejected(chat, {});

    // Combining marks, ZWJ sequences and Arabic stay in user text; each value is isolated.
    const ArgumentValue joined[] = {MakeTextArgument("player", "e\u0301 \U0001F469\u200d\U0001F4BB"),
                                    MakeTextArgument("message", "\u0645\u0631\u062d\u0628\u0627")};
    REQUIRE(Format(context, chat, joined, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) ==
          "\u2068e\u0301 \U0001F469\u200d\U0001F4BB\u2069 said: \u2068\u0645\u0631\u062d\u0628\u0627\u2069");

    // Game-origin text is not filtered for bidi controls, only validated.
    const ArgumentValue gameText[] = {MakeTextArgument("companion", "a\u202eb"),
                                      MakeEnumArgument("companion_gender", "feminine")};
    REQUIRE(Format(context, companion, gameText, output.GetSpan(), result) == Status::Ok);
    // Unknown tokens select `other` for `unknown: other`, which also accepts the literal token.
    const ArgumentValue unknownToken[] = {MakeTextArgument("companion", "Kit"),
                                          MakeEnumArgument("companion_gender", "other")};
    REQUIRE(Format(context, companion, unknownToken, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "\u2068Kit\u2069 joined the party. They are ready.");
    const ArgumentValue invalidToken[] = {MakeTextArgument("companion", "Kit"),
                                          MakeEnumArgument("companion_gender", "\xc0")};
    expectRejected(companion, invalidToken);

    // A translation may omit an argument, but the caller still supplies all of them.
    const ArgumentValue partyArguments[] = {MakeCountArgument("count", 0), MakeTextArgument("leader", "Mia")};
    REQUIRE(Format(context, party, partyArguments, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Nobody is online.");
    const ArgumentValue partyAsNumber[] = {MakeNumberArgument("count", 1.0), MakeTextArgument("leader", "Mia")};
    expectRejected(party, partyAsNumber);
}

TEST_CASE("Formatter contexts check their lease, bindings, backend data and storage")
{
    const Catalog english = Load(kCorpusEnUs);
    const Catalog french = Load(kCorpusFrFr);
    const MessageBinding count = Bind(english, corpus::kInventoryItemCount);
    const MessageBinding score = Bind(english, corpus::kHudScore);
    const MessageBinding frenchCount = Bind(french, corpus::kInventoryItemCount);
    const MessageBinding bindings[] = {count, count, Bind(english, corpus::kMenuPlay)};
    FormatterContext context = Prepare(english, bindings);
    CHECK(context.GetPreparedCount() == 1); // Duplicates collapse; static records need nothing.
    Output output;
    FormatResult result;
    const ArgumentValue arguments[] = {MakeCountArgument("count", 1)};
    const ArgumentValue scoreArguments[] = {MakeCountArgument("score", 7)};
    CHECK(Format(context, score, scoreArguments, output.GetSpan(), result) == Status::NotPrepared);
    CHECK(Format(context, frenchCount, arguments, output.GetSpan(), result) == Status::WrongCatalog);
    CHECK(output.IsUnchanged() == true);

    FormatterContext untouched = Prepare(english, bindings);
    const MessageBinding foreign[] = {frenchCount};
    CHECK(PrepareFormatterContext(english, foreign, GetSystemAllocationDomain(), untouched) == Status::WrongCatalog);
    CHECK(PrepareFormatterContext(Catalog{}, bindings, GetSystemAllocationDomain(), untouched) == Status::Invalid);
    const Catalog literal = Load(kSourceCatalog);
    CHECK(PrepareFormatterContext(literal, {}, GetSystemAllocationDomain(), untouched) == Status::Unsupported);
    Array<MessageBinding> many;
    REQUIRE(many.TryResize(MAX_PREPARED_MESSAGES + 1, count) == true);
    CHECK(PrepareFormatterContext(english, {many.GetData(), many.GetSize()}, GetSystemAllocationDomain(), untouched) ==
          Status::Limit);
    FailingHeap heap;
    CHECK(PrepareFormatterContext(english, bindings, heap.Domain, untouched) == Status::OutOfMemory);
    CHECK(heap.Attempts > 0);
    CHECK(untouched.GetPreparedCount() == 1);
    REQUIRE(Format(untouched, count, arguments, output.GetSpan(), result) == Status::Ok);

    // A package cooked with other linguistic data is refused, even when every byte is valid.
    Array<uint8> relabelled;
    REQUIRE(relabelled.TryResize(sizeof(kCorpusEnUs)) == true);
    for (usize i = 0; i < sizeof(kCorpusEnUs); ++i)
    {
        relabelled[i] = kCorpusEnUs[i];
    }
    const std::string_view bytes(reinterpret_cast<const char*>(relabelled.GetData()), relabelled.GetSize());
    const usize found = bytes.find("icu4c-78.3");
    REQUIRE(found != std::string_view::npos);
    relabelled[found + 9] = '4'; // icu4c-78.4
    const Catalog mismatched = Load({relabelled.GetData(), relabelled.GetSize()});
    CHECK(mismatched.GetLinguisticData() == "icu4c-78.4/cldr-48/unicode-17.0");
    CHECK(PrepareFormatterContext(mismatched, {}, GetSystemAllocationDomain(), untouched) ==
          Status::LinguisticDataMismatch);

    // The context retains its lease: replacing the caller's catalog keeps results valid.
    Catalog replaced = english;
    FormatterContext retained = Prepare(replaced, bindings);
    replaced = french;
    REQUIRE(Format(retained, count, arguments, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "You have 1 item.");
    CHECK(result.ProviderLocale == "en-US");
    CHECK(result.Provenance == Origin::Source);

    FormatterContext moved(std::move(retained));
    CHECK(retained.IsValid() == false); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(Format(retained, count, arguments, output.GetSpan(), result) == Status::Invalid); // NOLINT
    REQUIRE(Format(moved, count, arguments, output.GetSpan(), result) == Status::Ok);
    retained = std::move(moved);
    REQUIRE(Format(retained, count, arguments, output.GetSpan(), result) == Status::Ok);
}

TEST_CASE("Dynamic fallback records format with the provider locale's grammar")
{
    const Catalog canadian = Load(kReleaseFrCa);
    const MessageBinding lives = Bind(canadian, release::kHudLives);
    const MessageBinding greeting = Bind(canadian, release::kGreetingName);
    const MessageBinding mood = Bind(canadian, release::kMoodSelect);
    const MessageBinding bindings[] = {lives, greeting, mood};
    FormatterContext context = Prepare(canadian, bindings);
    Output output;
    FormatResult result;
    // French treats 0 and 1 as `one`; the pattern comes from fr-FR through fallback.
    REQUIRE(release::FormatHudLives(context, lives, 0, output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "0 vie");
    CHECK(result.Provenance == Origin::Fallback);
    CHECK(result.ProviderLocale == "fr-FR");
    // fr-CA's own translation omits the argument; validation still requires it.
    REQUIRE(release::FormatGreetingName(context, greeting, "Zo\u00e9", output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Bonjour!");
    CHECK(result.Provenance == Origin::Translation);
    CHECK(release::FormatGreetingName(context, greeting, "a\u202eb", output.GetSpan(), result) ==
          Status::InvalidArguments);
    REQUIRE(release::FormatMoodSelect(context, mood, "sleepy", output.GetSpan(), result) == Status::Ok);
    CHECK(output.GetText(result) == "Humeur inconnue.");
    CHECK(result.ProviderLocale == "fr-FR");
}

TEST_CASE("Unicode host shutdown follows every formatter context")
{
    EnsureHost();
    const Catalog english = Load(kCorpusEnUs);
    const MessageBinding count = Bind(english, corpus::kInventoryItemCount);
    const MessageBinding bindings[] = {count};
    {
        FormatterContext context = Prepare(english, bindings);
        REQUIRE(context.IsValid() == true);
    }
    REQUIRE(ShutdownUnicodeHost() == UnicodeStatus::Ok);
    CHECK(GetUnicodeAllocationStatistics().LiveBlocks == 0);
    FormatterContext context;
    CHECK(PrepareFormatterContext(english, bindings, GetSystemAllocationDomain(), context) ==
          Status::BackendUnavailable);
    EnsureHost();
    context = Prepare(english, bindings);
    CHECK(context.IsValid() == true);
}
#else
TEST_CASE("Builds without LUDUS_BUILD_UNICODE report an unavailable formatting backend")
{
    const Catalog english = Load(kCorpusEnUs);
    const MessageBinding count = Bind(english, corpus::kInventoryItemCount);
    const MessageBinding bindings[] = {count};
    FormatterContext context;
    CHECK(PrepareFormatterContext(english, bindings, GetSystemAllocationDomain(), context) ==
          Status::BackendUnavailable);
    CHECK(context.IsValid() == false);
    Output output;
    FormatResult result;
    CHECK(corpus::FormatInventoryItemCount(context, count, 1, output.GetSpan(), result) == Status::Invalid);
    CHECK(output.IsUnchanged() == true);
}
#endif
