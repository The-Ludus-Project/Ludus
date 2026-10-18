#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/localization/catalog.hpp>

#include "fixture.hpp"

#include <atomic>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::localization;
using ludus::foundation::AllocationDomain;
using ludus::foundation::Array;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::TryReadLittleEndian;
using ludus::foundation::TryWriteLittleEndian;

namespace
{
struct FaultHeap final
{
    usize Attempts{};
    usize Allocations{};
    usize Frees{};
    usize LiveBytes{};
    usize FailAt{};
    AllocationDomain Domain{this, Allocate, Free};

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        auto& heap = *static_cast<FaultHeap*>(context);
        if (++heap.Attempts == heap.FailAt)
        {
            return nullptr;
        }
        void* pointer = GetSystemAllocationDomain().TryAllocate(bytes, alignment);
        if (pointer != nullptr)
        {
            ++heap.Allocations;
            heap.LiveBytes += bytes;
        }
        return pointer;
    }
    // AllocationDomain callback ABI defines the adjacent context/pointer arguments.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void Free(void* context, void* pointer, usize bytes, usize alignment) noexcept
    {
        auto& heap = *static_cast<FaultHeap*>(context);
        ++heap.Frees;
        heap.LiveBytes -= bytes;
        GetSystemAllocationDomain().Free(pointer, bytes, alignment);
    }
};

uint32 Word(std::span<const uint8> bytes, usize offset)
{
    uint32 value{};
    REQUIRE(TryReadLittleEndian(bytes.subspan(offset), value));
    return value;
}

void Write(std::span<uint8> bytes, usize offset, uint32 value)
{
    REQUIRE(TryWriteLittleEndian(value, bytes.subspan(offset)));
}

Catalog Load(std::span<const uint8> bytes)
{
    Catalog result;
    Diagnostic diagnostic;
    REQUIRE(PrepareCatalog(bytes, GetSystemAllocationDomain(), result, diagnostic) == Status::Ok);
    REQUIRE(diagnostic.Result == Status::Ok);
    REQUIRE(diagnostic.Offset == 0);
    return result;
}

ResolvedText Resolve(const Catalog& catalog, MessageKey key)
{
    MessageBinding binding;
    ResolvedText result;
    REQUIRE(catalog.BindMessage(key, binding) == Status::Ok);
    REQUIRE(catalog.ResolveStatic(binding, result) == Status::Ok);
    return result;
}

template <typename Owner>
concept TemporaryResolve = requires { Owner{}.ResolveStatic(MessageBinding{}, std::declval<ResolvedText&>()); };
static_assert(TemporaryResolve<Catalog> == false);
static_assert(std::is_nothrow_copy_constructible_v<Catalog>);
static_assert(std::is_nothrow_move_constructible_v<Catalog>);
static_assert(noexcept(std::declval<Catalog&>().BindMessage({}, std::declval<MessageBinding&>())));
} // namespace

TEST_CASE("production cooker bytes resolve exact identities and preserve literal Unicode", "[localization]")
{
    const Catalog source = Load(kSourceCatalog);
    REQUIRE(source.IsValid());
    REQUIRE(source.GetDomain() == "game/ui");
    REQUIRE(source.GetLocale() == "en-US");
    REQUIRE(source.GetSourceLocale() == "en-US");
    REQUIRE(source.GetMessageCount() == 4);
    REQUIRE(Resolve(source, localization_fixture::kMenuPlay).Text == "Play");
    REQUIRE(Resolve(source, localization_fixture::kEmpty).Text.empty());
    REQUIRE(Resolve(source, localization_fixture::kLiteral).Text == "Apostrophe ' and {player} are literal.\n");
    REQUIRE(Resolve(source, localization_fixture::kUnicode).Text == "العربية 日本語 😀 é");
    REQUIRE(Resolve(source, localization_fixture::kMenuPlay).Provenance == Origin::Source);

    const Catalog french = Load(kFrenchCatalog);
    REQUIRE(french.GetLocale() == "fr-FR");
    REQUIRE(french.GetSourceLocale() == "en-US");
    REQUIRE(Resolve(french, localization_fixture::kMenuPlay).Text == "Jouer");
    REQUIRE(Resolve(french, localization_fixture::kMenuPlay).Provenance == Origin::Translation);
    const Catalog fallback = Load(kFallbackCatalog);
    REQUIRE(fallback.GetLocale() == "ja-JP");
    REQUIRE(Resolve(fallback, localization_fixture::kMenuPlay).Text == "Play");
    REQUIRE(Resolve(fallback, localization_fixture::kMenuPlay).Provenance == Origin::SourceFallback);
}

