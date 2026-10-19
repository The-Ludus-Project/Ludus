#pragma once
// World-demo frames expressed as engine Renderer scene items and a view.
//
// The runtime and the Editor's World viewport both draw a RenderFrame through
// these functions, so edit and play show the same geometry from the same
// extraction. See docs/architecture/editor-gui-systems.md#world-demo-on-the-engine-renderer.
#include "world.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/raster.h>
namespace ludus::world_demo
{
/// Channels of a linear RGBA colour, as a Draw stores it.
inline constexpr usize RGBA_CHANNELS = DRAW_COLOR_CHANNELS;
/// Index of the alpha channel in a linear RGBA colour.
inline constexpr usize RGBA_ALPHA = 3;
/// World units in front of the z = 0 play plane that hold every draw's depth key.
///
/// Draw order becomes depth so later draws cover earlier ones without blending.
/// The fixed camera stands at z = 1, so the band stays between the play plane and the camera.
inline constexpr foundation::float32 DRAW_ORDER_DEPTH_SPAN = 0.5F;
/// World-unit depth step between consecutive draws.
inline constexpr foundation::float32 DRAW_ORDER_DEPTH_STEP =
    DRAW_ORDER_DEPTH_SPAN / static_cast<foundation::float32>(MAX_RENDER_DRAWS);
/// Corners of the unit quad, counter-clockwise when viewed from +Z.
inline constexpr usize UNIT_QUAD_VERTEX_COUNT = 4;
/// Two triangles covering the unit quad.
inline constexpr usize UNIT_QUAD_INDEX_COUNT = 6;
/// Mesh-local unit quad positions in [-1, 1]; a draw's transform scales it by its half extent.
inline constexpr foundation::math::Vector3 UNIT_QUAD_POSITIONS[UNIT_QUAD_VERTEX_COUNT]{
    {-1, -1, 0},
    {1, -1, 0},
    {1, 1, 0},
    {-1, 1, 0},
};
/// Triangle list over UNIT_QUAD_POSITIONS.
inline constexpr uint32 UNIT_QUAD_INDICES[UNIT_QUAD_INDEX_COUNT] = {0, 1, 2, 0, 2, 3};
/// Scene items for one frame; capacity matches the extraction's draw bound.
using FrameItems = foundation::StaticArray<graphics::renderer::SceneItem, MAX_RENDER_DRAWS>;

/// Describe the shared unit quad for Renderer::CreateMesh.
/// @return A description borrowing the static UNIT_QUAD_* arrays, which never expire.
[[nodiscard]] graphics::renderer::MeshDescription GetUnitQuadMesh() noexcept;
/// Place the unit quad as one draw: scaled by its half extent, rotated counter-clockwise by
/// its angle in radians, centred on its position, and lifted by its draw-order depth key.
/// @param draw Extracted draw with finite, positive half extents.
/// @param order Zero-based position in the frame; later draws are nearer the camera.
/// @param outTransform Local-to-world transform, written only on Ready.
/// @return Ready, or InvalidDescription for nonfinite input, nonpositive extents or an order
/// at or beyond MAX_RENDER_DRAWS.
[[nodiscard]] graphics::rhi::RasterStatus
TryBuildDrawTransform(const Draw& draw, usize order, foundation::math::Affine3& outTransform) noexcept;
/// Convert every draw of a frame into opaque flat-material items over one quad mesh.
/// @param frame Extraction to convert; its draws must be opaque (alpha one).
/// @param quad Ready mesh created from GetUnitQuadMesh, borrowed into each item.
/// @param outItems Items in draw order, with source identities 1..count; written only on Ready.
/// @param outCount Number of meaningful items in outItems, written only on Ready.
/// @return Ready, or InvalidDescription for a count above MAX_RENDER_DRAWS, a translucent or
/// out-of-range colour, or a draw TryBuildDrawTransform rejects. Failure preserves both outputs.
[[nodiscard]] graphics::rhi::RasterStatus TryBuildFrameItems(const RenderFrame& frame,
                                                             graphics::renderer::Mesh quad,
                                                             FrameItems& outItems,
                                                             usize& outCount) noexcept;
/// Build the reverse-Z orthographic view of a frame's gameplay camera for a drawable.
/// Uses the game's checked camera_view builder, so the visible span follows the lens and
/// level minimum width exactly as the simulation's camera evaluates it.
/// @param frame Extraction whose camera sample is drawn.
/// @param width Drawable width in physical pixels.
/// @param height Drawable height in physical pixels.
/// @param outView View description, written only on Ready. Prepare it with the Renderer's
/// L1 views profile ready, and draw it with DrawView and a zero depth clear.
/// @return Ready, NotReady for a zero extent, or InvalidDescription for a camera sample the
/// builder rejects or a non-orthographic lens. Failure preserves outView.
[[nodiscard]] graphics::rhi::RasterStatus TryBuildFrameView(const RenderFrame& frame,
                                                            uint32 width,
                                                            uint32 height,
                                                            graphics::renderer::ViewDescription& outView) noexcept;
} // namespace ludus::world_demo
