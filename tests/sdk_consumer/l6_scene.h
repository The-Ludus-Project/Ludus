#pragma once
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
// Public-only fixture: one shared quad, an offscreen emitter and a perpendicular
// diffuse wall. Reused by installed SDK capture and real native/oracle comparison.
namespace ludus::qa
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
inline rhi::RasterStatus BuildL6Snapshot(rr::Renderer& renderer,
                                         rr::MaterialProgram program,
                                         rr::Snapshot& snapshot,
                                         float32 worldZOffset = 0,
                                         bool shouldIncludeDegenerateTriangles = false) noexcept
{
    using S = rhi::RasterStatus;
    const math::Vector3 positions[]{{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}};
    const math::Vector3 normals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr usize DEGENERATE_INDEX_COUNT = 3;
    constexpr usize TOTAL_INDEX_COUNT = QUAD_INDEX_COUNT + DEGENERATE_INDEX_COUNT;
    const StaticArray<uint32, TOTAL_INDEX_COUNT> indices{{0, 2, 1, 0, 3, 2, 0, 0, 1}};
    rr::MeshDescription meshDescription{positions,
                                        QUAD_VERTEX_COUNT,
                                        indices.GetData(),
                                        shouldIncludeDegenerateTriangles == true ? TOTAL_INDEX_COUNT
                                                                                 : QUAD_INDEX_COUNT};
    meshDescription.Normals = normals;
    rr::Mesh mesh;
    auto status = renderer.CreateMesh(meshDescription, mesh);
    if (status != S::Ready && status != S::Pending)
    {
        return status;
    }
    const auto meshStarted = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - meshStarted < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.GetStatus(mesh);
    }
    if (status != S::Ready)
    {
        return status;
    }
    rr::MaterialVersion materials[3];
    for (usize i = 0; i < 3; ++i)
    {
        rr::LitMaterialDescription material;
        material.Surface.Tint = i == 1 ? math::Vector4{.8F, .08F, .04F, 1} : math::Vector4{.6F, .6F, .6F, 1};
        material.Roughness = 1;
        if (i == 2)
        {
            material.Emission = {5, 4, 3};
        }
        rr::LightSelection selection;
        status = renderer.CreateLitMaterial(program, material, materials[i], selection);
        if (status != S::Ready && status != S::Pending)
        {
            return status;
        }
        const auto materialStarted = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - materialStarted < 10 * time::NANOSECONDS_PER_SECOND)
        {
            status = renderer.GetStatus(materials[i]);
        }
        if (status != S::Ready)
        {
            return status;
        }
    }
    rr::SceneItem items[3];
    for (usize i = 0; i < 3; ++i)
    {
        items[i].Geometry = mesh;
        items[i].Surface = materials[i];
        items[i].SourceId = i + 1;
    }
    items[0].Transform.Translation.Z = .7F;
    items[1].Transform.Columns[0] = {0, 0, -1};
    items[1].Transform.Columns[2] = {1, 0, 0};
    items[1].Transform.Translation = {1.2F, 0, .4F};
    items[2].Transform.Columns[0].X = .15F;
    items[2].Transform.Columns[1].Y = .15F;
    items[2].Transform.Translation = {-.8F, 1.5F, .2F};
    for (auto& item : items)
    {
        item.Transform.Translation.Z += worldZOffset;
    }
    status = renderer.CreateSnapshot(items, 3, snapshot);
    for (auto& material : materials)
    {
        static_cast<void>(renderer.Release(material));
    }
    static_cast<void>(renderer.Release(mesh));
    return status;
}
} // namespace ludus::qa
