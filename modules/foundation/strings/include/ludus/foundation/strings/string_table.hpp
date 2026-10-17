#pragma once

#include <ludus/foundation/base/types.h>

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
    /// Zero-based entry index; the default `0xffffffff` is the reserved invalid value. In a
    /// StringTable, index zero is the empty spelling. Range checks belong to the owning table.
    uint32 Value{0xffffffffU};
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
        return TableToken != 0 && Index.Value != 0xffffffffU;
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
    /// @param output Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty table; TooLarge, InvalidUtf8 or EmbeddedZero for a
    /// rejected spelling; CapacityExceeded, TooLarge or OutOfMemory when growth fails; or
    /// ProbeExhausted when corrupted lookup metadata has neither the spelling nor a free slot.
    /// Failure leaves the logical table unchanged; earlier successful reservations may remain.
    [[nodiscard]] StringStatus TryIntern(std::string_view value, NameId& output) noexcept;
    /// @brief Looks up an exact spelling without inserting or allocating.
    ///
    /// Serialized by the table mutex.
    /// @param value Counted bytes, checked against the configured spelling rules.
    /// @param output Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; NotFound when the spelling is absent; InvalidHandle for an empty table;
    /// TooLarge, InvalidUtf8 or EmbeddedZero for a rejected spelling; or ProbeExhausted when
    /// corrupted lookup metadata has neither the spelling nor a free slot.
    [[nodiscard]] StringStatus TryFind(std::string_view value, NameId& output) const noexcept;
    /// @brief Borrows the stored spelling of an ID issued by this table, without allocating.
    ///
    /// Serialized by the table mutex. The view covers the stored bytes, which never change; see the
    /// class description for its lifetime.
    /// @param id ID to resolve.
    /// @param output Receives the spelling on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty table, a representation-invalid ID or an index beyond
    /// this table's entries; or WrongTable when the ID carries another table's token.
    [[nodiscard]] StringStatus TryResolve(NameId id, std::string_view& output) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary table that is about to be destroyed.
    StringStatus TryResolve(NameId, std::string_view&) const&& = delete;
    /// @brief Caller quiesces ALL concurrent operations.
    ///
    /// Consumes this table and keeps every token/index stable; output must be empty. No allocation required.
    /// @param output Receives the table's storage on success; unchanged on failure.
    /// @return Ok, leaving this table empty; InvalidHandle when this table is empty; or
    /// InvalidArgument when `output` already owns a table. Failure leaves both objects unchanged.
    [[nodiscard]] StringStatus TryFreeze(FrozenStringTable& output) noexcept;

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
    /// @param output Receives the table-qualified ID on success; unchanged on failure.
    /// @return Ok; NotFound when the spelling is absent; InvalidHandle for an empty owner;
    /// TooLarge, InvalidUtf8 or EmbeddedZero for a rejected spelling; or ProbeExhausted when
    /// corrupted lookup metadata has neither the spelling nor a free slot.
    [[nodiscard]] StringStatus TryFind(std::string_view value, NameId& output) const noexcept;
    /// @brief Borrows the stored spelling of an ID issued before freezing, without locking or allocating.
    ///
    /// Concurrent calls are safe while the owner lives and is not moved or destroyed. The view stays
    /// valid until this owner is destroyed or assigned over.
    /// @param id ID to resolve.
    /// @param output Receives the spelling on success; unchanged on failure.
    /// @return Ok; InvalidHandle for an empty owner, a representation-invalid ID or an index beyond
    /// the frozen entries; or WrongTable when the ID carries another table's token.
    [[nodiscard]] StringStatus TryResolve(NameId id, std::string_view& output) const& noexcept;
    /// Deleted so a view cannot borrow from a temporary owner that is about to be destroyed.
    StringStatus TryResolve(NameId, std::string_view&) const&& = delete;

private:
    StringTable::State* mState{};
    friend class StringTable;
};
/// @brief Empty output required; failure preserves output.
///
/// The runtime token issuer must be linked once into the host; plugins use the host's table API.
/// It is a process-wide atomic counter, so concurrent calls are safe; each success takes a fresh
/// nonzero token that is never reissued. The new table contains only the empty spelling at index
/// zero. `config` is copied; `domain` is borrowed and must outlive the table and its frozen owner.
/// @param config Budgets, hash policy and spelling rules.
/// @param domain Allocation source for the table state, entries, lookup metadata and spelling bytes.
/// @param output Receives the new table on success; must be empty.
/// @return Ok; InvalidArgument when `output` is not empty, `MaxEntries` is zero or `0xffffffff`,
/// `MaxAllocatedBytes` cannot hold the table state, `MaxSpellingBytes` reaches half the usize range,
/// `HashPolicy` is not an enumerator, or Untrusted lacks `HasSecretKey`; OutOfMemory when the
/// domain fails; CapacityExceeded when the byte budget cannot hold the empty entry; or
/// TokenExhausted after the process has issued every token. Failure releases everything allocated.
[[nodiscard]] StringStatus
CreateTable(const StringTableConfig& config, const AllocationDomain& domain, StringTable& output) noexcept;
} // namespace ludus::foundation
