#include <ludus/graphics/ui_text/label_cache.hpp>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/text/text_adapter.hpp>

#include <new>

namespace ludus::graphics::ui_text
{
namespace
{
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace core = foundation::core;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
constexpr usize ATLAS_TEXELS = usize{text::CoverageAtlas::EXTENT} * text::CoverageAtlas::EXTENT;

/// One element's label: copied placements, the atlas keys that produced them and its placement.
struct Slot final
{
    core::StaticArray<text::TextQuad, LabelCache::MAX_LABEL_GLYPHS> Glyphs;
    core::StaticArray<text::GlyphKey, LabelCache::MAX_LABEL_GLYPHS> Keys;
    usize Count = 0;
    Label Placement;
};

[[nodiscard]] bool IsUnitChannel(float32 channel) noexcept
{
    return math::IsFinite(channel) == true && channel >= 0 && channel <= 1;
}

[[nodiscard]] bool IsValidSource(const LabelSource& source) noexcept
{
    const ludus::ui::Color& color = source.Color;
    return source.Element != 0 && source.FontVersion != 0 && math::IsFinite(source.Scale) == true && source.Scale > 0 &&
           math::IsFinite(source.OffsetX) == true && math::IsFinite(source.BaselineY) == true &&
           IsUnitChannel(color.R) == true && IsUnitChannel(color.G) == true && IsUnitChannel(color.B) == true &&
           IsUnitChannel(color.A) == true;
}

/// Copy one cached glyph's coverage from one atlas page into another under the same key.
[[nodiscard]] Status CopyGlyph(const text::CoverageAtlas& source,
                               text::GlyphKey key,
                               core::StaticArray<uint8, ATLAS_TEXELS>& inoutBytes,
                               text::CoverageAtlas& inoutDestination,
                               text::AtlasGlyph& outGlyph) noexcept
{
    text::AtlasGlyph cached;
    const Status found = source.Find(key, cached);
    if (found != Status::Ready)
    {
        // Cells never move or evict until Reset, so every key a live label holds is present.
        return Status::InvalidDescription;
    }
    const usize byteCount = usize{cached.Width} * cached.Height;
    if (byteCount != 0)
    {
        // UV edges are exact texel multiples of 1 / EXTENT, a power of two.
        const usize left = static_cast<usize>(cached.Uv.X * static_cast<float32>(text::CoverageAtlas::EXTENT));
        const usize top = static_cast<usize>(cached.Uv.Y * static_cast<float32>(text::CoverageAtlas::EXTENT));
        const uint8* page = source.Coverage();
        for (usize row = 0; row < cached.Height; ++row)
        {
            for (usize column = 0; column < cached.Width; ++column)
            {
                inoutBytes[row * cached.Width + column] =
                    page[(top + row) * text::CoverageAtlas::EXTENT + left + column];
            }
        }
    }
    return inoutDestination.Insert(key,
                                   byteCount != 0 ? inoutBytes.GetData() : nullptr,
                                   byteCount,
                                   cached.Width,
                                   cached.Height,
                                   cached.BearingLeft,
                                   cached.BearingTop,
                                   outGlyph);
}
} // namespace

struct LabelCache::State final
{
    explicit State(ludus::text::FontSystem* system) noexcept : System(system) {}

    ludus::text::FontSystem* System;
    core::StaticArray<Slot, MAX_LABELS> Slots;
    core::StaticArray<Label, MAX_LABELS> Labels;
    usize Count = 0;
    /// Compaction UVs per slot glyph, committed only when the whole compaction succeeds.
    core::StaticArray<core::StaticArray<math::Vector4, MAX_LABEL_GLYPHS>, MAX_LABELS> StagedUvs;
    core::StaticArray<text::TextQuad, MAX_LABEL_GLYPHS> Scratch;
    core::StaticArray<uint8, ATLAS_TEXELS> Bytes;
    usize Live = 0;
    uint64 Revision = 0;
    /// Live glyph cells when the revision last advanced.
    usize RevisionGlyphs = 0;

    /// Index of the element's slot, or Count when it has none.
    [[nodiscard]] usize Find(ludus::ui::WidgetId element) const noexcept
    {
        for (usize index = 0; index < Count; ++index)
        {
            if (Slots[index].Placement.Element == element)
            {
                return index;
            }
        }
        return Count;
    }

