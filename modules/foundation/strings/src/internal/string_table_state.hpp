#pragma once

#include <ludus/foundation/strings/string_table.hpp>

#include <mutex>

namespace ludus::foundation
{
struct StringTable::State final
{
    struct Entry final
    {
        uint64 Hash{};
        std::string_view Spelling;
    };
    struct Page final
    {
        Page* Next{};
        usize Capacity{};
        usize Used{};
        [[nodiscard]] char* Bytes() noexcept
        {
            return reinterpret_cast<char*>(this + 1);
        }
    };
    const AllocationDomain* Domain;
    StringTableConfig Config;
    uint32 Token{};
    uint32 Count{};
    usize Allocated{sizeof(State)};
    Entry* Entries{};
    usize EntryCapacity{};
    uint32* Slots{};
    usize SlotCapacity{};
    Page* Pages{};
    mutable std::mutex Mutex;
    // Private seam for collision tests, inaccessible through the installed API.
    uint64 (*HashOverride)(std::string_view) noexcept {};

    State(const AllocationDomain& domain, const StringTableConfig& config) noexcept : Domain(&domain), Config(config) {}
    ~State() noexcept;
    [[nodiscard]] StringStatus Validate(std::string_view value) const noexcept;
    [[nodiscard]] uint64 Hash(std::string_view value) const noexcept;
    // Writes the matching or first empty slot; ProbeExhausted when neither exists
    // within SlotCapacity probes (corrupted metadata) and outSlot is preserved.
    [[nodiscard]] StringStatus FindSlot(std::string_view value, uint64 hash, usize& outSlot) const noexcept;
    [[nodiscard]] StringStatus Find(std::string_view value, NameId& outId) const noexcept;
    [[nodiscard]] StringStatus Resolve(NameId id, std::string_view& outSpelling) const noexcept;
    [[nodiscard]] StringStatus Intern(std::string_view value, NameId& outId) noexcept;
    [[nodiscard]] StringStatus EnsureEntries() noexcept;
    [[nodiscard]] StringStatus EnsureSlots() noexcept;
    [[nodiscard]] StringStatus EnsurePage(usize bytes) noexcept;
    [[nodiscard]] void* Allocate(usize bytes, usize alignment) noexcept;
    void Free(void* pointer, usize bytes, usize alignment) noexcept;
    static void Destroy(State* state) noexcept;
};
} // namespace ludus::foundation
