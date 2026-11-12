// Benchmarks: Ludus::Array / Ludus::Array vs std::vector / std::array.
//
// Goal is NOT to manufacture a win over std::vector (a good std::vector on
// trivially-copyable types is already near-optimal). Goal: prove Ludus::Array
// is not materially worse on representative engine workloads, and record honest
// numbers. Compiled at -O2 with the pinned Clang 18. Simple wall-clock harness;
// each workload runs enough iterations to be measurable and is repeated, taking
// the best (min) time to reduce noise.

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::int32;
using ludus::foundation::isize;
using ludus::foundation::uint32;
using ludus::foundation::usize;

namespace
{
// Prevent the optimizer from deleting work whose result is unused.
template <typename T>
inline void DoNotOptimize(const T& value)
{
    asm volatile("" : : "r,m"(value) : "memory");
}

struct Result
{
    double ludusNs;
    double stdNs;
};

template <typename Fn>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): repetition count, then normalization element count.
double BestNs(Fn&& fn, int reps, long innerIters)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const ludus::foundation::time::Timestamp t0 = ludus::foundation::time::Now();
        fn();
        const ludus::foundation::time::Timestamp t1 = ludus::foundation::time::Now();
        const ludus::foundation::float64 ns = static_cast<ludus::foundation::float64>(t1.Nanoseconds - t0.Nanoseconds) /
                                              static_cast<ludus::foundation::float64>(innerIters);
        if (ns < best)
        {
            best = ns;
        }
    }
    return best;
}

void Row(const char* name, Result r)
{
    const double ratio = r.stdNs > 0.0 ? r.ludusNs / r.stdNs : 0.0;
    std::printf("%-38s  ludus=%10.4f  std=%10.4f  ratio=%.3f%s\n",
                name,
                r.ludusNs,
                r.stdNs,
                ratio,
                ratio <= 1.05 ? "" : "   (SLOWER)");
}

// A 32-byte POD "transform-like" struct: trivially copyable => relocatable.
struct Pod32
{
    float m[8];
};

// A non-trivial element (heap-owning): exercises the move+destroy path.
struct NonTrivial
{
    std::string s;
    NonTrivial() : s("x") {}
    explicit NonTrivial(int i) : s(std::to_string(i)) {}
};
} // namespace

