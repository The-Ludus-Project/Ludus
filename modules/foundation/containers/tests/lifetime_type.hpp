#pragma once

// Instrumented element types for container tests. These make lifetime bugs
// observable: every construct/copy/move/destroy is counted, and each live
// instance carries a canary so use-after-move / use-after-free is detectable.
//
// Tests assert the ledger balances (constructions == destructions, zero live at
// end) rather than just checking final values.

#include <ludus/foundation/containers/relocation.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/foundation/base/types.h>

#include <cstdint>
#include <cstdlib>

namespace ludus::containers::testing
{
using ludus::foundation::usize;

struct LifetimeLedger
{
    long DefaultCtor = 0;
    long ValueCtor = 0;
    long CopyCtor = 0;
    long MoveCtor = 0;
    long CopyAssign = 0;
    long MoveAssign = 0;
    long Dtor = 0;

    void Reset() noexcept
    {
        *this = LifetimeLedger{};
    }

    [[nodiscard]] long Constructions() const noexcept
    {
        return DefaultCtor + ValueCtor + CopyCtor + MoveCtor;
    }
    // Live instances = constructions - destructions. Must be 0 at end of scope.
    [[nodiscard]] long Live() const noexcept
    {
        return Constructions() - Dtor;
    }
    [[nodiscard]] bool Balanced() const noexcept
    {
        return Live() == 0;
    }
};

// A non-trivial tracked type. NOT trivially relocatable (has a user dtor), so it
// exercises the per-element move+destroy reallocation path.
class Tracked
{
public:
    static constexpr std::uint32_t kAlive = 0xA11'0CEDu;
    static constexpr std::uint32_t kDead = 0xDEAD'DEADu;

    explicit Tracked(LifetimeLedger* ledger = nullptr, int value = 0) noexcept : mLedger(ledger), mValue(value)
    {
        if (mLedger != nullptr)
        {
            ++mLedger->ValueCtor;
        }
    }

    Tracked(const Tracked& other) noexcept : mLedger(other.mLedger), mValue(other.mValue)
    {
        CheckAlive(other);
        if (mLedger != nullptr)
        {
            ++mLedger->CopyCtor;
        }
    }

    Tracked(Tracked&& other) noexcept : mLedger(other.mLedger), mValue(other.mValue)
    {
        CheckAlive(other);
        if (mLedger != nullptr)
        {
            ++mLedger->MoveCtor;
        }
        other.mValue = -1; // moved-from marker (object still alive/valid)
    }

    // Scalar-only members: self-assignment is a harmless no-op; the counter
    // increment is intentional test instrumentation.
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
    Tracked& operator=(const Tracked& other) noexcept
    {
        CheckAlive(*this);
        CheckAlive(other);
        mLedger = other.mLedger;
        mValue = other.mValue;
        if (mLedger != nullptr)
        {
            ++mLedger->CopyAssign;
        }
        return *this;
    }

    Tracked& operator=(Tracked&& other) noexcept
    {
        CheckAlive(*this);
        CheckAlive(other);
        mLedger = other.mLedger;
        mValue = other.mValue;
        if (mLedger != nullptr)
        {
            ++mLedger->MoveAssign;
        }
        other.mValue = -1;
        return *this;
    }

    ~Tracked()
    {
        CheckAlive(*this);
        if (mLedger != nullptr)
        {
            ++mLedger->Dtor;
        }
        mCanary = kDead;
    }

    [[nodiscard]] int Value() const noexcept
    {
        return mValue;
    }
    void SetValue(int v) noexcept
    {
        mValue = v;
    }
    [[nodiscard]] std::uint32_t Canary() const noexcept
    {
        return mCanary;
    }

    friend bool operator==(const Tracked& a, const Tracked& b) noexcept
    {
        return a.mValue == b.mValue;
    }

private:
    static void CheckAlive(const Tracked& t) noexcept
    {
        // If this fires (via ASan/UBSan or the abort below), a dead object was
        // touched — a lifetime bug in the container.
        if (t.mCanary != kAlive)
        {
            std::abort();
        }
    }

    LifetimeLedger* mLedger = nullptr;
    int mValue = 0;
    std::uint32_t mCanary = kAlive;
};

// Move-only tracked type (models UniquePtr-like elements: the logging sink case).
class MoveOnly
{
public:
    explicit MoveOnly(LifetimeLedger* ledger = nullptr, int value = 0) noexcept : mLedger(ledger), mValue(value)
    {
        if (mLedger != nullptr)
        {
            ++mLedger->ValueCtor;
        }
    }
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : mLedger(other.mLedger), mValue(other.mValue)
    {
        if (mLedger != nullptr)
        {
            ++mLedger->MoveCtor;
        }
        other.mValue = -1;
    }
    MoveOnly& operator=(MoveOnly&& other) noexcept
    {
        mLedger = other.mLedger;
        mValue = other.mValue;
        if (mLedger != nullptr)
        {
            ++mLedger->MoveAssign;
        }
        other.mValue = -1;
        return *this;
    }
    ~MoveOnly()
    {
        if (mLedger != nullptr)
        {
            ++mLedger->Dtor;
        }
    }
    [[nodiscard]] int Value() const noexcept
    {
        return mValue;
    }

private:
    LifetimeLedger* mLedger = nullptr;
    int mValue = 0;
};

// A type that opts into trivial relocation despite having a non-trivial-looking
// footprint (no internal pointers -> byte-relocatable). Verifies the memcpy
// growth path is taken (it counts moves; the fast path must produce ZERO).
struct Relocatable
{
    LifetimeLedger* Ledger = nullptr;
    int Value = 0;

    Relocatable() = default;
    explicit Relocatable(LifetimeLedger* ledger, int value) noexcept : Ledger(ledger), Value(value) {}
    Relocatable(const Relocatable&) = default;
    Relocatable(Relocatable&& other) noexcept : Ledger(other.Ledger), Value(other.Value)
    {
        if (Ledger != nullptr)
        {
            ++Ledger->MoveCtor;
        }
    }
    Relocatable& operator=(const Relocatable&) = default;
    Relocatable& operator=(Relocatable&&) = default;
    ~Relocatable() = default;
};

// Over-aligned element type (two cache lines). Verifies the aligned allocation
// seam and that Data() is correctly aligned.
constexpr usize OVER_ALIGNMENT = 128;
constexpr usize OVER_ALIGNED_PADDING = OVER_ALIGNMENT - sizeof(ludus::foundation::int32);

struct alignas(OVER_ALIGNMENT) OverAligned
{
    ludus::foundation::int32 Value = 0;
    ludus::foundation::StaticArray<ludus::foundation::uint8, OVER_ALIGNED_PADDING> Padding{};
    OverAligned() = default;
    explicit OverAligned(int v) noexcept : Value(v) {}
    friend bool operator==(const OverAligned& a, const OverAligned& b) noexcept
    {
        return a.Value == b.Value;
    }
};

// Empty element type.
struct Empty
{
    friend bool operator==(const Empty&, const Empty&) noexcept
    {
        return true;
    }
};
} // namespace ludus::containers::testing

// Opt the Relocatable test type into the memcpy fast path. Must be at global
// namespace scope; expands to a specialization inside ludus::foundation::core.
LUDUS_TRIVIALLY_RELOCATABLE(ludus::containers::testing::Relocatable)
