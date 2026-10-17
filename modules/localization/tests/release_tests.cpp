#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/localization/catalog.hpp>

#include "fixture.hpp"

#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

// Package version 2 (ludus-icu-mf1-v1) reader tests. Every fixture comes from
// the production release cooker replaying recorded pinned-ICU verdicts; see
// tests/generate_fixture.py and docs/modules/localization.md.

using namespace ludus::localization;
using ludus::foundation::AllocationDomain;
using ludus::foundation::Array;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::int32;
using ludus::foundation::TryReadLittleEndian;
using ludus::foundation::TryWriteLittleEndian;

namespace
{
namespace release = localization_release_fixture;
namespace corpus = localization_corpus;

constexpr usize HEADER_BYTES = 64;
constexpr usize RECORD_BYTES = 40;
constexpr usize ARGUMENT_BYTES = 36;
constexpr usize SLICE_BYTES = 8;
constexpr std::string_view LINGUISTIC_DATA = "icu4c-78.3/cldr-48/unicode-17.0";

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

uint32 ReadField(std::span<const uint8> bytes, usize offset)
{
    uint32 value{};
    REQUIRE(TryReadLittleEndian(bytes.subspan(offset), value));
    return value;
}

void WriteField(std::span<uint8> bytes, usize offset, uint32 value)
{
    REQUIRE(TryWriteLittleEndian(value, bytes.subspan(offset)));
}

Catalog Load(std::span<const uint8> bytes)
{
    Catalog result;
    Diagnostic diagnostic;
    REQUIRE(PrepareCatalog(bytes, GetSystemAllocationDomain(), result, diagnostic) == Status::Ok);
    REQUIRE(diagnostic.Result == Status::Ok);
    return result;
}

ResolvedText Resolve(const Catalog& catalog, const MessageDescriptor& descriptor)
{
    MessageBinding binding;
    ResolvedText result;
    REQUIRE(catalog.BindMessage(descriptor, binding) == Status::Ok);
    REQUIRE(catalog.ResolveStatic(binding, result) == Status::Ok);
    return result;
}

Status ResolveStatus(const Catalog& catalog, const MessageDescriptor& descriptor)
{
    MessageBinding binding;
    REQUIRE(catalog.BindMessage(descriptor, binding) == Status::Ok);
    ResolvedText result{"unchanged", Origin::Fallback, "unchanged"};
    const Status status = catalog.ResolveStatic(binding, result);
    if (status != Status::Ok)
    {
        REQUIRE(result.Text == "unchanged");
        REQUIRE(result.ProviderLocale == "unchanged");
    }
    return status;
}

Status Prepare(std::span<const uint8> bytes, Diagnostic& outDiagnostic)
{
    Catalog result;
    const Status status = PrepareCatalog(bytes, GetSystemAllocationDomain(), result, outDiagnostic);
    REQUIRE((status == Status::Ok) == result.IsValid());
    if (status != Status::Ok)
    {
        REQUIRE(outDiagnostic.Result == status);
        REQUIRE(outDiagnostic.Offset <= bytes.size());
    }
    return status;
}

Array<uint8> Copy(std::span<const uint8> bytes)
{
    Array<uint8> result;
    REQUIRE(result.TryAddRange(bytes));
    return result;
}

/// Offset of the record directory: header, four labels, then the provider directory.
usize GetRecords(std::span<const uint8> bytes)
{
    const usize labels = ReadField(bytes, 16) + ReadField(bytes, 20) + ReadField(bytes, 24) + ReadField(bytes, 52);
    return HEADER_BYTES + labels + static_cast<usize>(ReadField(bytes, 36)) * SLICE_BYTES;
}

usize GetArguments(std::span<const uint8> bytes)
{
    return GetRecords(bytes) + static_cast<usize>(ReadField(bytes, 12)) * RECORD_BYTES;
}

usize FindRecord(std::span<const uint8> bytes, std::string_view key)
{
    const usize records = GetRecords(bytes);
    for (usize i = 0; i < ReadField(bytes, 12); ++i)
    {
        const usize record = records + i * RECORD_BYTES;
        const std::string_view candidate{reinterpret_cast<const char*>(bytes.data()) + ReadField(bytes, record),
                                         ReadField(bytes, record + 4)};
        if (candidate == key)
        {
            return record;
        }
    }
    FAIL("missing fixture key");
    return 0;
}
} // namespace

