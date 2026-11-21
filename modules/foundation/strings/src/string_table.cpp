#include <ludus/foundation/strings/string_table.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include "internal/string_table_state.hpp"
#include "internal/table_token.hpp"

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
constexpr usize INITIAL_SLOT_CAPACITY = 16;
constexpr usize INITIAL_ENTRY_CAPACITY = 8;
// Lookup arrays keep this fraction of slots empty (at most 75% occupancy).
constexpr usize FREE_SLOT_FRACTION_DIVISOR = 4;
[[nodiscard]] constexpr usize GetOccupancyLimit(usize slotCapacity) noexcept
{
    return slotCapacity - slotCapacity / FREE_SLOT_FRACTION_DIVISOR;
}
std::atomic<uint32> gNextTableToken{1};
} // namespace

std::atomic<uint32>& detail::GetTableTokenIssuer() noexcept
{
    return gNextTableToken;
}

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
        if (Allocated > PeakAllocated)
        {
            PeakAllocated = Allocated;
        }
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
StringStatus StringTable::State::ResolveIndex(StringIndex index, std::string_view& outSpelling) const noexcept
{
    if (index.Value == StringIndex::INVALID_VALUE || index.Value >= Count)
    {
        return StringStatus::InvalidHandle;
    }
    outSpelling = Entries[index.Value].Spelling;
    return StringStatus::Ok;
}
void StringTable::State::RecordProbeDistance(usize distance) noexcept
{
    usize bucket = 0;
    for (usize remaining = distance; remaining != 0 && bucket + 1 < STRING_TABLE_PROBE_BUCKET_COUNT; remaining >>= 1U)
    {
        ++bucket;
    }
    ++ProbeDistanceCounts[bucket];
    if (distance > MaxProbeDistance)
    {
        MaxProbeDistance = distance;
    }
}
StringStatus StringTable::State::GrowEntries(usize capacity) noexcept
{
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
StringStatus StringTable::State::EnsureEntries() noexcept
{
    if (Count < EntryCapacity)
    {
        return StringStatus::Ok;
    }
    usize capacity = EntryCapacity == 0
                         ? INITIAL_ENTRY_CAPACITY
                         : (EntryCapacity <= Config.MaxEntries / 2 ? EntryCapacity * 2 : Config.MaxEntries);
    if (capacity > Config.MaxEntries)
    {
        capacity = Config.MaxEntries;
    }
    return GrowEntries(capacity);
}
StringStatus StringTable::State::GrowSlots(usize capacity) noexcept
{
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
    // Every caller keeps Count at most three quarters of the new capacity, so
    // every rehashed entry reaches an empty slot of this freshly cleared array.
    // The rehash already visits each entry, so it rebuilds the probe histogram.
    ProbeDistanceCounts = {};
    MaxProbeDistance = 0;
    const usize mask = capacity - 1;
    for (uint32 i = 0; i < Count; ++i)
    {
        const usize home = static_cast<usize>(Entries[i].Hash) & mask;
        usize slot = home;
        while (slots[slot] != StringIndex::INVALID_VALUE)
        {
            slot = (slot + 1) & mask;
        }
        slots[slot] = i;
        RecordProbeDistance((slot - home) & mask);
    }
    Free(Slots, SlotCapacity * sizeof(uint32), alignof(uint32));
    Slots = slots;
    SlotCapacity = capacity;
    return StringStatus::Ok;
}
StringStatus StringTable::State::EnsureSlots() noexcept
{
    if (SlotCapacity != 0 && Count + static_cast<usize>(1) <= GetOccupancyLimit(SlotCapacity))
    {
        return StringStatus::Ok;
    }
    return GrowSlots(SlotCapacity == 0 ? INITIAL_SLOT_CAPACITY : SlotCapacity * 2);
}
StringStatus StringTable::State::AddPage(usize capacity) noexcept
{
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
    ++PageCount;
    PageBytes += allocation;
    PageCapacityBytes += capacity;
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
    return AddPage(capacity);
}
StringStatus StringTable::State::Reserve(uint32 entryCount, usize spellingBytes) noexcept
{
    if (entryCount == 0)
    {
        return spellingBytes == 0 ? StringStatus::Ok : StringStatus::InvalidArgument;
    }
    if (entryCount > Config.MaxEntries - Count)
    {
        return StringStatus::CapacityExceeded;
    }
    const usize entryTotal = static_cast<usize>(Count) + entryCount;
    usize pageBytes = 0;
    if (TryAdd(spellingBytes, static_cast<usize>(entryCount), pageBytes) == false)
    {
        return StringStatus::TooLarge;
    }
    // Plan exact final capacities: insertion keeps at most three quarters of
    // the lookup slots occupied, and a spelling never spans pages.
    const usize entryCapacity = entryTotal > EntryCapacity ? entryTotal : EntryCapacity;
    usize slotCapacity = SlotCapacity == 0 ? INITIAL_SLOT_CAPACITY : SlotCapacity;
    while (entryTotal > GetOccupancyLimit(slotCapacity))
    {
        if (TryMultiply(slotCapacity, usize{2}, slotCapacity) == false)
        {
            return StringStatus::TooLarge;
        }
    }
    const bool shouldAddPage = Pages == nullptr || pageBytes > Pages->Capacity - Pages->Used;
    usize newEntryBytes = 0;
    usize newSlotBytes = 0;
    usize newPageBytes = 0;
    if (TryMultiply(entryCapacity, sizeof(Entry), newEntryBytes) == false ||
        TryMultiply(slotCapacity, sizeof(uint32), newSlotBytes) == false ||
        TryAdd(shouldAddPage == true ? pageBytes : usize{0},
               shouldAddPage == true ? sizeof(Page) : usize{0},
               newPageBytes) == false)
    {
        return StringStatus::TooLarge;
    }
    // Check the whole sequence's peak before allocating: each replaced array
    // coexists with its successor until the copy completes.
    const bool shouldGrowEntries = entryCapacity != EntryCapacity;
    const bool shouldGrowSlots = slotCapacity != SlotCapacity;
    const usize oldEntryBytes = EntryCapacity * sizeof(Entry);
    const usize oldSlotBytes = SlotCapacity * sizeof(uint32);
    const usize available = Config.MaxAllocatedBytes - Allocated;
    usize retained = 0;
    if (shouldGrowEntries == true)
    {
        if (newEntryBytes > available)
        {
            return StringStatus::CapacityExceeded;
        }
        retained = newEntryBytes - oldEntryBytes;
    }
    if (shouldGrowSlots == true)
    {
        if (retained > available || newSlotBytes > available - retained)
        {
            return StringStatus::CapacityExceeded;
        }
        retained += newSlotBytes - oldSlotBytes;
    }
    if (retained > available || newPageBytes > available - retained)
    {
        return StringStatus::CapacityExceeded;
    }
    StringStatus status = StringStatus::Ok;
    if (shouldGrowEntries == true)
    {
        status = GrowEntries(entryCapacity);
    }
    if (status == StringStatus::Ok && shouldGrowSlots == true)
    {
        status = GrowSlots(slotCapacity);
    }
    if (status == StringStatus::Ok && shouldAddPage == true)
    {
        status = AddPage(pageBytes);
    }
    return status;
}
void StringTable::State::CaptureSnapshot(StringTableSnapshot& outSnapshot) const noexcept
{
    outSnapshot = StringTableSnapshot
    {
        .EntryCount = Count,
        .MaxEntries = Config.MaxEntries,
        .EntryCapacity = EntryCapacity,
        .LookupSlotCount = SlotCapacity,
        .SpellingBytes = PageUsedBytes - Count,
        .TerminatorBytes = Count,
        .PageCount = PageCount,
        .PageBytes = PageBytes,
        .PageSlackBytes = PageCapacityBytes - PageUsedBytes,
        .EntryBytes = EntryCapacity * sizeof(Entry),
        .LookupBytes = SlotCapacity * sizeof(uint32),
        .StateBytes = sizeof(State),
        .AllocatedBytes = Allocated,
        .PeakAllocatedBytes = PeakAllocated,
        .MaxAllocatedBytes = Config.MaxAllocatedBytes,
        .DuplicateAdmissions = DuplicateAdmissions,
        .MaxProbeDistance = MaxProbeDistance,
        .ProbeDistanceCounts = ProbeDistanceCounts,
    };
}
StringStatus StringTable::State::Intern(std::string_view value, NameId& outId) noexcept
{
    const StringStatus valid = Validate(value);
    if (valid != StringStatus::Ok)
    {
        return valid;
    }
    const uint64 hash = Hash(value);
    const usize previousSlotCapacity = SlotCapacity;
    usize slot = 0;
    if (SlotCapacity != 0)
    {
        const StringStatus probed = FindSlot(value, hash, slot);
        if (probed != StringStatus::Ok)
        {
            return probed;
        }
        if (Slots[slot] != StringIndex::INVALID_VALUE)
        {
            outId = {Token, StringIndex{Slots[slot]}};
            ++DuplicateAdmissions;
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
    // Entry and page growth preserve slot indices; only rehashing invalidates
    // the empty slot found during the duplicate check.
    if (SlotCapacity != previousSlotCapacity)
    {
        status = FindSlot(value, hash, slot);
        if (status != StringStatus::Ok)
        {
            return status;
        }
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
    PageUsedBytes += value.size() + 1;
    RecordProbeDistance((slot - static_cast<usize>(hash)) & (SlotCapacity - 1));
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
        status = detail::IssueTableToken(gNextTableToken, state->Token);
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
StringStatus StringTable::TryResolveIndex(StringIndex index, std::string_view& outSpelling) const& noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    return mState->ResolveIndex(index, outSpelling);
}
StringStatus StringTable::TryReserve(uint32 entryCount, usize spellingBytes) noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    return mState->Reserve(entryCount, spellingBytes);
}
StringStatus StringTable::TryCaptureSnapshot(StringTableSnapshot& outSnapshot) const noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    const std::lock_guard lock(mState->Mutex);
    mState->CaptureSnapshot(outSnapshot);
    return StringStatus::Ok;
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
StringStatus FrozenStringTable::TryResolveIndex(StringIndex index, std::string_view& outSpelling) const& noexcept
{
    return mState == nullptr ? StringStatus::InvalidHandle : mState->ResolveIndex(index, outSpelling);
}
StringStatus FrozenStringTable::TryCaptureSnapshot(StringTableSnapshot& outSnapshot) const noexcept
{
    if (mState == nullptr)
    {
        return StringStatus::InvalidHandle;
    }
    mState->CaptureSnapshot(outSnapshot);
    return StringStatus::Ok;
}
} // namespace ludus::foundation
