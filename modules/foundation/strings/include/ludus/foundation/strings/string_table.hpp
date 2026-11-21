#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/status.hpp>

#include <string_view>

namespace ludus::foundation
{
/// @brief Index is meaningful only inside its enclosing table/snapshot.
///
/// Never persist a runtime NameId or compare indices across dictionaries.
struct StringIndex final
{
    /// @brief Reserved invalid index value (`0xffffffff`); never names an entry.
    ///
    /// Tables and dictionaries therefore hold fewer than 2^32 entries. Owners that store indices may
    /// also use it to mark unused slots.
    static constexpr uint32 INVALID_VALUE = 0xffffffffU;
    /// Zero-based entry index; the default is INVALID_VALUE. In a StringTable, index zero is the
    /// empty spelling. Range checks belong to the owning table.
    uint32 Value{INVALID_VALUE};
    /// Compares raw index values; meaningful only for indices of the same table or dictionary.
    friend constexpr bool operator==(StringIndex, StringIndex) noexcept = default;
};
/// @brief Process-local, table-qualified identity of one interned spelling.
///
/// A NameId holds no pointer and does not keep its table alive. The default value is invalid.
/// Only the issuing table (or the FrozenStringTable it became) resolves it; never persist it.
struct NameId final
{
    /// Token of the issuing table; zero means no table. Tokens are never reused in a process.
    uint32 TableToken{};
    /// Entry index inside the issuing table.
    StringIndex Index;
    /// @brief Checks the representation only: a nonzero token and a non-sentinel index.
    ///
    /// True does not prove that the issuing table is still alive or that the index is in range;
    /// TryResolve performs those checks.
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return TableToken != 0 && Index.Value != StringIndex::INVALID_VALUE;
    }
    /// @brief Compares token and index.
    ///
    /// IDs from one table are equal exactly when their spellings are equal, because a table stores
    /// each spelling once. IDs from different tables never compare equal, even for the same bytes.
    friend constexpr bool operator==(NameId, NameId) noexcept = default;
};
/// @brief Compile-time descriptor of a string literal; not an interned ID.
///
/// Bind `Spelling` explicitly through StringTable::TryIntern during setup and cache the NameId.
struct NameLiteral final
{
    /// Borrowed counted view of the literal, excluding its final (terminating) array element.
    std::string_view Spelling;
    /// FNV-1a-64 of `Spelling` (SymbolFingerprint64). It is not collision-free, is not the table's
    /// lookup hash and cannot stand in for a NameId.
    uint64 Fingerprint;
    /// Source must remain alive until binding. consteval does not prove lifetime.
    template <usize N>
    consteval explicit NameLiteral(const char (&spelling)[N]) noexcept
        : Spelling(spelling, N - 1), Fingerprint(SymbolFingerprint64(Spelling))
    {
    }
};
/// @brief Selects the lookup hash a StringTable applies to counted spelling bytes.
///
/// Either policy compares full bytes on lookup; the policy affects only collision resistance.
enum class StringHashPolicy : uint8
{
    /// Unkeyed XXH3 (TableHash64) for engine-controlled input. `StringTableConfig::Key` is ignored.
    Trusted,
    /// SipHash-2-4 keyed by `StringTableConfig::Key` (KeyedTableHash64) for hostile input. Requires
    /// `StringTableConfig::HasSecretKey`; it resists hash flooding but does not bound admitted volume.
    Untrusted
};
/// @brief Budgets, hash policy and spelling rules copied into a table by CreateTable.
///
/// The defaults are trusted-input settings. Settings cannot change after creation.
struct StringTableConfig final
{
    /// Largest accepted spelling in bytes, excluding the terminator the table stores after each
    /// spelling. Longer TryIntern/TryFind input fails with TooLarge. Must be below half the usize range.
    usize MaxSpellingBytes{4096};
    uint32 MaxEntries{65536};                        ///< includes empty entry at index zero
    usize MaxAllocatedBytes{usize{8} * 1024 * 1024}; ///< includes rehash/growth peak
    /// Lookup hash policy; a value outside the enumerators makes CreateTable fail.
    StringHashPolicy HashPolicy{StringHashPolicy::Trusted};
    /// SipHash key used only by StringHashPolicy::Untrusted; copied into the table's storage.
    SipHashKey Key{};
    bool HasSecretKey{}; ///< caller acquired a secret from platform entropy
    /// Reject spellings that are not well-formed UTF-8 with InvalidUtf8. False accepts arbitrary bytes.
    bool RequireUtf8{true};
    /// Reject spellings containing a zero byte with EmbeddedZero. Checked before UTF-8 validation.
    bool RejectEmbeddedZero{true};
};
/// @brief Number of power-of-two probe-distance buckets in StringTableSnapshot::ProbeDistanceCounts.
///
/// Bucket 0 counts entries in their home lookup slot, bucket `k` in `1..COUNT - 2` counts probe
/// distances `2^(k-1)` through `2^k - 1`, and the last bucket counts every distance of at least
/// `2^(COUNT - 2)` (64) slots.
inline constexpr usize STRING_TABLE_PROBE_BUCKET_COUNT = 8;
/// @brief Footprint, retention and probe counters of one table, captured in constant time.
///
/// All byte counts come from the table's own allocation accounting, so `AllocatedBytes` equals
/// `StateBytes + EntryBytes + LookupBytes + PageBytes` and matches the bytes the creation domain
/// currently holds for the table. Logical deduplication (DuplicateAdmissions) is reported separately
/// from committed memory: a table can avoid copies while spending more on metadata and page slack.
/// A snapshot copies counters only; it contains no spelling bytes and borrows nothing from the table.
struct StringTableSnapshot final
{
    /// Entries, including the empty spelling at index zero; valid indices are `0..EntryCount - 1`.
    uint32 EntryCount{};
    /// Configured entry limit (StringTableConfig::MaxEntries), including the empty entry.
    uint32 MaxEntries{};
    /// Entries the directory can hold before it must grow.
    usize EntryCapacity{};
    /// Lookup slots; insertion keeps at most three quarters of them occupied.
    usize LookupSlotCount{};
    /// Spelling payload bytes of every entry, excluding the terminator stored after each spelling.
    usize SpellingBytes{};
    /// Terminator bytes, one per entry.
    usize TerminatorBytes{};
    /// Spelling pages allocated; pages are never released before the table is destroyed.
    usize PageCount{};
    /// Bytes allocated for spelling pages, including their headers.
    usize PageBytes{};
    /// Page bytes holding neither spellings, terminators nor headers: unused tails of every page.
    usize PageSlackBytes{};
    /// Bytes allocated for the entry directory.
    usize EntryBytes{};
    /// Bytes allocated for the lookup array.
    usize LookupBytes{};
    /// Bytes of the fixed table state allocated by CreateTable.
    usize StateBytes{};
    /// Bytes currently allocated from the creation domain; never above MaxAllocatedBytes.
    usize AllocatedBytes{};
    /// Highest AllocatedBytes since creation, including the moments when an old and a replacement
    /// directory or lookup array coexisted during growth; never above MaxAllocatedBytes.
    usize PeakAllocatedBytes{};
    /// Configured byte budget (StringTableConfig::MaxAllocatedBytes).
    usize MaxAllocatedBytes{};
    /// Successful TryIntern calls that returned an existing entry instead of inserting one.
    uint64 DuplicateAdmissions{};
    /// Largest distance, in slots, between any entry's home lookup slot and the slot holding it.
    usize MaxProbeDistance{};
    /// Entries per probe-distance bucket (see STRING_TABLE_PROBE_BUCKET_COUNT); the counts sum to
    /// EntryCount. A successful lookup of an entry at distance `d` compares `d + 1` slots.
    StaticArray<uint32, STRING_TABLE_PROBE_BUCKET_COUNT> ProbeDistanceCounts{};
    /// Compares every counter, for example a frozen owner's snapshot with the one taken before freezing.
    friend constexpr bool operator==(const StringTableSnapshot&, const StringTableSnapshot&) noexcept = default;
};
class FrozenStringTable;
/// @brief Mutable, append-only owner of deduplicated spellings and their table-qualified IDs.
///
/// Create one with CreateTable; a default-constructed table is empty and its operations return
/// InvalidHandle. Spelling bytes, entries and lookup metadata are allocated from the creation
/// domain within `StringTableConfig::MaxAllocatedBytes`; that domain must outlive this table and
/// any FrozenStringTable it becomes. Entries are never removed or reused.
///
/// TryIntern, TryFind and TryResolve may be called concurrently from any thread; one internal
/// mutex serializes them. Move, move assignment, destruction and TryFreeze are not synchronized:
/// the caller must first quiesce every other operation on this object. A resolved view stays valid
/// across later insertion, growth, moves and freezing, until the storage owner is destroyed or
/// assigned over.
class StringTable final
{
public:
    /// Constructs an empty table that owns no storage.
    StringTable() noexcept = default;
    /// Releases all storage to the creation domain. Requires quiescence; outstanding views dangle.
    ~StringTable() noexcept;
    /// Takes ownership of the storage and token of `other`, which becomes empty. Requires quiescence.
    StringTable(StringTable&& other) noexcept;
    /// @brief Releases the current storage, then takes over `other`, which becomes empty.
    ///
    /// Views into the previously owned storage dangle. Self-move does nothing. Requires quiescence of
    /// both tables.
    StringTable& operator=(StringTable&& other) noexcept;
    /// Not copyable: a table uniquely owns its storage and token.
    StringTable(const StringTable&) = delete;
    /// Not copyable: a table uniquely owns its storage and token.
    StringTable& operator=(const StringTable&) = delete;
    /// Returns true when this object owns a table created by CreateTable and not moved or frozen away.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return mState != nullptr;
    }
    /// @brief Returns the ID of an exact spelling, inserting a copy when it is absent.
    ///
    /// A duplicate succeeds without allocating, even at the entry or byte budget. A new
    /// spelling consumes the next index only on success. Serialized by the table mutex.
    /// @param value Counted bytes, checked against the configured length, UTF-8 and zero-byte rules.
    /// @param outId Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty table; TooLarge, InvalidUtf8 or EmbeddedZero for a
    /// rejected spelling; CapacityExceeded, TooLarge or OutOfMemory when growth fails; or
    /// ProbeExhausted when corrupted lookup metadata has neither the spelling nor a free slot.
    /// Failure leaves the logical table unchanged; earlier successful reservations may remain.
    [[nodiscard]] StringStatus TryIntern(std::string_view value, NameId& outId) noexcept;
    /// @brief Looks up an exact spelling without inserting or allocating.
    ///
    /// Serialized by the table mutex.
    /// @param value Counted bytes, checked against the configured spelling rules.
    /// @param outId Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; NotFound when the spelling is absent; InvalidHandle for an empty table;
    /// TooLarge, InvalidUtf8 or EmbeddedZero for a rejected spelling; or ProbeExhausted when
    /// corrupted lookup metadata has neither the spelling nor a free slot.
    [[nodiscard]] StringStatus TryFind(std::string_view value, NameId& outId) const noexcept;
    /// @brief Borrows the stored spelling of an ID issued by this table, without allocating.
    ///
    /// Serialized by the table mutex. The view covers the stored bytes, which never change; see the
    /// class description for its lifetime.
    /// @param id ID to resolve.
    /// @param outSpelling Receives the spelling on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty table, a representation-invalid ID or an index beyond
    /// this table's entries; or WrongTable when the ID carries another table's token.
    [[nodiscard]] StringStatus TryResolve(NameId id, std::string_view& outSpelling) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary table that is about to be destroyed.
    StringStatus TryResolve(NameId, std::string_view&) const&& = delete;
    /// @brief Borrows the stored spelling at a table-local index, without allocating.
    ///
    /// Use it for compact records that store a StringIndex beside their known owning table. The index
    /// carries no table token, so this checks only that it names an existing entry of this table: an
    /// index issued by another table that happens to be in range resolves to this table's spelling.
    /// Serialized by the table mutex; the view has the lifetime described for the class.
    /// @param index Entry index, typically `NameId::Index` of an ID issued by this table.
    /// @param outSpelling Receives the spelling on success; unchanged on failure.
    /// @return Ok; or InvalidHandle for an empty table, StringIndex::INVALID_VALUE or an index at or
    /// beyond this table's entry count.
    [[nodiscard]] StringStatus TryResolveIndex(StringIndex index, std::string_view& outSpelling) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary table that is about to be destroyed.
    StringStatus TryResolveIndex(StringIndex, std::string_view&) const&& = delete;
    /// @brief Preallocates room for a known number of new spellings so that interning them allocates nothing.
    ///
    /// After success, TryIntern calls that add at most `entryCount` new entries whose spellings total
    /// at most `spellingBytes` bytes allocate nothing; duplicates consume no reservation. The entry
    /// directory and lookup array grow to their exact final capacity, and missing page room is
    /// reserved as one page of exactly `spellingBytes + entryCount` bytes (each spelling also stores a
    /// terminator); unused space in the current page is kept as slack because a spelling never spans
    /// pages. A reservation already satisfied allocates nothing. The budget check covers the growth
    /// peak, when an old and a replacement array coexist, before anything is allocated. Serialized
    /// by the table mutex.
    /// @param entryCount Distinct spellings that will be added beyond the current entries.
    /// @param spellingBytes Total payload bytes of those spellings, excluding terminators.
    /// @return Ok; InvalidHandle for an empty table; InvalidArgument when `spellingBytes` is nonzero
    /// but `entryCount` is zero; CapacityExceeded when the entries would exceed
    /// `StringTableConfig::MaxEntries` or the peak would exceed `MaxAllocatedBytes` (nothing is
    /// allocated); TooLarge when a size computation overflows; or OutOfMemory when the domain fails.
    /// After OutOfMemory, completed growth steps remain within the budget; entries, IDs and views
    /// never change.
    [[nodiscard]] StringStatus TryReserve(uint32 entryCount, usize spellingBytes) noexcept;
    /// @brief Copies footprint, retention and probe counters without allocating.
    ///
    /// Takes the table mutex for a constant-time copy, independent of the entry count: insertion
    /// maintains the counters in constant time and rehashing, which already visits every entry,
    /// rebuilds the probe histogram. The snapshot does not change the table.
    /// @param outSnapshot Receives the counters on success; unchanged on failure.
    /// @return Ok; or InvalidHandle for an empty table.
    [[nodiscard]] StringStatus TryCaptureSnapshot(StringTableSnapshot& outSnapshot) const noexcept;
    /// @brief Caller quiesces ALL concurrent operations.
    ///
    /// Consumes this table and keeps every token/index stable; `outTable` must be empty. No allocation required.
    /// @param outTable Receives the table's storage on success; unchanged on failure.
    /// @return Ok, leaving this table empty; InvalidHandle when this table is empty; or
    /// InvalidArgument when `outTable` already owns a table. Failure leaves both objects unchanged.
    [[nodiscard]] StringStatus TryFreeze(FrozenStringTable& outTable) noexcept;