TEST_CASE("version 2 packages resolve ICU-decoded static text", "[localization][l1]")
{
    const Catalog source = Load(kReleaseEnUs);
    REQUIRE(source.GetDomain() == "test/l1");
    REQUIRE(source.GetLocale() == "en-US");
    REQUIRE(source.GetSourceLocale() == "en-US");
    REQUIRE(source.GetLinguisticData() == LINGUISTIC_DATA);
    REQUIRE(source.GetMessageCount() == 10);
    // Apostrophe syntax is resolved by the cooker: no runtime pattern parser runs here.
    REQUIRE(Resolve(source, release::kEscapeBraces).Text == "Press {jump} to jump.");
    REQUIRE(Resolve(source, release::kEscapeApostrophes).Text == "It's literal: {} and don't.");
    REQUIRE(Resolve(source, release::kEscapeUnterminated).Text == "Open {to the end");
    REQUIRE(Resolve(source, release::kUnicodeMixed).Text == "العربية 日本語 😀 é");
    const ResolvedText start = Resolve(source, release::kMenuStart);
    REQUIRE(start.Text == "Start");
    REQUIRE(start.Provenance == Origin::Source);
    REQUIRE(start.ProviderLocale == "en-US");

    const Catalog french = Load(kReleaseFrFr);
    REQUIRE(Resolve(french, release::kEscapeBraces).Text == "Appuyez sur {jump} pour sauter.");
    REQUIRE(Resolve(french, release::kEscapeApostrophes).Text == "C'est littéral : {} et l'apostrophe reste.");
    REQUIRE(Resolve(french, release::kEscapeUnterminated).Text == "Ouvrir {jusqu'à la fin");
    REQUIRE(Resolve(french, release::kMenuStart).Provenance == Origin::Translation);
    // A draft translation never provides text; the optional message falls back to the source.
    const ResolvedText quit = Resolve(french, release::kMenuQuit);
    REQUIRE(quit.Text == "Quit");
    REQUIRE(quit.Provenance == Origin::SourceFallback);
    REQUIRE(quit.ProviderLocale == "en-US");
}

TEST_CASE("version 2 fallback is baked per record with its provider locale", "[localization][l1]")
{
    const Catalog canadian = Load(kReleaseFrCa);
    REQUIRE(canadian.GetLocale() == "fr-CA");
    const ResolvedText start = Resolve(canadian, release::kMenuStart);
    REQUIRE(start.Text == "Démarrer");
    REQUIRE(start.Provenance == Origin::Fallback);
    REQUIRE(start.ProviderLocale == "fr-FR");
    // The fr-CA record carries an approved status with a stale receipt: fr-FR provides it.
    const ResolvedText braces = Resolve(canadian, release::kEscapeBraces);
    REQUIRE(braces.Text == "Appuyez sur {jump} pour sauter.");
    REQUIRE(braces.ProviderLocale == "fr-FR");
    const ResolvedText quit = Resolve(canadian, release::kMenuQuit);
    REQUIRE(quit.Provenance == Origin::SourceFallback);
    REQUIRE(quit.ProviderLocale == "en-US");
    // An approved empty translation is present text, not a fallback request.
    const ResolvedText credits = Resolve(canadian, release::kMenuCredits);
    REQUIRE(credits.Text.empty());
    REQUIRE(credits.Provenance == Origin::Translation);
    REQUIRE(credits.ProviderLocale == "fr-CA");
}

TEST_CASE("messages with arguments bind exactly but never resolve statically", "[localization][l1]")
{
    const Catalog source = Load(kReleaseEnUs);
    const Catalog canadian = Load(kReleaseFrCa);
    for (const Catalog* catalog : {&source, &canadian})
    {
        REQUIRE(ResolveStatus(*catalog, release::kHudLives) == Status::RequiresArguments);
        REQUIRE(ResolveStatus(*catalog, release::kMoodSelect) == Status::RequiresArguments);
        // fr-CA's translation omits {name}; the source schema still requires formatting.
        REQUIRE(ResolveStatus(*catalog, release::kGreetingName) == Status::RequiresArguments);
        MessageBinding binding;
        REQUIRE(catalog->BindMessage(release::kHudLives.Key, binding) == Status::SchemaMismatch);
    }
    // A key-only bind is the empty-schema descriptor.
    MessageBinding binding;
    ResolvedText text;
    REQUIRE(source.BindMessage(release::kMenuStart.Key, binding) == Status::Ok);
    REQUIRE(source.ResolveStatic(binding, text) == Status::Ok);
    REQUIRE(text.Text == "Start");
}

