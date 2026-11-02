#include "internal/software_viewport.h"
#include "internal/preview_shading.h"

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/lighting_layout.h>

#include <QVector>

#include <cmath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Ambient term of the editor preview shading, matching the native Solid mode's spirit.
constexpr float32 AMBIENT_SHADING = .3F;
/// Diffuse weight of the fixed preview light.
constexpr float32 DIFFUSE_SHADING = .7F;
/// Background colour behind the scene (the native viewport's clear colour).
constexpr QRgb BACKGROUND_COLOUR = qRgba(9, 12, 15, 255);
/// Largest 8-bit channel value.
constexpr float32 CHANNEL_MAXIMUM = 255.0F;
/// 8-bit alpha of an opaque pixel.
constexpr int32 OPAQUE_ALPHA = 255;
/// Corners of a triangle, averaged for its centroid.
constexpr float32 TRIANGLE_CORNER_COUNT = 3.0F;
/// Maximum vertices after clipping a triangle against the near and far planes.
constexpr usize MAX_CLIPPED_VERTICES = 5;
/// Depth-buffer clear value; reverse-Z stores 1 at the near plane and 0 at the far plane.
constexpr float32 DEPTH_CLEAR = -1.0F;
/// Neutral grey for the Solid preview mode.
constexpr float32 SOLID_GREY = .8F;
/// Direction towards the editor preview light (same as the native Solid preview light, negated).
constexpr math::Vector3 PREVIEW_LIGHT_DIRECTION{-.4F, .8F, .5F};

struct ClipVertex final
{
    math::Vector4 PositionClip{};
    math::Vector2 Uv{};
};
struct ScreenVertex final
{
    float32 X = 0;
    float32 Y = 0;
    float32 InverseW = 0;
    /// Reverse-Z normalized depth Z/W, linear in screen space.
    float32 Depth = 0;
    math::Vector2 UvOverW{};
};
struct SurfaceColour final
{
    math::Vector4 Tint{1, 1, 1, 1};
    math::Vector3 Emission{};
    const QImage* TextureOrNull = nullptr;
    bool IsRepeating = false;
};
struct Target final
{
    QImage* ImageOrNull = nullptr;
    QVector<float32>* DepthOrNull = nullptr;
    int32 Width = 0;
    int32 Height = 0;
};