    void RefreshLabels() noexcept
    {
        for (usize index = 0; index < Count; ++index)
        {
            Labels[index] = Slots[index].Placement;
            Labels[index].Glyphs = {Slots[index].Glyphs.GetData(), Slots[index].Count};
        }
    }
};

LabelCache::LabelCache() noexcept = default;
LabelCache::~LabelCache() = default;

rhi::RasterStatus LabelCache::Initialize(ludus::text::FontSystem* system) noexcept
{
    if (system == nullptr)
    {
        return Status::InvalidDescription;
    }
    if (mState.Get() != nullptr)
    {
        return mState->System == system ? Status::Ready : Status::InvalidDescription;
    }
    for (text::CoverageAtlas& atlas : mAtlases)
    {
        const Status initialized = atlas.Initialize();
        if (initialized != Status::Ready)
        {
            return initialized;
        }
    }
    mState.Reset(new (std::nothrow) State(system));
    return mState.Get() == nullptr ? Status::OutOfMemory : Status::Ready;
}

rhi::RasterStatus LabelCache::SetLabel(const LabelSource& source, LabelUpdate& outUpdate) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (IsValidSource(source) == false)
    {
        return Status::InvalidDescription;
    }
    State& state = *mState;
    ludus::text::LayoutView view;
    const ludus::text::Status viewed = ludus::text::GetLayout(state.System, source.Layout, &view);
    if (viewed != ludus::text::Status::Ok)
    {
        return viewed == ludus::text::Status::InvalidHandle ? Status::InvalidHandle : Status::InvalidDescription;
    }
    const usize target = state.Find(source.Element);
    LabelUpdate update;
    update.Labels = state.Count;
    update.RequestedGlyphs = view.Glyphs.size();
    text::CoverageAtlas& live = mAtlases[state.Live];
    update.AtlasGlyphs = live.GlyphCount();
    update.OccupiedTexels = live.OccupiedTexels();
    if (target == state.Count && state.Count == MAX_LABELS)
    {
        update.Repair = LabelRepair::RemoveLabel;
        outUpdate = update;
        return Status::CapacityExceeded;
    }
    if (view.Glyphs.size() > MAX_LABEL_GLYPHS)
    {
        update.Repair = LabelRepair::ShortenLabel;
        outUpdate = update;
        return Status::CapacityExceeded;
    }
    usize count = 0;
    Status prepared = text::PrepareText(live,
                                        state.System,
                                        source.Layout,
                                        source.FontVersion,
                                        state.Scratch.GetData(),
                                        MAX_LABEL_GLYPHS,
                                        count);
    usize active = state.Live;
    if (prepared == Status::CapacityExceeded)
    {
        // Compact: rebuild only the cells live labels address, then retry the run, all in staging.
        const usize staging = 1 - state.Live;
        text::CoverageAtlas& rebuilt = mAtlases[staging];
        rebuilt.Reset();
        for (usize index = 0; index < state.Count; ++index)
        {
            if (index == target)
            {
                continue;
            }
            const Slot& slot = state.Slots[index];
            for (usize glyph = 0; glyph < slot.Count; ++glyph)
            {
                text::AtlasGlyph copied;
                const Status status = CopyGlyph(live, slot.Keys[glyph], state.Bytes, rebuilt, copied);
                if (status == Status::CapacityExceeded)
                {
                    update.Repair = LabelRepair::ReduceGlyphs;
                    update.AtlasGlyphs = rebuilt.GlyphCount();
                    update.OccupiedTexels = rebuilt.OccupiedTexels();
                    outUpdate = update;
                    return status;
                }
                if (status != Status::Ready)
                {
                    return status;
                }
                state.StagedUvs[index][glyph] = copied.Uv;
            }
        }
        prepared = text::PrepareText(rebuilt,
                                     state.System,
                                     source.Layout,
                                     source.FontVersion,
                                     state.Scratch.GetData(),
                                     MAX_LABEL_GLYPHS,
                                     count);
        if (prepared == Status::CapacityExceeded)
        {
            update.Repair = LabelRepair::ReduceGlyphs;
            update.AtlasGlyphs = rebuilt.GlyphCount();
            update.OccupiedTexels = rebuilt.OccupiedTexels();
            outUpdate = update;
            return prepared;
        }
        if (prepared != Status::Ready)
        {
            return prepared;
        }
        for (usize index = 0; index < state.Count; ++index)
        {
            if (index == target)
            {
                continue;
            }
            Slot& slot = state.Slots[index];
            for (usize glyph = 0; glyph < slot.Count; ++glyph)
            {
                slot.Glyphs[glyph].Uv = state.StagedUvs[index][glyph];
            }
        }
        active = staging;
        update.HasCompacted = true;
    }
    if (prepared != Status::Ready)
    {
        return prepared;
    }
    // Commit: PrepareText wrote one placement per shaped glyph, in layout order.
    const usize slotIndex = target == state.Count ? state.Count++ : target;
    Slot& slot = state.Slots[slotIndex];
    for (usize glyph = 0; glyph < count; ++glyph)
    {
        slot.Glyphs[glyph] = state.Scratch[glyph];
        slot.Keys[glyph] = {source.FontVersion, view.Glyphs[glyph].GlyphId, view.PixelHeight};
    }
    slot.Count = count;
    slot.Placement.Element = source.Element;
    slot.Placement.OffsetX = source.OffsetX;
    slot.Placement.BaselineY = source.BaselineY;
    slot.Placement.Color = source.Color;
    slot.Placement.Scale = source.Scale;
    state.Live = active;
    state.RefreshLabels();
    const text::CoverageAtlas& current = mAtlases[state.Live];
    if (update.HasCompacted == true || current.GlyphCount() != state.RevisionGlyphs)
    {
        ++state.Revision;
        state.RevisionGlyphs = current.GlyphCount();
    }
    update.Labels = state.Count;
    update.AtlasGlyphs = current.GlyphCount();
    update.OccupiedTexels = current.OccupiedTexels();
    outUpdate = update;
    return Status::Ready;
}

bool LabelCache::RemoveLabel(ludus::ui::WidgetId element) noexcept
{
    if (mState.Get() == nullptr)
    {
        return false;
    }
    State& state = *mState;
    const usize index = state.Find(element);
    if (index == state.Count)
    {
        return false;
    }
    --state.Count;
    if (index != state.Count)
    {
        state.Slots[index] = state.Slots[state.Count];
    }
    state.RefreshLabels();
    return true;
}

std::span<const Label> LabelCache::GetLabels() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    return {mState->Labels.GetData(), mState->Count};
}

const text::CoverageAtlas& LabelCache::GetAtlas() const noexcept
{
    return mAtlases[mState.Get() == nullptr ? 0 : mState->Live];
}

uint64 LabelCache::GetAtlasRevision() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->Revision;
}
} // namespace ludus::graphics::ui_text
