#include <ludus/foundation/strings/string_table.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include "internal/string_table_state.hpp"

#include <atomic>
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace ludus::foundation
{
// Thanks to Jason Gregory, Game Engine Architecture, 3rd ed., section 6.4,
// pp.456-469, and James Boer, "A Flexible Text Parsing System", Game Programming
// Gems 2, section 1.17, pp.112-117. Adapt pooled names and indexed spellings;
// retain full bytes in release and use table-qualified indices rather than
// hash-only identity. See docs/architecture/strings-gems-review.md.
namespace
{
constexpr usize kPageCapacity = usize{32} * 1024;
std::atomic<uint32> gNextTableToken{1};
StringStatus IssueToken(uint32& outToken) noexcept
{
    uint32 token = gNextTableToken.load(std::memory_order_relaxed);
    while (token != 0)
    {
        if (gNextTableToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed))
        {
            outToken = token;
            return StringStatus::Ok;
        }
    }
    return StringStatus::TokenExhausted;
}
} // namespace

void* StringTable::State::Allocate(usize bytes, usize alignment) noexcept
{
    if (bytes > Config.MaxAllocatedBytes - Allocated)
    {
        return nullptr;
    }
    void* pointer = Domain->TryAllocate(bytes, alignment);
    if (pointer != nullptr)
    {
        Allocated += bytes;
    }
    return pointer;
}
void StringTable::State::Free(void* pointer, usize bytes, usize alignment) noexcept
{
    if (pointer != nullptr)
    {
        Domain->Free(pointer, bytes, alignment);
        Allocated -= bytes;
    }
}
StringTable::State::~State() noexcept
{
    Free(Entries, EntryCapacity * sizeof(Entry), alignof(Entry));
    Free(Slots, SlotCapacity * sizeof(uint32), alignof(uint32));
    while (Pages != nullptr)
    {
        Page* page = Pages;
        Pages = page->Next;
        Free(page, sizeof(Page) + page->Capacity, alignof(Page));
    }
}
void StringTable::State::Destroy(State* state) noexcept
{
    if (state != nullptr)
    {
        const AllocationDomain* domain = state->Domain;
        state->~State();
        domain->Free(state, sizeof(State), alignof(State));
    }
}
StringStatus StringTable::State::Validate(std::string_view value) const noexcept
{
    if (value.size() > Config.MaxSpellingBytes)
    {
        return StringStatus::TooLarge;
    }
    if (Config.RejectEmbeddedZero && ValidateCString(value) != StringStatus::Ok)
    {
        return StringStatus::EmbeddedZero;
    }
    if (Config.RequireUtf8)
    {
        Utf8View validated;
        usize error = 0;
        return ValidateUtf8(value, validated, error);
    }
    return StringStatus::Ok;
}
uint64 StringTable::State::Hash(std::string_view value) const noexcept
{
    if (HashOverride != nullptr)
    {
        return HashOverride(value);
    }
    const std::span<const uint8> bytes{reinterpret_cast<const uint8*>(value.data()), value.size()};
    return Config.HashPolicy == StringHashPolicy::Trusted ? TableHash64(bytes) : KeyedTableHash64(bytes, Config.Key);
}
StringStatus StringTable::State::FindSlot(std::string_view value, uint64 hash, usize& outSlot) const noexcept
{
    // The load-factor invariant guarantees an empty slot, but visiting each slot
    // at most once keeps corrupted metadata from turning a lookup into a hang.
    const usize mask = SlotCapacity - 1;
    usize slot = static_cast<usize>(hash) & mask;
    for (usize probe = 0; probe < SlotCapacity; ++probe)
    {
        const uint32 index = Slots[slot];
        if (index == StringIndex::INVALID_VALUE)
        {
            outSlot = slot;
            return StringStatus::Ok;
        }
        const Entry& entry = Entries[index];
        if (entry.Hash == hash && entry.Spelling == value)
        {
            outSlot = slot;
            return StringStatus::Ok;
        }
        slot = (slot + 1) & mask;
    }
    return StringStatus::ProbeExhausted;
}
StringStatus StringTable::State::Find(std::string_view value, NameId& outId) const noexcept
{
    const StringStatus valid = Validate(value);
    if (valid != StringStatus::Ok)
    {
        return valid;
    }
    usize slot = 0;
    const StringStatus probed = FindSlot(value, Hash(value), slot);
    if (probed != StringStatus::Ok)
    {
        return probed;
    }
    if (Slots[slot] == StringIndex::INVALID_VALUE)
    {
        return StringStatus::NotFound;
    }
    outId = {Token, StringIndex{Slots[slot]}};
    return StringStatus::Ok;
}
StringStatus StringTable::State::Resolve(NameId id, std::string_view& outSpelling) const noexcept
{
    if (id.IsValid() == false)
    {
        return StringStatus::InvalidHandle;
    }
    if (id.TableToken != Token)
    {
        return StringStatus::WrongTable;
    }
    if (id.Index.Value >= Count)
    {
        return StringStatus::InvalidHandle;
    }
    outSpelling = Entries[id.Index.Value].Spelling;
    return StringStatus::Ok;
}
StringStatus StringTable::State::EnsureEntries() noexcept
{
    if (Count < EntryCapacity)
    {
        return StringStatus::Ok;
    }
    usize capacity =
        EntryCapacity == 0 ? 8 : (EntryCapacity <= Config.MaxEntries / 2 ? EntryCapacity * 2 : Config.MaxEntries);
    if (capacity > Config.MaxEntries)
    {
        capacity = Config.MaxEntries;
    }
    usize bytes = 0;
    if (TryMultiply(capacity, sizeof(Entry), bytes) == false)
    {
        return StringStatus::TooLarge;
    }
    if (bytes > Config.MaxAllocatedBytes - Allocated)
    {
        return StringStatus::CapacityExceeded;
    }
    auto* entries = static_cast<Entry*>(Allocate(bytes, alignof(Entry)));
    if (entries == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    for (usize i = 0; i < capacity; ++i)
    {
        new (entries + i) Entry{};
        if (i < Count)
        {
            entries[i] = Entries[i];
        }
    }
    Free(Entries, EntryCapacity * sizeof(Entry), alignof(Entry));
    Entries = entries;
    EntryCapacity = capacity;
    return StringStatus::Ok;
}
StringStatus StringTable::State::EnsureSlots() noexcept
{
    if (SlotCapacity != 0 && Count + static_cast<usize>(1) <= SlotCapacity - SlotCapacity / 4)
    {
        return StringStatus::Ok;
    }
    usize capacity = SlotCapacity == 0 ? 16 : SlotCapacity * 2;
    usize bytes = 0;
    if (capacity < SlotCapacity || TryMultiply(capacity, sizeof(uint32), bytes) == false)
    {
        return StringStatus::TooLarge;
    }
    if (bytes > Config.MaxAllocatedBytes - Allocated)
    {
        return StringStatus::CapacityExceeded;
    }
    auto* slots = static_cast<uint32*>(Allocate(bytes, alignof(uint32)));
    if (slots == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    for (usize i = 0; i < capacity; ++i)
    {
        new (slots + i) uint32{StringIndex::INVALID_VALUE};
    }
    // Growth keeps Count below 3/8 of the new capacity, so every rehashed
    // entry reaches an empty slot of this freshly cleared array.
    for (uint32 i = 0; i < Count; ++i)
    {
        usize slot = static_cast<usize>(Entries[i].Hash) & (capacity - 1);
        while (slots[slot] != StringIndex::INVALID_VALUE)
        {
            slot = (slot + 1) & (capacity - 1);
        }
        slots[slot] = i;
    }
    Free(Slots, SlotCapacity * sizeof(uint32), alignof(uint32));
    Slots = slots;
    SlotCapacity = capacity;
    return StringStatus::Ok;
}
StringStatus StringTable::State::EnsurePage(usize bytes) noexcept
{
    if (Pages != nullptr && bytes <= Pages->Capacity - Pages->Used)
    {
        return StringStatus::Ok;
    }
    const usize available = Config.MaxAllocatedBytes - Allocated;
    if (available < sizeof(Page) || bytes > available - sizeof(Page))
    {
        return StringStatus::CapacityExceeded;
    }
    usize capacity = bytes > kPageCapacity ? bytes : kPageCapacity;
    if (capacity > available - sizeof(Page))
    {
        capacity = available - sizeof(Page);
    }
    usize allocation = 0;
    if (TryAdd(capacity, sizeof(Page), allocation) == false)
    {
        return StringStatus::TooLarge;
    }
    void* storage = Allocate(allocation, alignof(Page));
    if (storage == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    Pages = new (storage) Page{ .Next = Pages, .Capacity = capacity };
    return StringStatus::Ok;
}
StringStatus StringTable::State::Intern(std::string_view value, NameId& outId) noexcept
{
    const StringStatus valid = Validate(value);
    if (valid != StringStatus::Ok)
    {
        return valid;
    }
    const uint64 hash = Hash(value);
    if (SlotCapacity != 0)
    {
        usize existing = 0;
        const StringStatus probed = FindSlot(value, hash, existing);
        if (probed != StringStatus::Ok)
        {
            return probed;
        }
        if (Slots[existing] != StringIndex::INVALID_VALUE)
        {
            outId = {Token, StringIndex{Slots[existing]}};
            return StringStatus::Ok;
        }
    }
    if (Count == Config.MaxEntries)
    {
        return StringStatus::CapacityExceeded;
    }
    StringStatus status = EnsureEntries();
    if (status != StringStatus::Ok)
    {
        return status;
    }
    status = EnsureSlots();
    if (status != StringStatus::Ok)
    {
        return status;
    }
    status = EnsurePage(value.size() + 1);
    if (status != StringStatus::Ok)
    {
        return status;
    }
    usize slot = 0;
    status = FindSlot(value, hash, slot);
    if (status != StringStatus::Ok)
    {
        return status;
    }
    // All fallible work precedes this commit. Source may borrow an old page;
    // append-only storage keeps it valid across metadata growth and rehashing.
    char* spelling = Pages->Bytes() + Pages->Used;
    if (value.empty() == false)
    {
        std::memcpy(spelling, value.data(), value.size());
    }
    spelling[value.size()] = '\0';
    Entries[Count] = {hash, std::string_view{spelling, value.size()}};
    Slots[slot] = Count;
    outId = {Token, StringIndex{Count}};
    Pages->Used += value.size() + 1;
    ++Count;
    return StringStatus::Ok;
}

StringStatus
CreateTable(const StringTableConfig& config, const AllocationDomain& domain, StringTable& outTable) noexcept
{
    if (outTable.IsValid() || config.MaxEntries == 0 || config.MaxEntries == StringIndex::INVALID_VALUE ||
        config.MaxAllocatedBytes < sizeof(StringTable::State) ||
        config.MaxSpellingBytes >= (static_cast<usize>(-1) >> 1) ||
        (config.HashPolicy != StringHashPolicy::Trusted && config.HashPolicy != StringHashPolicy::Untrusted) ||
        (config.HashPolicy == StringHashPolicy::Untrusted && config.HasSecretKey == false))
    {
        return StringStatus::InvalidArgument;
    }
    void* storage = domain.TryAllocate(sizeof(StringTable::State), alignof(StringTable::State));
    if (storage == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    auto* state = new (storage) StringTable::State{domain, config};
    NameId empty;
    StringStatus status = state->Intern({}, empty);
    if (status == StringStatus::Ok)
    {
        status = IssueToken(state->Token);
    }
    if (status != StringStatus::Ok)
    {
        StringTable::State::Destroy(state);
        return status;
    }
    outTable.mState = state;
    return StringStatus::Ok;
}
StringTable::~StringTable() noexcept
{
    State::Destroy(mState);
}
StringTable::StringTable(StringTable&& other) noexcept : mState(std::exchange(other.mState, nullptr)) {}
StringTable& StringTable::operator=(StringTable&& other) noexcept
{
    if (this != &other)
    {
        State::Destroy(mState);
        mState = std::exchange(other.mState, nullptr);
    }
    return *this;
}
StringStatus StringTable::TryIntern(std::string_view value, NameId& outId) noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    return mState->Intern(value, outId);
}
StringStatus StringTable::TryFind(std::string_view value, NameId& outId) const noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    return mState->Find(value, outId);
}
StringStatus StringTable::TryResolve(NameId id, std::string_view& outSpelling) const& noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    return mState->Resolve(id, outSpelling);
}
StringStatus StringTable::TryFreeze(FrozenStringTable& outTable) noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    if (outTable.IsValid())
    {
        return StringStatus::InvalidArgument;
    }
    outTable.mState = std::exchange(mState, nullptr);
    return StringStatus::Ok;
}
FrozenStringTable::~FrozenStringTable() noexcept
{
    StringTable::State::Destroy(mState);
}
FrozenStringTable::FrozenStringTable(FrozenStringTable&& other) noexcept : mState(std::exchange(other.mState, nullptr))
{
}
FrozenStringTable& FrozenStringTable::operator=(FrozenStringTable&& other) noexcept
{
    if (this != &other)
    {
        StringTable::State::Destroy(mState);
        mState = std::exchange(other.mState, nullptr);
    }
    return *this;
}
StringStatus FrozenStringTable::TryFind(std::string_view value, NameId& outId) const noexcept
{
    return mState == nullptr ? StringStatus::InvalidHandle : mState->Find(value, outId);
}
StringStatus FrozenStringTable::TryResolve(NameId id, std::string_view& outSpelling) const& noexcept
{
    return mState == nullptr ? StringStatus::InvalidHandle : mState->Resolve(id, outSpelling);
}
} // namespace ludus::foundation