[[nodiscard]] float32 GetEdge(const ScreenVertex& a, const ScreenVertex& b, float32 x, float32 y) noexcept
{
    return (b.X - a.X) * (y - a.Y) - (b.Y - a.Y) * (x - a.X);
}
[[nodiscard]] float32 WrapCoordinate(float32 value, bool isRepeating) noexcept
{
    if (isRepeating == false)
    {
        return math::Clamp(value, 0.0F, 1.0F);
    }
    const float32 wrapped = value - static_cast<float32>(math::Floor(static_cast<float64>(value)));
    return math::Clamp(wrapped, 0.0F, 1.0F);
}
[[nodiscard]] math::Vector4 SampleTexture(const SurfaceColour& surface, math::Vector2 uv) noexcept
{
    if (surface.TextureOrNull == nullptr || surface.TextureOrNull->isNull() == true)
    {
        return {1, 1, 1, 1};
    }
    const QImage& texture = *surface.TextureOrNull;
    const float32 u = WrapCoordinate(uv.X, surface.IsRepeating);
    const float32 v = WrapCoordinate(uv.Y, surface.IsRepeating);
    const int32 x = math::Min(static_cast<int32>(u * static_cast<float32>(texture.width())), texture.width() - 1);
    const int32 y = math::Min(static_cast<int32>(v * static_cast<float32>(texture.height())), texture.height() - 1);
    const QRgb texel = texture.pixel(x, y);
    return {static_cast<float32>(qRed(texel)) / CHANNEL_MAXIMUM,
            static_cast<float32>(qGreen(texel)) / CHANNEL_MAXIMUM,
            static_cast<float32>(qBlue(texel)) / CHANNEL_MAXIMUM,
            1};
}
[[nodiscard]] uint8 EncodeChannel(float32 value) noexcept
{
    return static_cast<uint8>(std::lround(static_cast<float64>(math::Clamp(value, 0.0F, 1.0F) * CHANNEL_MAXIMUM)));
}
/// Signed distance to a clip plane of the reverse-Z volume 0 <= Z <= W; nonnegative is inside.
[[nodiscard]] float32 GetPlaneDistance(const ClipVertex& vertex, bool isNearPlane) noexcept
{
    return isNearPlane == true ? vertex.PositionClip.W - vertex.PositionClip.Z : vertex.PositionClip.Z;
}
/// Clip a polygon against one plane (Sutherland-Hodgman); returns the new vertex count.
[[nodiscard]] usize ClipPlane(const ClipVertex (&input)[MAX_CLIPPED_VERTICES],
                              usize inputCount,
                              bool isNearPlane,
                              ClipVertex (&outVertices)[MAX_CLIPPED_VERTICES]) noexcept
{
    usize count = 0;
    for (usize i = 0; i < inputCount; ++i)
    {
        const ClipVertex& current = input[i];
        const ClipVertex& next = input[(i + 1) % inputCount];
        const float32 currentDistance = GetPlaneDistance(current, isNearPlane);
        const float32 nextDistance = GetPlaneDistance(next, isNearPlane);
        const bool isCurrentInside = currentDistance >= 0;
        const bool isNextInside = nextDistance >= 0;
        if (isCurrentInside == true && count < MAX_CLIPPED_VERTICES)
        {
            outVertices[count++] = current;
        }
        if (isCurrentInside != isNextInside && count < MAX_CLIPPED_VERTICES)
        {
            const float32 t = currentDistance / (currentDistance - nextDistance);
            outVertices[count++] =
            {
                .PositionClip = current.PositionClip + (next.PositionClip - current.PositionClip) * t,
                .Uv = current.Uv + (next.Uv - current.Uv) * t,
            };
        }
    }
    return count;
}
/// Clip a triangle to the near and far planes of the reverse-Z clip volume.
[[nodiscard]] usize ClipDepth(const ClipVertex (&triangle)[3], ClipVertex (&outVertices)[MAX_CLIPPED_VERTICES]) noexcept
{
    ClipVertex input[MAX_CLIPPED_VERTICES]{triangle[0], triangle[1], triangle[2]};
    ClipVertex nearClipped[MAX_CLIPPED_VERTICES];
    const usize nearCount = ClipPlane(input, 3, true, nearClipped);
    if (nearCount < 3)
    {
        return 0;
    }
    return ClipPlane(nearClipped, nearCount, false, outVertices);
}
[[nodiscard]] ScreenVertex ProjectVertex(const ClipVertex& vertex, int32 width, int32 height) noexcept
{
    const float32 inverseW = 1.0F / vertex.PositionClip.W;
    return
    {
        .X = (vertex.PositionClip.X * inverseW * .5F + .5F) * static_cast<float32>(width),
        .Y = (.5F - vertex.PositionClip.Y * inverseW * .5F) * static_cast<float32>(height),
        .InverseW = inverseW,
        .Depth = vertex.PositionClip.Z * inverseW,
        .UvOverW = vertex.Uv * inverseW,
    };
}
void DrawTriangle(Target& inoutTarget,
                  const ScreenVertex& a,
                  const ScreenVertex& b,
                  const ScreenVertex& c,
                  const SurfaceColour& surface,
                  float32 shading) noexcept
{
    const float32 area = GetEdge(a, b, c.X, c.Y);
    if (math::Abs(area) <= 0.0F)
    {
        return;
    }
    const int32 left = math::Max(0, static_cast<int32>(math::Min(a.X, math::Min(b.X, c.X))));
    const int32 right = math::Min(inoutTarget.Width - 1, static_cast<int32>(math::Max(a.X, math::Max(b.X, c.X))));
    const int32 top = math::Max(0, static_cast<int32>(math::Min(a.Y, math::Min(b.Y, c.Y))));
    const int32 bottom = math::Min(inoutTarget.Height - 1, static_cast<int32>(math::Max(a.Y, math::Max(b.Y, c.Y))));
    const float32 inverseArea = 1.0F / area;
    for (int32 y = top; y <= bottom; ++y)
    {
        QRgb* row = reinterpret_cast<QRgb*>(inoutTarget.ImageOrNull->scanLine(y));
        const float32 sampleY = static_cast<float32>(y) + .5F;
        for (int32 x = left; x <= right; ++x)
        {
            const float32 sampleX = static_cast<float32>(x) + .5F;
            const float32 weightA = GetEdge(b, c, sampleX, sampleY) * inverseArea;
            const float32 weightB = GetEdge(c, a, sampleX, sampleY) * inverseArea;
            const float32 weightC = 1.0F - weightA - weightB;
            if (weightA < 0 || weightB < 0 || weightC < 0)
            {
                continue;
            }
            // Reverse-Z: larger Z/W is closer, for perspective and orthographic cameras alike.
            const float32 fragmentDepth = weightA * a.Depth + weightB * b.Depth + weightC * c.Depth;
            float32& depth = (*inoutTarget.DepthOrNull)[y * inoutTarget.Width + x];
            if (fragmentDepth <= depth)
            {
                continue;
            }
            depth = fragmentDepth;
            const float32 inverseW = weightA * a.InverseW + weightB * b.InverseW + weightC * c.InverseW;
            const math::Vector2 uv = (a.UvOverW * weightA + b.UvOverW * weightB + c.UvOverW * weightC) / inverseW;
            const math::Vector4 texel = SampleTexture(surface, uv);
            const math::Vector3 colour{
                surface.Tint.X * texel.X * shading + surface.Emission.X,
                surface.Tint.Y * texel.Y * shading + surface.Emission.Y,
                surface.Tint.Z * texel.Z * shading + surface.Emission.Z,
            };
            row[x] = qRgba(EncodeChannel(colour.X), EncodeChannel(colour.Y), EncodeChannel(colour.Z), 255);
        }
    }
}
/// One-pixel depth-tested edge from a to b, as the GPU's line fill draws Wireframe; nearer or equal depth passes.
void DrawLine(Target& inoutTarget, const ScreenVertex& a, const ScreenVertex& b, QRgb colour) noexcept
{
    const float32 deltaX = b.X - a.X;
    const float32 deltaY = b.Y - a.Y;
    const float32 length = math::Max(math::Abs(deltaX), math::Abs(deltaY));
    const int32 steps = static_cast<int32>(std::ceil(length));
    for (int32 step = 0; step <= steps; ++step)
    {
        const float32 t = steps == 0 ? 0.0F : static_cast<float32>(step) / static_cast<float32>(steps);
        const int32 x = static_cast<int32>(std::floor(a.X + deltaX * t));
        const int32 y = static_cast<int32>(std::floor(a.Y + deltaY * t));
        if (x < 0 || y < 0 || x >= inoutTarget.Width || y >= inoutTarget.Height)
        {
            continue;
        }
        const float32 fragmentDepth = a.Depth + (b.Depth - a.Depth) * t;
        float32& depth = (*inoutTarget.DepthOrNull)[y * inoutTarget.Width + x];
        if (fragmentDepth < depth)
        {
            continue;
        }
        depth = fragmentDepth;
        reinterpret_cast<QRgb*>(inoutTarget.ImageOrNull->scanLine(y))[x] = colour;
    }
}
/// Wireframe edge colour shared with renderer_lit.slang, dimmed as the surface turns edge-on to the camera.
[[nodiscard]] QRgb ComputeWireframeColour(float32 facing) noexcept
{
    constexpr float32 PERCENT = 100.0F;
    const float32 edgeOn = static_cast<float32>(graphics::renderer::MATERIAL_DEBUG_WIREFRAME_EDGE_ON_PERCENT) / PERCENT;
    const float32 shade = edgeOn + (1.0F - edgeOn) * math::Clamp(facing, 0.0F, 1.0F);
    const auto scale = [shade](graphics::renderer::LightingLayoutValue channel) noexcept {
        return static_cast<int32>(std::lround(static_cast<float64>(static_cast<float32>(channel) * shade)));
    };
    return qRgba(scale(graphics::renderer::MATERIAL_DEBUG_WIREFRAME_RED),
                 scale(graphics::renderer::MATERIAL_DEBUG_WIREFRAME_GREEN),
                 scale(graphics::renderer::MATERIAL_DEBUG_WIREFRAME_BLUE),
                 OPAQUE_ALPHA);
}
[[nodiscard]] SurfaceColour DescribeSurface(const RenderScene& scene, const PreviewObject& object) noexcept
{
    SurfaceColour result;
    result.Tint = object.Tint;
    int32 texture = object.Texture;
    if (object.Surface >= 0 && object.Surface < scene.Surfaces.size())
    {
        const PreviewSurface& surface = scene.Surfaces[object.Surface];
        result.Tint = {result.Tint.X * surface.Tint.X,
                       result.Tint.Y * surface.Tint.Y,
                       result.Tint.Z * surface.Tint.Z,
                       result.Tint.W * surface.Tint.W};
        result.Emission = surface.Emission;
        result.IsRepeating = surface.Repeat;
        texture = texture >= 0 ? texture : surface.Texture;
    }
    if (texture >= 0 && texture < scene.Textures.size())
    {
        result.TextureOrNull = &scene.Textures[texture].Pixels;
    }
    return result;
}
} // namespace
QImage RenderSoftwareView(const RenderScene& scene,
                          const graphics::renderer::ViewDescription& view,
                          uint32 width,
                          uint32 height,
                          PreviewShading mode)
{
    if (width == 0 || height == 0)
    {
        return {};
    }
    QImage image(static_cast<int32>(width), static_cast<int32>(height), QImage::Format_ARGB32);
    if (image.isNull() == true)
    {
        return {};
    }
    image.fill(QColor::fromRgba(BACKGROUND_COLOUR));
    QVector<float32> depth(static_cast<qsizetype>(width) * static_cast<qsizetype>(height), DEPTH_CLEAR);
    Target target
    {
        .ImageOrNull = &image,
        .DepthOrNull = &depth,
        .Width = static_cast<int32>(width),
        .Height = static_cast<int32>(height),
    };
    math::Vector3 light;
    static_cast<void>(math::TryNormalize(PREVIEW_LIGHT_DIRECTION, 0.0F, light));
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            continue;
        }
        const PreviewMesh& mesh = scene.Meshes[object.Mesh];
        SurfaceColour surface = DescribeSurface(scene, object);
        if (mode == PreviewShading::Solid)
        {
            surface = { .Tint = {SOLID_GREY, SOLID_GREY, SOLID_GREY, 1} };
        }
        else if (mode == PreviewShading::Albedo)
        {
            surface.Emission = {};
        }
        for (qsizetype i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
            ClipVertex triangle[3];
            math::Vector3 world[3];
            bool isValid = true;
            for (qsizetype corner = 0; corner < 3; ++corner)
            {
                const uint32 index = mesh.Indices[i + corner];
                if (index >= static_cast<uint32>(mesh.Positions.size()))
                {
                    isValid = false;
                    break;
                }
                world[corner] = math::TransformPoint(object.Transform, mesh.Positions[index]);
                triangle[corner].PositionClip =
                    view.WorldToClip * math::Vector4{world[corner].X, world[corner].Y, world[corner].Z, 1};
                triangle[corner].Uv = index < static_cast<uint32>(mesh.Uvs.size()) ? mesh.Uvs[index] : math::Vector2{};
            }
            if (isValid == false)
            {
                continue;
            }
            math::Vector3 normal;
            const bool hasNormal =
                math::TryNormalize(math::Cross(world[1] - world[0], world[2] - world[0]), 0.0F, normal) ==
                math::MathStatus::Success;
            const float32 diffuse = hasNormal == true ? math::Abs(math::Dot(normal, light)) : 1.0F;
            const float32 shading = mode == PreviewShading::Albedo ? 1.0F : AMBIENT_SHADING + DIFFUSE_SHADING * diffuse;
            ClipVertex clipped[MAX_CLIPPED_VERTICES];
            const usize count = ClipDepth(triangle, clipped);
            if (count < 3)
            {
                continue;
            }
            const ScreenVertex first = ProjectVertex(clipped[0], target.Width, target.Height);
            if (IsPreviewLineShading(mode) == true)
            {
                // Near/far clipping can add polygon edges; they lie on the clip planes, as GPU line fill draws them.
                math::Vector3 toCameraWorld =
                    view.OrthographicCamera == true
                        ? -view.CameraForward
                        : view.CameraPosition - (world[0] + world[1] + world[2]) / TRIANGLE_CORNER_COUNT;
                static_cast<void>(math::TryNormalize(toCameraWorld, 0.0F, toCameraWorld));
                const QRgb colour =
                    ComputeWireframeColour(hasNormal == true ? math::Abs(math::Dot(normal, toCameraWorld)) : 1.0F);
                ScreenVertex previous = ProjectVertex(clipped[count - 1], target.Width, target.Height);
                for (usize corner = 0; corner < count; ++corner)
                {
                    const ScreenVertex current =
                        corner == 0 ? first : ProjectVertex(clipped[corner], target.Width, target.Height);
                    DrawLine(target, previous, current, colour);
                    previous = current;
                }
                continue;
            }
            for (usize fan = 1; fan + 1 < count; ++fan)
            {
                DrawTriangle(target,
                             first,
                             ProjectVertex(clipped[fan], target.Width, target.Height),
                             ProjectVertex(clipped[fan + 1], target.Width, target.Height),
                             surface,
                             shading);
            }
        }
    }
    return image;
}
} // namespace ludus::editor
