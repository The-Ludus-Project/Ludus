#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/shared_string.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/strings/string_table.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include "internal/reference_count.hpp"
#include "internal/string_table_state.hpp"
#include "internal/table_token.hpp"

#include <atomic>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
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
void CheckIntern(StringTable& table,
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
    const StringStatus status = table.TryIntern(spelling.Bytes, id);
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
        REQUIRE(table.TryResolve(id, view) == StringStatus::Ok);
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
    REQUIRE(table.TryFind(spelling.Bytes, found) == StringStatus::NotFound);
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
                coverage.WrongTable += expected == StringStatus::WrongTable ? 1 : 0;
                REQUIRE(heap.Attempts == attempts);
            }
            REQUIRE(StringTableTestAccess::Count(table) == reference.Spellings.GetSize());
            REQUIRE(StringTableTestAccess::Allocated(table) == heap.LiveBytes);
            REQUIRE(heap.PeakBytes <= config.MaxAllocatedBytes);
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
        for (uint32 index = 0; index < count; ++index)
        {
            CheckResolve(frozen, reference, NameId{tokens.Table, StringIndex{index}}, StringStatus::Ok);
        }
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