TEST_CASE("descriptor binding compares every meaningful schema field", "[localization][l1]")
{
    const Catalog catalog = Load(kReleaseFrFr);
    const ArgumentDescriptor name = release::kGreetingNameArguments[0];
    const ArgumentDescriptor lives = release::kHudLivesArguments[0];
    const ArgumentDescriptor mood = release::kMoodSelectArguments[0];
    REQUIRE(name.Origin == TextOrigin::User);
    REQUIRE(name.MaxBytes == 64);
    REQUIRE(lives.Maximum == 99);
    REQUIRE(mood.Unknown == UnknownPolicy::Other);

    const auto expectMismatch = [&catalog](MessageKey key, const ArgumentDescriptor& argument) {
        const ArgumentDescriptor arguments[] = {argument};
        MessageBinding binding;
        REQUIRE(catalog.BindMessage(MessageDescriptor{key, arguments}, binding) == Status::SchemaMismatch);
    };
    ArgumentDescriptor changed = name;
    changed.MaxBytes = 65;
    expectMismatch(release::kGreetingName.Key, changed);
    changed = name;
    changed.Origin = TextOrigin::Game;
    expectMismatch(release::kGreetingName.Key, changed);
    changed = name;
    changed.Name = "player";
    expectMismatch(release::kGreetingName.Key, changed);
    changed = name;
    changed.Type = ArgumentType::Enum;
    expectMismatch(release::kGreetingName.Key, changed);
    changed = lives;
    changed.Minimum = 1;
    expectMismatch(release::kHudLives.Key, changed);
    changed = lives;
    changed.Type = ArgumentType::Number;
    expectMismatch(release::kHudLives.Key, changed);
    changed = mood;
    changed.Unknown = UnknownPolicy::Reject;
    expectMismatch(release::kMoodSelect.Key, changed);
    constexpr std::string_view REORDERED[] = {"tense", "calm"};
    changed = mood;
    changed.Values = REORDERED;
    expectMismatch(release::kMoodSelect.Key, changed);
    changed.Values = std::span<const std::string_view>(REORDERED).first(1);
    expectMismatch(release::kMoodSelect.Key, changed);

    // Irrelevant fields are ignored: a Text argument's bounds do not participate.
    changed = name;
    changed.Minimum = -5;
    changed.Unknown = UnknownPolicy::Other;
    const ArgumentDescriptor tolerant[] = {changed};
    MessageBinding binding;
    REQUIRE(catalog.BindMessage(MessageDescriptor{release::kGreetingName.Key, tolerant}, binding) == Status::Ok);

    const ArgumentDescriptor extra[] = {name, lives};
    MessageBinding preserved;
    REQUIRE(catalog.BindMessage(release::kMenuStart, preserved) == Status::Ok);
    MessageBinding output = preserved;
    REQUIRE(catalog.BindMessage(MessageDescriptor{release::kGreetingName.Key, extra}, output) ==
            Status::SchemaMismatch);
    REQUIRE(catalog.BindMessage(MessageDescriptor{release::kMenuStart.Key, extra}, output) == Status::SchemaMismatch);
    ResolvedText text;
    REQUIRE(catalog.ResolveStatic(output, text) == Status::Ok);
    REQUIRE(text.Text == "Démarrer");

    ArgumentDescriptor many[MAX_ARGUMENTS + 1] = {};
    REQUIRE(catalog.BindMessage(MessageDescriptor{release::kGreetingName.Key, many}, output) == Status::Invalid);
}

TEST_CASE("exact keys never match prefixes or near collisions", "[localization][l1]")
{
    const Catalog catalog = Load(kReleaseEnUs);
    MessageBinding binding;
    for (std::string_view key : {"menu/star", "menu/startx", "menu/starT", "menu", "menu/start/x", "menu/quit-"})
    {
        REQUIRE(catalog.BindMessage(MessageKey{"test/l1", key}, binding) != Status::Ok);
    }
    REQUIRE(catalog.BindMessage(MessageKey{"test/l1", "menu//start"}, binding) == Status::Invalid);
    REQUIRE(catalog.BindMessage(MessageKey{"test/l2", "menu/start"}, binding) == Status::NotFound);
    REQUIRE(catalog.BindMessage(MessageKey{"test/l1", "menu/credit"}, binding) == Status::NotFound);
    REQUIRE(catalog.BindMessage(corpus::kMenuPlay, binding) == Status::NotFound);
}

