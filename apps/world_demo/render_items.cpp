#include "internal/render_items.h"
#include "internal/camera_view.h"
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
namespace ludus::world_demo
{
namespace
{
namespace math = foundation::math;
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
/// Largest camera distance from the render origin the view accepts, in world units.
constexpr float64 MAX_RELATIVE_CAMERA_DISTANCE = 1.0e6;
} // namespace

rr::MeshDescription GetUnitQuadMesh() noexcept
{
    rr::MeshDescription description;
    description.Positions = UNIT_QUAD_POSITIONS;
    description.VertexCount = UNIT_QUAD_VERTEX_COUNT;
    description.Indices = UNIT_QUAD_INDICES;
    description.IndexCount = UNIT_QUAD_INDEX_COUNT;
    return description;
}

rhi::RasterStatus TryBuildDrawTransform(const Draw& draw, usize order, math::Affine3& outTransform) noexcept
{
    const math::Vector2 position{draw.Position.X, draw.Position.Y};
    const math::Vector2 halfExtent{draw.HalfExtent.X, draw.HalfExtent.Y};
    if (order >= MAX_RENDER_DRAWS || math::IsFinite(position) == false || math::IsFinite(halfExtent) == false ||
        math::IsFinite(draw.Rotation) == false || halfExtent.X <= 0 || halfExtent.Y <= 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    // The reference renderer tested a point against the box after rotating it by -angle, so
    // the box itself turns counter-clockwise by +angle. Scale first, then rotate.
    const float32 cosine = math::Cos(draw.Rotation);
    const float32 sine = math::Sin(draw.Rotation);
    math::Affine3 transform;
    transform.Columns[0] = {cosine * halfExtent.X, sine * halfExtent.X, 0};
    transform.Columns[1] = {-sine * halfExtent.Y, cosine * halfExtent.Y, 0};
    transform.Translation = {position.X, position.Y, static_cast<float32>(order) * DRAW_ORDER_DEPTH_STEP};
    outTransform = transform;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus
TryBuildFrameItems(const RenderFrame& frame, rr::Mesh quad, FrameItems& outItems, usize& outCount) noexcept
{
    if (frame.Count > MAX_RENDER_DRAWS)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    // Validate everything before writing, so a rejected frame leaves the previous items intact.
    for (usize index = 0; index < frame.Count; ++index)
    {
        const Draw& draw = frame.Draws[index];
        math::Affine3 transform;
        if (TryBuildDrawTransform(draw, index, transform) != rhi::RasterStatus::Ready || draw.Color[RGBA_ALPHA] != 1)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
        {
            if (math::IsFinite(draw.Color[channel]) == false || draw.Color[channel] < 0 || draw.Color[channel] > 1)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
        }
    }
    for (usize index = 0; index < frame.Count; ++index)
    {
        const Draw& draw = frame.Draws[index];
        rr::SceneItem& item = outItems[index];
        item = rr::SceneItem{};
        item.Geometry = quad;
        (void)TryBuildDrawTransform(draw, index, item.Transform);
        item.Material.Color = {draw.Color[0], draw.Color[1], draw.Color[2], draw.Color[RGBA_ALPHA]};
        item.SourceId = static_cast<uint64>(index) + 1;
        item.Pass = rr::Layer::Opaque;
    }
    outCount = frame.Count;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus
TryBuildFrameView(const RenderFrame& frame, uint32 width, uint32 height, rr::ViewDescription& outView) noexcept
{
    if (width == 0 || height == 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (frame.Camera.Lens.Projection != gameplay::camera::CameraProjection::OrthographicFinite)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    CameraRenderView view;
    if (TryBuildCameraView(frame.Camera,
                           {
                               .Width = width,
                               .Height = height,
                           },
                           {},
                           MAX_RELATIVE_CAMERA_DISTANCE,
                           view) != gameplay::camera::CameraStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const math::Vector3d& eye = frame.Camera.Pose.Position;
    rr::ViewDescription candidate;
    candidate.WorldToClip = view.ViewProjection;
    candidate.Depth = rr::DepthConvention::ReverseZ;
    candidate.OrthographicCamera = true;
    candidate.CameraForward = math::Rotate(frame.Camera.Pose.Orientation, math::Vector3{0, 0, -1});
    // TryBuildCameraView bounded the eye by MAX_RELATIVE_CAMERA_DISTANCE, so it narrows exactly enough.
    candidate.CameraPosition = {static_cast<float32>(eye.X), static_cast<float32>(eye.Y), static_cast<float32>(eye.Z)};
    outView = candidate;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::world_demo
