#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/level.hpp>

#include <atomic>
#include <mutex>
#include <string_view>

namespace ludus::foundation::logging::internal
{

// Process-lifetime registry of per-category level overrides.
//
// Design contract (design.md section 3, requirements R13/R15):
//   * Reads (TryGetOverride) are on the logging hot path and MUST be lock-free
//     and allocation-free: a linear scan over a fixed, cache-friendly array of
//     atomically-published entries. No mutex, no hash map, no allocation.
//   * Writes (SetOverride/Clear) are cold-path control operations guarded by a
//     mutex. A new entry's payload is written before its slot is published with
//     a release store; readers acquire the published count so they never observe
//     a half-written entry (happens-before via Count).
//   * Full-name storage lets collision validation reject two distinct names
//     hashing to the same id rather than silently merging them (fixes F10).
//
// Capacity is fixed (no growth, no allocation on the hot path). Overflow of the
// override table is reported once via the emergency path by the caller layer;
// unregistered categories simply fall back to the global threshold, which is
// always safe.
class CategoryRegistry
{
public:
    static constexpr usize kCapacity = 256;

    // Hot path: lock-free. Returns true and sets `out` if `id` has an override.
    [[nodiscard]] bool TryGetOverride(uint32 id, LogLevel& out) const noexcept
    {
        const usize count = mCount.load(std::memory_order_acquire);
        for (usize i = 0; i < count; ++i)
        {
            if (mEntries[i].Id.load(std::memory_order_relaxed) == id)
            {
                out = mEntries[i].Level.load(std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    // Cold path: set or update the override for a category id. Returns false if
    // the table is full or a hash collision with a different name is detected.
    bool SetOverride(uint32 id, std::string_view name, LogLevel level) noexcept
    {
        std::lock_guard guard(mMutex);
        const usize count = mCount.load(std::memory_order_relaxed);
        for (usize i = 0; i < count; ++i)
        {
            if (mEntries[i].Id.load(std::memory_order_relaxed) == id)
            {
                // Collision guard: same id, different name => refuse to merge.
                if (NamesEqual(mEntries[i], name) == false)
                {
                    return false;
                }
                mEntries[i].Level.store(level, std::memory_order_relaxed);
                return true;
            }
        }
        if (count >= kCapacity)
        {
            return false;
        }
        // Write payload first, then publish by bumping the count with release.
        mEntries[count].Id.store(id, std::memory_order_relaxed);
        mEntries[count].Level.store(level, std::memory_order_relaxed);
        StoreName(mEntries[count], name);
        mCount.store(count + 1, std::memory_order_release);
        return true;
    }

    void Clear() noexcept
    {
        std::lock_guard guard(mMutex);
        mCount.store(0, std::memory_order_release);
    }

private:
    static constexpr usize kNameCap = 47;

    struct Entry
    {
        std::atomic<uint32> Id{0};
        std::atomic<LogLevel> Level{LogLevel::Info};
        char Name[kNameCap + 1] = {};
        usize NameLen = 0;
    };

    static void StoreName(Entry& entry, std::string_view name) noexcept
    {
        const usize n = name.size() < kNameCap ? name.size() : kNameCap;
        for (usize i = 0; i < n; ++i)
        {
            entry.Name[i] = name[i];
        }
        entry.Name[n] = '\0';
        entry.NameLen = n;
    }

    static bool NamesEqual(const Entry& entry, std::string_view name) noexcept
    {
        const usize n = name.size() < kNameCap ? name.size() : kNameCap;
        if (entry.NameLen != n)
        {
            return false;
        }
        for (usize i = 0; i < n; ++i)
        {
            if (entry.Name[i] != name[i])
            {
                return false;
            }
        }
        return true;
    }

    mutable std::mutex mMutex;
    std::atomic<usize> mCount{0};
    Entry mEntries[kCapacity];
};

} // namespace ludus::foundation::logging::internal
