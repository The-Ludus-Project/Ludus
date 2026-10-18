#include <ludus/foundation/base/types.h>

#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/shared_string.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/strings/string_table.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include "internal/reference_count.hpp"
#include "internal/string_table_state.hpp"

#include <atomic>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;

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
        std::thread threads[4];
        for (auto& thread : threads)
        {
            thread = std::thread([copy]() {
                for (usize i = 0; i < 10000; ++i)
                {
                    SharedString local = copy;
                    local = {};
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
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

TEST_CASE("tables serialize mutations and frozen reads need no allocations", "[strings][threads]")
{
    FaultHeap heap;
    StringTable table;
    REQUIRE(CreateTable({}, heap.Domain, table) == StringStatus::Ok);
    std::atomic<bool> good{true};
    std::thread threads[4];
    for (auto& thread : threads)
    {
        thread = std::thread([&]() {
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
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    REQUIRE(good.load());
    REQUIRE(StringTableTestAccess::Count(table) == 201);
    FrozenStringTable frozen;
    REQUIRE(table.TryFreeze(frozen) == StringStatus::Ok);
    const usize attempts = heap.Attempts;
    for (auto& thread : threads)
    {
        thread = std::thread([&]() {
            for (usize i = 0; i < 200; ++i)
            {
                NameId id;
                if (frozen.TryFind(std::to_string(i), id) != StringStatus::Ok)
                {
                    good = false;
                }
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
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
