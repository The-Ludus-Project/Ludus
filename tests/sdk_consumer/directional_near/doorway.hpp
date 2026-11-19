#pragma once

#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/views.hpp>

namespace ludus::testing::directional
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace rhi = graphics::rhi;
inline constexpr uint32 WIDTH = 96;
inline constexpr uint32 HEIGHT = 64;
inline constexpr float32 VERTICAL_FOV_RADIANS = 0.2F;
inline constexpr uint32 CHANNELS = 4;
inline constexpr usize WALL_VERTEX_COUNT = 4;
inline constexpr usize WALL_INDEX_COUNT = 6;

// Flat landscape, a thin horizontal roof and a wall with a doorway. The wall is outside the camera
// image while its shadow crosses the fixed floor probes. The two lights reach the wall from opposite sides.
inline constexpr float32 LANDSCAPE_HALF_METRES = 9000;
inline constexpr float32 WALL_X = 2;
inline constexpr float32 OPENING_HALF_WIDTH = 0.5F;
inline constexpr float32 WALL_TOP = 3;
struct DoorwayGeometry final
{
    Array<math::Vector3> Ground;
    Array<math::Vector3> GroundNormals;
    Array<uint32> GroundIndices;
    Array<math::Vector3> Casters;
    Array<math::Vector3> CasterNormals;
    Array<uint32> CasterIndices;
    DoorwayGeometry()
    {
        const StaticArray<uint32, WALL_INDEX_COUNT> front{{0, 1, 2, 0, 2, 3}};
        const StaticArray<float32, 4> edges{{-LANDSCAPE_HALF_METRES, -8, 8, LANDSCAPE_HALF_METRES}};
        for (usize row = 0; row < edges.GetSize(); ++row)
        {
            for (usize column = 0; column < edges.GetSize(); ++column)
            {
                Ground.Add({edges[column], edges[row], 0});
                GroundNormals.Add({0, 0, 1});
                if (row + 1 < edges.GetSize() && column + 1 < edges.GetSize())
                {
                    const uint32 start = static_cast<uint32>(row * edges.GetSize() + column);
                    const uint32 stride = static_cast<uint32>(edges.GetSize());
                    for (uint32 index :
                         {start, start + 1, start + stride + 1, start, start + stride + 1, start + stride})
                    {
                        GroundIndices.Add(index);
                    }
                }
            }
        }
        const auto addQuad = [&](const StaticArray<math::Vector3, WALL_VERTEX_COUNT>& vertices,
                                 math::Vector3 normalWorld) {
            const uint32 start = static_cast<uint32>(Casters.GetSize());
            for (math::Vector3 vertex : vertices)
            {
                Casters.Add(vertex);
                CasterNormals.Add(normalWorld);
            }
            for (uint32 index : front)
            {
                CasterIndices.Add(start + index);
            }
        };
        const auto addWall = [&](float32 left, float32 right) {
            addQuad({{{WALL_X, left, 0}, {WALL_X, right, 0}, {WALL_X, right, WALL_TOP}, {WALL_X, left, WALL_TOP}}},
                    {1, 0, 0});
        };
        addWall(-4, -OPENING_HALF_WIDTH);
        addWall(OPENING_HALF_WIDTH, 4);
        addQuad({{{1.75F, -4, WALL_TOP}, {2.25F, -4, WALL_TOP}, {2.25F, 4, WALL_TOP}, {1.75F, 4, WALL_TOP}}},
                {0, 0, 1});
    }
    void Describe(StaticArray<renderer::RenderGraphMesh, 2>& outMeshes) const
    {
        outMeshes[0].Geometry = {Ground.GetData(), Ground.GetSize(), GroundIndices.GetData(), GroundIndices.GetSize()};
        outMeshes[0].Geometry.Normals = GroundNormals.GetData();
        outMeshes[1].Geometry = {Casters.GetData(),
                                 Casters.GetSize(),
                                 CasterIndices.GetData(),
                                 CasterIndices.GetSize()};
        outMeshes[1].Geometry.Normals = CasterNormals.GetData();
    }
};
inline void AimCamera(math::Vector3 eyeWorld,
                      math::Vector3 targetWorld,
                      renderer::ViewDescription& outCamera,
                      uint32 width = WIDTH,
                      uint32 height = HEIGHT)
{
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping(
                {static_cast<float32>(width), static_cast<float32>(height), renderer::ScreenFit::Stretch},
                {0, 0, width, height},
                width,
                height,
                mapping) == rhi::RasterStatus::Ready);
    renderer::ProjectionDescription projection;
    projection.Kind = renderer::ProjectionKind::Perspective;
    REQUIRE(math::TryLookAt(eyeWorld, targetWorld, {0, 0, 1}, projection.WorldToView) == math::MathStatus::Success);
    projection.VerticalFovRadians = VERTICAL_FOV_RADIANS;
    projection.Near = 0.01F;
    projection.Far = 400;
    REQUIRE(renderer::BuildViewDescription(projection, mapping, outCamera) == rhi::RasterStatus::Ready);
    for (math::Vector4& column : outCamera.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outCamera.Depth = renderer::DepthConvention::Conventional;
    outCamera.InfiniteFar = false;
    outCamera.OrthographicCamera = false;
    outCamera.CameraPosition = eyeWorld;
    REQUIRE(math::TryNormalize(targetWorld - eyeWorld, outCamera.CameraForward) == math::MathStatus::Success);
}

} // namespace ludus::testing::directional
