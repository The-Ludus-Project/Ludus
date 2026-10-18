#pragma once
#include "l4_bake.h"
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/views.hpp>
namespace ludus::qa
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
// Public-only fixture shared by installed-SDK and real-GPU pixel tests.
struct L4Scene final
{
    rr::MaterialProgram Program;
    rr::MaterialVersion Materials[8];
    rr::Mesh Geometry;
    rr::Snapshot Casters, Current;
    rr::PreparedView View;
    rr::ShadowAtlas Atlas;
    rhi::TextureHandle Bake, Target, Display;
    rr::Presentation Tone, Encode;
    rr::ViewReport Report;
    usize Stage = 0;
    bool Hdr = false, Reference = false, Moved = false;

    static bool Accepted(rhi::RasterStatus s) noexcept
    {
        return s == rhi::RasterStatus::Ready || s == rhi::RasterStatus::Pending;
    }
    static rr::DirectLight Sun() noexcept
    {
        rr::DirectLight light;
        light.SourceId = 1;
        return light;
    }
    static math::Affine3 Transform(usize i) noexcept
    {
        math::Affine3 transform;
        if (i < 6)
        {
            transform.Translation = {static_cast<float32>(i % 3) * .65F - .65F, i < 3 ? .45F : -.45F, 0};
        }
        else if (i == 6)
        {
            transform.Columns[0].X = 3.5F;
            transform.Columns[1].Y = .25F;
        }
        else
        {
            transform.Columns[0].X = .6F;
            transform.Columns[1].Y = .6F;
            transform.Translation = {0, 0, .3F};
        }
        return transform;
    }
    [[nodiscard]] rr::LitMaterialDescription Material(usize i, const rr::DirectLight& light) const noexcept
    {
        rr::LitMaterialDescription material;
        material.Surface.Tint = {.5F, .5F, .5F, 1};
        material.Roughness = 1;
        material.Lights = &light;
        material.LightCount = 1;
        if (i == 1 || i == 2)
        {
            material.Bake = textures::l4_bake::Bake(Bake);
            material.Bake.SourceId = i + 1;
            material.Bake.Transform = Transform(i);
            if (i == 1)
            {
                material.Surface.Alpha = rr::MaterialAlpha::Masked;
            }
        }
        if (i == 3 || i == 5)
        {
            material.Emission = {4, 0, 0};
            material.LightCount = 0;
        }
        if (i == 4)
        {
            material.Metallic = 1;
            material.Surface.Tint = {1, 1, 1, 1};
            material.EnvironmentRadiance = {1, 1, 1};
            material.LightCount = 0;
        }
        if (i == 5)
        {
            material.Surface.Alpha = rr::MaterialAlpha::Painter;
            material.Surface.Tint.W = .5F;
        }
        return material;
    }
    void Items(rr::SceneItem (&items)[8], bool shadowed) const noexcept
    {
        for (usize i = 0; i < 8; ++i)
        {
            items[i].Geometry = Geometry;
            items[i].SourceId = i + 1;
            items[i].Transform = Transform(i);
            if (i < 7)
            {
                items[i].Surface = Materials[i == 6 && shadowed ? 7 : i];
            }
            if (i == 1 || i == 2)
            {
                items[i].BakeSceneRevision = 1;
                items[i].BakeDependencyRevision = i == 1 ? 11 : 12;
            }
            if (i == 5)
            {
                items[i].Pass = rr::Layer::Overlay;
            }
            if (i == 7)
            {
                items[i].Material.Color = {.25F, .25F, .25F, 1};
                if (Moved)
                {
                    items[i].Transform.Translation.X = .25F;
                }
            }
        }
    }
    [[nodiscard]] rr::ViewDescription Camera() const noexcept
    {
        rr::ViewDescription view;
        view.Reference = Reference;
        // Oblique orthographic view moves the caster away from its ground shadow.
        view.WorldToClip = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {-2, 0, -1, 0}, {1, 0, 1, 1}}};
        view.OrthographicCamera = true;
        view.CameraForward = {-2, 0, -1};
        return view;
    }
    rhi::RasterStatus Advance(rr::Renderer& renderer, rhi::DeviceHandle device, rhi::SurfaceHandle surface) noexcept
    {
        using S = rhi::RasterStatus;
        auto status = rhi::PollLifetime(device);
        if (status != S::Ready)
        {
            return status;
        }
        if (Stage == 0)
        {
            const math::Vector3 positions[]{{-.22F, -.24F, .5F},
                                            {.22F, -.24F, .5F},
                                            {.22F, .24F, .5F},
                                            {-.22F, .24F, .5F}};
            const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            const math::Vector2 uv[]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            const uint32 indices[]{0, 1, 2, 0, 2, 3};
            rr::MeshDescription mesh{positions, 4, indices, 6};
            mesh.Normals = normals;
            mesh.Texcoords = uv;
            status = renderer.CreateMesh(mesh, Geometry);
            if (Accepted(status) == false)
            {
                return status;
            }
            status = rhi::CreateTexture(device, textures::l4_bake::Description(), textures::l4_bake::Upload(), Bake);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 1;
        }
        if (Stage == 1)
        {
            status = renderer.GetStatus(Geometry);
            if (status != S::Ready)
            {
                return status;
            }
            status = renderer.GetStatus(Program);
            if (status != S::Ready)
            {
                return status;
            }
            status = rhi::GetStatus(device, Bake);
            if (status != S::Ready)
            {
                return status;
            }
            const auto light = Sun();
            rr::LightSelection selected;
            for (usize i = 0; i < 7; ++i)
            {
                status = renderer.CreateLitMaterial(Program, Material(i, light), Materials[i], selected);
                if (Accepted(status) == false)
                {
                    return status;
                }
            }
            Stage = 2;
        }
        if (Stage == 2)
        {
            for (usize i = 0; i < 7; ++i)
            {
                status = renderer.GetStatus(Materials[i]);
                if (status != S::Ready)
                {
                    return status;
                }
            }
            rr::SceneItem items[8];
            Items(items, false);
            status = renderer.CreateSnapshot(items, 8, Casters);
            if (status != S::Ready)
            {
                return status;
            }
            rr::ShadowRegion regions[4];
            for (usize i = 0; i < 4; ++i)
            {
                regions[i].Light = Sun();
                regions[i].WorldToClip = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, 0}, {0, 0, 1, 1}}};
                regions[i].Interior = {static_cast<uint32>(i % 2) * 16 + 1,
                                       static_cast<uint32>(i / 2) * 16 + 1,
                                       14,
                                       14};
                if (i == 0)
                {
                    regions[i].WorldToClip.Columns[3].Y = 3;
                }
                if (i >= 2)
                {
                    regions[i].Light.SourceId = 2;
                    regions[i].Light.Kind = rr::LightKind::Spot;
                    regions[i].Light.Position.Z = 2;
                    regions[i].Light.Intensity = 0;
                }
            }
            status = renderer.PrepareShadowAtlas(Casters, {32, 32, regions, 4}, Atlas);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 3;
        }
        if (Stage == 3)
        {
            status = renderer.GetStatus(Atlas);
            if (status != S::Ready)
            {
                return status;
            }
            const auto begin = rhi::BeginFrame(device, surface);
            if (begin == rhi::DeviceStatus::Skipped)
            {
                return S::NotReady;
            }
            if (begin != rhi::DeviceStatus::Ready)
            {
                return S::InvalidState;
            }
            status = renderer.DrawShadowAtlas(Atlas);
            if (status != S::Ready)
            {
                return status;
            }
            rhi::SubmissionToken completion;
            status = rhi::EndFrame(device, completion);
            if (status != S::Ready)
            {
                return status;
            }
            rr::DirectLight lights[2]{Sun(), Sun()};
            lights[1].SourceId = 2;
            lights[1].Kind = rr::LightKind::Spot;
            lights[1].Position.Z = 2;
            lights[1].Intensity = 0;
            auto material = Material(6, lights[0]);
            material.Lights = lights;
            material.LightCount = 2;
            material.Shadows = Atlas;
            rr::LightSelection selected;
            status = renderer.CreateLitMaterial(Program, material, Materials[7], selected);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 4;
        }
        if (Stage == 4)
        {
            status = renderer.GetStatus(Materials[7]);
            if (status != S::Ready)
            {
                return status;
            }
            rr::SceneItem items[8];
            Items(items, true);
            status = renderer.CreateSnapshot(items, 8, Current);
            if (status != S::Ready)
            {
                return status;
            }
            status = renderer.PrepareView(Current, Camera(), View, Report);
            if (Accepted(status) == false)
            {
                return status;
            }
            if (Report.BakeFallbacks != 1 || Report.ShadowFallbacks != 0)
            {
                return S::InvalidState;
            }
            status =
                rhi::CreateTexture(device,
                                   {96, 64, Hdr ? rhi::RasterFormat::Rgba16Float : rhi::RasterFormat::Rgba8Unorm, true},
                                   {},
                                   Target);
            if (Accepted(status) == false)
            {
                return status;
            }
            status = rhi::CreateTexture(device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, Display);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 5;
        }
        if (Stage == 5)
        {
            status = renderer.GetStatus(View);
            if (status != S::Ready)
            {
                return status;
            }
            status = rhi::GetStatus(device, Target);
            if (status != S::Ready)
            {
                return status;
            }
            status = rhi::GetStatus(device, Display);
            if (status != S::Ready)
            {
                return status;
            }
            rr::ViewMapping mapping;
            status = rr::ResolveViewMapping({96, 64}, {0, 0, 96, 64}, 96, 64, mapping);
            if (status != S::Ready)
            {
                return status;
            }
            rr::LightingOutput tone;
            tone.Encode = false;
            status = renderer.PrepareLightingPresentation(Target, mapping, tone, Tone);
            if (Accepted(status) == false)
            {
                return status;
            }
            rr::LightingOutput encode;
            encode.ToneMap = false;
            status = renderer.PrepareLightingPresentation(Display, mapping, encode, Encode);
            if (Accepted(status) == false)
            {
                return status;
            }
            Stage = 6;
        }
        if (Stage == 6)
        {
            status = renderer.GetStatus(Tone);
            if (status != S::Ready)
            {
                return status;
            }
            status = renderer.GetStatus(Encode);
            if (status != S::Ready)
            {
                return status;
            }
            // Leases survive caller ownership loss. Invalid authoring preserves the live scene.
            const auto light = Sun();
            auto invalid = Material(0, light);
            invalid.Roughness = 0;
            rr::MaterialVersion candidate;
            rr::LightSelection selected;
            if (renderer.CreateLitMaterial(Program, invalid, candidate, selected) != S::InvalidDescription)
            {
                return S::InvalidState;
            }
            status = renderer.Release(Atlas);
            if (status != S::Ready)
            {
                return status;
            }
            status = rhi::Destroy(device, Bake);
            if (status != S::Ready)
            {
                return status;
            }
            Stage = 7;
        }
        return S::Ready;
    }
    rhi::RasterStatus MoveCaster(rr::Renderer& renderer) noexcept
    {
        using S = rhi::RasterStatus;
        if (Moved == false)
        {
            Moved = true;
            rr::SceneItem items[8];
            Items(items, true);
            auto status = renderer.ReplaceSnapshot(items, 8, Current);
            if (status != S::Ready)
            {
                return status;
            }
            status = renderer.Release(View);
            if (status != S::Ready)
            {
                return status;
            }
            status = renderer.PrepareView(Current, Camera(), View, Report);
            if (Accepted(status) == false)
            {
                return status;
            }
            if (Report.BakeFallbacks != 1 || Report.ShadowFallbacks != 1)
            {
                return S::InvalidState;
            }
        }
        return renderer.GetStatus(View);
    }
    rhi::RasterStatus Draw(rr::Renderer& renderer) noexcept
    {
        using S = rhi::RasterStatus;
        rhi::RasterPassDescription pass;
        pass.Color = Target;
        auto status = renderer.DrawView(View, pass);
        if (status != S::Ready)
        {
            return status;
        }
        pass.Color = Display;
        status = renderer.DrawLightingPresentation(Tone, pass);
        if (status != S::Ready)
        {
            return status;
        }
        return renderer.DrawLightingPresentation(Encode);
    }
};
} // namespace ludus::qa