int main()
{
    constexpr int kReps = 7;
    constexpr long N = 100000;

    std::printf("=== Ludus::Array vs std::vector  (Clang 18, -O2; ns per element, best of %d) ===\n", kReps);

    // --- push N uint32 (trivial scalar) ---
    {
        auto ludus = BestNs(
            [] {
                Array<std::uint32_t> v;
                for (long i = 0; i < N; ++i)
                {
                    v.Add(static_cast<std::uint32_t>(i));
                }
                DoNotOptimize(v.GetData());
            },
            kReps,
            N);
        auto stdv = BestNs(
            [] {
                std::vector<std::uint32_t> v;
                for (long i = 0; i < N; ++i)
                {
                    v.push_back(static_cast<std::uint32_t>(i));
                }
                DoNotOptimize(v.data());
            },
            kReps,
            N);
        Row("push N uint32 (unreserved)", {ludus, stdv});
    }

    // --- reserve + push N uint32 ---
    {
        auto ludus = BestNs(
            [] {
                Array<std::uint32_t> v;
                v.EnsureCapacity(N);
                for (long i = 0; i < N; ++i)
                {
                    v.Add(static_cast<std::uint32_t>(i));
                }
                DoNotOptimize(v.GetData());
            },
            kReps,
            N);
        auto stdv = BestNs(
            [] {
                std::vector<std::uint32_t> v;
                v.reserve(N);
                for (long i = 0; i < N; ++i)
                {
                    v.push_back(static_cast<std::uint32_t>(i));
                }
                DoNotOptimize(v.data());
            },
            kReps,
            N);
        Row("reserve + push N uint32", {ludus, stdv});
    }

    // --- push N POD32 (trivially relocatable) ---
    {
        auto ludus = BestNs(
            [] {
                Array<Pod32> v;
                for (long i = 0; i < N; ++i)
                {
                    v.Add(Pod32{});
                }
                DoNotOptimize(v.GetData());
            },
            kReps,
            N);
        auto stdv = BestNs(
            [] {
                std::vector<Pod32> v;
                for (long i = 0; i < N; ++i)
                {
                    v.push_back(Pod32{});
                }
                DoNotOptimize(v.data());
            },
            kReps,
            N);
        Row("push N POD32 (unreserved)", {ludus, stdv});
    }

    // --- push N non-trivial (move+destroy relocation path) ---
    {
        constexpr long M = 20000;
        auto ludus = BestNs(
            [] {
                Array<NonTrivial> v;
                for (long i = 0; i < M; ++i)
                {
                    v.AddInPlace(static_cast<int>(i));
                }
                DoNotOptimize(v.GetData());
            },
            kReps,
            M);
        auto stdv = BestNs(
            [] {
                std::vector<NonTrivial> v;
                for (long i = 0; i < M; ++i)
                {
                    v.emplace_back(static_cast<int>(i));
                }
                DoNotOptimize(v.data());
            },
            kReps,
            M);
        Row("push N non-trivial (std::string elem)", {ludus, stdv});
    }

    // --- iteration / sum ---
    {
        Array<std::uint32_t> lv;
        std::vector<std::uint32_t> sv;
        for (long i = 0; i < N; ++i)
        {
            lv.Add(static_cast<std::uint32_t>(i));
            sv.push_back(static_cast<std::uint32_t>(i));
        }
        auto ludus = BestNs(
            [&] {
                std::uint64_t sum = 0;
                for (auto x : lv)
                {
                    sum += x;
                }
                DoNotOptimize(sum);
            },
            kReps * 4,
            N);
        auto stdv = BestNs(
            [&] {
                std::uint64_t sum = 0;
                for (auto x : sv)
                {
                    sum += x;
                }
                DoNotOptimize(sum);
            },
            kReps * 4,
            N);
        Row("iterate + sum N uint32", {ludus, stdv});
    }

    // --- random access ---
    {
        Array<std::uint32_t> lv;
        std::vector<std::uint32_t> sv;
        for (long i = 0; i < N; ++i)
        {
            lv.Add(static_cast<std::uint32_t>((i * 2654435761u) % N));
            sv.push_back(static_cast<std::uint32_t>((i * 2654435761u) % N));
        }
        auto ludus = BestNs(
            [&] {
                std::uint64_t sum = 0;
                for (long i = 0; i < N; ++i)
                {
                    sum += lv[static_cast<std::size_t>((i * 40503) % N)];
                }
                DoNotOptimize(sum);
            },
            kReps,
            N);
        auto stdv = BestNs(
            [&] {
                std::uint64_t sum = 0;
                for (long i = 0; i < N; ++i)
                {
                    sum += sv[static_cast<std::size_t>((i * 40503) % N)];
                }
                DoNotOptimize(sum);
            },
            kReps,
            N);
        Row("random access N uint32", {ludus, stdv});
    }

    // --- bulk append (borrowed view) ---
    {
        std::vector<std::uint32_t> src(N);
        for (long i = 0; i < N; ++i)
        {
            src[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i);
        }
        auto ludus = BestNs(
            [&] {
                Array<std::uint32_t> v;
                v.AddRange(BorrowedView<const std::uint32_t>(src.data(), src.size()));
                DoNotOptimize(v.GetData());
            },
            kReps,
            N);
        auto stdv = BestNs(
            [&] {
                std::vector<std::uint32_t> v;
                v.insert(v.end(), src.begin(), src.end());
                DoNotOptimize(v.data());
            },
            kReps,
            N);
        Row("bulk append N uint32", {ludus, stdv});
    }

    // --- copy whole ---
    {
        Array<Pod32> lv;
        std::vector<Pod32> sv;
        for (long i = 0; i < N; ++i)
        {
            lv.Add(Pod32{});
            sv.push_back(Pod32{});
        }
        auto ludus = BestNs(
            [&] {
                Array<Pod32> c = lv;
                DoNotOptimize(c.GetData());
            },
            kReps,
            N);
        auto stdv = BestNs(
            [&] {
                std::vector<Pod32> c = sv;
                DoNotOptimize(c.data());
            },
            kReps,
            N);
        Row("copy N POD32", {ludus, stdv});
    }

    // Resize batches model growing initialized build data, with and without
    // reserved capacity. External fill is valid on both sides of the alias fix;
    // timing the formerly dangling self-reference would not be a valid baseline.
    for (const bool reserveTail : {false, true})
    {
        constexpr usize kInitialSize = 64;
        constexpr usize kFinalSize = 1024;
        constexpr int32 kBatches = 4096;
        const uint32 fill = 42;
        auto ludus = BestNs(
            [&] {
                for (int32 batch = 0; batch < kBatches; ++batch)
                {
                    Array<uint32> values;
                    values.EnsureCapacity(reserveTail ? kFinalSize : kInitialSize);
                    values.Resize(kInitialSize, fill);
                    values.Resize(kFinalSize, fill);
                    DoNotOptimize(values.GetData());
                    if (values[0] + values[kInitialSize] + values.GetLast() != 3 * fill)
                    {
                        std::abort();
                    }
                }
            },
            kReps,
            static_cast<isize>(kBatches) * static_cast<isize>(kFinalSize));
        auto stdv = BestNs(
            [&] {
                for (int32 batch = 0; batch < kBatches; ++batch)
                {
                    std::vector<uint32> values;
                    values.reserve(reserveTail ? kFinalSize : kInitialSize);
                    values.resize(kInitialSize, fill);
                    values.resize(kFinalSize, fill);
                    DoNotOptimize(values.data());
                    if (values[0] + values[kInitialSize] + values.back() != 3 * fill)
                    {
                        std::abort();
                    }
                }
            },
            kReps,
            static_cast<isize>(kBatches) * static_cast<isize>(kFinalSize));
        Row(reserveTail ? "resize fill uint32 (reserved)" : "resize fill uint32 (growing)", {ludus, stdv});
    }

    std::printf("\n=== sizeof ===\n");
    std::printf("sizeof(Ludus::Array<int>)=%zu  sizeof(std::vector<int>)=%zu\n",
                sizeof(Array<int>),
                sizeof(std::vector<int>));
    std::printf("sizeof(Ludus::StaticArray<int,16>)=%zu  sizeof(std::array<int,16>)=%zu\n",
                sizeof(ludus::foundation::StaticArray<int, 16>),
                sizeof(std::array<int, 16>));

    return 0;
}
