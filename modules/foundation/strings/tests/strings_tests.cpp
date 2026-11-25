#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/shared_string.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/strings/string_table.hpp>
#include <ludus/foundation/strings/utf16.hpp>
#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include "internal/reference_count.hpp"
#include "internal/string_table_state.hpp"
#include "internal/table_token.hpp"

#include <atomic>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace threading = ludus::foundation::threading;

namespace ludus::foundation
{
struct StringTableTestAccess final
{
    static uint64 ConstantHash(std::string_view) noexcept
    {
        return 7;
    }
    static void ForceCollisions(StringTable& table) noexcept
    {
        auto& state = *table.mState;
        state.HashOverride = ConstantHash;
        state.Entries[0].Hash = 7;
        for (usize i = 0; i < state.SlotCapacity; ++i)
        {
            state.Slots[i] = StringIndex::INVALID_VALUE;
        }
        state.Slots[7] = 0;
        state.ProbeDistanceCounts = {};
        state.ProbeDistanceCounts[0] = 1;
        state.MaxProbeDistance = 0;
    }
    static usize PageHeaderBytes() noexcept
    {
        return sizeof(StringTable::State::Page);
    }
    // Independently recomputes every occupied slot's distance from its entry's
    // home slot by walking the whole lookup array.
    static void RecomputeProbeDistances(const StringTable& table, StringTableSnapshot& outSnapshot) noexcept
    {
        const StringTable::State& state = *table.mState;
        outSnapshot.ProbeDistanceCounts = {};
        outSnapshot.MaxProbeDistance = 0;
        const usize mask = state.SlotCapacity - 1;
        for (usize slot = 0; slot < state.SlotCapacity; ++slot)
        {
            const uint32 index = state.Slots[slot];
            if (index == StringIndex::INVALID_VALUE)
            {
                continue;
            }
            const usize distance = (slot - static_cast<usize>(state.Entries[index].Hash)) & mask;
            usize bucket = STRING_TABLE_PROBE_BUCKET_COUNT - 1;
            for (usize candidate = 0; candidate + 1 < STRING_TABLE_PROBE_BUCKET_COUNT; ++candidate)
            {
                if (distance < (usize{1} << candidate))
                {
                    bucket = candidate;
                    break;
                }
            }
            ++outSnapshot.ProbeDistanceCounts[bucket];
            outSnapshot.MaxProbeDistance =
                distance > outSnapshot.MaxProbeDistance ? distance : outSnapshot.MaxProbeDistance;
        }
    }
    static uint32 Count(const StringTable& table) noexcept
    {
        return table.mState->Count;
    }
    static usize SlotCapacity(const StringTable& table) noexcept
    {
        return table.mState->SlotCapacity;
    }
    static void SetSlot(StringTable& table, usize slot, StringIndex index) noexcept
    {
        table.mState->Slots[slot] = index.Value;
    }
    static usize Allocated(const StringTable& table) noexcept
    {
        return table.mState->Allocated;
    }
};
} // namespace ludus::foundation

namespace
{
struct FaultHeap final
{
    struct Record final
    {
        void* Pointer{};
        usize Bytes{};
        usize Alignment{};
    };
    std::mutex Mutex;
    Record Records[1024]{};
    usize Attempts{};
    usize Allocations{};
    usize Frees{};
    usize LiveBytes{};
    usize PeakBytes{};
    usize FailAt{};
    bool BadFree{};
    AllocationDomain Domain{this, Allocate, Free};

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        auto& heap = *static_cast<FaultHeap*>(context);
        const std::lock_guard lock(heap.Mutex);
        if (++heap.Attempts == heap.FailAt)
        {
            return nullptr;
        }
        void* pointer = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
        if (pointer == nullptr)
        {
            return nullptr;
        }
        for (auto& record : heap.Records)
        {
            if (record.Pointer == nullptr)
            {
                record = {pointer, bytes, alignment};
                ++heap.Allocations;
                heap.LiveBytes += bytes;
                if (heap.LiveBytes > heap.PeakBytes)
                {
                    heap.PeakBytes = heap.LiveBytes;
                }
                return pointer;
            }
        }
        ::operator delete(pointer, std::align_val_t{alignment});
        return nullptr;
    }
    // Allocator callback ABI fixes the context/pointer order.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void Free(void* context, void* pointer, usize bytes, usize alignment) noexcept
    {
        auto& heap = *static_cast<FaultHeap*>(context);
        const std::lock_guard lock(heap.Mutex);
        for (auto& record : heap.Records)
        {
            if (record.Pointer == pointer)
            {
                heap.BadFree |= record.Bytes != bytes || record.Alignment != alignment;
                heap.LiveBytes -= record.Bytes;
                record = {};
                ++heap.Frees;
                ::operator delete(pointer, std::align_val_t{alignment});
                return;
            }
        }
        heap.BadFree = true;
    }
};
constexpr NameLiteral kPlayer{"player"};
static_assert(kPlayer.Fingerprint == SymbolFingerprint64("player"));
static_assert(SymbolFingerprint64("") == 14695981039346656037ULL);
static_assert(SymbolFingerprint64("a") == 0xaf63dc4c8601ec8cULL);
static_assert(SymbolFingerprint64("foobar") == 0x85944171f73967e8ULL);
static_assert(std::is_copy_constructible_v<String> == false);
static_assert(std::is_nothrow_move_constructible_v<String>);
static_assert(sizeof(NameId) == 8);
static_assert(sizeof(SharedString) == sizeof(void*));
template <typename Owner>
concept TemporaryView = requires { Owner{}.GetView(); };
static_assert(TemporaryView<String> == false && TemporaryView<SharedString> == false &&
              TemporaryView<StaticString<4>> == false);
} // namespace

TEST_CASE("bounded strings keep counted zeros, overlaps and old values on overflow", "[strings]")
{
    StaticString<0> empty;
    REQUIRE(empty.TryAssign({}) == StringStatus::Ok);
    REQUIRE(empty.TryAppend("x") == StringStatus::CapacityExceeded);
    REQUIRE(empty.GetData()[0] == '\0');
    StaticString<8> value;
    REQUIRE(value.TryAssign("abcdef") == StringStatus::Ok);
    REQUIRE(value.TryAssign(value.GetView().substr(2)) == StringStatus::Ok);
    REQUIRE(value.GetView() == "cdef");
    REQUIRE(value.TryAppend(value.GetView()) == StringStatus::Ok);
    REQUIRE(value.GetView() == "cdefcdef");
    REQUIRE(value.GetData()[8] == '\0');
    REQUIRE(value.TryAppend("x") == StringStatus::CapacityExceeded);
    REQUIRE(value.TryAssign("012345678") == StringStatus::CapacityExceeded);
    REQUIRE(value.GetView() == "cdefcdef");
    REQUIRE(value.TryAssign(std::string_view{"a\0b", 3}) == StringStatus::Ok);
    REQUIRE(value.GetSize() == 3);
    REQUIRE(ValidateCString(value.GetView()) == StringStatus::EmbeddedZero);
    value.Clear();
    REQUIRE(value.IsEmpty());
}

TEST_CASE("unique strings preserve allocation provenance, failures and aliased slices", "[strings][allocation]")
{
    FaultHeap first;
    FaultHeap second;
    {
        String value(first.Domain);
        REQUIRE(value.TryAssign("01234567890123456789012") == StringStatus::Ok);
        REQUIRE(first.Attempts == 0);
        REQUIRE(value.TryAppend(value.GetView()) == StringStatus::Ok);
        REQUIRE(first.Allocations == 1);
        const std::string expected(value.GetView());
        const char* pointer = value.GetData();
        first.FailAt = first.Attempts + 1;
        REQUIRE(value.TryAppend(value.GetView()) == StringStatus::OutOfMemory);
        REQUIRE(value.GetView() == expected);
        REQUIRE(value.GetData() == pointer);
        first.FailAt = 0;
        const std::string_view parts[]{value.GetView(), value.GetView().substr(2, 7)};
        REQUIRE(value.TryAppendMany(parts) == StringStatus::Ok);
        REQUIRE(value.GetView() == expected + expected + expected.substr(2, 7));
        REQUIRE(value.GetData()[value.GetSize()] == '\0');
        REQUIRE(first.Allocations == 2);
        REQUIRE(value.TryAssign(value.GetView().substr(3, 30)) == StringStatus::Ok);
        REQUIRE(value.GetView() == (expected + expected).substr(3, 30));
        const usize capacity = value.GetCapacity();
        value.Clear();
        REQUIRE(value.GetCapacity() == capacity);
        REQUIRE(value.TryEnsureCapacity(static_cast<usize>(-1)) == StringStatus::TooLarge);
        REQUIRE(value.GetCapacity() == capacity);
        REQUIRE(value.TryAssign(expected) == StringStatus::Ok);
        String destination(second.Domain);
        REQUIRE(destination.TryAssign(std::string(80, 'x')) == StringStatus::Ok);
        second.FailAt = second.Attempts + 1;
        REQUIRE(value.CloneTo(second.Domain, destination) == StringStatus::OutOfMemory);
        REQUIRE(destination.GetView() == std::string(80, 'x'));
        destination = std::move(value);
        REQUIRE(&destination.GetDomain() == &first.Domain);
        // NOLINTNEXTLINE(bugprone-use-after-move): moved-from empty is the public contract under test.
        REQUIRE(value.IsEmpty());
        REQUIRE(destination.GetView() == expected);
        String& same = destination;
        destination = std::move(same);
        REQUIRE(destination.GetView() == expected);
    }
    REQUIRE(first.LiveBytes == 0);
    REQUIRE(second.LiveBytes == 0);
    REQUIRE_FALSE((first.BadFree || second.BadFree));
}

TEST_CASE("builder reserves once per batch and can be reused after taking ownership", "[strings]")
{
    FaultHeap heap;
    {
        StringBuilder builder(heap.Domain);
        const std::string_view parts[]{"long asset directory/", "characters/", "player.mesh"};
        REQUIRE(builder.TryAppendMany(parts) == StringStatus::Ok);
        REQUIRE(heap.Allocations == 1);
        String result = builder.TakeString();
        REQUIRE(result.GetView() == "long asset directory/characters/player.mesh");
        REQUIRE(builder.GetView().empty());
        REQUIRE(builder.TryAppend("new") == StringStatus::Ok);
        REQUIRE(heap.Allocations == 1);
    }
    REQUIRE(heap.LiveBytes == 0);
}

TEST_CASE("shared strings copy without allocation and release through the original domain", "[strings][allocation]")
{
    FaultHeap heap;
    {
        SharedString value;
        REQUIRE(CreateShared({}, heap.Domain, value) == StringStatus::Ok);
        REQUIRE(heap.Attempts == 0);
        REQUIRE(CreateShared(std::string_view{"hello\0world", 11}, heap.Domain, value) == StringStatus::Ok);
        SharedString copy = value;
        REQUIRE(copy.GetData() == value.GetData());
        REQUIRE(heap.Allocations == 1);
        SharedString& same = value;
        value = same;
        value = std::move(same);
        REQUIRE(value == copy);
        heap.FailAt = heap.Attempts + 1;
        REQUIRE(CreateShared("replacement", heap.Domain, value) == StringStatus::OutOfMemory);
        REQUIRE(value == copy);
        heap.FailAt = 0;
        SharedString independent;
        REQUIRE(CreateShared(value.GetView(), heap.Domain, independent) == StringStatus::Ok);
        REQUIRE(independent == value);
        REQUIRE(independent.GetData() != value.GetData());
        threading::Thread threads[4];
        for (threading::Thread& thread : threads)
        {
            REQUIRE(thread.Start([copy]() {
                for (usize i = 0; i < 10000; ++i)
                {
                    SharedString local = copy;
                    local = {};
                }
            }) == threading::ThreadStatus::Ok);
        }
        for (threading::Thread& thread : threads)
        {
            REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
        }
        REQUIRE(heap.Allocations == 2);
        String mutableCopy(heap.Domain);
        REQUIRE(CopyToString(value, mutableCopy) == StringStatus::Ok);
        REQUIRE(mutableCopy.GetView() == value.GetView());
        REQUIRE(CreateShared(value.GetView().substr(2), heap.Domain, value) == StringStatus::Ok);
        REQUIRE(value.GetView() == copy.GetView().substr(2));
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.Allocations == heap.Frees);
    REQUIRE_FALSE(heap.BadFree);
    std::atomic<uint8> references{253};
    REQUIRE_FALSE(detail::RetainReference(references));
    REQUIRE(detail::RetainReference(references));
    REQUIRE_FALSE(detail::ReleaseReference(references));
    REQUIRE_FALSE(detail::RetainReference(references));
    REQUIRE(references.load() == 255);
}

