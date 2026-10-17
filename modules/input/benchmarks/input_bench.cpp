// Bounded performance-evidence harness for the keyboard reducer (M4, design
// section 8). Measures the required workloads, reports median and p95 per-batch
// times, the private storage footprint, allocation counts, and trace on/off
// cost. This is a planning-grade wall-clock harness, not a hardware-latency or
// whole-engine-determinism claim. Build at the project's Development/Release
// flags; run manually and paste output into the evidence doc.

#include <ludus/foundation/containers/array.hpp>
#include <ludus/input/input_debug.h>
#include <ludus/input/keyboard.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace ludus::input;
using ludus::foundation::Array;
using ludus::foundation::float64;
using ludus::foundation::usize;
using Clock = std::chrono::steady_clock;

namespace
{
// Allocation counter to prove zero hot-path allocation (construction excluded).
std::size_t gAllocs = 0;
bool gTrack = false;

template <typename T>
inline void doNotOptimize(const T& value)
{
    // An empty asm statement is the benchmark optimization barrier; it emits no instructions.
    asm volatile("" : : "r,m"(value) : "memory"); // NOLINT(portability-no-assembler)
}

float64 percentile(Array<float64>& inoutSamples, float64 p)
{
    std::sort(inoutSamples.begin(), inoutSamples.end());
    const usize index = static_cast<usize>(p * static_cast<float64>(inoutSamples.GetSize() - 1));
    return inoutSamples[index];
}

KeyboardRecord down(Key key) noexcept
{
    return KeyboardRecord{ .Transition = KeyTransition::Down, .PhysicalKey = key };
}
KeyboardRecord up(Key key) noexcept
{
    return KeyboardRecord{ .Transition = KeyTransition::Up, .PhysicalKey = key };
}

void focus(InputSystem& system) noexcept
{
    FocusBaseline baseline;
    baseline.Focused = true;
    system.RequestReset(ResetReason::FocusEntered, baseline);
}

struct Timing
{
    double medianNs;
    double p95Ns;
};

struct MeasureConfig
{
    int iterations;
    int warmup;
};

template <typename Fn>
Timing measure(Fn&& batch, MeasureConfig config)
{
    for (int i = 0; i < config.warmup; ++i)
    {
        batch();
    }
    Array<float64> samples;
    samples.EnsureCapacity(static_cast<usize>(config.iterations));
    for (int i = 0; i < config.iterations; ++i)
    {
        const auto start = Clock::now();
        batch();
        const auto end = Clock::now();
        samples.Add(std::chrono::duration<float64, std::nano>(end - start).count());
    }
    return Timing{percentile(samples, 0.5), percentile(samples, 0.95)};
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t n)
{
    if (gTrack)
    {
        ++gAllocs;
    }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr)
    {
        std::abort(); // engine code is -fno-exceptions; cannot throw bad_alloc
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    if (gTrack)
    {
        ++gAllocs;
    }
    return std::malloc(n == 0 ? 1 : n);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

int main()
{
    std::printf("# Ludus keyboard reducer — performance evidence\n");
    std::printf("sizeof(InputSystem) public handle: %zu bytes\n", sizeof(InputSystem));
    std::printf("sizeof(InputDebugTrace): %zu bytes\n", sizeof(InputDebugTrace));
    std::printf("PENDING_CAPACITY=%zu ACTION_CAPACITY=%zu BINDING_CAPACITY=%zu TRACE_CAPACITY=%zu\n",
                PENDING_CAPACITY,
                ACTION_CAPACITY,
                BINDING_CAPACITY,
                TRACE_CAPACITY);

    constexpr MeasureConfig CONFIG{ .iterations = 2000, .warmup = 200 };

    // Allocation accounting: one construction allocation, zero across hot ops.
    {
        gAllocs = 0;
        gTrack = true;
        InputSystem system;
        gTrack = false;
        const std::size_t ctorAllocs = gAllocs;

        focus(system);
        gTrack = true;
        gAllocs = 0;
        ludus::foundation::uint64 step = 1;
        for (int i = 0; i < 5000; ++i)
        {
            (void)system.Ingest(down(Key::KeyW));
            (void)system.Ingest(up(Key::KeyW));
            (void)system.ConsumeStep(step++);
            ActionState s;
            (void)system.GetAction(0, s);
            doNotOptimize(s);
        }
        gTrack = false;
        std::printf("allocations: construction=%zu, hot-loop(5000 iters)=%zu\n", ctorAllocs, gAllocs);
    }

    // Workload: idle steps (no events).
    {
        InputSystem system;
        focus(system);
        ludus::foundation::uint64 step = 1;
        const Timing t = measure([&] { (void)system.ConsumeStep(step++); }, CONFIG);
        std::printf("idle step:              median=%.1f ns  p95=%.1f ns\n", t.medianNs, t.p95Ns);
    }

    // Workload: normal typing/movement batch (WASD + space, 8 transitions/step).
    {
        InputSystem system;
        focus(system);
        const Binding bindings[] = {
            Binding{ .Action = 0, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
            Binding
            {
                .Action = 1,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Positive,
                .PhysicalKey = Key::KeyD,
            },
            Binding
            {
                .Action = 1,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Negative,
                .PhysicalKey = Key::KeyA,
            },
            Binding
            {
                .Action = 2,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Positive,
                .PhysicalKey = Key::KeyW,
            },
            Binding
            {
                .Action = 2,
                .Kind = ActionKind::Axis1D,
                .Direction = AxisDirection::Negative,
                .PhysicalKey = Key::KeyS,
            },
        };
        (void)system.ReplaceBindings(BindingMap{ .Bindings = bindings });
        ludus::foundation::uint64 step = 1;
        (void)system.ConsumeStep(step++);
        const Timing t = measure(
            [&] {
                (void)system.Ingest(down(Key::KeyW));
                (void)system.Ingest(down(Key::KeyD));
                (void)system.Ingest(down(Key::Space));
                (void)system.Ingest(up(Key::Space));
                (void)system.Ingest(up(Key::KeyD));
                (void)system.Ingest(up(Key::KeyW));
                (void)system.ConsumeStep(step++);
            },
            CONFIG);
        std::printf("typing/movement batch:  median=%.1f ns  p95=%.1f ns\n", t.medianNs, t.p95Ns);
    }

    // Workload: repeated/unknown records (all ignored/rejected).
    {
        InputSystem system;
        focus(system);
        (void)system.Ingest(down(Key::KeyW));
        ludus::foundation::uint64 step = 1;
        (void)system.ConsumeStep(step++);
        const Timing t = measure(
            [&] {
                for (int i = 0; i < 32; ++i)
                {
                    KeyboardRecord repeat
                    {
                        .Transition = KeyTransition::Down,
                        .PhysicalKey = Key::KeyW,
                        .Repeat = true,
                    };
                    (void)system.Ingest(repeat);
                    (void)system.Ingest(down(static_cast<Key>(50000)));
                }
                (void)system.ConsumeStep(step++);
            },
            CONFIG);
        std::printf("repeated/unknown batch: median=%.1f ns  p95=%.1f ns\n", t.medianNs, t.p95Ns);
    }

    // Workload: 256 transitions with 256 bindings (saturated step).
    {
        InputSystem system;
        focus(system);
        static Binding bindings[BINDING_CAPACITY];
        for (ludus::foundation::usize i = 0; i < BINDING_CAPACITY; ++i)
        {
            bindings[i] = Binding
            {
                .Action = static_cast<ActionId>(i % ACTION_CAPACITY),
                .Kind = ActionKind::Button,
                .PhysicalKey = static_cast<Key>(1 + (i % (KEY_COUNT - 1))),
            };
        }
        const BindingStatus status = system.ReplaceBindings(BindingMap{ .Bindings = bindings });
        ludus::foundation::uint64 step = 1;
        (void)system.ConsumeStep(step++);
        const Timing t = measure(
            [&] {
                // 128 down then 128 up across distinct keys = 256 transitions.
                for (ludus::foundation::usize i = 0; i < 128; ++i)
                {
                    (void)system.Ingest(down(static_cast<Key>(1 + i)));
                }
                for (ludus::foundation::usize i = 0; i < 128; ++i)
                {
                    (void)system.Ingest(up(static_cast<Key>(1 + i)));
                }
                (void)system.ConsumeStep(step++);
            },
            CONFIG);
        std::printf("256 transitions/256 bindings (map=%s): median=%.1f ns  p95=%.1f ns\n",
                    status == BindingStatus::Ok ? "Ok" : "Dup",
                    t.medianNs,
                    t.p95Ns);
    }

    // Workload: reset/overflow storm.
    {
        InputSystem system;
        ludus::foundation::uint64 step = 1;
        const Timing t = measure(
            [&] {
                FocusBaseline b;
                b.Focused = true;
                for (int i = 0; i < 16; ++i)
                {
                    system.RequestReset(ResetReason::FocusEntered, b);
                    (void)system.Ingest(down(Key::KeyW));
                    (void)system.Ingest(up(Key::KeyW));
                }
                (void)system.ConsumeStep(step++);
            },
            CONFIG);
        std::printf("reset storm:            median=%.1f ns  p95=%.1f ns\n", t.medianNs, t.p95Ns);
    }

    // Trace on vs off cost on the typing/movement batch.
    {
        auto run = [&](bool withTrace) -> Timing {
            InputSystem system;
            InputDebugTrace trace;
            if (withTrace)
            {
                system.SetDebugTrace(&trace);
            }
            focus(system);
            ludus::foundation::uint64 step = 1;
            (void)system.ConsumeStep(step++);
            return measure(
                [&] {
                    (void)system.Ingest(down(Key::KeyW));
                    (void)system.Ingest(up(Key::KeyW));
                    (void)system.ConsumeStep(step++);
                    trace.Clear();
                },
                CONFIG);
        };
        const Timing off = run(false);
        const Timing on = run(true);
        std::printf("trace off: median=%.1f ns   trace on: median=%.1f ns\n", off.medianNs, on.medianNs);
    }

    return 0;
}