TEST_CASE("corpus packages share one schema in every locale", "[localization][l1]")
{
    const std::span<const uint8> packages[] = {kCorpusEnUs, kCorpusFrFr, kCorpusRuRu, kCorpusAr, kCorpusJaJp};
    const MessageDescriptor* dynamic[] = {&corpus::kChatPlayerSaid,
                                          &corpus::kHudDistance,
                                          &corpus::kHudScore,
                                          &corpus::kInventoryItemCount,
                                          &corpus::kLootPickedUp,
                                          &corpus::kPartyOnline,
                                          &corpus::kQuestCompanionJoined,
                                          &corpus::kRacePlace};
    for (std::span<const uint8> bytes : packages)
    {
        const Catalog catalog = Load(bytes);
        REQUIRE(catalog.GetLinguisticData() == LINGUISTIC_DATA);
        REQUIRE(catalog.GetMessageCount() == 11);
        for (const MessageDescriptor* descriptor : dynamic)
        {
            REQUIRE(ResolveStatus(catalog, *descriptor) == Status::RequiresArguments);
        }
        REQUIRE(Resolve(catalog, corpus::kMenuPlay).Provenance ==
                (catalog.GetLocale() == "en-US" ? Origin::Source : Origin::Translation));
    }
    REQUIRE(Resolve(Load(kCorpusEnUs), corpus::kLiteralBraces).Text ==
            "Type {name} to insert a name. Don't worry: it's literal.");
    REQUIRE(Resolve(Load(kCorpusFrFr), corpus::kLiteralBraces).Text ==
            "Tapez {name} pour insérer un nom. Pas d'inquiétude : c’est littéral.");
    REQUIRE(Resolve(Load(kCorpusJaJp), corpus::kMenuSubtitle).Text.empty());
    REQUIRE(Resolve(Load(kCorpusJaJp), corpus::kMenuPlay).Text == "プレイ");
}

TEST_CASE("version 2 leases keep their bytes and tokens across replacement", "[localization][l1]")
{
    Catalog active = Load(kReleaseEnUs);
    const Catalog retained = active;
    MessageBinding binding;
    REQUIRE(active.BindMessage(release::kMenuStart, binding) == Status::Ok);
    Diagnostic diagnostic;
    REQUIRE(PrepareCatalog(kReleaseEnUs, GetSystemAllocationDomain(), active, diagnostic) == Status::Ok);
    ResolvedText text;
    // Identical bytes still get a fresh token: old bindings never cross loads.
    REQUIRE(active.ResolveStatic(binding, text) == Status::WrongCatalog);
    REQUIRE(retained.ResolveStatic(binding, text) == Status::Ok);
    REQUIRE(text.Text == "Start");
    REQUIRE(PrepareCatalog(kReleaseFrCa, GetSystemAllocationDomain(), active, diagnostic) == Status::Ok);
    REQUIRE(retained.ResolveStatic(binding, text) == Status::Ok);
    REQUIRE(text.Text == "Start");
    REQUIRE(text.ProviderLocale == "en-US");

    Catalog moved = std::move(active);
    REQUIRE(active.IsValid() == false); // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    REQUIRE(active.GetLinguisticData().empty());
    REQUIRE(moved.GetLocale() == "fr-CA");

    // A failed replacement preserves the previous catalog.
    const Array<uint8> broken = Copy(std::span<const uint8>(kReleaseFrFr).first(HEADER_BYTES));
    REQUIRE(PrepareCatalog(broken.AsSpan(), GetSystemAllocationDomain(), moved, diagnostic) == Status::Invalid);
    REQUIRE(moved.GetLocale() == "fr-CA");

    FailingHeap heap;
    REQUIRE(PrepareCatalog(kReleaseFrFr, heap.Domain, moved, diagnostic) == Status::OutOfMemory);
    REQUIRE(heap.Attempts == 1);
    REQUIRE(moved.GetLocale() == "fr-CA");
}