private:
    struct State;
    State* mState{};
    friend class FrozenStringTable;
    friend StringStatus CreateTable(const StringTableConfig&, const AllocationDomain&, StringTable&) noexcept;
    friend struct StringTableTestAccess;
};
/// @brief Immutable owner produced by StringTable::TryFreeze; it cannot be thawed.
///
/// It keeps the frozen table's token, entries and storage, so earlier IDs and views stay valid.
/// TryFind and TryResolve take no lock, never allocate, and may run concurrently from any thread.
/// Move, move assignment and destruction require the caller to quiesce those readers first. The
/// creation domain must outlive this owner. A default-constructed owner is empty.
class FrozenStringTable final
{
public:
    /// Constructs an empty owner; its lookups return InvalidHandle.
    FrozenStringTable() noexcept = default;
    /// Releases all storage to the creation domain. Requires quiescence; outstanding views dangle.
    ~FrozenStringTable() noexcept;
    /// Takes ownership of the storage of `other`, which becomes empty. Requires quiescence.
    FrozenStringTable(FrozenStringTable&& other) noexcept;
    /// @brief Releases the current storage, then takes over `other`, which becomes empty.
    ///
    /// Views into the previously owned storage dangle. Self-move does nothing. Requires quiescence of
    /// both owners.
    FrozenStringTable& operator=(FrozenStringTable&& other) noexcept;
    /// Not copyable: the owner uniquely owns its storage and token.
    FrozenStringTable(const FrozenStringTable&) = delete;
    /// Not copyable: the owner uniquely owns its storage and token.
    FrozenStringTable& operator=(const FrozenStringTable&) = delete;
    /// Returns true when this object owns frozen storage that has not been moved away.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return mState != nullptr;
    }
    /// @brief Looks up an exact spelling in the immutable table without locking or allocating.
    ///
    /// Concurrent calls are safe while the owner lives and is not moved or destroyed.
    /// @param value Counted bytes, checked against the configured spelling rules.
    /// @param outId Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; NotFound when the spelling is absent; InvalidHandle for an empty owner;
    /// TooLarge, InvalidUtf8 or EmbeddedZero for a rejected spelling; or ProbeExhausted when
    /// corrupted lookup metadata has neither the spelling nor a free slot.
    [[nodiscard]] StringStatus TryFind(std::string_view value, NameId& outId) const noexcept;
    /// @brief Borrows the stored spelling of an ID issued before freezing, without locking or allocating.
    ///
    /// Concurrent calls are safe while the owner lives and is not moved or destroyed. The view stays
    /// valid until this owner is destroyed or assigned over.
    /// @param id ID to resolve.
    /// @param outSpelling Receives the spelling on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty owner, a representation-invalid ID or an index beyond
    /// the frozen entries; or WrongTable when the ID carries another table's token.
    [[nodiscard]] StringStatus TryResolve(NameId id, std::string_view& outSpelling) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary owner that is about to be destroyed.
    StringStatus TryResolve(NameId, std::string_view&) const&& = delete;
    /// @brief Borrows the stored spelling at a table-local index without locking or allocating.
    ///
    /// The index carries no table token: this checks only that it names one of the frozen entries,
    /// so an in-range index issued by another table resolves to this owner's spelling. Concurrent
    /// calls are safe while the owner lives and is not moved or destroyed; the view stays valid
    /// until this owner is destroyed or assigned over.
    /// @param index Entry index, typically `NameId::Index` of an ID issued before freezing.
    /// @param outSpelling Receives the spelling on success; unchanged on failure.
    /// @return Ok; or InvalidHandle for an empty owner, StringIndex::INVALID_VALUE or an index at or
    /// beyond the frozen entry count.
    [[nodiscard]] StringStatus TryResolveIndex(StringIndex index, std::string_view& outSpelling) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary owner that is about to be destroyed.
    StringStatus TryResolveIndex(StringIndex, std::string_view&) const&& = delete;
    /// @brief Copies the frozen table's counters in constant time without locking or allocating.
    ///
    /// The counters are those of the table at freezing and never change afterwards. Concurrent calls
    /// are safe while the owner lives and is not moved or destroyed.
    /// @param outSnapshot Receives the counters on success; unchanged on failure.
    /// @return Ok; or InvalidHandle for an empty owner.
    [[nodiscard]] StringStatus TryCaptureSnapshot(StringTableSnapshot& outSnapshot) const noexcept;

