#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// Depth tests for debug geometry; every variant preserves scene depth.
enum class OverlayDepth : foundation::uint8
{
    /// Display overlay with no depth test.
    Always,
    /// Conventional [0,1] depth, accepting equality.
    Conventional,
    /// Reverse-Z [0,1] depth, accepting equality.
    ReverseZ,
};
/// One generic overlay vertex, independent of fonts and native GPU objects.
struct OverlayVertex final
{
    /// Homogeneous canonical clip position; W must be positive, Z in [0,W].
    foundation::math::Vector4 Clip;
    /// Top-left normalized atlas UV; Z is 0 for solid geometry or 1 for coverage.
    foundation::math::Vector4 Coverage;
    /// Straight linear display RGBA in [0,1]; shader premultiplies after coverage.
    foundation::math::Vector4 Color{1, 1, 1, 1};
};
/// Compatibility state for one painter-ordered command.
struct OverlayStyle final
{
    /// Depth behavior; never writes depth.
    OverlayDepth Depth = OverlayDepth::Always;
    /// Half-open physical scissor, used only when Clip is true.
    rhi::RasterRectangle Scissor;
    /// Enable explicit scissor, including an empty rectangle.
    bool Clip = false;
};
/// Whether OverlayList extends the run that ends with `previous` when it appends a command styled `next`.
///
/// Runs merge when depth behavior and scissor enablement agree and, for scissored commands, the
/// scissor rectangles are equal. Coverage sampling and color never split a run, so solid quads and
/// glyph coverage quads share one draw. Adapters use this to count draws before appending.
/// @param previous Style of the list's last run.
/// @param next Style of the command being appended.
/// @return true when no new run (draw) is opened.
[[nodiscard]] bool CanMergeOverlayStyles(const OverlayStyle& previous, const OverlayStyle& next) noexcept;
/// Borrowed command range in an OverlayList; order is significant.
struct OverlayCommand final
{
    /// First vertex in the list.
    foundation::uint32 First = 0;
    /// Number of triangle-list vertices.
    foundation::uint32 Count = 0;
    /// Copied compatibility state.
    OverlayStyle Style;
};
/// Reusable bounded CPU triangle arena. Single owner thread; no append allocations.
/// Initialize reserves storage once; Clear resets counts, never capacity. Commands
/// merge only when adjacent and their depth/scissor state agrees. Failure preserves
/// all geometry; capacity failures increment DroppedCommands. See renderer-systems.md.
class OverlayList final
{
public:
    /// Maximum vertices (512 independent quads) per prepared version.
    static constexpr foundation::usize MAX_VERTICES = 3072;
    /// Maximum adjacent compatibility runs per prepared version.
    static constexpr foundation::usize MAX_COMMANDS = 256;
    /// Construct without allocation; Initialize before use.
    OverlayList() noexcept;
    /// Release CPU storage; GPU prepared copies are independent.
    ~OverlayList();
    /// Allocate the reusable arena. Ready also means already initialized.
    [[nodiscard]] rhi::RasterStatus Initialize() noexcept;
    /// Empty the list and diagnostics without allocating.
    void Clear() noexcept;
    /// Append complete triangles. Copies vertices; count must be divisible by three.
    /// Invalid/nonfinite colors, clip positions, UVs or styles preserve the list.
    [[nodiscard]] rhi::RasterStatus
    Append(const OverlayVertex* vertices, foundation::usize count, const OverlayStyle& style = {}) noexcept;
    /// Append a physical-pixel quad, top-left coordinates and top-left UVs.
    /// Extent must be positive, rectangle and UV edges finite and ordered. Zero
    /// area succeeds without geometry. Solid ignores UV sampling; coverage uses R8.
    [[nodiscard]] rhi::RasterStatus Quad(foundation::math::Vector4 rectangle,
                                         foundation::math::Vector4 uv,
                                         foundation::math::Vector4 color,
                                         foundation::uint32 width,
                                         foundation::uint32 height,
                                         bool coverage = false,
                                         const OverlayStyle& style = {}) noexcept;
    /// Expand homogeneous endpoints into a butt-cap physical-pixel line (minimum
    /// width one). Clips W >= 1e-6 and canonical near/far planes before division.
    /// Zero projected length becomes a square; fully clipped lines produce no draw.
    /// Width <= 0, nonfinite data or zero viewport extent are invalid.
    [[nodiscard]] rhi::RasterStatus Line(foundation::math::Vector4 start,
                                         foundation::math::Vector4 end,
                                         foundation::math::Vector4 color,
                                         foundation::float32 physicalWidth,
                                         foundation::uint32 width,
                                         foundation::uint32 height,
                                         const OverlayStyle& style = {}) noexcept;
    /// Borrow contiguous vertices until Clear, Append, or destruction.
    [[nodiscard]] const OverlayVertex* Vertices() const noexcept;
    /// Active vertex count; zero before initialization.
    [[nodiscard]] foundation::usize VertexCount() const noexcept;
    /// Borrow painter-ordered runs until list mutation/destruction.
    [[nodiscard]] const OverlayCommand* Commands() const noexcept;
    /// Active command count.
    [[nodiscard]] foundation::usize CommandCount() const noexcept;
    /// Complete append operations rejected for capacity since Clear.
    [[nodiscard]] foundation::uint64 DroppedCommands() const noexcept;

private:
    struct State;
    foundation::UniquePtr<State> mState;
};
namespace internal
{
/// Private identity access for independently retained overlay versions.
struct OverlayAccess;
} // namespace internal
/// Owned immutable prepared overlay; generation-checked and scoped to one owner.
/// Release once between frames; accepted GPU work retains exact resources.
struct PreparedOverlay final
{
private:
    foundation::uint64 Owner = 0, Generation = 0;
    foundation::uint32 Slot = 0;
    /// Grants private access to incarnation identities.
    friend struct internal::OverlayAccess;
};
/// Renderer composition adapter for generic solid and coverage triangle lists.
/// Single RHI owner thread. Four owned versions and shared RHI transfer/resource
/// quotas impose backpressure; no waits, reshaping or allocation while drawing.
/// Shader source is installed as renderer_overlay.lusl. Reset between frames;
/// device loss invalidates GPU versions and requires explicit Initialize/rebuild.
class OverlayRenderer final
{
public:
    /// Construct without allocation or device access.
    OverlayRenderer() noexcept;
    /// Release logical GPU ownership; RHI retires accepted uses.
    ~OverlayRenderer();
    /// Create fixed pipelines, sampler and sequential index buffer between frames.
    /// Ready/Pending establishes ownership; failure requires Reset before retry.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::RasterShaderDescription& vertex,
                                               const rhi::RasterShaderDescription& fragment) noexcept;
    /// Advance asynchronous setup/transfer validation between frames, without waits.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Drop prepared versions and fixed resources, between frames.
    void Reset() noexcept;
    /// Copy a list and schedule a complete vertex-buffer upload before return.
    /// atlas is a ready immutable sampled R8 texture; null selects solid-only white.
    /// Captures its exact view/binding version. Null output required; Pending owns
    /// a ticket, caller may immediately Clear/reuse the CPU list. Failures preserve
    /// output. Retained earlier versions are never overwritten or evicted.
    [[nodiscard]] rhi::RasterStatus
    Prepare(const OverlayList& list, rhi::TextureHandle atlas, PreparedOverlay& output) noexcept;
    /// Poll uploaded bytes and build the immutable binding between frames.
    [[nodiscard]] rhi::RasterStatus GetStatus(PreparedOverlay overlay) noexcept;
    /// Compose into an already initialized acquired surface or RGBA8 linear target.
    /// All passes Load/Store color; no clear occurs. Depth-tested commands Load/Store
    /// retained scene depth read-only. Intersects command scissor with pass scissor.
    /// Draws in painter order; failures after encoding can leave partial frame work.
    /// GetStatus must have returned Ready before opening the frame.
    [[nodiscard]] rhi::RasterStatus Draw(PreparedOverlay overlay,
                                         const rhi::RasterPassDescription& description) noexcept;
    /// Clear owned identity; submitted resources retire only after RHI completion.
    [[nodiscard]] rhi::RasterStatus Release(PreparedOverlay& overlay) noexcept;

private:
    struct State;
    foundation::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