TEST_CASE("version 2 rejects every truncation and targeted corruption", "[localization][l1]")
{
    const std::span<const uint8> original = kReleaseFrCa;
    Diagnostic diagnostic;
    for (usize size = 0; size < original.size(); ++size)
    {
        REQUIRE(Prepare(original.first(size), diagnostic) != Status::Ok);
    }
    Array<uint8> extended = Copy(original);
    REQUIRE(extended.TryAdd(uint8{0}));
    WriteField(extended.AsSpan(), 8, static_cast<uint32>(extended.GetSize()));
    REQUIRE(Prepare(extended.AsSpan(), diagnostic) == Status::Invalid);

    const auto expect = [&](usize offset, uint32 value, Status expected) {
        Array<uint8> bytes = Copy(original);
        WriteField(bytes.AsSpan(), offset, value);
        Diagnostic local;
        REQUIRE(Prepare(bytes.AsSpan(), local) == expected);
    };
    expect(4, 3, Status::Unsupported);   // Unknown package version.
    expect(28, 20, Status::Unsupported); // Record width.
    expect(32, 1, Status::Unsupported);  // Profile ID.
    expect(44, 32, Status::Unsupported); // Argument record width.
    expect(56, 1, Status::Invalid);      // Reserved.
    expect(36, 9, Status::Limit);        // Provider count above the fallback ceiling.
    expect(36, 0, Status::Limit);

    const usize start = FindRecord(original, "menu/start");
    const usize lives = FindRecord(original, "hud/lives");
    expect(start + 20, 3, Status::Invalid); // Provider index outside the path.
    expect(start + 16, 2, Status::Invalid); // Unknown kind.
    expect(lives + 16, 0, Status::Invalid); // Static record that declares arguments.
    expect(start + 16, 1, Status::Invalid); // Dynamic record without arguments.
    expect(start + 32, 0, Status::Invalid); // Source revision zero.
    expect(start + 36, 2, Status::Invalid); // Unknown flag.
    expect(start + 12, 64 * 1024 + 1, Status::Limit);
    expect(lives + 12, 16 * 1024 + 1, Status::Limit); // Dynamic patterns have the smaller ceiling.
    expect(lives + 24, ReadField(original, lives + 24) + 1, Status::Invalid);

    const usize argument = GetArguments(original) + ReadField(original, lives + 24) * ARGUMENT_BYTES;
    expect(argument + 8, 4, Status::Invalid);                                // Unknown argument type.
    expect(argument + 12, 1, Status::Invalid);                               // Policy on a count.
    expect(argument + 16, 7, Status::Invalid);                               // Text limit on a count.
    expect(argument + 20, static_cast<uint32>(int32{100}), Status::Invalid); // Minimum above maximum.
    expect(argument + 24, 1000000001, Status::Invalid);                      // Outside +/-1e9.
    expect(argument + 32, 1, Status::Invalid);                               // Enum values on a count.

    // Mutating any single byte must either fail validation or produce a catalog
    // whose every public read stays in bounds.
    Array<uint8> mutated = Copy(original);
    for (usize i = 0; i < mutated.GetSize(); ++i)
    {
        const uint8 saved = mutated[i];
        mutated[i] = static_cast<uint8>(saved ^ 0xa5U);
        Catalog candidate;
        if (PrepareCatalog(mutated.AsSpan(), GetSystemAllocationDomain(), candidate, diagnostic) == Status::Ok)
        {
            for (const MessageDescriptor* descriptor :
                 {&release::kMenuStart, &release::kMenuQuit, &release::kHudLives, &release::kMoodSelect})
            {
                MessageBinding binding;
                ResolvedText text;
                if (candidate.BindMessage(*descriptor, binding) == Status::Ok)
                {
                    static_cast<void>(candidate.ResolveStatic(binding, text));
                }
            }
        }
        else
        {
            REQUIRE(diagnostic.Offset <= mutated.GetSize());
        }
        mutated[i] = saved;
    }
}

TEST_CASE("version 1 literal catalogs keep their contract under the version 2 reader", "[localization][l1]")
{
    const Catalog french = Load(kFrenchCatalog);
    REQUIRE(french.GetLinguisticData().empty());
    MessageBinding binding;
    ResolvedText text;
    REQUIRE(french.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
    REQUIRE(french.ResolveStatic(binding, text) == Status::Ok);
    REQUIRE(text.Provenance == Origin::Translation);
    REQUIRE(text.ProviderLocale == "fr-FR");
    const Catalog fallback = Load(kFallbackCatalog);
    REQUIRE(fallback.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
    REQUIRE(fallback.ResolveStatic(binding, text) == Status::Ok);
    REQUIRE(text.Provenance == Origin::SourceFallback);
    REQUIRE(text.ProviderLocale == "en-US");
    // Version 1 messages never declare arguments, so a nonempty descriptor cannot match.
    REQUIRE(french.BindMessage(MessageDescriptor{localization_fixture::kMenuPlay, release::kHudLivesArguments},
                               binding) == Status::SchemaMismatch);
}
