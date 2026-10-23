#pragma once

// View inputs and per-view results of the Renderer, split from renderer.hpp so
// code that only stores or forwards a view does not parse the whole Renderer.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/world_grid.hpp>

namespace ludus::graphics::renderer
{
/// Explicit pairing of projection matrices, clear values and comparisons.
enum class DepthConvention : ludus::foundation::uint8
{
    /// R1 compatibility: near zero, far one, clear one, strict Less.
    Conventional,
    /// Near one, far zero, clear zero, strict Greater.
    ReverseZ,
};
/// Finite canonical clip transform: XY [-1,1], Z [0,1], Y up.
struct ViewDescription final
{
    /// Finite world-to-clip matrix matching the explicit Depth convention.
    ludus::foundation::math::Matrix4 WorldToClip = ludus::foundation::math::Matrix4::Identity();
    /// Finite nonnegative world-unit margin for conservative opaque AABB culling.
    ludus::foundation::float32 CullMargin = 0;
    /// Keep source order and one draw per object, with visibility disabled, for image comparison.
    bool Reference = false;
    /// L5 diagnostic: evaluate all admitted lights while retaining ordinary culling/batching.
    bool DirectLighting = false;
    /// Opt-in screen-size LOD; Reference bypasses LOD, ShadowCasters always uses full geometry.
    bool EnableLod = false;
    /// Physical viewport dimensions used only for LOD; both positive when EnableLod is true.
    ludus::foundation::uint32 LodWidth = 0;
    /// Physical viewport height for LOD, positive when EnableLod is true.
    ludus::foundation::uint32 LodHeight = 0;
    /// Fractional transition band [0,0.5]; previous selection comes from SceneItem::PreviousLod.
    ludus::foundation::float32 LodHysteresis = 0.1F;
    /// Schedule a bounded R2 upload version instead of immediate immutable creation.
    /// PrepareView then returns Pending; poll GetStatus between frames before drawing.
    /// Caller snapshots/arrays may be released immediately; accepted GPU versions never change.
    bool ScheduledUpload = false;
    /// Defaults preserve existing L0 projection behavior; reverse-Z requires InitializeViews.
    DepthConvention Depth = DepthConvention::Conventional;
    /// Select five-plane culling for an infinite reverse-Z perspective projection.
    bool InfiniteFar = false;
    /// Finite world-space camera position in meters, [-1000000,1000000]; required for lit shading.
    ludus::foundation::math::Vector3 CameraPosition{0, 0, 1};
    /// Prepare an opaque/masked caster view; skips overlays and disables camera visibility shortcuts.
    /// ShadowAtlas preparation sets this; ordinary callers should keep false.
    bool ShadowCasters = false;
    /// Use constant CameraForward for orthographic specular evaluation; perspective uses CameraPosition.
    bool OrthographicCamera = false;
    /// Nonzero finite world direction from camera into scene, normalized during preparation.
    ludus::foundation::math::Vector3 CameraForward{0, 0, -1};
    /// Canonical overlay crop transform, applied independently of the world camera.
    ludus::foundation::math::Matrix4 OverlayToClip = ludus::foundation::math::Matrix4::Identity();
    /// World-unit camera distances {near, far} that MaterialDebugView::LinearDepth maps to black
    /// and white. Perspective views measure radial distance from CameraPosition; orthographic
    /// views measure distance along CameraForward. Finite, 0 <= near < far <= 1e7, or {0, 0}
    /// (the default) to leave depth unmapped (black). Ignored by ordinary lit programs.
    ludus::foundation::math::Vector2 DebugDepthRange{0, 0};
    /// Optional depth-tested world coordinate guide. It does not alter scene bounds, item counts or shadows.
    /// Ignored by reference/path tracing captures; raster drawing retains guide buffers with this view.
    WorldGridDescription WorldGrid{};
};
/// Packet counts and a diagnostic prefix, copied to caller storage; IDs follow draw/instance order.
struct ViewReport final
{
    /// Number of submitted snapshot objects.
    ludus::foundation::uint32 Submitted = 0;
    /// Retained visible objects, including overlays.
    ludus::foundation::uint32 Visible = 0;
    /// Objects conservatively rejected by the CPU clip/frustum tests.
    ludus::foundation::uint32 Culled = 0;
    /// Indexed draws after adjacent compatible instancing.
    ludus::foundation::uint32 Draws = 0;
    /// Visible lit instances using environment because an authored bake key/transform mismatched.
    ludus::foundation::uint32 BakeFallbacks = 0;
    /// Visible lit instances using unshadowed direct light after conservative caster invalidation.
    ludus::foundation::uint32 ShadowFallbacks = 0;
    /// Number of conservative 16-object groups rejected before individual AABB tests.
    ludus::foundation::uint32 CulledGroups = 0;
    /// Number of individual AABB tests; fully inside/rejected groups avoid them.
    ludus::foundation::uint32 BoundsTests = 0;
    /// Number of visible objects selecting coarse detail.
    ludus::foundation::uint32 LodObjects = 0;
    /// First Reported coarse selections; feed back by source identity for hysteresis.
    bool SelectedLods[256]{};
    /// First Reported source identities in actual per-instance order.
    ludus::foundation::uint64 SourceIds[256]{};
    /// Number of meaningful entries in the inline arrays, min(Visible, 256).
    ludus::foundation::uint32 Reported = 0;
    /// Optional borrowed full-order output, written only after successful preparation.
    /// Null omits this output; otherwise DetailCapacity must accommodate every visible object.
    ludus::foundation::uint64* SourceIdOutput = nullptr;
    /// Optional borrowed full-order LOD output with the same capacity contract.
    bool* SelectedLodOutput = nullptr;
    /// Caller-provided element capacity for either nonnull output array. Failure preserves both arrays.
    ludus::foundation::usize DetailCapacity = 0;
};
} // namespace ludus::graphics::renderer
