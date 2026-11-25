#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/text/coverage_atlas.hpp>
#include <ludus/graphics/ui_text/ui_text.hpp>
#include <ludus/text/font_system.h>
#include <ludus/ui/context.h>

#include <span>

/// @file
/// Native-only bounded owner of Ui labels and the coverage atlas they sample. Shaping and
/// rasterization go through GraphicsText `PrepareText`, which the browser build does not provide.
/// Contract and acceptance: the "U1 text adapter" section of docs/architecture/ui.md.

namespace ludus::graphics::ui_text
{
/// The change a caller makes after LabelCache::SetLabel reports CapacityExceeded.
enum class LabelRepair : foundation::uint8
{
    /// No capacity repair applies: success, or a failure with another cause.
    None,
    /// Every label slot holds another element's label; remove one with RemoveLabel first.
    RemoveLabel,
    /// The run has more glyphs than one label holds (MAX_LABEL_GLYPHS); shorten it or split it
    /// across several elements.
    ShortenLabel,
    /// Even after compaction the live labels plus this run need more glyph cells or texels than the
    /// atlas holds; remove labels, use fewer distinct pixel sizes or shorten the text.
    ReduceGlyphs,
};

/// Counters and repair for one LabelCache::SetLabel call, written on Ready and CapacityExceeded.
struct LabelUpdate final
{
    /// The atlas was rebuilt with only the glyphs live labels use to make room. Every label's UVs
    /// changed and the revision advanced; publish the atlas again before drawing the new labels.
    bool HasCompacted = false;
    /// What to change when the call failed for capacity.
    LabelRepair Repair = LabelRepair::None;
    /// Labels held after the call.
    foundation::usize Labels = 0;
    /// Glyphs in the requested run.
    foundation::usize RequestedGlyphs = 0;
    /// Sized glyph cells in the live atlas after the call; on a ReduceGlyphs failure, the cells the
    /// compaction attempt had placed when the atlas ran out.
    foundation::usize AtlasGlyphs = 0;
    /// Texels the corresponding atlas occupies, including filtering gutters.
    foundation::usize OccupiedTexels = 0;
};

/// The shaped run and placement for one element's label.
struct LabelSource final
{
    /// Element the label is drawn on; nonzero. Setting it again replaces that element's label.
    ludus::ui::WidgetId Element = 0;
    /// Live shaped run in the cache's FontSystem, shaped at the physical pixel height for Scale.
    /// Borrowed only during SetLabel; the cache copies glyph placements and atlas keys.
    ludus::text::LayoutId Layout;
    /// Nonzero font-content incarnation, as for `text::PrepareText`; change it after a font reload.
    foundation::uint64 FontVersion = 0;
    /// Physical pixels per logical pixel the run was shaped for; finite and positive.
    foundation::float32 Scale = 1;
    /// Logical pen origin from the element's layout box left edge; finite.
    foundation::float32 OffsetX = 0;
    /// Logical baseline from the element's layout box top edge; finite.
    foundation::float32 BaselineY = 0;
    /// Straight-alpha linear RGB text color, channels in [0, 1].
    ludus::ui::Color Color{1, 1, 1, 1};
};

/// Bounded CPU owner of at most MAX_LABELS element labels and the coverage atlas they sample.
///
/// Single owner thread. Initialize reserves everything; SetLabel and RemoveLabel never allocate.
/// The cache holds two CPU atlases: the live one that labels address and a staging one for
/// compaction. When the live atlas is full, SetLabel rebuilds the glyphs that live labels use into
/// the staging atlas, copying their coverage without rasterizing again, adds the new run and swaps
/// the atlases. Compaction loses no authored data, so it is applied automatically. If the run still
/// does not fit, SetLabel returns CapacityExceeded with a LabelRepair and leaves every label,
/// including the element's last ready label, and the live atlas's existing cells unchanged.
///
/// The cache holds no GPU objects and survives device loss: publish GetAtlas() to the new device
/// and rebuild overlay lists from GetLabels() without shaping or rasterizing again.
class LabelCache final
{
public:
    /// Maximum labels, one per element.
    static constexpr foundation::usize MAX_LABELS = 32;
    /// Maximum glyphs in one label.
    static constexpr foundation::usize MAX_LABEL_GLYPHS = 64;
    /// Construct without allocation.
    LabelCache() noexcept;
    /// Release CPU storage; published atlas textures are independent RHI resources.
    ~LabelCache();
    /// Not copyable: labels borrow storage the cache owns.
    LabelCache(const LabelCache&) = delete;
    /// Not copyable: labels borrow storage the cache owns.
    LabelCache& operator=(const LabelCache&) = delete;
    /// Not movable: GetLabels() and GetAtlas() results borrow the cache in place.
    LabelCache(LabelCache&&) = delete;
    /// Not movable: GetLabels() and GetAtlas() results borrow the cache in place.
    LabelCache& operator=(LabelCache&&) = delete;
    /// Reserve label storage and both CPU atlases once.
    /// @param system Borrowed live native font owner that shapes every LabelSource::Layout; it must
    /// outlive each SetLabel call.
    /// @return Ready, also when already initialized with the same system; InvalidDescription for a
    /// null system or a different system after initialization; OutOfMemory when reservation fails.
    [[nodiscard]] rhi::RasterStatus Initialize(ludus::text::FontSystem* system) noexcept;
    /// Copy a shaped run into the element's label, rasterizing glyphs the atlas does not hold.
    /// @param source Run and placement; see LabelSource for validity.
    /// @param outUpdate Receives counters on Ready and counters plus a repair on CapacityExceeded;
    /// unchanged for other failures.
    /// @return Ready; NotReady before Initialize; InvalidDescription for an invalid source;
    /// InvalidHandle for a stale layout; CapacityExceeded with LabelRepair when the label, its slot
    /// or the atlas cannot hold the run; other GraphicsText `PrepareText` failures. Every failure
    /// keeps all labels and the live atlas cells that labels address.
    [[nodiscard]] rhi::RasterStatus SetLabel(const LabelSource& source, LabelUpdate& outUpdate) noexcept;
    /// Remove the element's label; its atlas cells are reclaimed by a later compaction.
    /// @return true when the element had a label.
    [[nodiscard]] bool RemoveLabel(ludus::ui::WidgetId element) noexcept;
    /// Borrow the current labels for AppendUi, in no particular order. Valid until the next
    /// SetLabel, RemoveLabel or destruction; empty before Initialize.
    [[nodiscard]] std::span<const Label> GetLabels() const noexcept;
    /// The live atlas that GetLabels() glyph UVs address. Publish it again whenever
    /// GetAtlasRevision() changes, and after device loss.
    [[nodiscard]] const text::CoverageAtlas& GetAtlas() const noexcept;
    /// Advances whenever labels may address atlas cells absent from a previous revision's
    /// coverage: on new glyph cells and on compaction. Starts at zero.
    [[nodiscard]] foundation::uint64 GetAtlasRevision() const noexcept;

private:
    struct State;
    /// One live atlas plus one compaction target.
    static constexpr foundation::usize ATLAS_COUNT = 2;
    foundation::UniquePtr<State> mState;
    /// Live and staging atlases; mState names which one is live.
    foundation::core::StaticArray<text::CoverageAtlas, ATLAS_COUNT> mAtlases;
};
} // namespace ludus::graphics::ui_text
