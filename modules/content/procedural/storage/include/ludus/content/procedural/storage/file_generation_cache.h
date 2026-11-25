#pragma once

// A persistent generation cache store on Content's file reads and atomic saves.
// Each entry is a content-addressed object file, privately staged and verified by
// reading it back, and a completion record written last that names the object's
// length and digest. A reader trusts neither file names nor existence: it verifies
// the record, then the object's length and digest, and reports any mismatch as
// Corrupt so the entry is rebuilt. Contract:
// docs/architecture/procedural-content-generation.md#incremental-cooking-and-cache-keys.

#include <ludus/foundation/base/core.h>

#include <ludus/content/limits.h>
#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_cache.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <string_view>

namespace ludus::content::procedural
{
/// Version of the completion record layout below.
inline constexpr uint32 FILE_GENERATION_CACHE_VERSION = 1;
/// Largest entry the store admits: Content's SaveFile ceiling.
inline constexpr usize MAX_FILE_GENERATION_CACHE_ENTRY_BYTES = content::MAX_SAVE_BYTES;
/// Bytes of a completion record: magic 4, version 4, key 32, state 1, object length 8, object digest 32, and the
/// digest of the preceding fields 32.
inline constexpr usize FILE_GENERATION_CACHE_RECORD_BYTES = 113;
/// Capacity of a store file name: two hexadecimal digests, a separator and the longest suffix.
inline constexpr usize MAX_FILE_GENERATION_CACHE_NAME_BYTES = 144;

/// A store file name relative to the store root.
using FileGenerationCacheName = foundation::StaticString<MAX_FILE_GENERATION_CACHE_NAME_BYTES>;

/// @brief The completion record file of a key: `<key hex>.entry`.
[[nodiscard]] FileGenerationCacheName GetFileGenerationCacheRecordName(const GenerationDigest& key) noexcept;

/// @brief The object file of a key's entry bytes: `<key hex>-<object digest hex>.object`, lowercase hexadecimal.
[[nodiscard]] FileGenerationCacheName GetFileGenerationCacheObjectName(const GenerationDigest& key,
                                                                       const GenerationDigest& object) noexcept;

/// @brief A GenerationCacheStore persisted as files in one existing directory.
///
/// TryWrite stages the bytes as a content-addressed object, reads the object back to verify its length and digest,
/// then publishes the key's completion record last, so a reader never sees an entry whose bytes were not verified.
/// An interrupted write leaves either the previous entry or no entry. Remove replaces the record with a removal
/// record. Objects that no record names are unreferenced staging: never read, and not deleted in this version,
/// because Content's file adapter has no delete operation.
///
/// Each read and write is synchronous file I/O through Content's ReadFile and SaveFile; call it outside frame-critical
/// paths. A single writer owns a directory at a time; concurrent writers are refused by SaveFile's digest check
/// rather than coordinated. Single-owner; not thread-safe.
class FileGenerationCacheStore final : public GenerationCacheStore
{
public:
    /// @brief Use an existing directory as the store root.
    /// @param root A trusted existing directory of at most content::MAX_ROOT_BYTES bytes.
    /// @param maximumEntryBytes Largest entry TryWrite admits, at most MAX_FILE_GENERATION_CACHE_ENTRY_BYTES.
    /// @return False for an empty or overlong root or an out-of-range budget; the store is then unchanged.
    [[nodiscard]] bool TryOpen(std::string_view root, usize maximumEntryBytes) noexcept;
    /// Whether TryOpen succeeded.
    [[nodiscard]] LUDUS_INLINE bool IsOpen() const noexcept
    {
        return mRoot.IsEmpty() == false;
    }

    /// @brief Read and verify an entry: the completion record, then the object's length and digest.
    /// @return Found; Absent without a record or after Remove; Corrupt, after removing the entry, when the record or
    ///         object does not verify; or Failed when the store is closed or a file cannot be read.
    [[nodiscard]] GenerationStoreRead TryRead(const GenerationDigest& key,
                                              foundation::Array<uint8>& outBytes) noexcept override;
    /// @brief Stage, verify and publish an entry.
    /// @return False when the store is closed, the bytes exceed the budget, or a file cannot be written or verified;
    ///         the previous entry then remains served.
    [[nodiscard]] bool TryWrite(const GenerationDigest& key,
                                foundation::BorrowedView<const uint8> bytes) noexcept override;
    /// Replace the key's completion record with a removal record; an absent key or a closed store is ignored.
    void Remove(const GenerationDigest& key) noexcept override;

private:
    [[nodiscard]] bool tryReplaceFile(std::string_view name, foundation::BorrowedView<const uint8> bytes) noexcept;

    foundation::StaticString<content::MAX_ROOT_BYTES> mRoot;
    usize mMaximumEntryBytes = 0;
};
} // namespace ludus::content::procedural
