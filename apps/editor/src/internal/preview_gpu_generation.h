#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>

#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
inline constexpr foundation::usize PREVIEW_MESH_CAPACITY = 6;
inline constexpr foundation::usize PREVIEW_TEXTURE_CAPACITY = 8;

/// Outcome of one nonblocking preparation step; only Ready exposes a drawable snapshot.
enum class PreviewPreparationStatus : foundation::uint8
{
    Pending,
    Ready,
    Failed,
};

enum class PreviewLightingStage : foundation::uint8
{
    Inputs,
    Casters,
    Shadows,
    Submission,
    Materials,
    Ready,
};

/// Owner-thread GPU generation, frozen to one validated scene and shading mode.
/// The device outlives this owner. Consumers release all image leases before destruction.
/// Preparation never reads mutable workspace/camera state or publishes a display image.
class PreviewGpuGeneration final
{
public:
    PreviewGpuGeneration(graphics::rhi::DeviceHandle device, const RenderScene& source, PreviewShading shading);
    ~PreviewGpuGeneration() noexcept;
    PreviewGpuGeneration(const PreviewGpuGeneration&) = delete;
    PreviewGpuGeneration& operator=(const PreviewGpuGeneration&) = delete;

    /// Advance at most one resource stage; Pending owns accepted work and is polled again.
    /// Failed is terminal for this generation; the caller retains the previous display generation.
    /// surface is borrowed for shadow submission. outError may supply a lighting diagnostic.
    [[nodiscard]] PreviewPreparationStatus Prepare(graphics::rhi::SurfaceHandle surface, QString& outError) noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsPrepared() const noexcept
    {
        return mStage == PreparationStage::Ready && mHasFailed == false;
    }
    [[nodiscard]] graphics::rhi::RasterStatus
    CreateLightingMaterial(foundation::usize objectIndex,
                           bool hasShadows,
                           graphics::renderer::MaterialVersion& outMaterial) noexcept;

    [[nodiscard]] LUDUS_INLINE graphics::renderer::Mesh GetMesh(foundation::usize index) const noexcept
    {
        return mMeshes[index];
    }
    [[nodiscard]] LUDUS_INLINE graphics::renderer::Snapshot GetSnapshot() const noexcept
    {
        return mSnapshot;
    }
    // Generation-local handles used by viewport and shadow preparation.
    const graphics::rhi::DeviceHandle Device;
    graphics::renderer::Renderer Renderer;
    graphics::renderer::RasterViewport Viewport;
    foundation::StaticArray<graphics::rhi::TextureHandle, PREVIEW_TEXTURE_CAPACITY> Textures{};
    foundation::uint32 ExternalLeases = 0;
    const RenderScene Source;
    const PreviewShading Shading;
    const QByteArray LightingKey;
    preview_lighting::Inputs Lighting;
    graphics::renderer::EnvironmentMap Environment;
    bool HasEnvironment = false;
    graphics::renderer::ShadowAtlas Shadows;
    graphics::renderer::Snapshot Casters;
    foundation::StaticArray<graphics::renderer::MaterialVersion, content::scene_format::MAX_OBJECTS> CasterMaterials{};
    graphics::rhi::SubmissionToken ShadowProducer;
    PreviewLightingStage LightingStage = PreviewLightingStage::Inputs;
    foundation::usize MaterialCursor = 0;
    bool IsShadowFrameOpen = false;

private:
    enum class PreparationStage : foundation::uint8
    {
        Renderer,
        Views,
        Materials,
        Assets,
        Snapshot,
        Ready,
    };
    [[nodiscard]] LUDUS_INLINE bool shouldUseLitMaterials() const noexcept
    {
        return Source.Lit == true && Shading != PreviewShading::Albedo;
    }
    /// Cooked scenes take the authored-lighting path for Lit and every debug view, so debug
    /// views read the same surface maps, clusters and lights as Lit.
    [[nodiscard]] LUDUS_INLINE bool shouldUseAuthoredLighting() const noexcept
    {
        return Source.Lit == true && (Shading == PreviewShading::Lit || IsPreviewDebugShading(Shading) == true);
    }
    /// Unlit authored Worlds draw debug views with the L4 debug program and editor-derived normals.
    [[nodiscard]] LUDUS_INLINE bool shouldUseUnlitDebugView() const noexcept
    {
        return Source.Lit == false && IsPreviewDebugShading(Shading) == true;
    }
    /// Material slot shared by untextured objects in unlit debug views, after the scene's surfaces.
    [[nodiscard]] LUDUS_INLINE foundation::int32 getDefaultDebugMaterial() const noexcept
    {
        return static_cast<foundation::int32>(Source.Surfaces.size());
    }
    [[nodiscard]] bool createUnlitDebugMaterials() noexcept;
    [[nodiscard]] bool acceptStage(graphics::rhi::RasterStatus status, PreparationStage next) noexcept;
    [[nodiscard]] bool prepareRenderer() noexcept;
    [[nodiscard]] bool prepareViews() noexcept;
    [[nodiscard]] bool prepareMaterials() noexcept;
    [[nodiscard]] bool prepareAssets() noexcept;
    [[nodiscard]] bool prepareSnapshot(graphics::rhi::SurfaceHandle surface, QString& outError) noexcept;
    [[nodiscard]] bool prepareResources(graphics::rhi::SurfaceHandle surface, QString& outError) noexcept;
    [[nodiscard]] bool prepareLighting(graphics::rhi::SurfaceHandle surface, QString& outError) noexcept;
    foundation::StaticArray<graphics::renderer::Mesh, PREVIEW_MESH_CAPACITY> mMeshes{};
    foundation::StaticArray<graphics::renderer::MaterialVersion, content::scene_format::MAX_OBJECTS> mMaterials{};
    graphics::renderer::MaterialProgram mProgram{};
    graphics::renderer::Snapshot mSnapshot{};

    PreparationStage mStage = PreparationStage::Renderer;
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    bool mIsLightingInitialized = false;
    bool mAreAssetsCreated = false;
#endif
    bool mHasFailed = false;
};
} // namespace ludus::editor
