#pragma once

// Content-addressed cache of pure stage products. The store holds bytes; this
// layer frames every entry with its key, schema, length and payload digest and
// validates them on every hit, so a corrupt entry is discarded and rebuilt
// rather than trusted. Contract:
// docs/architecture/procedural-content-generation.md#incremental-cooking-and-cache-keys.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>

namespace ludus::content::procedural
{
/// Version of the cache entry framing written by TryStoreGenerationProduct.
inline constexpr uint32 GENERATION_CACHE_ENTRY_VERSION = 1;

/// @brief Result of reading a store.
enum class GenerationStoreRead : uint8
{
    /// The bytes were read.
    Found,
    /// The store has no entry for the key.
    Absent,
    /// The entry exists but could not be read, for example on allocation failure.
    Failed,
    /// The store's own integrity check rejected the entry, for example a stored length or digest that does not match
    /// the bytes; the store no longer serves it.
    Corrupt,
};

/// @brief Result of a cache lookup; every miss names its reason.
enum class GenerationCacheOutcome : uint8
{
    /// A validated entry was found.
    Hit,
    /// No entry exists for the key.
    Absent,
    /// The store or the entry's framing, key, length or payload digest did not verify; it was removed.
    Corrupt,
    /// The entry verified but holds a different product schema; it was removed.
    SchemaMismatch,
    /// The store failed to read the entry; it was left in place.
    ReadFailed,
};

/// @brief Byte storage behind the generation cache.
///
/// Implementations store opaque bytes by key. They need not validate content; TryFindGenerationProduct does. A
/// store is used by one thread at a time unless the implementation documents otherwise.
class GenerationCacheStore
{
public:
    /// Destroy the store.
    virtual ~GenerationCacheStore() = default;

    /// @brief Read the bytes stored for a key.
    /// @param key The entry key.
    /// @param outBytes Replaced with the entry bytes on Found; unchanged otherwise.
    /// @return Found, Absent, Failed, or Corrupt when the store's own verification rejects the entry.
    [[nodiscard]] virtual GenerationStoreRead TryRead(const GenerationDigest& key,
                                                      foundation::Array<uint8>& outBytes) noexcept = 0;
    /// @brief Store bytes for a key, replacing an existing entry.
    /// @return False when the store refuses or cannot hold the bytes; an existing entry is then unchanged.
    [[nodiscard]] virtual bool TryWrite(const GenerationDigest& key,
                                        foundation::BorrowedView<const uint8> bytes) noexcept = 0;
    /// Remove the entry for a key; absent keys are ignored.
    virtual void Remove(const GenerationDigest& key) noexcept = 0;

protected:
    /// Construct the interface.
    GenerationCacheStore() noexcept = default;
    /// Implementations decide whether they are copyable.
    GenerationCacheStore(const GenerationCacheStore&) noexcept = default;
    /// Implementations decide whether they are copyable.
    GenerationCacheStore& operator=(const GenerationCacheStore&) noexcept = default;
};

/// @brief Byte budgets of a MemoryGenerationCacheStore.
struct GenerationCacheLimits final
{
    /// Most entries held at once.
    usize MaximumEntries = 0;
    /// Most entry bytes held at once.
    usize MaximumBytes = 0;
};

/// @brief An in-process cache store with fixed entry and byte budgets.
///
/// Writes that would exceed a budget are refused; nothing is evicted implicitly. Single-owner; not thread-safe.
class MemoryGenerationCacheStore final : public GenerationCacheStore
{
public:
    /// Create an empty store with the given budgets.
    explicit MemoryGenerationCacheStore(GenerationCacheLimits limits) noexcept;

    /// @copydoc GenerationCacheStore::TryRead
    [[nodiscard]] GenerationStoreRead TryRead(const GenerationDigest& key,
                                              foundation::Array<uint8>& outBytes) noexcept override;
    /// @copydoc GenerationCacheStore::TryWrite
    [[nodiscard]] bool TryWrite(const GenerationDigest& key,
                                foundation::BorrowedView<const uint8> bytes) noexcept override;
    /// @copydoc GenerationCacheStore::Remove
    void Remove(const GenerationDigest& key) noexcept override;

    /// Number of entries held.
    [[nodiscard]] LUDUS_INLINE usize GetEntryCount() const noexcept
    {
        return mEntries.GetSize();
    }
    /// Total bytes held across entries.
    [[nodiscard]] LUDUS_INLINE usize GetByteCount() const noexcept
    {
        return mByteCount;
    }
    /// @brief Writable bytes of an entry, for repair tools and corruption tests; null when absent.
    ///
    /// The view is valid until the next write or removal. Changing it does not update any digest.
    [[nodiscard]] foundation::BorrowedView<uint8> FindEntryBytes(const GenerationDigest& key) noexcept;

private:
    foundation::SortedMap<GenerationDigest, foundation::Array<uint8>, GenerationDigestLess> mEntries;
    GenerationCacheLimits mLimits;
    usize mByteCount = 0;
};

/// @brief Find and validate a cached stage product.
///
/// Verifies the entry framing version, stored key, schema, payload length and payload digest. A corrupt or
/// mismatched entry is removed from the store so the caller rebuilds and stores it again.
/// @param inoutStore The store; an invalid entry is removed from it.
/// @param key The product's cache key.
/// @param schema The product schema the caller expects.
/// @param outPayload Replaced with the payload on Hit; unchanged otherwise.
/// @return Hit or the reason for the miss.
[[nodiscard]] GenerationCacheOutcome TryFindGenerationProduct(GenerationCacheStore& inoutStore,
                                                              const GenerationDigest& key,
                                                              uint32 schema,
                                                              foundation::Array<uint8>& outPayload) noexcept;

/// @brief Frame a product payload and store it under its key.
/// @param inoutStore The store that receives the entry.
/// @param key The product's cache key.
/// @param schema The product schema.
/// @param payload A valid view of the product's canonical bytes.
/// @return False when the payload view is invalid, framing allocation fails or the store refuses the entry. A
///         refused store never changes a generation result; the product is simply rebuilt on the next request.
[[nodiscard]] bool TryStoreGenerationProduct(GenerationCacheStore& inoutStore,
                                             const GenerationDigest& key,
                                             uint32 schema,
                                             foundation::BorrowedView<const uint8> payload) noexcept;
} // namespace ludus::content::procedural