TEST_CASE("UTF-8 accepts scalar boundaries and rejects malformed bytes without replacing the proof", "[strings][utf8]")
{
    Utf8View output;
    usize error = 99;
    REQUIRE(ValidateUtf8("valid", output, error) == StringStatus::Ok);
    REQUIRE(error == 0);
    const std::string_view invalid[]{"\xc0\x80",
                                     "\xc1\xbf",
                                     "\xe0\x80\x80",
                                     "\xed\xa0\x80",
                                     "\xf0\x80\x80\x80",
                                     "\xf4\x90\x80\x80",
                                     "\xf5\x80\x80\x80",
                                     "\x80",
                                     "\xc2",
                                     "\xe1\x80",
                                     "\xf1\x80\x80",
                                     "\xe1x\x80"};
    for (auto bytes : invalid)
    {
        const std::string prefixed = std::string("ok") + std::string(bytes);
        REQUIRE(ValidateUtf8(prefixed, output, error) == StringStatus::InvalidUtf8);
        REQUIRE(error == 2);
        REQUIRE(output.GetView() == "valid");
    }
    const char embedded[]{'a', '\0', 'b'};
    REQUIRE(ValidateUtf8({embedded, 3}, output, error) == StringStatus::Ok);
    // Exhaustively generate Unicode scalar encodings with an independent
    // code-point encoder, including the U+10FFFF boundary and excluding surrogates.
    for (uint32 scalar = 0; scalar <= 0x10ffff; ++scalar)
    {
        if (scalar >= 0xd800 && scalar <= 0xdfff)
        {
            continue;
        }
        char bytes[4]{};
        usize length = 1;
        if (scalar < 0x80)
        {
            bytes[0] = static_cast<char>(scalar);
        }
        else if (scalar < 0x800)
        {
            length = 2;
            bytes[0] = static_cast<char>(0xc0 | (scalar >> 6));
            bytes[1] = static_cast<char>(0x80 | (scalar & 63));
        }
        else if (scalar < 0x10000)
        {
            length = 3;
            bytes[0] = static_cast<char>(0xe0 | (scalar >> 12));
            bytes[1] = static_cast<char>(0x80 | ((scalar >> 6) & 63));
            bytes[2] = static_cast<char>(0x80 | (scalar & 63));
        }
        else
        {
            length = 4;
            bytes[0] = static_cast<char>(0xf0 | (scalar >> 18));
            bytes[1] = static_cast<char>(0x80 | ((scalar >> 12) & 63));
            bytes[2] = static_cast<char>(0x80 | ((scalar >> 6) & 63));
            bytes[3] = static_cast<char>(0x80 | (scalar & 63));
        }
        if (ValidateUtf8({bytes, length}, output, error) != StringStatus::Ok)
        {
            FAIL("validator rejected scalar " << scalar);
        }
    }
}

