#pragma once

// Golden scatter fixture shared by the native tests and the WebAssembly math
// probe (tools/web-math-probe), so both targets check the same pinned digest.
// Header-only and free of the test framework.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/environment/scatter.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural::golden
{
/// World seed of the golden fixture.
inline constexpr uint64 WORLD_SEED = 0x5eed0001u;
/// Game-range domains of the golden fixture.
inline constexpr ScatterDomains DOMAINS = {11u, 12u, 13u};
/// Variants of the first layer.
inline constexpr ScatterVariant SHRUB_VARIANTS[] = {{12u}, {20u}, {30u}};
/// Variants of the second layer.
inline constexpr ScatterVariant STONE_VARIANTS[] = {{25u}};
/// Keep-out rectangles of the room.
inline constexpr ScatterRectangle KEEP_OUTS[] = {{-60, -40, 20, 40}, {300, 100, 380, 200}};
/// Storage that holds every candidate of one golden layer.
inline constexpr usize STORAGE_PLACEMENTS = 2048;
/// Pinned placement count of both layers for SCATTER_LAYOUT_VERSION 1.
inline constexpr usize PLACEMENT_COUNT = 123;
/// Pinned digest of both layers for SCATTER_LAYOUT_VERSION 1. A different value changes every saved world's
/// placements and requires a new layout version, never an updated constant.
inline constexpr uint64 PLACEMENT_DIGEST = 0x6ecc6396fa22b77fULL;

/// A two-layer room recipe whose views borrow its own arrays, so it is neither copied nor moved.
struct GoldenScatter
{
    GoldenScatter() noexcept = default;
    GoldenScatter(const GoldenScatter&) = delete;
    GoldenScatter& operator=(const GoldenScatter&) = delete;

    ScatterLayer Layers[2] = {};
    ScatterRecipe Recipe;
    ScatterKeys Keys;
};

/// Fill the golden recipe and derive its keys; false only if key derivation fails.
[[nodiscard]] inline bool InitializeGoldenScatter(GoldenScatter& outScatter) noexcept
{
    outScatter.Layers[0].Id = 3;
    outScatter.Layers[0].CellSize = 40;
    outScatter.Layers[0].CandidatesPerCell = 4;
    outScatter.Layers[0].Variants = foundation::BorrowedView<const ScatterVariant>(SHRUB_VARIANTS);
    outScatter.Layers[0].MinimumScale = 800;
    outScatter.Layers[0].MaximumScale = 1200;
    outScatter.Layers[1].Id = 9;
    outScatter.Layers[1].CellSize = 25;
    outScatter.Layers[1].CandidatesPerCell = 2;
    outScatter.Layers[1].Variants = foundation::BorrowedView<const ScatterVariant>(STONE_VARIANTS);
    outScatter.Recipe.Room = 7;
    outScatter.Recipe.Region = {-300, -200, 500, 260};
    outScatter.Recipe.KeepOuts = foundation::BorrowedView<const ScatterRectangle>(KEEP_OUTS);
    outScatter.Recipe.Layers = foundation::BorrowedView<const ScatterLayer>(outScatter.Layers);
    return TryMakeScatterKeys(WORLD_SEED, DOMAINS, outScatter.Keys) == ScatterStatus::Success;
}

/// FNV-1a over placement fields in little-endian order, independent of struct padding.
class PlacementDigest final
{
public:
    /// Fold one placement into the digest.
    void Add(const ScatterPlacement& placement) noexcept
    {
        addValue<sizeof(uint64)>(PackScatterScope(placement.Identity));
        addValue<sizeof(int32)>(static_cast<uint32>(placement.LatticeX));
        addValue<sizeof(int32)>(static_cast<uint32>(placement.LatticeY));
        addValue<sizeof(uint32)>(placement.Variant);
        addValue<sizeof(uint16)>(placement.Rotation);
        addValue<sizeof(uint16)>(placement.Scale);
    }
    /// Digest of every placement added so far.
    [[nodiscard]] uint64 GetValue() const noexcept
    {
        return mValue;
    }

private:
    static constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    static constexpr uint64 FNV_PRIME = 0x100000001b3ULL;
    static constexpr uint64 BYTE_MASK = 0xffu;
    static constexpr usize BITS_PER_BYTE = 8;

    template <usize ByteCount>
    void addValue(uint64 value) noexcept
    {
        for (usize i = 0; i < ByteCount; ++i)
        {
            mValue ^= (value >> (i * BITS_PER_BYTE)) & BYTE_MASK;
            mValue *= FNV_PRIME;
        }
    }

    uint64 mValue = FNV_OFFSET_BASIS;
};

/// Outcome of ComputeGoldenDigest.
struct GoldenDigest final
{
    /// Success, or the first failing kernel status.
    ScatterStatus Status = ScatterStatus::Success;
    /// Total placements of all layers.
    usize Count = 0;
    /// Digest of all layers' placements, in layer then canonical order.
    uint64 Value = 0;
};

/// Generate every golden layer in order and digest the placements.
/// @param scatter An initialized golden fixture.
/// @param storage Scratch for one layer, at least STORAGE_PLACEMENTS elements.
[[nodiscard]] inline GoldenDigest ComputeGoldenDigest(const GoldenScatter& scatter,
                                                      foundation::BorrowedView<ScatterPlacement> storage) noexcept
{
    PlacementDigest digest;
    GoldenDigest result;
    for (usize layer = 0; layer < scatter.Recipe.Layers.GetSize(); ++layer)
    {
        ScatterCellRange cells;
        usize count = 0;
        result.Status = TryGetScatterCells(scatter.Recipe, layer, cells);
        if (result.Status == ScatterStatus::Success)
        {
            result.Status = TryGenerateScatter(scatter.Recipe, scatter.Keys, layer, cells, storage, count);
        }
        if (result.Status != ScatterStatus::Success)
        {
            return result;
        }
        for (usize i = 0; i < count; ++i)
        {
            digest.Add(storage[i]);
        }
        result.Count += count;
    }
    result.Value = digest.GetValue();
    return result;
}
} // namespace ludus::content::procedural::golden