TEST_CASE("bindings reject wrong owners and preserve outputs on failure", "[localization]")
{
    Catalog source = Load(kSourceCatalog);
    const Catalog other = Load(kSourceCatalog); // Same bytes still have a different owner token.
    MessageBinding binding;
    REQUIRE(source.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
    REQUIRE(source.BindMessage({"game/other", "menu/play"}, binding) == Status::NotFound);
    REQUIRE(source.BindMessage({"game/ui", "missing"}, binding) == Status::NotFound);
    REQUIRE(source.BindMessage({"game/ui", "MENU/PLAY"}, binding) == Status::Invalid);
    ResolvedText result{"sentinel", Origin::Translation, "sentinel"};
    REQUIRE(other.ResolveStatic(binding, result) == Status::WrongCatalog);
    REQUIRE(other.ResolveStatic({}, result) == Status::WrongCatalog);
    REQUIRE(result.Text == "sentinel");
    REQUIRE(result.Provenance == Origin::Translation);
    REQUIRE(source.ResolveStatic(binding, result) == Status::Ok);
    REQUIRE(result.Text == "Play");
    Catalog empty;
    REQUIRE(empty.GetDomain().empty());
    REQUIRE(empty.GetLocale().empty());
    REQUIRE(empty.GetSourceLocale().empty());
    REQUIRE(empty.GetMessageCount() == 0);
    REQUIRE(empty.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Invalid);
    REQUIRE(empty.ResolveStatic(binding, result) == Status::Invalid);
    REQUIRE(result.Text == "Play");
    Catalog moved = std::move(source);
    // The public contract explicitly guarantees a queryable invalid moved-from owner.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    REQUIRE(source.IsValid() == false);
    REQUIRE(source.GetMessageCount() == 0);
    REQUIRE(moved.ResolveStatic(binding, result) == Status::Ok);
    moved = std::move(empty);
    REQUIRE(moved.IsValid() == false);
}

TEST_CASE("one allocation prepares a catalog; leases and warm resolution allocate nothing",
          "[localization][allocation]")
{
    FaultHeap heap;
    {
        Catalog active;
        Diagnostic diagnostic;
        REQUIRE(PrepareCatalog(kSourceCatalog, heap.Domain, active, diagnostic) == Status::Ok);
        REQUIRE(heap.Attempts == 1);
        REQUIRE(heap.Allocations == 1);
        MessageBinding binding;
        REQUIRE(active.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
        Catalog retained = active;
        ResolvedText old;
        REQUIRE(retained.ResolveStatic(binding, old) == Status::Ok);
        const usize attempts = heap.Attempts;
        for (usize index = 0; index < 10000; ++index)
        {
            // Deliberately exercise lease retain/release rather than only a borrowed reference.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            Catalog lease = retained;
            ResolvedText result;
            REQUIRE(lease.ResolveStatic(binding, result) == Status::Ok);
            REQUIRE(result.Text == "Play");
        }
        REQUIRE(heap.Attempts == attempts);
        REQUIRE(heap.Frees == 0);
        heap.FailAt = heap.Attempts + 1;
        REQUIRE(PrepareCatalog(kFrenchCatalog, heap.Domain, active, diagnostic) == Status::OutOfMemory);
        REQUIRE(diagnostic.Result == Status::OutOfMemory);
        REQUIRE(active.ResolveStatic(binding, old) == Status::Ok);
        REQUIRE(old.Text == "Play");
        heap.FailAt = 0;
        REQUIRE(PrepareCatalog(kFrenchCatalog, heap.Domain, active, diagnostic) == Status::Ok);
        REQUIRE(heap.Allocations == 2);
        REQUIRE(active.ResolveStatic(binding, old) == Status::WrongCatalog);
        REQUIRE(old.Text == "Play");
        REQUIRE(retained.ResolveStatic(binding, old) == Status::Ok);
        active = {};
        REQUIRE(heap.Frees == 1);
        REQUIRE(old.Text == "Play");
    }
    REQUIRE(heap.Frees == heap.Allocations);
    REQUIRE(heap.LiveBytes == 0);
}

TEST_CASE("caller storage is copied; separate leases read safely during replacement", "[localization][lifetime]")
{
    Array<uint8> input = Array<uint8>::FromRange(kSourceCatalog);
    Catalog active = Load(input);
    for (uint8& value : input)
    {
        value = 0; // Overwrite the caller storage in place.
    }
    MessageBinding binding;
    REQUIRE(active.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
    std::atomic<bool> good{true};
    Array<std::thread> readers;
    for (usize index = 0; index < 4; ++index)
    {
        readers.AddInPlace([lease = active, binding, &good]() noexcept {
            for (usize iteration = 0; iteration < 10000; ++iteration)
            {
                // Stress concurrent retains/releases of separate immutable handles.
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
                Catalog copy = lease;
                ResolvedText text;
                if (copy.ResolveStatic(binding, text) != Status::Ok || text.Text != "Play")
                {
                    good.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    active = Load(kFrenchCatalog);
    for (auto& reader : readers)
    {
        reader.join();
    }
    REQUIRE(good.load());
    REQUIRE(Resolve(active, localization_fixture::kMenuPlay).Text == "Jouer");
}

TEST_CASE("truncated unaligned and corrupt packages are bounded and transactional", "[localization][codec]")
{
    FaultHeap heap;
    Catalog active;
    Diagnostic diagnostic;
    REQUIRE(PrepareCatalog(kSourceCatalog, heap.Domain, active, diagnostic) == Status::Ok);
    MessageBinding binding;
    REQUIRE(active.BindMessage(localization_fixture::kMenuPlay, binding) == Status::Ok);
    const usize attempts = heap.Attempts;
    for (usize length = 0; length < sizeof(kSourceCatalog); ++length)
    {
        REQUIRE(PrepareCatalog(std::span{kSourceCatalog}.first(length), heap.Domain, active, diagnostic) != Status::Ok);
        REQUIRE(diagnostic.Result != Status::Ok);
    }
    Array<uint8> bytes = Array<uint8>::FromRange(kSourceCatalog);
    const usize records = 48 + Word(bytes, 16) + Word(bytes, 20) + Word(bytes, 24);
    const usize text = Word(bytes, records + 8);
    const usize key = Word(bytes, records);
    const auto reject = [&](usize offset, uint32 value) {
        auto corrupt = bytes;
        Write(corrupt, offset, value);
        REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) != Status::Ok);
    };
    for (usize offset : {usize{0},
                         usize{4},
                         usize{8},
                         usize{12},
                         usize{16},
                         usize{20},
                         usize{24},
                         usize{28},
                         usize{32},
                         usize{36},
                         usize{40},
                         usize{44}})
    {
        reject(offset, 0xffffffffU);
    }
    reject(records, 0xffffffffU);
    reject(records + 4, 129);
    reject(records + 8, 0xffffffffU);
    reject(records + 12, 65537);
    reject(records + 16, 1); // A source catalog cannot claim translated origin.
    auto corrupt = bytes;
    corrupt[key] = 'A';
    REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) == Status::Invalid);
    corrupt = bytes;
    corrupt[text] = 0; // First record has empty text: mutate its next key instead.
    REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) == Status::Invalid);
    // Literal record contains nonempty text; reject embedded NUL and invalid UTF-8.
    const usize literalText = Word(bytes, records + 20 + 8);
    corrupt = bytes;
    corrupt[literalText] = 0;
    REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) == Status::Invalid);
    REQUIRE(diagnostic.Offset == literalText);
    corrupt[literalText] = 0xc0;
    REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) == Status::Invalid);
    REQUIRE(diagnostic.Offset == literalText);
    // Non-increasing exact keys are rejected, even when the byte layout is valid.
    corrupt = bytes;
    const usize secondKey = Word(bytes, records + 20);
    corrupt[secondKey] = 'a';
    REQUIRE(PrepareCatalog(corrupt, heap.Domain, active, diagnostic) == Status::Invalid);
    REQUIRE(heap.Attempts == attempts); // Invalid packages never allocate.
    ResolvedText old;
    REQUIRE(active.ResolveStatic(binding, old) == Status::Ok);
    REQUIRE(old.Text == "Play");
    bytes.InsertAt(0, 0xff);
    Catalog unaligned = Load(std::span<const uint8>{bytes}.subspan(1));
    REQUIRE(Resolve(unaligned, localization_fixture::kMenuPlay).Text == "Play");
}

TEST_CASE("bounded mutation corpus never publishes an invalid lease", "[localization][codec]")
{
    // Exercise header/record arithmetic and UTF-8 boundaries under ASan/UBSan.
    for (usize offset = 0; offset < sizeof(kSourceCatalog); ++offset)
    {
        Array<uint8> bytes = Array<uint8>::FromRange(kSourceCatalog);
        bytes[offset] ^= 0xff;
        Catalog catalog;
        Diagnostic diagnostic;
        const auto status = PrepareCatalog(bytes, GetSystemAllocationDomain(), catalog, diagnostic);
        REQUIRE(catalog.IsValid() == (status == Status::Ok));
        REQUIRE(diagnostic.Result == status);
        if (status == Status::Ok)
        {
            REQUIRE(catalog.GetMessageCount() == 4);
            MessageBinding binding;
            const auto bound = catalog.BindMessage(localization_fixture::kMenuPlay, binding);
            REQUIRE((bound == Status::Ok || bound == Status::NotFound));
            if (bound == Status::Ok)
            {
                ResolvedText text;
                REQUIRE(catalog.ResolveStatic(binding, text) == Status::Ok);
                REQUIRE(text.Provenance == Origin::Source);
            }
        }
    }
}