TEST_CASE("interning is exact through forced collisions, growth and freezing", "[strings][intern]")
{
    FaultHeap heap;
    {
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        StringTableTestAccess::ForceCollisions(table);
        NameId empty;
        REQUIRE(table.TryFind({}, empty) == StringStatus::Ok);
        REQUIRE(empty.IsValid());
        REQUIRE(empty.Index.Value == 0);
        NameId first;
        REQUIRE(table.TryIntern(kPlayer.Spelling, first) == StringStatus::Ok);
        std::string_view borrowed;
        REQUIRE(table.TryResolve(first, borrowed) == StringStatus::Ok);
        const char* stable = borrowed.data();
        for (usize i = 0; i < 300; ++i)
        {
            NameId id;
            REQUIRE(table.TryIntern(std::to_string(i) + std::string(200, 'x'), id) == StringStatus::Ok);
            REQUIRE(id != first);
        }
        REQUIRE(borrowed == "player");
        REQUIRE(borrowed.data() == stable);
        NameId duplicate;
        const usize allocations = heap.Allocations;
        REQUIRE(table.TryIntern("player", duplicate) == StringStatus::Ok);
        REQUIRE(duplicate == first);
        REQUIRE(heap.Allocations == allocations);
        REQUIRE(table.TryFind("Player", duplicate) == StringStatus::NotFound);
        REQUIRE(duplicate == first);
        REQUIRE(table.TryIntern(std::string_view{"a\0b", 3}, duplicate) == StringStatus::EmbeddedZero);
        REQUIRE(table.TryIntern("\xff", duplicate) == StringStatus::InvalidUtf8);
        StringTable other;
        REQUIRE(CreateTable({}, heap.Domain, other) == StringStatus::Ok);
        REQUIRE(other.TryIntern("player", duplicate) == StringStatus::Ok);
        REQUIRE(duplicate != first);
        std::string_view preserved = "preserved";
        REQUIRE(other.TryResolve(first, preserved) == StringStatus::WrongTable);
        REQUIRE(preserved == "preserved");
        REQUIRE(table.TryResolve({}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(table.TryResolve({first.TableToken, StringIndex{9999}}, preserved) == StringStatus::InvalidHandle);
        FrozenStringTable frozen;
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        REQUIRE_FALSE(table.IsValid());
        REQUIRE(frozen.TryResolve(first, preserved) == StringStatus::Ok);
        REQUIRE(preserved.data() == stable);
        REQUIRE(frozen.TryFind("player", duplicate) == StringStatus::Ok);
        REQUIRE(duplicate == first);
        REQUIRE(heap.Allocations >= allocations);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("colliding probes wrap around the lookup array up to the load limit", "[strings][intern]")
{
    FaultHeap heap;
    {
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        StringTableTestAccess::ForceCollisions(table);
        constexpr usize kNameCount = 11;
        const std::string_view names[kNameCount] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"};
        // The empty entry and these names occupy exactly the 3/4 load limit. All
        // share one start slot, so the later probes wrap past the array's end.
        const usize capacity = StringTableTestAccess::SlotCapacity(table);
        REQUIRE(StringTableTestAccess::Count(table) + kNameCount == capacity - capacity / 4);
        NameId ids[kNameCount]{};
        for (usize i = 0; i < kNameCount; ++i)
        {
            REQUIRE(table.TryIntern(names[i], ids[i]) == StringStatus::Ok);
        }
        REQUIRE(StringTableTestAccess::SlotCapacity(table) == capacity);
        NameId found;
        for (usize i = 0; i < kNameCount; ++i)
        {
            REQUIRE(table.TryFind(names[i], found) == StringStatus::Ok);
            REQUIRE(found == ids[i]);
        }
        NameId preserved = ids[0];
        REQUIRE(table.TryFind("absent", preserved) == StringStatus::NotFound);
        REQUIRE(preserved == ids[0]);
        // One more colliding entry grows the lookup array and rehashes every entry.
        NameId grown;
        REQUIRE(table.TryIntern("absent", grown) == StringStatus::Ok);
        REQUIRE(StringTableTestAccess::SlotCapacity(table) == capacity * 2);
        for (usize i = 0; i < kNameCount; ++i)
        {
            REQUIRE(table.TryFind(names[i], found) == StringStatus::Ok);
            REQUIRE(found == ids[i]);
        }
        REQUIRE(table.TryFind("absent", found) == StringStatus::Ok);
        REQUIRE(found == grown);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.BadFree == false);
}

TEST_CASE("full lookup arrays fail lookups explicitly and leave the table intact", "[strings][intern]")
{
    FaultHeap heap;
    {
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        StringTableTestAccess::ForceCollisions(table);
        NameId player;
        REQUIRE(table.TryIntern(kPlayer.Spelling, player) == StringStatus::Ok);
        NameId enemy;
        REQUIRE(table.TryIntern("enemy", enemy) == StringStatus::Ok);
        // Corrupt the lookup array so that no slot is empty: every slot names
        // "player" except the last slot probed from the shared colliding hash.
        const usize capacity = StringTableTestAccess::SlotCapacity(table);
        const usize start = static_cast<usize>(StringTableTestAccess::ConstantHash({})) & (capacity - 1);
        for (usize slot = 0; slot < capacity; ++slot)
        {
            StringTableTestAccess::SetSlot(table, slot, player.Index);
        }
        StringTableTestAccess::SetSlot(table, (start + capacity - 1) & (capacity - 1), enemy.Index);
        const uint32 count = StringTableTestAccess::Count(table);
        const usize allocated = StringTableTestAccess::Allocated(table);
        const usize allocations = heap.Allocations;

        NameId found;
        REQUIRE(table.TryFind("enemy", found) == StringStatus::Ok);
        REQUIRE(found == enemy);
        REQUIRE(table.TryFind("player", found) == StringStatus::Ok);
        REQUIRE(found == player);
        NameId preserved = player;
        REQUIRE(table.TryFind("absent", preserved) == StringStatus::ProbeExhausted);
        REQUIRE(preserved == player);
        REQUIRE(table.TryIntern("absent", preserved) == StringStatus::ProbeExhausted);
        REQUIRE(preserved == player);
        REQUIRE(StringTableTestAccess::Count(table) == count);
        REQUIRE(StringTableTestAccess::Allocated(table) == allocated);
        REQUIRE(heap.Allocations == allocations);
        REQUIRE(table.TryIntern("enemy", found) == StringStatus::Ok);
        REQUIRE(found == enemy);
        std::string_view spelling;
        REQUIRE(table.TryResolve(player, spelling) == StringStatus::Ok);
        REQUIRE(spelling == "player");
        REQUIRE(table.TryResolve(enemy, spelling) == StringStatus::Ok);
        REQUIRE(spelling == "enemy");

        FrozenStringTable frozen;
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        REQUIRE(frozen.TryFind("absent", preserved) == StringStatus::ProbeExhausted);
        REQUIRE(preserved == player);
        REQUIRE(frozen.TryFind("enemy", found) == StringStatus::Ok);
        REQUIRE(found == enemy);
        REQUIRE(heap.Allocations == allocations);
    }
    {
        // Distinct real hashes take the hash-mismatch path through every slot.
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        NameId player;
        REQUIRE(table.TryIntern(kPlayer.Spelling, player) == StringStatus::Ok);
        const usize capacity = StringTableTestAccess::SlotCapacity(table);
        for (usize slot = 0; slot < capacity; ++slot)
        {
            StringTableTestAccess::SetSlot(table, slot, player.Index);
        }
        NameId found;
        REQUIRE(table.TryFind("player", found) == StringStatus::Ok);
        REQUIRE(found == player);
        NameId preserved = player;
        REQUIRE(table.TryFind("absent", preserved) == StringStatus::ProbeExhausted);
        REQUIRE(table.TryIntern("absent", preserved) == StringStatus::ProbeExhausted);
        REQUIRE(preserved == player);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.BadFree == false);
}

TEST_CASE("table limits admit duplicates and failures consume no indices", "[strings][allocation]")
{
    FaultHeap heap;
    {
        StringTable bounded;
        StringTableConfig config;
        config.MaxEntries = 2;
        REQUIRE(CreateTable(config, heap.Domain, bounded) == StringStatus::Ok);
        NameId first;
        REQUIRE(bounded.TryIntern("first", first) == StringStatus::Ok);
        NameId preserved = first;
        REQUIRE(bounded.TryIntern("second", preserved) == StringStatus::CapacityExceeded);
        REQUIRE(preserved == first);
        heap.FailAt = heap.Attempts + 1;
        REQUIRE(bounded.TryIntern("first", preserved) == StringStatus::Ok);
        REQUIRE(heap.Attempts + 1 == heap.FailAt);
    }
    REQUIRE(heap.LiveBytes == 0);
    heap.FailAt = 0;
    for (usize failure = 1; failure <= 4; ++failure)
    {
        StringTable table;
        StringTableConfig config;
        config.MaxSpellingBytes = 40000;
        config.MaxAllocatedBytes = 200000;
        REQUIRE(CreateTable(config, heap.Domain, table) == StringStatus::Ok);
        for (usize i = 0; i < 7; ++i)
        {
            NameId id;
            REQUIRE(table.TryIntern(std::to_string(i), id) == StringStatus::Ok);
        }
        const uint32 before = StringTableTestAccess::Count(table);
        const std::string large(35000, 'x');
        NameId result;
        heap.FailAt = heap.Attempts + failure;
        const StringStatus status = table.TryIntern(large, result);
        REQUIRE(heap.PeakBytes <= config.MaxAllocatedBytes);
        if (status == StringStatus::OutOfMemory)
        {
            REQUIRE_FALSE(result.IsValid());
            REQUIRE(StringTableTestAccess::Count(table) == before);
            REQUIRE(table.TryFind(large, result) == StringStatus::NotFound);
        }
        else
        {
            REQUIRE(status == StringStatus::Ok);
        }
        heap.FailAt = 0;
        REQUIRE(table.TryIntern(large, result) == StringStatus::Ok);
        REQUIRE(result.Index.Value == before);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("creation and table byte budgets account for every allocation", "[strings][allocation]")
{
    for (usize failure = 1; failure <= 4; ++failure)
    {
        FaultHeap heap;
        heap.FailAt = failure;
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::OutOfMemory);
        REQUIRE_FALSE(table.IsValid());
        REQUIRE(heap.LiveBytes == 0);
        REQUIRE_FALSE(heap.BadFree);
    }
    FaultHeap heap;
    {
        StringTableConfig config;
        config.MaxAllocatedBytes = 1500;
        StringTable table;
        REQUIRE(CreateTable(config, heap.Domain, table) == StringStatus::Ok);
        NameId id;
        for (usize i = 0; i < 7; ++i)
        {
            REQUIRE(table.TryIntern(std::to_string(i), id) == StringStatus::Ok);
        }
        const NameId preserved = id;
        const usize attempts = heap.Attempts;
        REQUIRE(table.TryIntern("growth", id) == StringStatus::CapacityExceeded);
        REQUIRE(id == preserved);
        REQUIRE(heap.Attempts == attempts);
        REQUIRE(table.TryIntern("6", id) == StringStatus::Ok);
        REQUIRE(id == preserved);
        REQUIRE(StringTableTestAccess::Allocated(table) == heap.LiveBytes);
        REQUIRE(heap.PeakBytes <= config.MaxAllocatedBytes);
        FrozenStringTable frozen;
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        StringTable another;
        REQUIRE(CreateTable({}, heap.Domain, another) == StringStatus::Ok);
        REQUIRE(another.TryFreeze(frozen) == StringStatus::InvalidArgument);
        REQUIRE(another.IsValid());
        REQUIRE(frozen.TryFind("6", id) == StringStatus::Ok);
        REQUIRE(id == preserved);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

namespace
{
template <typename Lookup>
concept TemporaryIndexView = requires(std::string_view& outView) { Lookup{}.TryResolveIndex(StringIndex{}, outView); };
static_assert(TemporaryIndexView<StringTable> == false && TemporaryIndexView<FrozenStringTable> == false);
static_assert(std::is_trivially_copyable_v<StringTableSnapshot>);

// Checks the snapshot's byte identities against the heap's own record of the
// table's live allocations, and its probe histogram against a full recount.
StringTableSnapshot CheckSnapshotAccounting(const StringTable& table, const FaultHeap& heap)
{
    StringTableSnapshot snapshot;
    const usize attempts = heap.Attempts;
    REQUIRE(table.TryCaptureSnapshot(snapshot) == StringStatus::Ok);
    REQUIRE(heap.Attempts == attempts);
    REQUIRE(snapshot.AllocatedBytes == heap.LiveBytes);
    REQUIRE(snapshot.AllocatedBytes ==
            snapshot.StateBytes + snapshot.EntryBytes + snapshot.LookupBytes + snapshot.PageBytes);
    REQUIRE(snapshot.PageBytes == snapshot.SpellingBytes + snapshot.TerminatorBytes + snapshot.PageSlackBytes +
                                      snapshot.PageCount * StringTableTestAccess::PageHeaderBytes());
    REQUIRE(snapshot.TerminatorBytes == snapshot.EntryCount);
    REQUIRE(snapshot.EntryCount == StringTableTestAccess::Count(table));
    REQUIRE(snapshot.EntryCount <= snapshot.EntryCapacity);
    REQUIRE(snapshot.EntryCount <= snapshot.LookupSlotCount - snapshot.LookupSlotCount / 4);
    REQUIRE(snapshot.LookupSlotCount == StringTableTestAccess::SlotCapacity(table));
    REQUIRE(snapshot.AllocatedBytes <= snapshot.PeakAllocatedBytes);
    REQUIRE(snapshot.PeakAllocatedBytes <= snapshot.MaxAllocatedBytes);
    StringTableSnapshot recount;
    StringTableTestAccess::RecomputeProbeDistances(table, recount);
    REQUIRE(snapshot.ProbeDistanceCounts == recount.ProbeDistanceCounts);
    REQUIRE(snapshot.MaxProbeDistance == recount.MaxProbeDistance);
    usize histogramEntries = 0;
    for (const uint32 entries : snapshot.ProbeDistanceCounts)
    {
        histogramEntries += entries;
    }
    REQUIRE(histogramEntries == snapshot.EntryCount);
    return snapshot;
}
} // namespace

TEST_CASE("table-local indices resolve only entries of their owner and keep views stable", "[strings][intern]")
{
    FaultHeap heap;
    {
        const StringTable empty;
        std::string_view preserved = "preserved";
        REQUIRE(empty.TryResolveIndex(StringIndex{0}, preserved) == StringStatus::InvalidHandle);
        StringTableSnapshot untouched;
        untouched.EntryCount = 3;
        REQUIRE(empty.TryCaptureSnapshot(untouched) == StringStatus::InvalidHandle);
        REQUIRE(untouched.EntryCount == 3);

        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        REQUIRE(table.TryResolveIndex(StringIndex{0}, preserved) == StringStatus::Ok);
        REQUIRE(preserved.empty());
        REQUIRE(preserved.data()[0] == '\0');
        NameId player;
        REQUIRE(table.TryIntern("player", player) == StringStatus::Ok);
        std::string_view stable;
        REQUIRE(table.TryResolveIndex(player.Index, stable) == StringStatus::Ok);
        REQUIRE(stable == "player");
        preserved = "preserved";
        REQUIRE(table.TryResolveIndex(StringIndex{}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(table.TryResolveIndex(StringIndex{2}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(table.TryResolveIndex(StringIndex{StringIndex::INVALID_VALUE - 1}, preserved) ==
                StringStatus::InvalidHandle);
        REQUIRE(preserved == "preserved");

        // Growth of the directory, lookup array and pages never moves a spelling.
        for (usize i = 0; i < 400; ++i)
        {
            NameId id;
            REQUIRE(table.TryIntern(std::to_string(i) + std::string(100, 'y'), id) == StringStatus::Ok);
        }
        std::string_view after;
        REQUIRE(table.TryResolveIndex(player.Index, after) == StringStatus::Ok);
        REQUIRE(after.data() == stable.data());

        // The index carries no token: an in-range index of another table names
        // this table's entry, so records must keep their owning table.
        StringTable other;
        REQUIRE(CreateTable({}, heap.Domain, other) == StringStatus::Ok);
        NameId enemy;
        REQUIRE(other.TryIntern("enemy", enemy) == StringStatus::Ok);
        REQUIRE(enemy.Index == player.Index);
        std::string_view foreign;
        REQUIRE(table.TryResolveIndex(enemy.Index, foreign) == StringStatus::Ok);
        REQUIRE(foreign == "player");
        REQUIRE(table.TryResolve(enemy, foreign) == StringStatus::WrongTable);

        FrozenStringTable frozen;
        preserved = "preserved";
        REQUIRE(frozen.TryResolveIndex(StringIndex{0}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(frozen.TryCaptureSnapshot(untouched) == StringStatus::InvalidHandle);
        REQUIRE(untouched.EntryCount == 3);
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        REQUIRE(table.TryResolveIndex(player.Index, preserved) == StringStatus::InvalidHandle);
        REQUIRE(preserved == "preserved");
        const usize attempts = heap.Attempts;
        REQUIRE(frozen.TryResolveIndex(player.Index, after) == StringStatus::Ok);
        REQUIRE(after.data() == stable.data());
        REQUIRE(frozen.TryResolveIndex(StringIndex{402}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(frozen.TryResolveIndex(StringIndex{}, preserved) == StringStatus::InvalidHandle);
        REQUIRE(preserved == "preserved");
        REQUIRE(heap.Attempts == attempts);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("reservations let known content intern without allocating", "[strings][allocation]")
{
    constexpr uint32 NAME_COUNT = 500;
    FaultHeap heap;
    {
        StringTable empty;
        REQUIRE(empty.TryReserve(1, 1) == StringStatus::InvalidHandle);

        StringTable table;
        StringTableConfig config;
        config.MaxEntries = NAME_COUNT + 2;
        REQUIRE(CreateTable(config, heap.Domain, table) == StringStatus::Ok);
        NameId first;
        REQUIRE(table.TryIntern("first", first) == StringStatus::Ok);
        const StringTableSnapshot before = CheckSnapshotAccounting(table, heap);
        usize attempts = heap.Attempts;
        REQUIRE(table.TryReserve(0, 0) == StringStatus::Ok);
        REQUIRE(table.TryReserve(0, 1) == StringStatus::InvalidArgument);
        REQUIRE(table.TryReserve(NAME_COUNT + 1, 0) == StringStatus::CapacityExceeded);
        REQUIRE(table.TryReserve(1, static_cast<usize>(-1)) == StringStatus::TooLarge);
        REQUIRE(heap.Attempts == attempts);
        REQUIRE(CheckSnapshotAccounting(table, heap) == before);

        Array<std::string> names;
        usize spellingBytes = 0;
        for (uint32 i = 0; i < NAME_COUNT; ++i)
        {
            // Lengths vary so each spelling must pack exactly into the reserved page.
            REQUIRE(names.TryAdd(std::to_string(i) + std::string(i % 197, 'n')) == true);
            spellingBytes += names[i].size();
        }
        REQUIRE(table.TryReserve(NAME_COUNT, spellingBytes) == StringStatus::Ok);
        const StringTableSnapshot reserved = CheckSnapshotAccounting(table, heap);
        REQUIRE(reserved.EntryCapacity == NAME_COUNT + 2);
        REQUIRE(reserved.PageCount == before.PageCount + 1);
        REQUIRE(reserved.PageSlackBytes == before.PageSlackBytes + spellingBytes + NAME_COUNT);
        // An already satisfied reservation allocates nothing.
        attempts = heap.Attempts;
        REQUIRE(table.TryReserve(NAME_COUNT, spellingBytes) == StringStatus::Ok);
        REQUIRE(table.TryReserve(1, 1) == StringStatus::Ok);
        for (const std::string& name : names)
        {
            NameId id;
            REQUIRE(table.TryIntern(name, id) == StringStatus::Ok);
            REQUIRE(table.TryIntern(name, id) == StringStatus::Ok);
        }
        REQUIRE(heap.Attempts == attempts);
        const StringTableSnapshot filled = CheckSnapshotAccounting(table, heap);
        REQUIRE(filled.EntryCount == NAME_COUNT + 2);
        REQUIRE(filled.PageCount == reserved.PageCount);
        REQUIRE(filled.AllocatedBytes == reserved.AllocatedBytes);
        REQUIRE(filled.PageSlackBytes == before.PageSlackBytes);
        REQUIRE(filled.SpellingBytes == before.SpellingBytes + spellingBytes);
        REQUIRE(filled.DuplicateAdmissions == before.DuplicateAdmissions + NAME_COUNT);
        REQUIRE(table.TryReserve(1, 0) == StringStatus::CapacityExceeded);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("reservation budgets are exact at the growth peak and failures change nothing", "[strings][allocation]")
{
    constexpr uint32 NAME_COUNT = 1000;
    struct ReservationCase final
    {
        const char* Name;
        usize SpellingBytes;
        bool ShouldAddPage;
    };
    // Small payloads fit the current page, so the peak is the moment the old
    // and replacement lookup arrays coexist; large payloads end at the page.
    const ReservationCase cases[]{
        {"fits current page", 2000, false},
        {"needs a reserved page", 40000, true},
    };
    for (const ReservationCase& reservation : cases)
    {
        CAPTURE(reservation.Name);
        FaultHeap reference;
        usize peak = 0;
        {
            StringTable table;
            REQUIRE(CreateTable({}, reference.Domain, table) == StringStatus::Ok);
            const StringTableSnapshot before = CheckSnapshotAccounting(table, reference);
            REQUIRE(table.TryReserve(NAME_COUNT, reservation.SpellingBytes) == StringStatus::Ok);
            const StringTableSnapshot after = CheckSnapshotAccounting(table, reference);
            REQUIRE((after.PageCount == before.PageCount + 1) == reservation.ShouldAddPage);
            // The old lookup array is still part of before.AllocatedBytes while
            // its replacement is allocated.
            const usize lookupPeak = before.AllocatedBytes - before.EntryBytes + after.EntryBytes + after.LookupBytes;
            REQUIRE((lookupPeak > after.AllocatedBytes) == (reservation.ShouldAddPage == false));
            peak = reservation.ShouldAddPage == true ? after.AllocatedBytes : lookupPeak;
            REQUIRE(after.PeakAllocatedBytes == peak);
            REQUIRE(reference.PeakBytes == peak);
        }
        for (usize budget = peak - 1; budget <= peak; ++budget)
        {
            CAPTURE(budget);
            FaultHeap heap;
            StringTableConfig config;
            config.MaxAllocatedBytes = budget;
            StringTable table;
            REQUIRE(CreateTable(config, heap.Domain, table) == StringStatus::Ok);
            const StringTableSnapshot before = CheckSnapshotAccounting(table, heap);
            const usize attempts = heap.Attempts;
            const StringStatus status = table.TryReserve(NAME_COUNT, reservation.SpellingBytes);
            if (budget < peak)
            {
                REQUIRE(status == StringStatus::CapacityExceeded);
                REQUIRE(heap.Attempts == attempts);
                REQUIRE(CheckSnapshotAccounting(table, heap) == before);
            }
            else
            {
                REQUIRE(status == StringStatus::Ok);
                const StringTableSnapshot after = CheckSnapshotAccounting(table, heap);
                REQUIRE(after.PeakAllocatedBytes == budget);
                REQUIRE(heap.PeakBytes == budget);
            }
        }
        // A domain failure at each growth step keeps every entry and the budget.
        for (usize failure = 1; failure <= 3; ++failure)
        {
            CAPTURE(failure);
            FaultHeap heap;
            StringTable table;
            REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
            NameId player;
            REQUIRE(table.TryIntern("player", player) == StringStatus::Ok);
            std::string_view stable;
            REQUIRE(table.TryResolve(player, stable) == StringStatus::Ok);
            heap.FailAt = heap.Attempts + failure;
            const StringStatus status = table.TryReserve(NAME_COUNT, reservation.SpellingBytes);
            const bool hasFailed = heap.Attempts == heap.FailAt;
            heap.FailAt = 0;
            REQUIRE(status == (hasFailed == true ? StringStatus::OutOfMemory : StringStatus::Ok));
            const StringTableSnapshot after = CheckSnapshotAccounting(table, heap);
            REQUIRE(after.EntryCount == 2);
            std::string_view resolved;
            REQUIRE(table.TryResolve(player, resolved) == StringStatus::Ok);
            REQUIRE(resolved.data() == stable.data());
            NameId found;
            REQUIRE(table.TryFind("player", found) == StringStatus::Ok);
            REQUIRE(found == player);
            REQUIRE(table.TryReserve(NAME_COUNT, reservation.SpellingBytes) == StringStatus::Ok);
        }
    }
}

TEST_CASE("snapshots report exact footprint, duplicates and probe distances without allocating", "[strings][intern]")
{
    FaultHeap heap;
    {
        StringTable table;
        REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
        const StringTableSnapshot created = CheckSnapshotAccounting(table, heap);
        REQUIRE(created.EntryCount == 1);
        REQUIRE(created.SpellingBytes == 0);
        REQUIRE(created.PageCount == 1);
        REQUIRE(created.DuplicateAdmissions == 0);
        REQUIRE(created.MaxEntries == StringTableConfig{}.MaxEntries);
        REQUIRE(created.MaxAllocatedBytes == StringTableConfig{}.MaxAllocatedBytes);
        REQUIRE(created.ProbeDistanceCounts[0] == 1);

        // A constant hash places the n-th entry n slots from the shared home slot.
        StringTableTestAccess::ForceCollisions(table);
        constexpr usize COLLIDING_NAME_COUNT = 70;
        usize spellingBytes = 0;
        for (usize i = 1; i <= COLLIDING_NAME_COUNT; ++i)
        {
            const std::string name = "c" + std::to_string(i);
            spellingBytes += name.size();
            NameId id;
            REQUIRE(table.TryIntern(name, id) == StringStatus::Ok);
            REQUIRE(table.TryIntern(name, id) == StringStatus::Ok);
            CheckSnapshotAccounting(table, heap);
        }
        // Lookups never change the counters.
        NameId found;
        REQUIRE(table.TryFind("c1", found) == StringStatus::Ok);
        REQUIRE(table.TryFind("absent", found) == StringStatus::NotFound);
        const StringTableSnapshot collided = CheckSnapshotAccounting(table, heap);
        REQUIRE(collided.EntryCount == COLLIDING_NAME_COUNT + 1);
        REQUIRE(collided.SpellingBytes == spellingBytes);
        REQUIRE(collided.DuplicateAdmissions == COLLIDING_NAME_COUNT);
        REQUIRE(collided.MaxProbeDistance == COLLIDING_NAME_COUNT);
        const StaticArray<uint32, STRING_TABLE_PROBE_BUCKET_COUNT> expected{1, 1, 2, 4, 8, 16, 32, 7};
        REQUIRE(collided.ProbeDistanceCounts == expected);
        REQUIRE(collided.PeakAllocatedBytes > collided.AllocatedBytes);

        // Freezing transfers the counters unchanged; concurrent frozen
        // snapshots take no lock and allocate nothing.
        FrozenStringTable frozen;
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        const usize attempts = heap.Attempts;
        std::atomic<bool> isGood{true};
        threading::Thread threads[4];
        for (threading::Thread& thread : threads)
        {
            REQUIRE(thread.Start([&]() {
                for (usize i = 0; i < 200; ++i)
                {
                    StringTableSnapshot snapshot;
                    if (frozen.TryCaptureSnapshot(snapshot) != StringStatus::Ok || (snapshot == collided) == false)
                    {
                        isGood = false;
                    }
                }
            }) == threading::ThreadStatus::Ok);
        }
        for (threading::Thread& thread : threads)
        {
            REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
        }
        REQUIRE(isGood.load() == true);
        REQUIRE(heap.Attempts == attempts);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("tables serialize mutations and frozen reads need no allocations", "[strings][threads]")
{
    FaultHeap heap;
    StringTable table;
    REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
    std::atomic<bool> good{true};
    threading::Thread threads[4];
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Start([&]() {
            for (usize i = 0; i < 200; ++i)
            {
                NameId id;
                std::string_view resolved;
                if (table.TryIntern(std::to_string(i), id) != StringStatus::Ok ||
                    table.TryResolve(id, resolved) != StringStatus::Ok || resolved != std::to_string(i))
                {
                    good = false;
                }
            }
        }) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    REQUIRE(good.load());
    REQUIRE(StringTableTestAccess::Count(table) == 201);
    FrozenStringTable frozen;
    REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
    const usize attempts = heap.Attempts;
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Start([&]() {
            for (usize i = 0; i < 200; ++i)
            {
                NameId id;
                if (frozen.TryFind(std::to_string(i), id) != StringStatus::Ok)
                {
                    good = false;
                }
            }
        }) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    REQUIRE(good.load());
    REQUIRE(heap.Attempts == attempts);
}

TEST_CASE("hash policies use exact counted bytes and official golden vectors", "[strings][hash]")
{
    REQUIRE(TableHash64({}) == 0x2d06800538d394c2ULL);
    REQUIRE(StableFingerprint128({}) == Fingerprint128{0x6001c324468d497fULL, 0x99aa06d3014798d8ULL});
    REQUIRE(SymbolFingerprint64(std::string_view{"a\0b", 3}) != SymbolFingerprint64("a"));
    const uint8 message[15]{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    // Aumasson/Bernstein, SipHash paper appendix A and reference vectors:
    // https://github.com/veorq/SipHash/blob/master/vectors.h (CC0).
    constexpr SipHashKey key{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
    REQUIRE(KeyedTableHash64({}, key) == 0x726fdb47dd0e0e31ULL);
    REQUIRE(KeyedTableHash64({message, 1}, key) == 0x74f839c593dc67fdULL);
    REQUIRE(KeyedTableHash64(message, key) == 0xa129ca6149be45e5ULL);
    // All 64 official length vectors cover full-word and tail boundaries.
    const uint64 vectors[]{
        0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL, 0x85676696d7fb7e2dULL,
        0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL, 0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL,
        0x93f5f5799a932462ULL, 0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
        0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL, 0xa129ca6149be45e5ULL,
        0x3f2acc7f57c29bdbULL, 0x699ae9f52cbe4794ULL, 0x4bc1b3f0968dd39cULL, 0xbb6dc91da77961bdULL,
        0xbed65cf21aa2ee98ULL, 0xd0f2cbb02e3b67c7ULL, 0x93536795e3a33e88ULL, 0xa80c038ccd5ccec8ULL,
        0xb8ad50c6f649af94ULL, 0xbce192de8a85b8eaULL, 0x17d835b85bbb15f3ULL, 0x2f2e6163076bcfadULL,
        0xde4daaaca71dc9a5ULL, 0xa6a2506687956571ULL, 0xad87a3535c49ef28ULL, 0x32d892fad841c342ULL,
        0x7127512f72f27cceULL, 0xa7f32346f95978e3ULL, 0x12e0b01abb051238ULL, 0x15e034d40fa197aeULL,
        0x314dffbe0815a3b4ULL, 0x027990f029623981ULL, 0xcadcd4e59ef40c4dULL, 0x9abfd8766a33735cULL,
        0x0e3ea96b5304a7d0ULL, 0xad0c42d6fc585992ULL, 0x187306c89bc215a9ULL, 0xd4a60abcf3792b95ULL,
        0xf935451de4f21df2ULL, 0xa9538f0419755787ULL, 0xdb9acddff56ca510ULL, 0xd06c98cd5c0975ebULL,
        0xe612a3cb9ecba951ULL, 0xc766e62cfcadaf96ULL, 0xee64435a9752fe72ULL, 0xa192d576b245165aULL,
        0x0a8787bf8ecb74b2ULL, 0x81b3e73d20b49b6fULL, 0x7fa8220ba3b2eceaULL, 0x245731c13ca42499ULL,
        0xb78dbfaf3a8d83bdULL, 0xea1ad565322a1a0bULL, 0x60e61c23a3795013ULL, 0x6606d7e446282b93ULL,
        0x6ca4ecb15c5f91e1ULL, 0x9f626da15c9625f3ULL, 0xe51b38608ef25f57ULL, 0x958a324ceb064572ULL,
    };
    uint8 sequence[64]{};
    for (usize i = 0; i < 64; ++i)
    {
        sequence[i] = static_cast<uint8>(i);
    }
    for (usize length = 0; length < 64; ++length)
    {
        REQUIRE(KeyedTableHash64({sequence, length}, key) == vectors[length]);
    }
    StringTable table;
    StringTableConfig config;
    config.HashPolicy = StringHashPolicy::Untrusted;
    REQUIRE(CreateTable(config, GetSystemAllocationDomain(), table) == StringStatus::InvalidArgument);
    config.HasSecretKey = true;
    config.Key = key;
    config.RequireUtf8 = false;
    config.RejectEmbeddedZero = false;
    REQUIRE(CreateTable(config, GetSystemAllocationDomain(), table) == StringStatus::Ok);
    NameId id;
    REQUIRE(table.TryIntern(std::string_view{"\xff\0", 2}, id) == StringStatus::Ok);
    std::string_view resolved;
    REQUIRE(table.TryResolve(id, resolved) == StringStatus::Ok);
    REQUIRE(resolved == std::string_view{"\xff\0", 2});
}

TEST_CASE("reduced-width token issuers report exhaustion without wrapping", "[strings][intern]")
{
    std::atomic<uint8> next{1};
    uint8 token = 0;
    for (uint32 expected = 1; expected <= 0xffU; ++expected)
    {
        REQUIRE(detail::IssueTableToken(next, token) == StringStatus::Ok);
        REQUIRE(token == expected);
    }
    REQUIRE(next.load() == 0);
    for (usize attempt = 0; attempt < 3; ++attempt)
    {
        REQUIRE(detail::IssueTableToken(next, token) == StringStatus::TokenExhausted);
        REQUIRE(token == 0xffU);
        REQUIRE(next.load() == 0);
    }
}

TEST_CASE("table creation fails cleanly once the process token issuer is exhausted", "[strings][intern][allocation]")
{
    // Moving the process issuer to its last 32-bit token reaches exhaustion
    // without creating 2^32 tables. The restored value exceeds every token still
    // held by a live table, and the guard also restores after a failed assertion.
    struct IssuerRestore final
    {
        std::atomic<uint32>* Issuer;
        uint32 Saved;
        ~IssuerRestore() noexcept
        {
            Issuer->store(Saved);
        }
    };
    std::atomic<uint32>& issuer = detail::GetTableTokenIssuer();
    const IssuerRestore restore{&issuer, issuer.load()};
    REQUIRE(restore.Saved != 0);
    FaultHeap heap;
    {
        StringTable last;
        issuer.store(0xffffffffU);
        REQUIRE(CreateTable({}, heap.Domain, last) == StringStatus::Ok);
        REQUIRE(issuer.load() == 0);
        NameId player;
        REQUIRE(last.TryIntern(kPlayer.Spelling, player) == StringStatus::Ok);
        REQUIRE(player.TableToken == 0xffffffffU);
        REQUIRE(player.IsValid() == true);

        const usize liveBytes = heap.LiveBytes;
        const usize allocations = heap.Allocations;
        const usize frees = heap.Frees;
        for (usize attempt = 0; attempt < 3; ++attempt)
        {
            StringTable exhausted;
            REQUIRE(CreateTable({}, heap.Domain, exhausted) == StringStatus::TokenExhausted);
            REQUIRE(exhausted.IsValid() == false);
            REQUIRE(issuer.load() == 0);
            NameId preserved = player;
            REQUIRE(exhausted.TryIntern("enemy", preserved) == StringStatus::InvalidHandle);
            REQUIRE(preserved == player);
        }
        // Each failed creation allocated its state and empty entry before asking
        // for a token, then released every byte through the same domain.
        REQUIRE(heap.Allocations > allocations);
        REQUIRE(heap.Allocations - allocations == heap.Frees - frees);
        REQUIRE(heap.LiveBytes == liveBytes);

        // A nonempty output is rejected before any token is requested.
        REQUIRE(CreateTable({}, heap.Domain, last) == StringStatus::InvalidArgument);
        REQUIRE(heap.Allocations - allocations == heap.Frees - frees);
        std::string_view spelling;
        REQUIRE(last.TryResolve(player, spelling) == StringStatus::Ok);
        REQUIRE(spelling == "player");
        NameId found;
        REQUIRE(last.TryFind("player", found) == StringStatus::Ok);
        REQUIRE(found == player);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.Allocations == heap.Frees);
    REQUIRE(heap.BadFree == false);
}

namespace
{
// SplitMix64 step, with thanks to Guy L. Steele Jr., Doug Lea and Christine H.
// Flood, "Fast Splittable Pseudorandom Number Generators", OOPSLA 2014,
// https://doi.org/10.1145/2660193.2660195. Used only to drive deterministic sequences.
uint64 NextRandom(uint64& inoutState) noexcept
{
    inoutState += 0x9e3779b97f4a7c15ULL;
    uint64 mixed = inoutState;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    return mixed ^ (mixed >> 31U);
}
usize PickBelow(uint64& inoutState, usize bound) noexcept
{
    return static_cast<usize>(NextRandom(inoutState) % bound);
}

constexpr usize SHORT_SPELLING_COUNT = 400;
constexpr usize CASE_VARIANT_STRIDE = 8;
constexpr usize LONG_SPELLING_COUNT = 60;
constexpr usize ENCODED_SPELLING_COUNT = 20;
// Operation mix per hundred steps: interns, then exact lookups, then resolutions.
constexpr usize INTERN_PERCENT = 55;
constexpr usize FIND_PERCENT = 25;
// Chance per step of arming an allocation fault while none is pending. A fault
// stays armed until an allocation consumes it, so it lands at a random step.
constexpr usize FAULT_ARM_PERCENT = 10;
// One intern performs at most three allocations: entries, lookup slots, page.
constexpr usize INTERN_ALLOCATION_STAGES = 3;
constexpr NameId SENTINEL_ID{0x5eedU, StringIndex{0x1234U}};
constexpr std::string_view SENTINEL_VIEW = "sentinel";

// A spelling, and whether its construction deliberately breaks UTF-8.
struct SequenceSpelling final
{
    std::string Bytes;
    bool IsInvalidUtf8{};
};
struct DifferentialScenario final
{
    const char* Name;
    uint64 Seed;
    usize Steps;
    StringTableConfig Config;
    bool ShouldForceCollisions;
    bool CanExhaustByteBudget; ///< a new spelling below MaxEntries may meet CapacityExceeded
    usize MinimumSlotGrowths;
};
struct DifferentialCoverage final
{
    usize Inserted{};
    usize Duplicates{};
    usize Rejected{};
    usize OutOfMemory{};
    usize CapacityExceeded{};
    usize WrongTable{};
    usize SlotGrowths{};
};
using ReferenceIndices = std::unordered_map<std::string, uint32>;
// Independent model of a table: insertion-ordered exact bytes and the first
// view the table returned for each index.
struct ReferenceTable final
{
    Array<std::string> Spellings{std::string{}};
    ReferenceIndices Indices{{std::string{}, 0U}};
    Array<const char*> Views;
};
struct SequenceTokens final
{
    uint32 Table;
    uint32 Other;
};

void AddSpelling(Array<SequenceSpelling>& inoutSpellings, std::string bytes, bool isInvalidUtf8)
{
    const bool isAdded = inoutSpellings.TryAdd({std::move(bytes), isInvalidUtf8});
    REQUIRE(isAdded == true);
}
Array<SequenceSpelling> MakeSequenceSpellings(usize maxSpellingBytes, uint64& inoutState)
{
    Array<SequenceSpelling> spellings;
    AddSpelling(spellings, std::string{}, false);
    for (usize i = 0; i < SHORT_SPELLING_COUNT; ++i)
    {
        AddSpelling(spellings, "name" + std::to_string(i), false);
        if (i % CASE_VARIANT_STRIDE == 0)
        {
            AddSpelling(spellings, "Name" + std::to_string(i), false);
        }
    }
    // Long spellings share a prefix and differ only in its length and suffix.
    const usize longSpan = maxSpellingBytes / 2;
    for (usize i = 0; i < LONG_SPELLING_COUNT; ++i)
    {
        const usize length = longSpan + PickBelow(inoutState, longSpan - 8);
        AddSpelling(spellings, std::string(length, 'p') + std::to_string(i), false);
    }
    AddSpelling(spellings, std::string(maxSpellingBytes, 'm'), false);
    AddSpelling(spellings, std::string(maxSpellingBytes - 1, 'm'), false);
    AddSpelling(spellings, std::string(maxSpellingBytes + 1, 'm'), false);
    // Length is checked before zero bytes and UTF-8.
    std::string oversized(maxSpellingBytes + 1, 'z');
    oversized[0] = '\0';
    oversized[1] = '\xff';
    AddSpelling(spellings, oversized, true);
    for (usize i = 0; i < ENCODED_SPELLING_COUNT; ++i)
    {
        const std::string suffix = std::to_string(i);
        AddSpelling(spellings, std::string("\xc3\xa9t\xc3\xa9") + suffix, false);
        AddSpelling(spellings, std::string("\xe2\x82\xac") + suffix, false);
        AddSpelling(spellings, std::string("\xf0\x9f\x8e\xae") + suffix, false);
        // Rejected unless the scenario admits opaque bytes.
        AddSpelling(spellings, std::string("zero") + std::string(1, '\0') + suffix, false);
        AddSpelling(spellings, std::string("\xff") + suffix, true);
        AddSpelling(spellings, std::string("\xc0\x80") + suffix, true);
        AddSpelling(spellings, std::string("\xed\xa0\x80") + suffix, true);
        AddSpelling(spellings, suffix + std::string("\xc3"), true);
    }
    // Zero bytes are checked before UTF-8.
    AddSpelling(spellings, std::string(1, '\0'), false);
    AddSpelling(spellings, std::string("\0\xff", 2), true);
    return spellings;
}
StringStatus ComputeExpectedValidation(const StringTableConfig& config, const SequenceSpelling& spelling)
{
    if (spelling.Bytes.size() > config.MaxSpellingBytes)
    {
        return StringStatus::TooLarge;
    }
    if (config.RejectEmbeddedZero == true && spelling.Bytes.find('\0') != std::string::npos)
    {
        return StringStatus::EmbeddedZero;
    }
    if (config.RequireUtf8 == true && spelling.IsInvalidUtf8 == true)
    {
        return StringStatus::InvalidUtf8;
    }
    return StringStatus::Ok;
}
NameId
MakeResolveCase(uint64& inoutState, const ReferenceTable& reference, SequenceTokens tokens, StringStatus& outExpected)
{
    const usize count = reference.Spellings.GetSize();
    const StringIndex known{static_cast<uint32>(PickBelow(inoutState, count))};
    switch (PickBelow(inoutState, 6))
    {
        case 0:
            outExpected = StringStatus::InvalidHandle;
            return {tokens.Table, StringIndex{static_cast<uint32>(count + PickBelow(inoutState, 4))}};
        case 1:
            outExpected = StringStatus::InvalidHandle;
            return {tokens.Table, StringIndex{}};
        case 2:
            outExpected = StringStatus::InvalidHandle;
            return {0, known};
        case 3:
            outExpected = StringStatus::WrongTable;
            return {tokens.Other, known};
        default:
            outExpected = StringStatus::Ok;
            return {tokens.Table, known};
    }
}
template <typename Lookup>
void CheckFind(const Lookup& lookup,
               const StringTableConfig& config,
               const ReferenceTable& reference,
               uint32 token,
               const SequenceSpelling& spelling)
{
    const StringStatus expected = ComputeExpectedValidation(config, spelling);
    NameId found = SENTINEL_ID;
    const StringStatus status = lookup.TryFind(spelling.Bytes, found);
    if (expected != StringStatus::Ok)
    {
        REQUIRE(status == expected);
        REQUIRE(found == SENTINEL_ID);
        return;
    }
    const ReferenceIndices::const_iterator match = reference.Indices.find(spelling.Bytes);
    if (match == reference.Indices.end())
    {
        REQUIRE(status == StringStatus::NotFound);
        REQUIRE(found == SENTINEL_ID);
        return;
    }
    REQUIRE(status == StringStatus::Ok);
    REQUIRE(found == NameId{token, StringIndex{match->second}});
}
template <typename Lookup>
void CheckResolve(const Lookup& lookup, const ReferenceTable& reference, NameId id, StringStatus expected)
{
    std::string_view resolved = SENTINEL_VIEW;
    REQUIRE(lookup.TryResolve(id, resolved) == expected);
    if (expected != StringStatus::Ok)
    {
        REQUIRE(resolved.data() == SENTINEL_VIEW.data());
        REQUIRE(resolved.size() == SENTINEL_VIEW.size());
        return;
    }
    // Exact counted bytes, terminated, at the address first returned for this index.
    const std::string& spelling = reference.Spellings[id.Index.Value];
    REQUIRE(resolved.size() == spelling.size());
    REQUIRE(resolved == spelling);
    REQUIRE(resolved.data() == reference.Views[id.Index.Value]);
    REQUIRE(resolved.data()[resolved.size()] == '\0');
}
// The token-free index form checks only the entry range of the lookup's own table.
template <typename Lookup>
void CheckResolveIndex(const Lookup& lookup, const ReferenceTable& reference, StringIndex index)
{
    std::string_view resolved = SENTINEL_VIEW;
    const bool isInRange = index.Value < reference.Spellings.GetSize();
    REQUIRE(lookup.TryResolveIndex(index, resolved) ==
            (isInRange == true ? StringStatus::Ok : StringStatus::InvalidHandle));
    if (isInRange == false)
    {
        REQUIRE(resolved.data() == SENTINEL_VIEW.data());
        REQUIRE(resolved.size() == SENTINEL_VIEW.size());
        return;
    }
    REQUIRE(resolved == reference.Spellings[index.Value]);
    REQUIRE(resolved.data() == reference.Views[index.Value]);
}
void CheckIntern(StringTable& inoutTable,
                 const FaultHeap& heap,
                 const DifferentialScenario& scenario,
                 ReferenceTable& inoutReference,
                 uint32 token,
                 const SequenceSpelling& spelling,
                 DifferentialCoverage& inoutCoverage)
{
    const StringStatus expected = ComputeExpectedValidation(scenario.Config, spelling);
    const usize attempts = heap.Attempts;
    const usize count = inoutReference.Spellings.GetSize();
    NameId id = SENTINEL_ID;
    const StringStatus status = inoutTable.TryIntern(spelling.Bytes, id);
    if (expected != StringStatus::Ok)
    {
        REQUIRE(status == expected);
        REQUIRE(id == SENTINEL_ID);
        REQUIRE(heap.Attempts == attempts);
        ++inoutCoverage.Rejected;
        return;
    }
    const ReferenceIndices::const_iterator match = inoutReference.Indices.find(spelling.Bytes);
    if (match != inoutReference.Indices.end())
    {
        // Duplicates succeed without allocating, even at a limit or with a fault armed.
        REQUIRE(status == StringStatus::Ok);
        REQUIRE(id == NameId{token, StringIndex{match->second}});
        REQUIRE(heap.Attempts == attempts);
        ++inoutCoverage.Duplicates;
        return;
    }
    if (count == scenario.Config.MaxEntries)
    {
        REQUIRE(status == StringStatus::CapacityExceeded);
        REQUIRE(id == SENTINEL_ID);
        REQUIRE(heap.Attempts == attempts);
        ++inoutCoverage.CapacityExceeded;
        return;
    }
    if (status == StringStatus::Ok)
    {
        // Success never follows an injected allocation failure.
        REQUIRE((heap.FailAt == 0 || heap.Attempts < heap.FailAt));
        REQUIRE(id == NameId{token, StringIndex{static_cast<uint32>(count)}});
        std::string_view view;
        REQUIRE(inoutTable.TryResolve(id, view) == StringStatus::Ok);
        REQUIRE(inoutReference.Spellings.TryAdd(spelling.Bytes) == true);
        inoutReference.Indices.emplace(spelling.Bytes, static_cast<uint32>(count));
        REQUIRE(inoutReference.Views.TryAdd(view.data()) == true);
        ++inoutCoverage.Inserted;
        return;
    }
    // A failed insertion preserves the output, consumes no index and stays absent.
    REQUIRE(id == SENTINEL_ID);
    if (status == StringStatus::OutOfMemory)
    {
        REQUIRE(heap.FailAt != 0);
        REQUIRE(heap.Attempts == heap.FailAt);
        ++inoutCoverage.OutOfMemory;
    }
    else
    {
        REQUIRE(status == StringStatus::CapacityExceeded);
        REQUIRE(scenario.CanExhaustByteBudget == true);
        ++inoutCoverage.CapacityExceeded;
    }
    NameId found = SENTINEL_ID;
    REQUIRE(inoutTable.TryFind(spelling.Bytes, found) == StringStatus::NotFound);
    REQUIRE(found == SENTINEL_ID);
}
DifferentialCoverage RunDifferentialSequence(const DifferentialScenario& scenario)
{
    CAPTURE(scenario.Name, scenario.Seed);
    const StringTableConfig& config = scenario.Config;
    uint64 random = scenario.Seed;
    const Array<SequenceSpelling> spellings = MakeSequenceSpellings(config.MaxSpellingBytes, random);
    DifferentialCoverage coverage;
    FaultHeap heap;
    FaultHeap otherHeap;
    {
        StringTable table;
        REQUIRE(CreateTable(config, heap.Domain, table) == StringStatus::Ok);
        if (scenario.ShouldForceCollisions == true)
        {
            StringTableTestAccess::ForceCollisions(table);
        }
        StringTable other;
        REQUIRE(CreateTable(config, otherHeap.Domain, other) == StringStatus::Ok);
        NameId empty;
        REQUIRE(table.TryFind({}, empty) == StringStatus::Ok);
        NameId otherEmpty;
        REQUIRE(other.TryFind({}, otherEmpty) == StringStatus::Ok);
        REQUIRE(empty == NameId{empty.TableToken, StringIndex{0}});
        REQUIRE(otherEmpty.TableToken != empty.TableToken);
        const SequenceTokens tokens{empty.TableToken, otherEmpty.TableToken};

        ReferenceTable reference;
        std::string_view emptyView;
        REQUIRE(table.TryResolve(empty, emptyView) == StringStatus::Ok);
        REQUIRE(reference.Views.TryAdd(emptyView.data()) == true);
        usize slotCapacity = StringTableTestAccess::SlotCapacity(table);
        for (usize step = 0; step < scenario.Steps; ++step)
        {
            CAPTURE(step);
            const SequenceSpelling& spelling = spellings[PickBelow(random, spellings.GetSize())];
            const usize operation = PickBelow(random, 100);
            if (heap.FailAt == 0 && PickBelow(random, 100) < FAULT_ARM_PERCENT)
            {
                heap.FailAt = heap.Attempts + 1 + PickBelow(random, INTERN_ALLOCATION_STAGES);
            }
            const usize attempts = heap.Attempts;
            if (operation < INTERN_PERCENT)
            {
                CheckIntern(table, heap, scenario, reference, tokens.Table, spelling, coverage);
                if (heap.FailAt != 0 && heap.Attempts >= heap.FailAt)
                {
                    heap.FailAt = 0;
                }
            }
            else if (operation < INTERN_PERCENT + FIND_PERCENT)
            {
                CheckFind(table, config, reference, tokens.Table, spelling);
                REQUIRE(heap.Attempts == attempts);
            }
            else
            {
                StringStatus expected = StringStatus::Ok;
                const NameId id = MakeResolveCase(random, reference, tokens, expected);
                CheckResolve(table, reference, id, expected);
                CheckResolveIndex(table, reference, id.Index);
                coverage.WrongTable += expected == StringStatus::WrongTable ? 1 : 0;
                REQUIRE(heap.Attempts == attempts);
            }
            REQUIRE(StringTableTestAccess::Count(table) == reference.Spellings.GetSize());
            REQUIRE(StringTableTestAccess::Allocated(table) == heap.LiveBytes);
            REQUIRE(heap.PeakBytes <= config.MaxAllocatedBytes);
            // Snapshot counters agree with the heap, the model and a full recount.
            const StringTableSnapshot snapshot = CheckSnapshotAccounting(table, heap);
            REQUIRE(snapshot.DuplicateAdmissions == coverage.Duplicates);
            REQUIRE(snapshot.PeakAllocatedBytes == heap.PeakBytes);
            usize spellingBytes = 0;
            for (const std::string& stored : reference.Spellings)
            {
                spellingBytes += stored.size();
            }
            REQUIRE(snapshot.SpellingBytes == spellingBytes);
            const usize capacity = StringTableTestAccess::SlotCapacity(table);
            if (capacity != slotCapacity)
            {
                REQUIRE(capacity == slotCapacity * 2);
                ++coverage.SlotGrowths;
                slotCapacity = capacity;
            }
        }

        heap.FailAt = 0;

        // Freezing into an occupied owner fails and leaves the table usable.
        FrozenStringTable occupied;
        REQUIRE(other.TryFreeze(occupied) == StringStatus::Ok);
        REQUIRE(table.TryFreeze(occupied) == StringStatus::InvalidArgument);
        REQUIRE(table.IsValid() == true);
        CheckFind(table, config, reference, tokens.Table, spellings[1]);
        FrozenStringTable frozen;
        REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
        REQUIRE(table.IsValid() == false);
        NameId closed = SENTINEL_ID;
        REQUIRE(table.TryIntern("name0", closed) == StringStatus::InvalidHandle);
        REQUIRE(closed == SENTINEL_ID);

        // Frozen reads agree with the model for every spelling and index, and
        // keep each view where the mutable table first returned it.
        const usize attempts = heap.Attempts;
        const usize liveBytes = heap.LiveBytes;
        for (const SequenceSpelling& spelling : spellings)
        {
            CheckFind(frozen, config, reference, tokens.Table, spelling);
        }
        const uint32 count = static_cast<uint32>(reference.Spellings.GetSize());
        for (uint32 index = 0; index <= count; ++index)
        {
            if (index < count)
            {
                CheckResolve(frozen, reference, NameId{tokens.Table, StringIndex{index}}, StringStatus::Ok);
            }
            CheckResolveIndex(frozen, reference, StringIndex{index});
        }
        CheckResolveIndex(frozen, reference, StringIndex{});
        CheckResolve(frozen, reference, NameId{tokens.Table, StringIndex{count}}, StringStatus::InvalidHandle);
        CheckResolve(frozen, reference, NameId{tokens.Other, StringIndex{0}}, StringStatus::WrongTable);
        CheckResolve(occupied, reference, NameId{tokens.Table, StringIndex{0}}, StringStatus::WrongTable);
        REQUIRE(heap.Attempts == attempts);
        REQUIRE(heap.LiveBytes == liveBytes);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.Allocations == heap.Frees);
    REQUIRE(heap.BadFree == false);
    REQUIRE(otherHeap.LiveBytes == 0);
    REQUIRE(otherHeap.BadFree == false);
    return coverage;
}
} // namespace

TEST_CASE("table operation sequences match a reference model", "[strings][intern][allocation]")
{
    constexpr uint32 ENTRY_LIMIT = 48;
    constexpr usize BYTE_LIMIT = usize{96} * 1024;
    constexpr usize LARGE_SPELLING_BYTES = 40000;
    StringTableConfig keyed;
    keyed.HashPolicy = StringHashPolicy::Untrusted;
    keyed.Key = SipHashKey{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
    keyed.HasSecretKey = true;
    StringTableConfig opaque;
    opaque.MaxSpellingBytes = LARGE_SPELLING_BYTES;
    opaque.RequireUtf8 = false;
    opaque.RejectEmbeddedZero = false;
    StringTableConfig entryLimited;
    entryLimited.MaxEntries = ENTRY_LIMIT;
    StringTableConfig byteLimited;
    byteLimited.MaxAllocatedBytes = BYTE_LIMIT;
    const DifferentialScenario scenarios[]{
        {"trusted hash", 0x1001U, 3000, {}, false, false, 6},
        {"forced collisions", 0x2002U, 3000, {}, true, false, 6},
        {"keyed hash", 0x3003U, 2000, keyed, false, false, 6},
        {"opaque bytes and large pages", 0x4004U, 2000, opaque, false, false, 6},
        {"entry limit", 0x5005U, 1000, entryLimited, true, false, 2},
        {"byte budget", 0x6006U, 1500, byteLimited, false, true, 4},
    };
    for (const DifferentialScenario& scenario : scenarios)
    {
        const DifferentialCoverage coverage = RunDifferentialSequence(scenario);
        CAPTURE(scenario.Name,
                coverage.Inserted,
                coverage.Duplicates,
                coverage.Rejected,
                coverage.OutOfMemory,
                coverage.CapacityExceeded,
                coverage.WrongTable,
                coverage.SlotGrowths);
        // Each sequence reached the behaviour it exists to compare.
        REQUIRE(coverage.Duplicates > 0);
        REQUIRE(coverage.Rejected > 0);
        REQUIRE(coverage.OutOfMemory > 0);
        REQUIRE(coverage.WrongTable > 0);
        REQUIRE(coverage.SlotGrowths >= scenario.MinimumSlotGrowths);
        const bool isLimited = scenario.Config.MaxEntries == ENTRY_LIMIT || scenario.CanExhaustByteBudget == true;
        REQUIRE((coverage.CapacityExceeded > 0) == isLimited);
    }
}

TEST_CASE("unique strings shrink and erase checked byte ranges without changing failed owners", "[strings][allocation]")
{
    FaultHeap heap;
    {
        String value(heap.Domain);
        REQUIRE(value.TryAssign("short") == StringStatus::Ok);
        REQUIRE(value.TryShrinkToFit() == StringStatus::Ok);
        REQUIRE(value.GetCapacity() == String::kInlineCapacity);
        REQUIRE(heap.Attempts == 0);

        const std::string longText = std::string(40, 'a') + std::string("\0z", 2);
        REQUIRE(value.TryEnsureCapacity(100) == StringStatus::Ok);
        REQUIRE(value.TryAssign(longText) == StringStatus::Ok);
        REQUIRE(heap.LiveBytes == 101);
        const char* pointer = value.GetData();
        heap.FailAt = heap.Attempts + 1;
        REQUIRE(value.TryShrinkToFit() == StringStatus::OutOfMemory);
        REQUIRE(value.GetView() == longText);
        REQUIRE(value.GetData() == pointer);
        REQUIRE(value.GetCapacity() == 100);
        heap.FailAt = 0;
        REQUIRE(value.TryShrinkToFit() == StringStatus::Ok);
        REQUIRE(value.GetCapacity() == longText.size());
        REQUIRE(value.GetView() == longText);
        REQUIRE(value.GetData()[value.GetSize()] == '\0');
        REQUIRE(heap.LiveBytes == longText.size() + 1);
        const usize attempts = heap.Attempts;
        REQUIRE(value.TryShrinkToFit() == StringStatus::Ok);
        REQUIRE(heap.Attempts == attempts);

        REQUIRE(value.TryErase(value.GetSize() + 1, 0) == StringStatus::OutOfRange);
        REQUIRE(value.TryErase(2, value.GetSize() - 1) == StringStatus::OutOfRange);
        REQUIRE(value.TryErase(0, static_cast<usize>(-1)) == StringStatus::OutOfRange);
        REQUIRE(value.GetView() == longText);
        REQUIRE(value.TryErase(value.GetSize(), 0) == StringStatus::Ok);
        REQUIRE(value.TryErase(1, 0) == StringStatus::Ok);
        REQUIRE(value.GetView() == longText);
        // Erase keeps the embedded zero and moves the terminator to the new end.
        REQUIRE(value.TryErase(2, 36) == StringStatus::Ok);
        REQUIRE(value.GetView() == std::string_view("aaaa\0z", 6));
        REQUIRE(value.GetData()[value.GetSize()] == '\0');
        REQUIRE(value.GetCapacity() == longText.size());
        // A value that fits inline returns there without a new allocation.
        REQUIRE(value.TryShrinkToFit() == StringStatus::Ok);
        REQUIRE(heap.Attempts == attempts);
        REQUIRE(value.GetCapacity() == String::kInlineCapacity);
        REQUIRE(value.GetView() == std::string_view("aaaa\0z", 6));
        REQUIRE(heap.LiveBytes == 0);
        REQUIRE(value.TryErase(0, value.GetSize()) == StringStatus::Ok);
        REQUIRE(value.IsEmpty());
        REQUIRE(value.GetData()[0] == '\0');
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE(heap.Allocations == heap.Frees);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("bounded strings erase ranges and truncate only at code point boundaries", "[strings][utf8]")
{
    StaticString<8> value;
    REQUIRE(value.TryAssign("abcdefgh") == StringStatus::Ok);
    REQUIRE(value.TryErase(7, 2) == StringStatus::OutOfRange);
    REQUIRE(value.TryErase(9, 0) == StringStatus::OutOfRange);
    REQUIRE(value.GetView() == "abcdefgh");
    REQUIRE(value.TryErase(1, 3) == StringStatus::Ok);
    REQUIRE(value.GetView() == "aefgh");
    REQUIRE(value.GetData()[5] == '\0');

    Utf8View text;
    usize errorOffset = 0;
    // "ab", U+00E9 (2 bytes), U+20AC (3 bytes), U+1F600 (4 bytes).
    const std::string_view mixed = "ab\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    REQUIRE(ValidateUtf8(mixed, text, errorOffset) == StringStatus::Ok);
    bool isTruncated = false;
    StaticString<6> shortName;
    shortName.AssignTruncatedUtf8(text, isTruncated);
    REQUIRE(isTruncated);
    REQUIRE(shortName.GetView() == "ab\xC3\xA9");
    StaticString<7> exactName;
    exactName.AssignTruncatedUtf8(text, isTruncated);
    REQUIRE(isTruncated);
    REQUIRE(exactName.GetView() == "ab\xC3\xA9\xE2\x82\xAC");
    StaticString<11> wholeName;
    wholeName.AssignTruncatedUtf8(text, isTruncated);
    REQUIRE_FALSE(isTruncated);
    REQUIRE(wholeName.GetView() == mixed);
    StaticString<0> none;
    none.AssignTruncatedUtf8(text, isTruncated);
    REQUIRE(isTruncated);
    REQUIRE(none.IsEmpty());
    // The input may borrow the destination's own bytes.
    StaticString<11> self;
    REQUIRE(self.TryAssign(mixed) == StringStatus::Ok);
    REQUIRE(ValidateUtf8(self.GetView().substr(2), text, errorOffset) == StringStatus::Ok);
    self.AssignTruncatedUtf8(text, isTruncated);
    REQUIRE_FALSE(isTruncated);
    REQUIRE(self.GetView() == mixed.substr(2));
    // Every capacity keeps a well-formed prefix.
    for (usize limit = 0; limit <= mixed.size(); ++limit)
    {
        REQUIRE(ValidateUtf8(mixed, text, errorOffset) == StringStatus::Ok);
        const usize boundary = FindPrecedingUtf8Boundary(text, limit);
        REQUIRE(boundary <= limit);
        Utf8View prefix;
        REQUIRE(ValidateUtf8(mixed.substr(0, boundary), prefix, errorOffset) == StringStatus::Ok);
        REQUIRE((limit - boundary) < 4);
    }
}

TEST_CASE("checked UTF-8 slices start and end on code point boundaries", "[strings][utf8]")
{
    const std::string_view mixed = "ab\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    Utf8View text;
    usize errorOffset = 0;
    REQUIRE(ValidateUtf8(mixed, text, errorOffset) == StringStatus::Ok);
    const usize boundaries[]{0, 1, 2, 4, 7, 11};
    const auto isBoundary = [&](usize offset) {
        for (const usize boundary : boundaries)
        {
            if (boundary == offset)
            {
                return true;
            }
        }
        return false;
    };
    Utf8View sentinel;
    REQUIRE(ValidateUtf8("sentinel", sentinel, errorOffset) == StringStatus::Ok);
    for (usize offset = 0; offset <= mixed.size() + 1; ++offset)
    {
        REQUIRE(FindPrecedingUtf8Boundary(text, offset) <= offset);
        REQUIRE(isBoundary(FindPrecedingUtf8Boundary(text, offset)));
        for (usize size = 0; size <= mixed.size() + 1; ++size)
        {
            Utf8View slice = sentinel;
            const StringStatus status = TrySliceUtf8(text, offset, size, slice);
            CAPTURE(offset, size);
            if (offset > mixed.size() || size > mixed.size() - offset)
            {
                REQUIRE(status == StringStatus::OutOfRange);
                REQUIRE(slice.GetView() == "sentinel");
            }
            else if (isBoundary(offset) == false || isBoundary(offset + size) == false)
            {
                REQUIRE(status == StringStatus::SplitsCodePoint);
                REQUIRE(slice.GetView() == "sentinel");
            }
            else
            {
                REQUIRE(status == StringStatus::Ok);
                REQUIRE(slice.GetView().data() == mixed.data() + offset);
                REQUIRE(slice.GetView() == mixed.substr(offset, size));
                Utf8View revalidated;
                REQUIRE(ValidateUtf8(slice.GetView(), revalidated, errorOffset) == StringStatus::Ok);
            }
        }
    }
    REQUIRE(TrySliceUtf8(text, 1, static_cast<usize>(-1), sentinel) == StringStatus::OutOfRange);
}

TEST_CASE("display repair replaces maximal ill-formed subparts and respects byte limits", "[strings][utf8][allocation]")
{
    constexpr std::string_view REPLACEMENT = "\xEF\xBF\xBD";
    struct RepairCase final
    {
        std::string_view Input;
        std::string Expected;
        usize Replacements;
    };
    // The first row is the Unicode Standard's example of substituting maximal subparts.
    const std::string table38 = std::string("a") + std::string(REPLACEMENT) + std::string(REPLACEMENT) +
                                std::string(REPLACEMENT) + "b" + std::string(REPLACEMENT) + "c" +
                                std::string(REPLACEMENT) + std::string(REPLACEMENT) + "d";
    const std::string one(REPLACEMENT);
    const std::string two = one + one;
    const std::string three = two + one;
    const std::string four = three + one;
    const RepairCase cases[]{
        {"\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64", table38, 6},
        {"", "", 0},
        {"plain", "plain", 0},
        {std::string_view("a\0b", 3), std::string("a\0b", 3), 0},
        {"\xC0\x80", two, 2},
        {"\xE0\x80\x80", three, 3},
        {"\xED\xA0\x80", three, 3},
        {"\xF4\x90\x80\x80", four, 4},
        {"\xF5", one, 1},
        {"\xE1\x80", one, 1},
        {"\xF0\x9F\x98", one, 1},
        {"\xE1x\x80", one + "x" + one, 2},
        {"\xC3\xA9\xFF\xC3\xA9", "\xC3\xA9" + one + "\xC3\xA9", 1},
    };
    for (const RepairCase& repairCase : cases)
    {
        String repaired;
        usize replacements = 99;
        REQUIRE(TryRepairUtf8(repairCase.Input, 64, repaired, replacements) == StringStatus::Ok);
        CAPTURE(repairCase.Input);
        REQUIRE(repaired.GetView() == repairCase.Expected);
        REQUIRE(replacements == repairCase.Replacements);
        Utf8View text;
        usize errorOffset = 0;
        REQUIRE(ValidateUtf8(repaired.GetView(), text, errorOffset) == StringStatus::Ok);
    }
    // Well-formed input is copied unchanged and malformed input always becomes well-formed.
    for (uint32 pair = 0; pair <= 0xFFFF; ++pair)
    {
        const char bytes[]{static_cast<char>(pair >> 8), static_cast<char>(pair & 0xFF)};
        const std::string_view input(bytes, 2);
        String repaired;
        usize replacements = 0;
        Utf8View text;
        usize errorOffset = 0;
        const bool isValid = ValidateUtf8(input, text, errorOffset) == StringStatus::Ok;
        if (TryRepairUtf8(input, 8, repaired, replacements) != StringStatus::Ok || (replacements == 0) != isValid ||
            (isValid == true && repaired.GetView() != input) ||
            ValidateUtf8(repaired.GetView(), text, errorOffset) != StringStatus::Ok)
        {
            FAIL("repair of two bytes " << pair);
        }
    }

    FaultHeap heap;
    {
        String output(heap.Domain);
        REQUIRE(output.TryAssign("kept") == StringStatus::Ok);
        usize replacements = 99;
        const std::string_view malformed = "name\xFF";
        // The limit counts repaired bytes: 4 + 3.
        REQUIRE(TryRepairUtf8(malformed, 6, output, replacements) == StringStatus::CapacityExceeded);
        REQUIRE(output.GetView() == "kept");
        REQUIRE(replacements == 99);
        REQUIRE(TryRepairUtf8(malformed, 7, output, replacements) == StringStatus::Ok);
        REQUIRE(output.GetView() == "name\xEF\xBF\xBD");
        REQUIRE(replacements == 1);
        const std::string longMalformed = std::string(30, 'x') + "\x80";
        heap.FailAt = heap.Attempts + 1;
        REQUIRE(TryRepairUtf8(longMalformed, 64, output, replacements) == StringStatus::OutOfMemory);
        REQUIRE(output.GetView() == "name\xEF\xBF\xBD");
        REQUIRE(replacements == 1);
        heap.FailAt = 0;
        REQUIRE(TryRepairUtf8(longMalformed, 64, output, replacements) == StringStatus::Ok);
        REQUIRE(&output.GetDomain() == &heap.Domain);
        REQUIRE(output.GetView() == std::string(30, 'x') + one);
        // The input may borrow the output.
        REQUIRE(output.TryAppend("\xC3") == StringStatus::Ok);
        REQUIRE(TryRepairUtf8(output.GetView(), 64, output, replacements) == StringStatus::Ok);
        std::string twice(30, 'x');
        twice += two;
        REQUIRE(output.GetView() == twice);
        REQUIRE(replacements == 1);
        REQUIRE(heap.Allocations == 2);
    }
    REQUIRE(heap.LiveBytes == 0);
    REQUIRE_FALSE(heap.BadFree);
}

TEST_CASE("strict UTF-16 conversion reports malformed input and leaves outputs unchanged", "[strings][utf8]")
{
    const std::string_view mixed = "ab\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    const std::u16string_view wide = u"abé€\U0001F600";
    char16_t units[8]{};
    usize unitCount = 0;
    usize errorOffset = 99;
    REQUIRE(MeasureUtf8AsUtf16(mixed, unitCount, errorOffset) == StringStatus::Ok);
    REQUIRE(unitCount == 6);
    REQUIRE(errorOffset == 0);
    REQUIRE(ConvertUtf8ToUtf16(mixed, units, unitCount, errorOffset) == StringStatus::Ok);
    REQUIRE(std::u16string_view(units, unitCount) == wide);
    char bytes[16]{};
    usize byteCount = 0;
    REQUIRE(MeasureUtf16AsUtf8(wide, byteCount, errorOffset) == StringStatus::Ok);
    REQUIRE(byteCount == mixed.size());
    REQUIRE(ConvertUtf16ToUtf8(wide, bytes, byteCount, errorOffset) == StringStatus::Ok);
    REQUIRE(std::string_view(bytes, byteCount) == mixed);

    // Capacity failures write nothing and keep the previous counts.
    char16_t smallUnits[5]{u'x', u'x', u'x', u'x', u'x'};
    unitCount = 77;
    REQUIRE(ConvertUtf8ToUtf16(mixed, smallUnits, unitCount, errorOffset) == StringStatus::CapacityExceeded);
    REQUIRE(unitCount == 77);
    REQUIRE(std::u16string_view(smallUnits, 5) == u"xxxxx");
    char smallBytes[10]{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    byteCount = 77;
    REQUIRE(ConvertUtf16ToUtf8(wide, smallBytes, byteCount, errorOffset) == StringStatus::CapacityExceeded);
    REQUIRE(byteCount == 77);
    REQUIRE(std::string_view(smallBytes, 10) == "xxxxxxxxxx");

    // Malformed UTF-8 reports the first ill-formed byte and writes nothing.
    REQUIRE(ConvertUtf8ToUtf16("ok\xC3\xA9\xED\xA0\x80", units, unitCount, errorOffset) == StringStatus::InvalidUtf8);
    REQUIRE(errorOffset == 4);
    REQUIRE(unitCount == 77);
    REQUIRE(std::u16string_view(units, 6) == wide);
    REQUIRE(MeasureUtf8AsUtf16("\xE1\x80", unitCount, errorOffset) == StringStatus::InvalidUtf8);
    REQUIRE(errorOffset == 0);
    REQUIRE(unitCount == 77);
    // Unpaired surrogates report their unit offset.
    const char16_t loneHigh[]{u'a', 0xD83D};
    const char16_t loneLow[]{u'a', u'b', 0xDE00, u'c'};
    const char16_t highThenText[]{0xD83D, u'x'};
    const char16_t reversed[]{0xDE00, 0xD83D};
    struct SurrogateCase final
    {
        std::u16string_view Units;
        usize Offset;
    };
    const SurrogateCase surrogates[]{
        {{loneHigh, 2}, 1},
        {{loneLow, 4}, 2},
        {{highThenText, 2}, 0},
        {{reversed, 2}, 0},
    };
    for (const SurrogateCase& surrogate : surrogates)
    {
        errorOffset = 99;
        REQUIRE(ConvertUtf16ToUtf8(surrogate.Units, bytes, byteCount, errorOffset) == StringStatus::InvalidUtf16);
        REQUIRE(errorOffset == surrogate.Offset);
        REQUIRE(byteCount == 77);
        REQUIRE(std::string_view(bytes, mixed.size()) == mixed);
    }
    // Embedded U+0000 is a scalar like any other.
    REQUIRE(ConvertUtf8ToUtf16(std::string_view("a\0b", 3), units, unitCount, errorOffset) == StringStatus::Ok);
    REQUIRE(std::u16string_view(units, unitCount) == std::u16string_view(u"a\0b", 3));

    // Every scalar round-trips; the expected UTF-16 is computed independently of the converter.
    for (uint32 scalar = 0; scalar <= 0x10FFFF; ++scalar)
    {
        if (scalar >= 0xD800 && scalar <= 0xDFFF)
        {
            continue;
        }
        char16_t expected[2]{};
        usize expectedUnits = 1;
        if (scalar < 0x10000)
        {
            expected[0] = static_cast<char16_t>(scalar);
        }
        else
        {
            expectedUnits = 2;
            expected[0] = static_cast<char16_t>(0xD800 + ((scalar - 0x10000) >> 10));
            expected[1] = static_cast<char16_t>(0xDC00 + ((scalar - 0x10000) & 0x3FF));
        }
        char encoded[4]{};
        usize encodedSize = 0;
        char16_t converted[2]{};
        usize convertedUnits = 0;
        if (ConvertUtf16ToUtf8({expected, expectedUnits}, encoded, encodedSize, errorOffset) != StringStatus::Ok ||
            ConvertUtf8ToUtf16({encoded, encodedSize}, converted, convertedUnits, errorOffset) != StringStatus::Ok ||
            std::u16string_view(converted, convertedUnits) != std::u16string_view(expected, expectedUnits))
        {
            FAIL("UTF-16 round trip of scalar " << scalar);
        }
        Utf8View text;
        if (ValidateUtf8({encoded, encodedSize}, text, errorOffset) != StringStatus::Ok)
        {
            FAIL("UTF-16 conversion produced ill-formed UTF-8 for scalar " << scalar);
        }
    }
}

TEST_CASE("char8_t adapters borrow units as bytes and copy bytes into units", "[strings][utf8]")
{
    const std::string_view bytes = GetByteView(u8"café");
    REQUIRE(bytes == "caf\xC3\xA9");
    REQUIRE(GetByteView(std::u8string_view(u8"a\0b", 3)) == std::string_view("a\0b", 3));
    char8_t units[5]{u8'x', u8'x', u8'x', u8'x', u8'x'};
    usize unitCount = 9;
    REQUIRE(TryCopyToUtf8Units("too long", units, unitCount) == StringStatus::CapacityExceeded);
    REQUIRE(unitCount == 9);
    REQUIRE(std::u8string_view(units, 5) == u8"xxxxx");
    REQUIRE(TryCopyToUtf8Units(bytes, units, unitCount) == StringStatus::Ok);
    REQUIRE(unitCount == 5);
    REQUIRE(std::u8string_view(units, unitCount) == u8"café");
}

#if defined(LUDUS_PLATFORM_WINDOWS)
TEST_CASE("Windows wide-character conversion matches the UTF-16 conversion", "[strings][utf8]")
{
    const std::string_view mixed = "ab\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    wchar_t units[8]{};
    usize unitCount = 0;
    usize errorOffset = 99;
    REQUIRE(ConvertUtf8ToUtf16(mixed, units, unitCount, errorOffset) == StringStatus::Ok);
    REQUIRE(std::wstring_view(units, unitCount) == L"abé€\U0001F600");
    char bytes[16]{};
    usize byteCount = 0;
    REQUIRE(MeasureUtf16AsUtf8(std::wstring_view(units, unitCount), byteCount, errorOffset) == StringStatus::Ok);
    REQUIRE(byteCount == mixed.size());
    REQUIRE(ConvertUtf16ToUtf8(std::wstring_view(units, unitCount), bytes, byteCount, errorOffset) == StringStatus::Ok);
    REQUIRE(std::string_view(bytes, byteCount) == mixed);
    const wchar_t loneHigh[]{L'a', static_cast<wchar_t>(0xD83D)};
    byteCount = 77;
    REQUIRE(ConvertUtf16ToUtf8(std::wstring_view(loneHigh, 2), bytes, byteCount, errorOffset) ==
            StringStatus::InvalidUtf16);
    REQUIRE(errorOffset == 1);
    REQUIRE(byteCount == 77);
}
#endif
