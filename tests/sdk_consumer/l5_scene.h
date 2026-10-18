#pragma once
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/views.hpp>
namespace ludus::qa
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
// Public-only fixture: 128 localized lights, 256 objects, overflow and outside-grid variants.
struct L5Scene final
{
    rr::MaterialProgram Program;
    rr::MaterialVersion Material;
    rr::Mesh Geometry;
    rr::Snapshot Current;
    rr::PreparedView View;
    rr::Presentation Output;
    rhi::TextureHandle Target;
    rr::ViewReport Report;
    rr::SceneItem Items[256];
    rr::DirectLight Lights[128];
    usize Stage = 0;
    uint32 Operation = 0;
    uint32 Scenario = 0, Width = 96, Height = 64;
    bool Reference = false, Hdr = false;
    static bool Accepted(rhi::RasterStatus status) noexcept
    {
        return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
    }
    [[nodiscard]] rhi::RasterStatus
    Advance(rr::Renderer& renderer, rhi::DeviceHandle device, rhi::SurfaceHandle) noexcept
    {
        using S = rhi::RasterStatus;
        if (Stage == 0)
        {
            auto status = renderer.GetStatus(Program);
            if (status != S::Ready)
            {
                return status;
            }
            const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
            const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            const uint32 indices[]{0, 1, 2, 0, 2, 3};
            rr::MeshDescription mesh{positions, 4, indices, 6};
            mesh.Normals = normals;
            Operation = 1;
            status = renderer.CreateMesh(mesh, Geometry);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 1;
        }
        if (Stage == 1)
        {
            auto status = renderer.GetStatus(Geometry);
            if (status != S::Ready)
            {
                return status;
            }
            auto& lights = Lights;
            for (usize i = 0; i < 128; ++i)
            {
                auto& light = lights[i];
                light.SourceId = i + 1;
                light.Kind = Scenario == 1 ? rr::LightKind::Directional : rr::LightKind::Point;
                light.Position = {static_cast<float32>(i % 16) * .25F - 2,
                                  static_cast<float32>((i / 16) % 8) * .25F - 1,
                                  1};
                light.Range = .75F;
                light.Intensity = Scenario == 1 ? .02F : .2F;
                light.Color = {1, .6F, .2F};
            }
            rr::LightClusterDescription grid;
            grid.Bounds = Scenario == 2 ? math::Aabb3{{3, 3, 3}, {4, 4, 4}} : math::Aabb3{{-2, -2, 0}, {2, 2, 2}};
            rr::LitMaterialDescription description;
            description.Lights = lights;
            description.LightCount = 128;
            description.Clusters = &grid;
            description.Surface.Tint = {.5F, .5F, .5F, 1};
            description.Roughness = 1;
            rr::LightSelection selected;
            Operation = 2;
            status = renderer.CreateLitMaterial(Program, description, Material, selected);
            if (Accepted(status) == false)
            {
                return status;
            }
            if (selected.Count != 128 || selected.Dropped != 0)
            {
                return S::InvalidState;
            }
            Stage = 2;
        }
        if (Stage == 2)
        {
            auto status = renderer.GetStatus(Material);
            if (status != S::Ready)
            {
                return status;
            }
            rhi::TextureDescription target{Width,
                                           Height,
                                           Hdr ? rhi::RasterFormat::Rgba16Float : rhi::RasterFormat::Rgba8Unorm};
            target.Attachment = true;
            Operation = 3;
            status = rhi::CreateTexture(device, target, {}, Target);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 3;
        }
        if (Stage == 3)
        {
            auto status = rhi::GetStatus(device, Target);
            if (status != S::Ready)
            {
                return status;
            }
            auto& items = Items;
            for (usize i = 0; i < 256; ++i)
            {
                items[i].Geometry = Geometry;
                items[i].Surface = Material;
                items[i].SourceId = i + 1;
                const auto quadWidth = Width / 20, quadHeight = Height / 20;
                const auto cellWidth = Width / 16, cellHeight = Height / 8;
                const auto halfWidth = cellWidth / 2, halfHeight = cellHeight / 2;
                const auto pixelX = (i % 16) * cellWidth + halfWidth;
                const auto pixelY = ((i / 16) % 8) * cellHeight + halfHeight;
                const auto centerX = static_cast<float32>(pixelX) + static_cast<float32>(quadWidth % 2) * .5F;
                const auto centerY = static_cast<float32>(pixelY) + static_cast<float32>(quadHeight % 2) * .5F;
                items[i].Transform.Columns[0].X = static_cast<float32>(quadWidth) / static_cast<float32>(Width);
                items[i].Transform.Columns[1].Y = static_cast<float32>(quadHeight) / static_cast<float32>(Height);
                // Pixel-aligned edges avoid comparing backend raster edge rounding as lighting error.
                items[i].Transform.Translation = {centerX * 2 / static_cast<float32>(Width) - 1 +
                                                      (i < 128 ? 10.0F : 0.0F),
                                                  1 - centerY * 2 / static_cast<float32>(Height),
                                                  0};
            }
            Operation = 4;
            status = renderer.CreateSnapshot(items, 256, Current);
            if (status != S::Ready)
            {
                return status;
            }
            rr::ViewDescription view;
            view.Reference = Reference;
            Operation = 5;
            status = renderer.PrepareView(Current, view, View, Report);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 4;
        }
        if (Stage == 4)
        {
            auto status = renderer.GetStatus(View);
            if (status != S::Ready)
            {
                return status;
            }
            rr::ViewMapping mapping;
            status = rr::ResolveViewMapping({static_cast<float32>(Width), static_cast<float32>(Height)},
                                            {0, 0, Width, Height},
                                            Width,
                                            Height,
                                            mapping);
            if (status != S::Ready)
            {
                return status;
            }
            Operation = 6;
            status = renderer.PrepareLightingPresentation(Target, mapping, {}, Output);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 5;
        }
        auto status = renderer.GetStatus(View);
        if (status != S::Ready)
        {
            return status;
        }
        return renderer.GetStatus(Output);
    }
    [[nodiscard]] rhi::RasterStatus Draw(rr::Renderer& renderer) const noexcept
    {
        rhi::RasterPassDescription pass;
        pass.Color = Target;
        auto status = renderer.DrawView(View, pass);
        if (status == rhi::RasterStatus::Ready)
        {
            status = renderer.DrawLightingPresentation(Output);
        }
        return status;
    }
};
} // namespace ludus::qa
