// Standalone measurement tool; standard maps are comparison baselines.
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

using ludus::foundation::float64;
using ludus::foundation::SortedMap;
using ludus::foundation::StaticArray;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace timing = ludus::foundation::time;

namespace
{
constexpr usize kQueries = 100000;
constexpr usize kSamples = 7;
volatile uint64 checksum = 0;

template <typename Function>
float64 MedianNs(Function&& function, usize operations)
{
    StaticArray<float64, kSamples> samples{};
    for (usize sample = 0; sample < kSamples; ++sample)
    {
        const timing::Timestamp start = timing::Now();
        const uint64 result = function();
        const timing::Timestamp stop = timing::Now();
        checksum = result;
        samples[sample] = static_cast<float64>(stop.Nanoseconds - start.Nanoseconds) / static_cast<float64>(operations);
    }
    std::sort(samples.begin(), samples.end());
    return samples[kSamples / 2];
}

void Measure(usize count)
{
    SortedMap<uint64, uint64> sorted;
    std::unordered_map<uint64, uint64> hashed;
    std::map<uint64, uint64> tree;
    sorted.EnsureCapacity(count);
    hashed.reserve(count);
    for (usize i = 0; i < count; ++i)
    {
        const auto key = static_cast<uint64>(i) * 2;
        sorted.AddInPlace(key, key + 1);
        hashed.emplace(key, key + 1);
        tree.emplace(key, key + 1);
    }
    StaticArray<uint64, kQueries> queries{};
    uint64 state = 0x9e3779b97f4a7c15ULL;
    for (uint64& query : queries)
    {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        query = (state * 0x2545f4914f6cdd1dULL) % (count * 2);
    }
    // Same random queries, about 50% misses. Setup excluded; no allocations.
    const float64 sortedLookup = MedianNs(
        [&] {
            uint64 total = 0;
            for (const uint64 key : queries)
            {
                const uint64* value = sorted.Find(key);
                total += value == nullptr ? 0 : *value;
            }
            return total;
        },
        kQueries);
    const float64 hashLookup = MedianNs(
        [&] {
            uint64 total = 0;
            for (const uint64 key : queries)
            {
                const auto found = hashed.find(key);
                total += found == hashed.end() ? 0 : found->second;
            }
            return total;
        },
        kQueries);
    const float64 treeLookup = MedianNs(
        [&] {
            uint64 total = 0;
            for (const uint64 key : queries)
            {
                const auto found = tree.find(key);
                total += found == tree.end() ? 0 : found->second;
            }
            return total;
        },
        kQueries);
    const usize batches = 500;
    // Descending input exercises worst-case sorted insertion rather than just
    // sorted append. Pre-reserved buffers retained between batches.
    const float64 sortedInsert = MedianNs(
        [&] {
            for (usize batch = 0; batch < batches; ++batch)
            {
                sorted.Clear();
                for (usize i = count; i != 0; --i)
                {
                    sorted.AddInPlace(static_cast<uint64>(i), static_cast<uint64>(i));
                }
            }
            return sorted.GetSize();
        },
        batches * count);
    const float64 hashInsert = MedianNs(
        [&] {
            for (usize batch = 0; batch < batches; ++batch)
            {
                hashed.clear();
                for (usize i = count; i != 0; --i)
                {
                    hashed.emplace(static_cast<uint64>(i), static_cast<uint64>(i));
                }
            }
            return hashed.size();
        },
        batches * count);
    std::printf("%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%zu\n",
                count,
                sortedLookup,
                hashLookup,
                treeLookup,
                sortedInsert,
                hashInsert,
                sorted.GetCapacity() * sizeof(SortedMap<uint64, uint64>::Entry));
}
} // namespace

int main()
{
    std::puts(
        "count,sorted_lookup_ns,hash_lookup_ns,tree_lookup_ns,sorted_insert_ns,hash_insert_ns,sorted_storage_bytes");
    for (const usize count : {1U, 4U, 16U, 64U, 256U, 1024U})
    {
        Measure(count);
    }
    return 0;
}