private:
    StringTable::State* mState{};
    friend class StringTable;
};
/// @brief Empty `outTable` required; failure preserves it.
///
/// The runtime token issuer must be linked once into the host; plugins use the host's table API.
/// It is a process-wide atomic counter, so concurrent calls are safe; each success takes a fresh
/// nonzero token that is never reissued. The new table contains only the empty spelling at index
/// zero. `config` is copied; `domain` is borrowed and must outlive the table and its frozen owner.
/// @param config Budgets, hash policy and spelling rules.
/// @param domain Allocation source for the table state, entries, lookup metadata and spelling bytes.
/// @param outTable Receives the new table on success; must be empty.
/// @return Ok; InvalidArgument when `outTable` is not empty, `MaxEntries` is zero or StringIndex::INVALID_VALUE,
/// `MaxAllocatedBytes` cannot hold the table state, `MaxSpellingBytes` reaches half the usize range,
/// `HashPolicy` is not an enumerator, or Untrusted lacks `HasSecretKey`; OutOfMemory when the
/// domain fails; CapacityExceeded when the byte budget cannot hold the empty entry; or
/// TokenExhausted after the process has issued every token. Failure releases everything allocated.
[[nodiscard]] StringStatus
CreateTable(const StringTableConfig& config, const AllocationDomain& domain, StringTable& outTable) noexcept;
} // namespace ludus::foundation
