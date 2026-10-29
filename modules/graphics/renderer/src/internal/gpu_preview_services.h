#pragma once

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include <cstring>

#include "gpu_preview_graph.h"
#include "gpu_preview_ray.h"
#include "ltc_table_data.h"
#include "render_graph_ray_scene.h"
#include "scene_memory.h"
// Included only by renderer.cpp. CPU work here only
// freezes input, builds the static BVH, packs bytes, and observes submission state.
// Transport, accumulation and display conversion are exclusively shader work.
namespace ludus::graphics::renderer
{
namespace
{
using internal::GpuPreviewRecord;
using internal::GpuPreviewState;
using internal::ReferenceJob;
/// Bytes in one RGBA8 texel read from an authored preview image.
constexpr usize RGBA8_TEXEL_BYTES = 4;
/// Largest exposure multiplier a GPU preview accepts.
constexpr float32 MAX_GPU_PREVIEW_EXPOSURE = 10000;
/// GPU working memory charged per preview pixel: accumulation, sample and output images.
constexpr usize GPU_PREVIEW_BYTES_PER_PIXEL = 64;
static_assert(LUDUS_GRAPH_VISIBILITY_BINDING ==
              content::render_graph_format::FIRST_STORAGE_SLOT + content::render_graph_format::MAX_STORAGE_BUFFERS);
// Shared cold serializer is defined by reference_services.h in this translation unit.
bool ReferenceFingerprint(ReferenceJob& inoutJob,
                          usize byteBudget,
                          SceneShaderDiagnostics* diagnosticsOrNull = nullptr) noexcept;
constexpr StaticArray<rhi::RasterBinding, LUDUS_GPU_PREVIEW_BINDING_COUNT> GPU_COMPUTE_BINDINGS{
    {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Compute, sizeof(internal::GpuPreviewUniform)},
     {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, 16},
     {2, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, 16},
     {LUDUS_GPU_PREVIEW_COLORS_BINDING,
      rhi::RasterBindingKind::StorageReadWrite,
      rhi::RasterVisibility::Compute,
      LUDUS_GPU_PREVIEW_VECTOR_BYTES},
     {LUDUS_GPU_PREVIEW_TEXELS_BINDING,
      rhi::RasterBindingKind::StorageRead,
      rhi::RasterVisibility::Compute,
      LUDUS_GPU_PREVIEW_VECTOR_BYTES},
     {LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING, rhi::RasterBindingKind::RayScene, rhi::RasterVisibility::Compute, 0}}};
constexpr StaticArray<rhi::RasterBinding, LUDUS_GRAPH_VISIBILITY_BINDING_COUNT> GPU_VISIBILITY_BINDINGS{{
    GPU_COMPUTE_BINDINGS[0],
    GPU_COMPUTE_BINDINGS[1],
    GPU_COMPUTE_BINDINGS[2],
    GPU_COMPUTE_BINDINGS[3],
    GPU_COMPUTE_BINDINGS[4],
    GPU_COMPUTE_BINDINGS[5],
    {LUDUS_GRAPH_VISIBILITY_BINDING, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Compute, 0},
    {LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING, rhi::RasterBindingKind::TextureDepth2D, rhi::RasterVisibility::Compute, 0},
}};
constexpr rhi::RasterBinding GPU_IMAGE_BINDINGS[]{
    {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, sizeof(math::Vector4)},
    {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Fragment, sizeof(math::Vector4)}};
float32 GpuBits(uint32 value) noexcept
{
    float32 result = 0;
    std::memcpy(&result, &value, sizeof(value));
    return result;
}
math::Vector4 GpuVector(math::Vector3 value, float32 w = 0) noexcept
{
    return {value.X, value.Y, value.Z, w};
}
bool SameGpuTexture(const rhi::TextureCpuContent& a, const rhi::TextureCpuContent& b) noexcept
{
    return a.Bytes == b.Bytes && a.Width == b.Width && a.Height == b.Height && a.RowPitch == b.RowPitch &&
           a.Format == b.Format;
}
Status PackGpuScene(const ReferenceJob& job,
                    bool shouldPackVisibility,
                    bool shouldPartitionNativeOpacity,
                    usize budget,
                    Array<math::Vector4>& data,
                    Array<math::Vector4>& texels,
                    internal::GpuPreviewUniform& outUniform,
                    SceneShaderDiagnostics* diagnosticsOrNull) noexcept
{
    const internal::ReferenceSceneData& scene = job.Scene;
    if (scene.EmitterCdf.GetSize() != scene.Emitters.GetSize())
    {
        return Status::InvalidDescription; // Kernels select emitters through the packed CDF.
    }
    const uint64 visibilityWide = uint64(scene.Triangles.GetSize()) * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    const uint64 nodesWide = visibilityWide + (shouldPackVisibility == true ? scene.Triangles.GetSize() : 0);
    const uint64 orderWide = nodesWide + uint64(scene.Nodes.GetSize()) * 3;
    const uint64 emittersWide = orderWide + scene.Order.GetSize();
    if (job.ShaderEmitterBounds.GetSize() != job.ShaderEmitters.GetSize())
    {
        return Status::InvalidDescription;
    }
    const uint64 surfacesWide = emittersWide + scene.Emitters.GetSize() +
                                uint64(job.ShaderEmitters.GetSize()) * (1 + LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS);
    const uint64 lightsWide = surfacesWide + uint64(scene.InstanceCount) * LUDUS_GPU_PREVIEW_SURFACE_WORDS;
    const uint64 environmentWide = lightsWide + uint64(job.Description.LightCount) * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
    const uint64 sizeWide =
        environmentWide + (job.EnvironmentOrNull != nullptr ? 1 + job.EnvironmentOrNull->Pixels.GetSize() : 0);
    if (sizeWide > ~uint32{0})
    {
        return Status::CapacityExceeded;
    }
    const usize nodes = static_cast<usize>(nodesWide), order = static_cast<usize>(orderWide);
    const usize emitters = static_cast<usize>(emittersWide), surfaces = static_cast<usize>(surfacesWide);
    const usize dataSize = math::Max(static_cast<usize>(sizeWide), usize{1});
    usize size = 0;
    Array<usize> textures;
    const usize textureCount = scene.InstanceCount * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE;
    const uint64 textureOffsetBytes = uint64{textureCount} * sizeof(usize);
    const auto getImage = [&scene](usize index) noexcept -> const rhi::TextureCpuContent& {
        const internal::ReferenceInstance& instance = scene.Instances[index / LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE];
        return index % LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE == 0   ? instance.Texture
               : index % LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE == 1 ? instance.SurfaceMap
                                                                     : instance.EmissionMap;
    };
    if (textureOffsetBytes > budget || textures.TryResize(textureCount) == false)
    {
        return textureOffsetBytes > budget ? internal::RejectSceneMemory(diagnosticsOrNull,
                                                                         SceneMemoryBudget::CpuScratch,
                                                                         textureOffsetBytes,
                                                                         budget)
                                           : Status::OutOfMemory;
    }
    for (usize i = 0; i < textureCount; ++i)
    {
        const rhi::TextureCpuContent& image = getImage(i);
        textures[i] = size;
        if (image.Bytes == nullptr)
        {
            continue;
        }
        bool isShared = false;
        for (usize j = 0; j < i; ++j)
        {
            if (getImage(j).Bytes != nullptr && SameGpuTexture(image, getImage(j)))
            {
                textures[i] = textures[j];
                isShared = true;
                break;
            }
        }
        if (isShared == false)
        {
            const uint64 words = (uint64(image.Width) * image.Height + 3) / 4;
            usize added = 0;
            if (foundation::TryIntegerCast(words, added) == false || foundation::TryAdd(size, added, size) == false)
            {
                return Status::CapacityExceeded;
            }
        }
    }
    // A float4 offset is serialized as uint32; every allocation is bounded before narrowing.
    if (size == 0)
    {
        size = 1;
    }
    if (size > ~uint32{0})
    {
        return Status::CapacityExceeded;
    }
    const uint64 requiredBytes =
        (uint64{size} + dataSize) * sizeof(math::Vector4) + uint64{textures.GetSize()} * sizeof(usize);
    if (requiredBytes > budget)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull, SceneMemoryBudget::CpuScratch, requiredBytes, budget);
    }
    if (data.TryResize(dataSize) == false || texels.TryResize(size) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < scene.Triangles.GetSize(); ++i)
    {
        const internal::ReferenceTriangle& t = scene.Triangles[i];
        math::Vector4* out = data.GetData() + i * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        out[0] = GpuVector(t.Positions[0], GpuBits(t.Instance));
        out[1] = GpuVector(t.Positions[1], t.Area);
        out[2] = GpuVector(t.Positions[2], GpuBits(t.Primitive));
        for (usize corner = 0; corner < 3; ++corner)
        {
            out[3 + corner] = GpuVector(t.Normals[corner]);
        }
        out[6] = {t.Uvs[0].X, t.Uvs[0].Y, t.Uvs[1].X, t.Uvs[1].Y};
        out[7] = {t.Uvs[2].X, t.Uvs[2].Y, 0, 0};
        out[LUDUS_GPU_PREVIEW_GEOMETRIC_NORMAL_WORD] = GpuVector(t.GeometricNormal, GpuBits(t.ShaderSlot));
        out[LUDUS_GRAPH_EMITTER_WORD] = GpuVector(t.Error, GpuBits(t.ShaderEmitter));
        out[LUDUS_GRAPH_INSTANCE_WORD].W = GpuBits(t.ShaderInstance);
        out[LUDUS_GRAPH_MATERIAL_WORD].W = GpuBits(t.ShaderMaterial);
        out[LUDUS_GRAPH_MATERIAL_KIND_WORD].W = GpuBits(t.ShaderMaterialKind);
        out[LUDUS_GRAPH_SOURCE_WORD].Z = GpuBits(static_cast<uint32>(t.ShaderSourceId));
        out[LUDUS_GRAPH_SOURCE_WORD].W =
            GpuBits(static_cast<uint32>(t.ShaderSourceId >> LUDUS_GRAPH_SOURCE_HIGH_SHIFT));
        for (usize corner = 0; corner < 3; ++corner)
        {
            out[10 + corner] = t.Tangents[corner];
            out[13 + corner] = t.Colors[corner];
        }
    }
    if (shouldPackVisibility == true)
    {
        // A sorted identity indirection preserves material-slot packing and native BVH order.
        // Thanks to Burns and Hunt, "The Visibility Buffer: A Cache-Friendly Approach to Deferred
        // Shading", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/04/ : defer surface
        // reconstruction through triangle/instance identity rather than evaluated material channels.
        math::Vector4* visibility = data.GetData() + static_cast<usize>(visibilityWide);
        for (usize index = 0; index < scene.Triangles.GetSize(); ++index)
        {
            const internal::ReferenceTriangle& triangle = scene.Triangles[index];
            visibility[index] = {GpuBits(triangle.ShaderInstance),
                                 GpuBits(triangle.Primitive),
                                 GpuBits(static_cast<uint32>(index)),
                                 GpuBits(triangle.ShaderDraw)};
        }
        const auto isEarlier = [](const math::Vector4& left, const math::Vector4& right) noexcept {
            uint32 leftInstance = 0;
            uint32 rightInstance = 0;
            uint32 leftPrimitive = 0;
            uint32 rightPrimitive = 0;
            std::memcpy(&leftInstance, &left.X, sizeof(leftInstance));
            std::memcpy(&rightInstance, &right.X, sizeof(rightInstance));
            std::memcpy(&leftPrimitive, &left.Y, sizeof(leftPrimitive));
            std::memcpy(&rightPrimitive, &right.Y, sizeof(rightPrimitive));
            return leftInstance < rightInstance || (leftInstance == rightInstance && leftPrimitive < rightPrimitive);
        };
        Sort(visibility, scene.Triangles.GetSize(), isEarlier);
    }
    for (usize i = 0; i < scene.Nodes.GetSize(); ++i)
    {
        const internal::ReferenceBvhNode& n = scene.Nodes[i];
        math::Vector4* out = data.GetData() + nodes + i * 3;
        out[0] = GpuVector(n.Bounds.Min, GpuBits(n.Left));
        out[1] = GpuVector(n.Bounds.Max, GpuBits(n.Right));
        out[2] = {GpuBits(n.First), GpuBits(n.Count), GpuBits(n.SplitAxis), 0};
    }
    internal::PackGpuPreviewRayOrder(scene, shouldPartitionNativeOpacity, data.GetData() + order);
    if (internal::PackReferenceEmitterSelection(scene, {data.GetData() + emitters, scene.Emitters.GetSize()}) == false)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < job.ShaderEmitters.GetSize(); ++i)
    {
        data[emitters + scene.Emitters.GetSize() + i] = job.ShaderEmitters[i];
    }
    math::Vector4* boundsWords = data.GetData() + emitters + scene.Emitters.GetSize() + job.ShaderEmitters.GetSize();
    for (usize i = 0; i < job.ShaderEmitterBounds.GetSize(); ++i)
    {
        const internal::ReferenceEmitterBounds& bounds = job.ShaderEmitterBounds[i];
        math::Vector4* out = boundsWords + i * LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS;
        out[0] = GpuVector(bounds.SphereWorld.Center, bounds.SphereWorld.Radius);
        out[1] = GpuVector(bounds.NormalAxisWorld, bounds.CosConeHalfAngle);
        out[2] = {GpuBits(bounds.IsEmissionTwoSided == true ? LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_TWO_SIDED : 0U),
                  0,
                  0,
                  0};
    }
    for (usize i = 0; i < scene.InstanceCount; ++i)
    {
        const internal::ReferenceInstance& instance = scene.Instances[i];
        const internal::ReferenceSurfaceData& surface = *instance.Surface;
        math::Vector4* surfaceWords = data.GetData() + surfaces + i * LUDUS_GPU_PREVIEW_SURFACE_WORDS;
        surfaceWords[LUDUS_GPU_PREVIEW_BASE_COLOR_WORD] = GpuVector(surface.Bsdf.BaseColor, surface.Bsdf.Metallic);
        surfaceWords[LUDUS_GPU_PREVIEW_ROUGHNESS_WORD] =
        {
            .X = surface.Bsdf.Roughness,
            .Y = surface.AlphaCutoff,
            .Z = GpuBits(instance.Texture.Width),
            .W = GpuBits(instance.Texture.Height),
        };
        surfaceWords[LUDUS_GPU_PREVIEW_TINT_WORD] = surface.Tint;
        surfaceWords[LUDUS_GPU_PREVIEW_UV_TRANSFORM_WORD] = surface.UvTransform;
        surfaceWords[LUDUS_GPU_PREVIEW_EMISSION_WORD] =
            GpuVector(surface.Emission, GpuBits(surface.HasTexture == true ? 1U : 0U));
        const uint32 flags =
            (surface.Sampler.Filter == rhi::RasterFilter::Linear ? LUDUS_GPU_PREVIEW_TEXTURE_LINEAR_FLAG : 0U) |
            (surface.Sampler.U == rhi::RasterAddress::Repeat ? LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_U_FLAG : 0U) |
            (surface.Sampler.V == rhi::RasterAddress::Repeat ? LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_V_FLAG : 0U) |
            (instance.Texture.Format == rhi::RasterFormat::Rgba8Srgb ? LUDUS_GPU_PREVIEW_TEXTURE_SRGB_FLAG : 0U);
        surfaceWords[LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD] =
        {
            .X = GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE])),
            .Y = GpuBits(flags),
            .Z = GpuBits(surface.Alpha == MaterialAlpha::Masked ? 1U : 0U),
            .W = GpuBits(surface.IsEmissionTwoSided == true ? 1U : 0U),
        };
        surfaceWords[LUDUS_GPU_PREVIEW_SURFACE_MAP_WORD] =
        {
            .X = surface.NormalScale,
            .Y = GpuBits(instance.SurfaceMap.Width),
            .Z = GpuBits(instance.SurfaceMap.Height),
            .W = GpuBits(surface.HasSurfaceMap == true ? 1U : 0U),
        };
        surfaceWords[LUDUS_GPU_PREVIEW_SURFACE_OPTIONS_WORD] =
        {
            .X = GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE + 1])),
            .Y = GpuBits(flags & LUDUS_GPU_PREVIEW_TEXTURE_SAMPLER_FLAGS),
            .Z = GpuBits(surface.AreAuthoredInputsEnabled == true ? 1U : 0U),
            .W = 0,
        };
        surfaceWords[LUDUS_GPU_PREVIEW_EMISSION_MAP_WORD] =
        {
            .X = GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE + 2])),
            .Y = GpuBits((flags & LUDUS_GPU_PREVIEW_TEXTURE_SAMPLER_FLAGS) |
                         (instance.EmissionMap.Format == rhi::RasterFormat::Rgba8Srgb
                              ? LUDUS_GPU_PREVIEW_TEXTURE_SRGB_FLAG
                              : 0U)),
            .Z = GpuBits(instance.EmissionMap.Width),
            .W = GpuBits(instance.EmissionMap.Height),
        };
    }
    for (usize i = 0; i < textureCount; ++i)
    {
        const rhi::TextureCpuContent& image = getImage(i);
        if (image.Bytes == nullptr)
        {
            continue;
        }
        bool isShared = false;
        for (usize j = 0; j < i; ++j)
        {
            isShared = isShared || (textures[j] == textures[i] && getImage(j).Bytes != nullptr);
        }
        if (isShared)
        {
            continue;
        }
        for (usize y = 0; y < image.Height; ++y)
        {
            for (usize x = 0; x < image.Width; ++x)
            {
                uint32 word = 0;
                if (foundation::TryReadLittleEndian(
                        {image.Bytes + y * image.RowPitch + x * RGBA8_TEXEL_BYTES, RGBA8_TEXEL_BYTES},
                        word) == false)
                {
                    return Status::InvalidDescription;
                }
                const usize pixel = y * image.Width + x;
                math::Vector4& v = texels[textures[i] + pixel / 4];
                const float32 bits = GpuBits(word);
                switch (pixel % 4)
                {
                    case 0:
                        v.X = bits;
                        break;
                    case 1:
                        v.Y = bits;
                        break;
                    case 2:
                        v.Z = bits;
                        break;
                    default:
                        v.W = bits;
                        break;
                }
            }
        }
    }
    for (usize i = 0; i < job.Description.LightCount; ++i)
    {
        const DirectLight& light = job.Lights[i];
        math::Vector4* outLight = data.GetData() + static_cast<usize>(lightsWide) + i * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
        outLight[0] = GpuVector(light.Color * light.Intensity, static_cast<float32>(light.Kind));
        outLight[1] = GpuVector(light.Position, light.Range);
        outLight[2] = GpuVector(light.Direction, light.OuterCos);
        outLight[LUDUS_GRAPH_LIGHT_SOURCE_WORD] = {
            light.InnerCos,
            GpuBits(static_cast<uint32>(light.SourceId)),
            GpuBits(static_cast<uint32>(light.SourceId >> LUDUS_GRAPH_SOURCE_HIGH_SHIFT)),
            0};
    }
    outUniform.ClipToWorld = job.ClipToWorld;
    outUniform.CameraPositionWorld =
        GpuVector(job.Description.CameraPosition, job.Description.OrthographicCamera ? 1.0F : 0.0F);
    outUniform.Dimensions[0] = job.Description.Width;
    outUniform.Dimensions[1] = job.Description.Height;
    outUniform.Dimensions[2] = job.Description.MaxDepth;
    constexpr usize FOURTH_COMPONENT = 3;
    constexpr usize RANDOM_KEY_WORD_BITS = 32;
    constexpr uint32 CENTER_RAYS_FLAG = 4;
    outUniform.Dimensions[FOURTH_COMPONENT] = job.Description.RouletteDepth;
    outUniform.Offsets[0] = static_cast<uint32>(nodes);
    outUniform.Offsets[1] = static_cast<uint32>(order);
    outUniform.Offsets[2] = static_cast<uint32>(emitters);
    outUniform.Offsets[FOURTH_COMPONENT] = static_cast<uint32>(surfaces);
    if (job.EnvironmentOrNull != nullptr)
    {
        const internal::EnvironmentData& environment = *job.EnvironmentOrNull;
        const usize start = static_cast<usize>(environmentWide);
        data[start] = {GpuBits(environment.Width), GpuBits(environment.Height), 0, 0};
        for (usize pixel = 0; pixel < environment.Pixels.GetSize(); ++pixel)
        {
            data[start + 1 + pixel] = environment.Pixels[pixel];
            data[start + 1 + pixel].W =
                environment.Weight > 0
                    ? static_cast<float32>(environment.Distribution[pixel] / environment.Weight)
                    : static_cast<float32>(pixel + 1) / static_cast<float32>(environment.Pixels.GetSize());
        }
        outUniform.SurfaceLayout[FOURTH_COMPONENT] =
            static_cast<uint32>(environmentWide) + 1; // Zero denotes absent; offset zero remains representable.
    }
    outUniform.Counts[0] = static_cast<uint32>(scene.Triangles.GetSize());
    outUniform.Counts[1] = static_cast<uint32>(scene.Nodes.GetSize());
    outUniform.Counts[2] = static_cast<uint32>(scene.Emitters.GetSize());
    outUniform.Counts[FOURTH_COMPONENT] = static_cast<uint32>(job.Description.LightCount);
    outUniform.Sampling[0] = static_cast<uint32>(job.Key.Value);
    outUniform.Sampling[1] = static_cast<uint32>(job.Key.Value >> RANDOM_KEY_WORD_BITS);
    outUniform.Sampling[2] = (job.Description.ReverseZ ? 1U : 0U) | (job.Description.InfiniteFar ? 2U : 0U) |
                             (job.Description.CenterRays == true ? CENTER_RAYS_FLAG : 0U);
    bool isOpaque = true;
    for (usize i = 0; i < scene.InstanceCount; ++i)
    {
        isOpaque = isOpaque == true && scene.Instances[i].Surface->Alpha == MaterialAlpha::Opaque;
    }
    outUniform.Sampling[2] |= isOpaque == true ? LUDUS_GPU_PREVIEW_OPAQUE_FLAG : 0;
    uint32 nativeOpaqueTriangleCount = 0;
    if (shouldPartitionNativeOpacity == true && scene.Order.IsEmpty() == false)
    {
        std::memcpy(&nativeOpaqueTriangleCount, &data[order].Y, sizeof(nativeOpaqueTriangleCount));
    }
    if (isOpaque == false && nativeOpaqueTriangleCount != 0)
    {
        outUniform.Sampling[2] |= LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG;
    }
    outUniform.Sampling[FOURTH_COMPONENT] = job.Description.TargetSamples;
    outUniform.Environment =
        GpuVector(job.Description.Environment, GpuBits(static_cast<uint32>(job.ShaderEmitters.GetSize())));
    outUniform.Options.Z = GpuBits(static_cast<uint32>(scene.InstanceCount));
    outUniform.SurfaceLayout[0] = LUDUS_GPU_PREVIEW_SURFACE_WORDS;
    outUniform.SurfaceLayout[1] = static_cast<uint32>(lightsWide);
    outUniform.SurfaceLayout[2] = LUDUS_GPU_PREVIEW_KERNEL_VERSION;
    outUniform.Options.X = job.Description.SecondaryDistance;
    outUniform.Options.W = GpuBits(job.SceneData);
    outUniform.BoundsMinimumWorld = GpuVector(scene.Bounds.Min, GpuBits(static_cast<uint32>(job.SourceGeneration)));
    outUniform.BoundsMaximumWorld =
        GpuVector(scene.Bounds.Max,
                  GpuBits(static_cast<uint32>(job.SourceGeneration >> LUDUS_GRAPH_SOURCE_HIGH_SHIFT)));
    return Status::Ready;
}
Status PollGpuJob(rhi::DeviceHandle device, GpuPreviewState& inoutState, GpuPreviewRecord& inoutJob) noexcept
{
    if (inoutJob.Setup == Status::Pending)
    {
        const rhi::BufferHandle buffers[]{inoutJob.Data,
                                          inoutJob.Texels,
                                          inoutJob.Parameters,
                                          inoutJob.Accumulation,
                                          inoutJob.Colors,
                                          inoutJob.ImageParameters};
        for (rhi::BufferHandle buffer : buffers)
        {
            const Status status = rhi::GetStatus(device, buffer);
            if (status != Status::Ready)
            {
                return Accepted(status) ? Status::Pending : (inoutJob.Setup = status);
            }
        }
        const Status storageStatus = inoutJob.Storage.Poll();
        if (storageStatus != Status::Ready)
        {
            return Accepted(storageStatus) == true ? Status::Pending : (inoutJob.Setup = storageStatus);
        }
        if (inoutState.HasNativeRayQueries == true)
        {
            if (inoutJob.IsNativeSceneCreated == false)
            {
                if (inoutJob.HasNativeGeometry == true)
                {
                    const Status geometryStatus = rhi::GetStatus(device, inoutJob.NativeGeometry);
                    if (geometryStatus != Status::Ready)
                    {
                        return Accepted(geometryStatus) == true ? Status::Pending : (inoutJob.Setup = geometryStatus);
                    }
                }
                const rhi::RayInstanceDescription instance
                {
                    .Geometry = inoutJob.NativeGeometry,
                    .AlphaMasked = (inoutJob.Uniform.Sampling[2] & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) == 0,
                };
                const rhi::RaySceneDescription description
                {
                    .Instances = inoutJob.HasNativeGeometry == true ? &instance : nullptr,
                    .Count = inoutJob.HasNativeGeometry == true ? usize{1} : usize{0},
                    .ByteBudget = inoutState.Options.GpuByteBudget - inoutState.GpuBytes,
                };
                const Status sceneStatus = rhi::CreateRayScene(device, description, inoutJob.NativeScene);
                if (Accepted(sceneStatus) == false)
                {
                    return inoutJob.Setup = sceneStatus;
                }
                inoutJob.IsNativeSceneCreated = true;
                rhi::RayAllocation allocation;
                const Status allocationStatus = rhi::GetRayAllocation(device, inoutJob.NativeScene, allocation);
                if (allocationStatus != Status::Ready)
                {
                    return inoutJob.Setup = allocationStatus;
                }
                const usize nativeBytes = allocation.ResidentBytes + allocation.ScratchBytes;
                inoutJob.GpuBytes += nativeBytes;
                inoutState.GpuBytes += nativeBytes;
            }
            const Status sceneStatus = rhi::GetStatus(device, inoutJob.NativeScene);
            if (sceneStatus != Status::Ready)
            {
                return Accepted(sceneStatus) == true ? Status::Pending : (inoutJob.Setup = sceneStatus);
            }
        }
        if (inoutJob.ComputeBindingsCreated == false &&
            (inoutState.HasVisibilityBuffer == false || rhi::IsNull(inoutJob.VisibilityTexture) == false))
        {
            const usize pixels = usize(inoutJob.Width) * inoutJob.Height;
            StaticArray<rhi::RasterBindingResource, internal::GPU_PREVIEW_COMPUTE_BINDING_CAPACITY> bindings{
                {{0, inoutJob.Parameters, 0, sizeof(internal::GpuPreviewUniform)},
                 {1, inoutJob.Data, 0, inoutJob.DataBytes},
                 {2, inoutJob.Accumulation, 0, pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES},
                 {LUDUS_GPU_PREVIEW_COLORS_BINDING, inoutJob.Colors, 0, pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES},
                 {LUDUS_GPU_PREVIEW_TEXELS_BINDING, inoutJob.Texels, 0, inoutJob.TexelBytes},
                 { .Binding = LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING, .Scene = inoutJob.NativeScene }}};
            const Status created = [&]() noexcept {
                const usize baseBindingCount =
                    LUDUS_GPU_PREVIEW_BINDING_COUNT - (inoutState.HasNativeRayQueries == true ? 0 : 1);
                usize count =
                    baseBindingCount + (inoutState.HasPersistentBuffers == true ? internal::GPU_GRAPH_BUFFER_COUNT
                                                                                : inoutJob.Storage.GetCount());
                for (usize index = 0; index < inoutJob.Storage.GetCount(); ++index)
                {
                    bindings[baseBindingCount + index] = inoutJob.Storage.GetBinding(index);
                }
                for (usize slot = 0; inoutState.HasPersistentBuffers == true && slot < internal::GPU_GRAPH_BUFFER_COUNT;
                     ++slot)
                {
                    const GpuPreviewGraphBuffer& buffer = inoutJob.GraphBuffers[slot];
                    const Status ready = rhi::GetStatus(device, buffer.Buffer);
                    if (ready != Status::Ready)
                    {
                        return ready;
                    }
                    bindings[baseBindingCount + slot] = {static_cast<uint32>(LUDUS_GRAPH_BUFFER0_BINDING + slot),
                                                         buffer.Buffer,
                                                         0,
                                                         buffer.Size};
                }
                if (inoutState.HasVisibilityBuffer == true)
                {
                    bindings[count++] =
                    {
                        .Binding = LUDUS_GRAPH_VISIBILITY_BINDING,
                        .Texture = inoutJob.VisibilityView,
                    };
                    bindings[count++] =
                    {
                        .Binding = LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING,
                        .Texture = inoutJob.VisibilityDepthView,
                    };
                }
                if (inoutState.HasRayPathCapture == true)
                {
                    const Status ready = rhi::GetStatus(device, inoutJob.RayPathCapture.Buffer);
                    if (ready != Status::Ready)
                    {
                        return ready;
                    }
                    bindings[count++] = {LUDUS_RAY_PATH_CAPTURE_BINDING,
                                         inoutJob.RayPathCapture.Buffer,
                                         0,
                                         inoutJob.RayPathCapture.Size};
                }
                if (inoutState.HasLtcTable == true)
                {
                    bindings[count++] = {LUDUS_GRAPH_LTC_TABLE_BINDING,
                                         inoutState.LtcTable,
                                         0,
                                         sizeof(internal::LTC_TABLE)};
                }
                return rhi::CreateBindingSet(device,
                                             inoutState.ComputeLayout,
                                             {bindings.GetData(), count},
                                             inoutJob.ComputeBindings);
            }();
            if (Accepted(created) == false)
            {
                return inoutJob.Setup = created;
            }
            inoutJob.ComputeBindingsCreated = true;
        }
        if (inoutJob.ImageBindingsCreated == false)
        {
            constexpr usize IMAGE_BINDING_COUNT = 2;
            const StaticArray<rhi::RasterBindingResource, IMAGE_BINDING_COUNT> bindings{
                {{0, inoutJob.ImageParameters, 0, sizeof(math::Vector4)},
                 {1, inoutJob.Colors, 0, usize(inoutJob.Width) * inoutJob.Height * sizeof(math::Vector4)}}};
            const Status created =
                rhi::CreateBindingSet(device, inoutState.ImageLayout, bindings.AsSpan(), inoutJob.ImageBindings);
            if (Accepted(created) == false)
            {
                return inoutJob.Setup = created;
            }
            inoutJob.ImageBindingsCreated = true;
        }
        Status status =
            inoutJob.ComputeBindingsCreated == true ? rhi::GetStatus(device, inoutJob.ComputeBindings) : Status::Ready;
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, inoutJob.ImageBindings);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, inoutJob.Frames[0].Texture);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, inoutJob.Frames[1].Texture);
        }
        if (status != Status::Ready)
        {
            return Accepted(status) ? Status::Pending : (inoutJob.Setup = status);
        }
        if (inoutJob.DenoisingQuality != 0)
        {
            const Status denoising = inoutJob.Denoiser.Prepare(device,
                                                               inoutJob.Colors,
                                                               inoutJob.Width,
                                                               inoutJob.Height,
                                                               inoutJob.DenoisingQuality);
            if (denoising != Status::Ready)
            {
                return inoutJob.Setup = denoising;
            }
        }
        inoutJob.Setup = Status::Ready;
    }
    if (inoutJob.Setup != Status::Ready)
    {
        return inoutJob.Setup;
    }
    if (inoutJob.Info.Pending)
    {
        const Status status = rhi::GetStatus(device, inoutJob.Frames[inoutJob.PendingFrame].Producer);
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            inoutJob.Setup = status;
            return status;
        }
        inoutJob.Info.Pending = false;
        ++inoutJob.Info.CompletedSamples;
        inoutJob.LatestFrame = inoutJob.PendingFrame;
        inoutJob.HasImage = true;
        inoutJob.Frames[inoutJob.LatestFrame].Samples = inoutJob.Info.CompletedSamples;
        inoutJob.Info.Complete = inoutJob.Info.CompletedSamples == inoutJob.Info.TargetSamples;
    }
    return Status::Ready;
}
// Create one job's views of the producer's color/depth image; Ready once its compute bindings read them.
Status PrepareVisibilityInput(rhi::DeviceHandle device,
                              GpuPreviewState& inoutState,
                              GpuPreviewRecord& inoutJob,
                              const VisibilityRasterPass& visibility) noexcept
{
    if (visibility.ProducerOrNull == nullptr || rhi::IsNull(visibility.Attachment.Color) == true)
    {
        return Status::InvalidDescription;
    }
    rhi::TextureDescription inputDescription;
    Status status = rhi::GetTextureDescription(device, visibility.Attachment.Color, inputDescription);
    if (status != Status::Ready || inputDescription.Width != inoutJob.Width ||
        inputDescription.Height != inoutJob.Height || inputDescription.Attachment == false ||
        inputDescription.Format != rhi::RasterFormat::Rgba8Unorm || inputDescription.SampleCount != 1)
    {
        return status == Status::Ready ? Status::InvalidDescription : status;
    }
    if (rhi::IsNull(inoutJob.VisibilityTexture) == true)
    {
        rhi::TextureViewHandle colorView;
        rhi::TextureViewHandle depthView;
        status = rhi::CreateTextureView(device, visibility.Attachment.Color, colorView);
        if (Accepted(status) == true)
        {
            rhi::TextureViewDescription depth;
            depth.Aspect = rhi::TextureAspect::Depth;
            status = rhi::CreateTextureView(device, visibility.Attachment.Color, depth, depthView);
        }
        if (Accepted(status) == false)
        {
            static_cast<void>(rhi::Destroy(device, colorView));
            static_cast<void>(rhi::Destroy(device, depthView));
            return status;
        }
        inoutJob.VisibilityTexture = visibility.Attachment.Color;
        inoutJob.VisibilityView = colorView;
        inoutJob.VisibilityDepthView = depthView;
        inoutJob.Setup = Status::Pending;
    }
    status = rhi::GetStatus(device, inoutJob.VisibilityView);
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutJob.VisibilityDepthView);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    return PollGpuJob(device, inoutState, inoutJob);
}
} // namespace
void Renderer::State::DropGpuPreview(usize slot) noexcept
{
    GpuPreviewRecord& job = GpuPreview->Jobs[slot];
    job.CameraUpdate.Reset();
    job.Denoiser.Reset();
    job.DenoisingQuality = 0;
    job.HasPendingCamera = false;
    static_cast<void>(rhi::Destroy(Device, job.ComputeBindings));
    job.RayPathCapture = {};
    for (usize bufferIndex = 0; bufferIndex < internal::GPU_GRAPH_BUFFER_COUNT; ++bufferIndex)
    {
        if (job.HasOwnedGraphBuffers[bufferIndex] == true)
        {
            static_cast<void>(rhi::Destroy(Device, job.GraphBuffers[bufferIndex].Buffer));
        }
        job.GraphBuffers[bufferIndex] = {};
        job.HasOwnedGraphBuffers[bufferIndex] = false;
    }
    job.Storage.Reset();
    static_cast<void>(rhi::Destroy(Device, job.VisibilityView));
    static_cast<void>(rhi::Destroy(Device, job.VisibilityDepthView));
    job.VisibilityTexture = {};
    static_cast<void>(rhi::Destroy(Device, job.ImageBindings));
    static_cast<void>(rhi::Destroy(Device, job.NativeScene));
    if (job.Scene != internal::NO_GPU_PREVIEW_SCENE)
    {
        // The last referencing job releases the shared scene, once its own producers retired.
        internal::GpuPreviewSceneRecord& scene = GpuPreview->Scenes[job.Scene];
        scene.References -= 1;
        if (scene.References == 0)
        {
            static_cast<void>(rhi::Destroy(Device, scene.NativeGeometry));
            static_cast<void>(rhi::Destroy(Device, scene.Data));
            static_cast<void>(rhi::Destroy(Device, scene.Texels));
            GpuPreview->GpuBytes -= scene.GpuBytes;
            scene = {};
        }
        job.Scene = internal::NO_GPU_PREVIEW_SCENE;
    }
    job.NativeGeometry = {};
    job.Data = {};
    job.Texels = {};
    job.HasNativeGeometry = false;
    job.IsNativeSceneCreated = false;
    static_cast<void>(rhi::Destroy(Device, job.Parameters));
    static_cast<void>(rhi::Destroy(Device, job.Accumulation));
    static_cast<void>(rhi::Destroy(Device, job.Colors));
    static_cast<void>(rhi::Destroy(Device, job.ImageParameters));
    for (internal::GpuFrameRecord& frame : job.Frames)
    {
        static_cast<void>(rhi::Destroy(Device, frame.Texture));
        frame.Owned = false;
        frame.Producer = {};
        if (frame.Generation != LAST_ID)
        {
            ++frame.Generation;
        }
    }
    GpuPreview->GpuBytes -= job.GpuBytes;
    job.GpuBytes = 0;
    job.ComputeBindingsCreated = job.ImageBindingsCreated = false;
    job.Used = job.Owned = false;
    if (job.Generation != LAST_ID)
    {
        ++job.Generation;
    }
}
void Renderer::State::RetireGpuPreviews() noexcept
{
    for (usize i = 0; i < internal::GPU_PREVIEW_JOB_COUNT; ++i)
    {
        GpuPreviewRecord& job = GpuPreview->Jobs[i];
        if (job.Used && job.Owned == false && job.Frames[0].Owned == false && job.Frames[1].Owned == false)
        {
            const Status status = PollGpuJob(Device, *GpuPreview, job);
            if (status != Status::Pending && status != Status::NotReady)
            {
                DropGpuPreview(i);
            }
        }
    }
}
void Renderer::State::ClearGpuPreview() noexcept
{
    if (GpuPreview.Get() == nullptr)
    {
        return;
    }
    for (usize i = 0; i < internal::GPU_PREVIEW_JOB_COUNT; ++i)
    {
        if (GpuPreview->Jobs[i].Used)
        {
            DropGpuPreview(i);
        }
    }
    internal::GpuPreviewState& state = *GpuPreview;
    static_cast<void>(rhi::Destroy(Device, state.ComputePipeline));
    static_cast<void>(rhi::Destroy(Device, state.NativeRayPipeline));
    static_cast<void>(rhi::Destroy(Device, state.NativeRayShader));
    static_cast<void>(rhi::Destroy(Device, state.ImagePipeline));
    static_cast<void>(rhi::Destroy(Device, state.ComputeLayout));
    static_cast<void>(rhi::Destroy(Device, state.ImageLayout));
    static_cast<void>(rhi::Destroy(Device, state.ComputeShader));
    static_cast<void>(rhi::Destroy(Device, state.VertexShader));
    static_cast<void>(rhi::Destroy(Device, state.FragmentShader));
    static_cast<void>(rhi::Destroy(Device, state.Quad));
    static_cast<void>(rhi::Destroy(Device, state.Indices));
    static_cast<void>(rhi::Destroy(Device, state.LtcTable));
    GpuPreview.Reset();
}
Status Renderer::EnableGpuPreview(const rhi::RasterShaderDescription& compute,
                                  const rhi::RasterShaderDescription& vertex,
                                  const rhi::RasterShaderDescription& fragment,
                                  const GpuPreviewOptions& options) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (mState->GpuPreview || mState->Reference)
    {
        return Status::InvalidState;
    }
    Status status = RendererAdmission(mState->Device);
    if (status != Status::Ready)
    {
        return status;
    }
    if (options.CpuByteBudget <= sizeof(GpuPreviewState) || options.GpuByteBudget < internal::GPU_PREVIEW_QUAD_BYTES ||
        math::IsFinite(options.Exposure) == false || options.Exposure <= 0 ||
        options.Exposure > MAX_GPU_PREVIEW_EXPOSURE)
    {
        return Status::InvalidDescription;
    }
    rhi::ComputeCapabilities caps;
    status = rhi::GetComputeCapabilities(mState->Device, caps);
    if (status != Status::Ready)
    {
        return status;
    }
    // A kernel reading the renderer's LTC table declares it last, after every other slot.
    const rhi::RasterBinding* ltcTableBindingOrNull =
        compute.Bindings.empty() == false && compute.Bindings.back().Binding == LUDUS_GRAPH_LTC_TABLE_BINDING
            ? &compute.Bindings.back()
            : nullptr;
    if (ltcTableBindingOrNull != nullptr &&
        (ltcTableBindingOrNull->Kind != rhi::RasterBindingKind::StorageRead ||
         ltcTableBindingOrNull->Visibility != rhi::RasterVisibility::Compute ||
         ltcTableBindingOrNull->MinSize != LUDUS_GPU_PREVIEW_VECTOR_BYTES ||
         ltcTableBindingOrNull->HasDynamicOffset == true || ltcTableBindingOrNull->Count != 1 ||
         ltcTableBindingOrNull->IsRuntimeSized == true || ltcTableBindingOrNull->HasPartialBinding == true ||
         ltcTableBindingOrNull->Indexing != rhi::RasterBindingIndexing::Constant))
    {
        return Status::InvalidDescription;
    }
    const std::span<const rhi::RasterBinding> leadingBindings = compute.Bindings.first(
        compute.Bindings.size() - (ltcTableBindingOrNull != nullptr ? LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT : 0));
    // A diagnostic capture-variant kernel declares its capture buffer after every frozen ABI slot.
    const rhi::RasterBinding* captureBindingOrNull =
        leadingBindings.empty() == false && leadingBindings.back().Binding == LUDUS_RAY_PATH_CAPTURE_BINDING
            ? &leadingBindings.back()
            : nullptr;
    if (captureBindingOrNull != nullptr &&
        (captureBindingOrNull->Kind != rhi::RasterBindingKind::StorageReadWrite ||
         captureBindingOrNull->Visibility != rhi::RasterVisibility::Compute ||
         captureBindingOrNull->MinSize != LUDUS_RAY_PATH_CAPTURE_WORD_BYTES ||
         captureBindingOrNull->HasDynamicOffset == true || captureBindingOrNull->Count != 1 ||
         captureBindingOrNull->IsRuntimeSized == true || captureBindingOrNull->HasPartialBinding == true ||
         captureBindingOrNull->Indexing != rhi::RasterBindingIndexing::Constant))
    {
        return Status::InvalidDescription;
    }
    const std::span<const rhi::RasterBinding> kernelBindings =
        leadingBindings.first(leadingBindings.size() - (captureBindingOrNull != nullptr ? 1 : 0));
    bool hasNativeRayQueries = false;
    for (const rhi::RasterBinding& binding : kernelBindings)
    {
        hasNativeRayQueries = hasNativeRayQueries == true || binding.Kind == rhi::RasterBindingKind::RayScene;
    }
    if (hasNativeRayQueries == true)
    {
        rhi::RayCapabilities rayCapabilities;
        status = rhi::GetRayCapabilities(mState->Device, rayCapabilities);
        if (status != Status::Ready || rayCapabilities.StaticTriangles == false ||
            rayCapabilities.InlineQueries == false)
        {
            return status == Status::Ready ? Status::Unsupported : status;
        }
    }
    const usize baseBindingCount = GPU_COMPUTE_BINDINGS.GetSize() - (hasNativeRayQueries == true ? 0 : 1);
    // Frozen kernel ABI: consume independently cooked target reflection, never invent it.
    namespace format = content::render_graph_format;
    const bool hasVisibilityBuffer =
        kernelBindings.size() >= baseBindingCount + LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT &&
        kernelBindings[kernelBindings.size() - LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT].Kind ==
            rhi::RasterBindingKind::Texture2D;
    const usize visibilityBindingCount = hasVisibilityBuffer == true ? LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT : 0;
    const usize bufferBindingCount = kernelBindings.size() - visibilityBindingCount;
    const usize storageBindingCount = baseBindingCount + options.StorageBuffers.GetSize();
    const bool hasPersistentBuffers = options.StorageBuffers.IsEmpty() == true &&
                                      bufferBindingCount == baseBindingCount + internal::GPU_GRAPH_BUFFER_COUNT;
    if (options.StorageBuffers.IsValid() == false || options.StorageBuffers.GetSize() > format::MAX_STORAGE_BUFFERS ||
        (hasPersistentBuffers == false && bufferBindingCount != storageBindingCount) || compute.WorkgroupSize[0] == 0 ||
        compute.WorkgroupSize[1] == 0 || compute.WorkgroupSize[2] != 1)
    {
        return Status::InvalidDescription;
    }
    if (compute.WorkgroupSize[0] > caps.MaxWorkgroupSize[0] || compute.WorkgroupSize[1] > caps.MaxWorkgroupSize[1] ||
        uint64(compute.WorkgroupSize[0]) * compute.WorkgroupSize[1] > caps.MaxWorkgroupInvocations)
    {
        return Status::Unsupported;
    }
    for (usize i = 0; i < baseBindingCount; ++i)
    {
        const ludus::graphics::rhi::RasterBinding& a = kernelBindings[i];
        const rhi::RasterBinding& b = GPU_COMPUTE_BINDINGS[i];
        if (a.Binding != b.Binding || a.Kind != b.Kind || a.Visibility != b.Visibility || a.MinSize != b.MinSize)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize index = baseBindingCount; hasPersistentBuffers == true && index < bufferBindingCount; ++index)
    {
        const rhi::RasterBinding& binding = kernelBindings[index];
        if (binding.Binding != LUDUS_GRAPH_BUFFER0_BINDING + index - baseBindingCount ||
            binding.Kind != rhi::RasterBindingKind::StorageReadWrite ||
            binding.Visibility != rhi::RasterVisibility::Compute || binding.MinSize != LUDUS_GPU_PREVIEW_VECTOR_BYTES)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize i = 0; i < options.StorageBuffers.GetSize(); ++i)
    {
        const format::StorageBuffer& storage = options.StorageBuffers[i];
        const rhi::RasterBinding& binding = kernelBindings[baseBindingCount + i];
        constexpr uint32 WORD_BYTES = sizeof(uint32);
        if (storage.Slot < format::FIRST_STORAGE_SLOT ||
            storage.Slot >= format::FIRST_STORAGE_SLOT + format::MAX_STORAGE_BUFFERS || storage.Stride == 0 ||
            storage.Stride > format::MAX_STORAGE_STRIDE || storage.Stride % WORD_BYTES != 0 ||
            storage.Count > format::MAX_STORAGE_ELEMENTS || storage.IsReadOnly > 1 ||
            (i != 0 && options.StorageBuffers[i - 1].Slot >= storage.Slot) || binding.Binding != storage.Slot ||
            binding.MinSize != storage.Stride ||
            binding.Kind != (storage.IsReadOnly == 1 ? rhi::RasterBindingKind::StorageRead
                                                     : rhi::RasterBindingKind::StorageReadWrite) ||
            binding.Visibility != rhi::RasterVisibility::Compute || binding.HasDynamicOffset == true ||
            binding.Count != 1 || binding.IsRuntimeSized == true || binding.HasPartialBinding == true ||
            binding.Indexing != rhi::RasterBindingIndexing::Constant)
        {
            return Status::InvalidDescription;
        }
    }
    if (hasVisibilityBuffer == true)
    {
        for (usize i = 0; i < LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT; ++i)
        {
            const rhi::RasterBinding& actual = kernelBindings[bufferBindingCount + i];
            const rhi::RasterBinding& expected = GPU_VISIBILITY_BINDINGS[LUDUS_GPU_PREVIEW_BINDING_COUNT + i];
            if (actual.Binding != expected.Binding || actual.Kind != expected.Kind ||
                actual.Visibility != expected.Visibility || actual.MinSize != expected.MinSize ||
                actual.HasDynamicOffset == true || actual.Count != 1 || actual.IsRuntimeSized == true ||
                actual.HasPartialBinding == true || actual.Indexing != rhi::RasterBindingIndexing::Constant)
            {
                return Status::InvalidDescription;
            }
        }
    }
    if (vertex.Inputs.size() != 1 || vertex.Inputs[0].Location != 0 ||
        vertex.Inputs[0].Format != rhi::RasterVertexFormat::Float2 || vertex.Bindings.empty() == false ||
        fragment.Bindings.size() != 2)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < fragment.Bindings.size(); ++i)
    {
        const rhi::RasterBinding& expected = GPU_IMAGE_BINDINGS[i];
        const rhi::RasterBinding& actual = fragment.Bindings[i];
        if (actual.Binding != expected.Binding || actual.Kind != expected.Kind ||
            actual.Visibility != expected.Visibility || actual.MinSize != expected.MinSize)
        {
            return Status::InvalidDescription;
        }
    }
    GpuPreviewState* allocation = new (std::nothrow) GpuPreviewState;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<GpuPreviewState> state(allocation);
    state->Options = options;
    state->HasPersistentBuffers = hasPersistentBuffers;
    state->HasNativeRayQueries = hasNativeRayQueries;
    for (usize i = 0; i < options.StorageBuffers.GetSize(); ++i)
    {
        state->StorageDescriptions[i] = options.StorageBuffers[i];
    }
    state->Options.NativeRayStagesOrNull = nullptr;
    state->Options.StorageBuffers = {state->StorageDescriptions.GetData(), options.StorageBuffers.GetSize()};
    state->WorkgroupWidth = compute.WorkgroupSize[0];
    state->WorkgroupHeight = compute.WorkgroupSize[1];
    state->ComputeBindingCount = compute.Bindings.size();
    for (usize i = 0; i < compute.Bindings.size(); ++i)
    {
        state->ComputeBindings[i] = compute.Bindings[i];
    }
    state->HasVisibilityBuffer = hasVisibilityBuffer;
    state->HasRayPathCapture = captureBindingOrNull != nullptr;
    state->HasLtcTable = ltcTableBindingOrNull != nullptr;
    if (state->HasLtcTable == true &&
        options.GpuByteBudget - internal::GPU_PREVIEW_QUAD_BYTES < sizeof(internal::LTC_TABLE))
    {
        return Status::CapacityExceeded;
    }
    status = EnableReferenceRendering(
        {},
        {ReferenceTraversal::CpuOracle, options.CpuByteBudget - sizeof(GpuPreviewState), 0, 1});
    if (status != Status::Ready)
    {
        return status;
    }
    mState->GpuPreview = core::Move(state);
    internal::GpuPreviewState& target = *mState->GpuPreview;
    if (options.NativeRayStagesOrNull != nullptr)
    {
        const rhi::RasterShaderDescription& native = *options.NativeRayStagesOrNull;
        if (native.Artifact.HasNativeRayStages == false || native.Artifact.Stage != rhi::ShaderStage::Compute ||
            native.WorkgroupSize[0] != compute.WorkgroupSize[0] ||
            native.WorkgroupSize[1] != compute.WorkgroupSize[1] || native.WorkgroupSize[2] != compute.WorkgroupSize[2])
        {
            mState->ClearGpuPreview();
            mState->ClearReference();
            return Status::InvalidDescription;
        }
        rhi::RayCapabilities rayCapabilities;
        status = rhi::GetRayCapabilities(mState->Device, rayCapabilities);
        if (status != Status::Ready || rayCapabilities.RayPipelines == false)
        {
            mState->ClearGpuPreview();
            mState->ClearReference();
            return Status::Unsupported;
        }
        status = rhi::CreateRasterShader(mState->Device, *options.NativeRayStagesOrNull, target.NativeRayShader);
        target.HasNativeRayShader = Accepted(status);
        if (Accepted(status) == false)
        {
            mState->ClearGpuPreview();
            mState->ClearReference();
            return status;
        }
    }
    status = rhi::CreateRasterShader(mState->Device, compute, target.ComputeShader);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(mState->Device, vertex, target.VertexShader);
    }
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(mState->Device, fragment, target.FragmentShader);
    }
    if (Accepted(status))
    {
        status = PollGpuPreview();
    }
    if (Accepted(status) == false)
    {
        mState->ClearGpuPreview();
        mState->ClearReference();
    }
    return status;
}
Status Renderer::EnableTriangleRayPipeline(const rhi::RasterShaderDescription& compute,
                                           const rhi::RasterShaderDescription& vertex,
                                           const rhi::RasterShaderDescription& fragment,
                                           const GpuPreviewOptions& options) noexcept
{
    const Status status = EnableGpuPreview(compute, vertex, fragment, options);
    if (status == Status::Ready || status == Status::Pending)
    {
        mState->GpuPreview->IsGraphRayScene = true;
    }
    return status;
}
Status Renderer::PollGpuPreview() noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::NotReady;
    }
    Status status = RendererAdmission(mState->Device);
    if (status != Status::Ready)
    {
        return status;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    if (state.Setup != Status::Pending)
    {
        if (state.Setup == Status::Ready)
        {
            mState->RetireGpuPreviews();
        }
        return state.Setup;
    }
    const rhi::RasterVertexStream streams[]{{sizeof(math::Vector2), false}};
    const rhi::RasterVertexAttribute attributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float2}};
    for (;;)
    {
        bool created = false;
        switch (state.Phase)
        {
            case 0:
                if (state.HasNativeRayShader == true)
                {
                    status = rhi::GetStatus(mState->Device, state.NativeRayShader);
                    if (status != Status::Ready)
                    {
                        break;
                    }
                }
                status = rhi::GetStatus(mState->Device, state.ComputeShader);
                if (status == Status::Ready)
                {
                    status = rhi::GetStatus(mState->Device, state.VertexShader);
                }
                if (status == Status::Ready)
                {
                    status = rhi::GetStatus(mState->Device, state.FragmentShader);
                }
                if (status == Status::Ready)
                {
                    status = rhi::CreateBindingLayout(mState->Device,
                                                      {state.ComputeBindings.GetData(), state.ComputeBindingCount},
                                                      state.ComputeLayout);
                    created = Accepted(status);
                }
                break;
            case 1:
                status = rhi::GetStatus(mState->Device, state.ComputeLayout);
                if (status == Status::Ready)
                {
                    status = rhi::CreateBindingLayout(mState->Device, GPU_IMAGE_BINDINGS, state.ImageLayout);
                    created = Accepted(status);
                }
                break;
            case 2:
                status = rhi::GetStatus(mState->Device, state.ImageLayout);
                if (status == Status::Ready)
                {
                    status = rhi::CreateComputePipeline(mState->Device,
                                                        {state.ComputeShader, state.ComputeLayout},
                                                        state.ComputePipeline);
                    created = Accepted(status);
                }
                break;
            case 3:
                if (state.HasNativeRayShader == true)
                {
                    if (state.HasCreatedNativeRayPipeline == false)
                    {
                        status = rhi::CreateComputePipeline(mState->Device,
                                                            {state.NativeRayShader, state.ComputeLayout},
                                                            state.NativeRayPipeline);
                        state.HasCreatedNativeRayPipeline = Accepted(status);
                        if (status != Status::Ready)
                        {
                            break;
                        }
                    }
                    status = rhi::GetStatus(mState->Device, state.NativeRayPipeline);
                    if (status != Status::Ready)
                    {
                        break;
                    }
                }
                status = rhi::GetStatus(mState->Device, state.ComputePipeline);
                if (status == Status::Ready)
                {
                    status = rhi::CreateRasterPipeline(mState->Device,
                                                       {state.VertexShader,
                                                        state.FragmentShader,
                                                        state.ImageLayout,
                                                        streams,
                                                        attributes,
                                                        false,
                                                        false,
                                                        rhi::RasterTarget::Rgba8Unorm},
                                                       state.ImagePipeline);
                    created = Accepted(status);
                }
                break;
            default: {
                status = rhi::GetStatus(mState->Device, state.ImagePipeline);
                if (status != Status::Ready)
                {
                    break;
                }
                const math::Vector2 corners[]{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                const uint16 indices[]{0, 1, 2, 0, 2, 3};
                if (state.QuadCreated == false)
                {
                    status = rhi::CreateBuffer(mState->Device,
                                               {rhi::BufferRole::Vertex, sizeof(corners)},
                                               {reinterpret_cast<const uint8*>(corners), sizeof(corners)},
                                               state.Quad);
                    state.QuadCreated = Accepted(status);
                }
                if (Accepted(status) && state.IndicesCreated == false)
                {
                    status = rhi::CreateBuffer(mState->Device,
                                               {rhi::BufferRole::Index16, sizeof(indices)},
                                               {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                                               state.Indices);
                    state.IndicesCreated = Accepted(status);
                }
                if (Accepted(status) == true && state.HasLtcTable == true && state.IsLtcTableCreated == false)
                {
                    status = rhi::CreateBuffer(
                        mState->Device,
                        {rhi::BufferRole::Storage, sizeof(internal::LTC_TABLE)},
                        {reinterpret_cast<const uint8*>(internal::LTC_TABLE.GetData()), sizeof(internal::LTC_TABLE)},
                        state.LtcTable);
                    state.IsLtcTableCreated = Accepted(status);
                }
                if (Accepted(status))
                {
                    status = rhi::GetStatus(mState->Device, state.Quad);
                }
                if (status == Status::Ready)
                {
                    status = rhi::GetStatus(mState->Device, state.Indices);
                }
                if (status == Status::Ready && state.HasLtcTable == true)
                {
                    status = rhi::GetStatus(mState->Device, state.LtcTable);
                }
                if (status == Status::Ready)
                {
                    static_assert(sizeof(corners) + sizeof(indices) == internal::GPU_PREVIEW_QUAD_BYTES);
                    state.GpuBytes = internal::GPU_PREVIEW_QUAD_BYTES +
                                     (state.HasLtcTable == true ? sizeof(internal::LTC_TABLE) : 0);
                    state.Setup = Status::Ready;
                    return status;
                }
                break;
            }
        }
        if (Accepted(status) == false)
        {
            state.Setup = status;
            return status;
        }
        // A creator may publish Pending: phase advances only when its output exists.
        if (created)
        {
            ++state.Phase;
        }
        if (status == Status::Pending)
        {
            return status;
        }
    }
}
Status Renderer::BeginGpuPreview(Snapshot snapshot,
                                 const ReferenceCaptureDescription& description,
                                 renderer::GpuPreview& outOutput,
                                 const RenderGraphSceneDescription* graphSceneOrNull,
                                 const GpuPreviewGraphInvocation* invocationOrNull) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::NotReady;
    }
    Status status = PollGpuPreview();
    if (status != Status::Ready)
    {
        return status;
    }
    if (invocationOrNull != nullptr && invocationOrNull->HasNativeRayInvocation == true &&
        mState->GpuPreview->HasNativeRayShader == false)
    {
        return Status::Unsupported;
    }
    if (Access::Owner(outOutput) != 0)
    {
        return Status::InvalidState;
    }
    State::SnapshotRecord* source = Resolve(mState->SnapshotRecords, mState->Owner, snapshot);
    if (source == nullptr && graphSceneOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (graphSceneOrNull != nullptr && mState->GpuPreview->IsGraphRayScene == false)
    {
        return Status::InvalidState;
    }
    if (description.MaxDepth > LUDUS_GPU_PREVIEW_MAX_DEPTH ||
        description.TargetSamples > content::render_graph_format::MAX_ACCUMULATION_SAMPLES ||
        uint64(description.Width) * description.Height > (uint64{1} << 24))
    {
        return Status::Unsupported;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    if (invocationOrNull != nullptr &&
        (state.IsGraphRayScene == false ||
         (state.HasPersistentBuffers == false &&
          (invocationOrNull->Program != 0 || invocationOrNull->Buffers.IsEmpty() == false)) ||
         (invocationOrNull->ToneMappingExposure != 0 &&
          (invocationOrNull->ToneMappingExposure >= content::render_graph_format::MIN_TONE_MAPPING_EXPOSURE &&
           invocationOrNull->ToneMappingExposure <= content::render_graph_format::MAX_TONE_MAPPING_EXPOSURE) ==
              false) ||
         invocationOrNull->DenoisingQuality >
             static_cast<uint32>(content::render_graph_format::ProcessingQuality::High) ||
         invocationOrNull->AccumulationSamples > content::render_graph_format::MAX_ACCUMULATION_SAMPLES ||
         invocationOrNull->Program >= content::render_graph_format::MAX_RAY_PASSES ||
         invocationOrNull->Buffers.IsValid() == false ||
         (invocationOrNull->Buffers.GetSize() != 0 &&
          invocationOrNull->Buffers.GetSize() != internal::GPU_GRAPH_BUFFER_COUNT)))
    {
        return Status::InvalidDescription;
    }
    const bool hasRayPathCapture = invocationOrNull != nullptr && invocationOrNull->RayPathCapture.Size != 0 &&
                                   invocationOrNull->RayPathCapture.Buffer != rhi::BufferHandle{};
    if (hasRayPathCapture != state.HasRayPathCapture ||
        (hasRayPathCapture == true && invocationOrNull->RayPathCapture.Size % LUDUS_RAY_PATH_CAPTURE_WORD_BYTES != 0))
    {
        return Status::InvalidDescription;
    }
    if (invocationOrNull != nullptr && invocationOrNull->DenoisingQuality != 0 &&
        internal::IsGpuDenoisingAvailable() == false)
    {
        return Status::Unsupported;
    }
    usize sharedScene = internal::NO_GPU_PREVIEW_SCENE;
    if (invocationOrNull != nullptr && invocationOrNull->SceneSourceOrNull != nullptr)
    {
        const internal::GpuPreviewRecord* sceneSourceOrNull =
            Resolve(state.Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, *invocationOrNull->SceneSourceOrNull);
        if (graphSceneOrNull == nullptr || sceneSourceOrNull == nullptr ||
            sceneSourceOrNull->Scene == internal::NO_GPU_PREVIEW_SCENE)
        {
            return Status::InvalidHandle;
        }
        sharedScene = sceneSourceOrNull->Scene;
    }
    usize slot = 0;
    status = FreeSlot(state.Jobs, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    for (usize i = 0; source != nullptr && i < source->Count; ++i)
    {
        if (source->Items[i].Input.Pass == Layer::Overlay)
        {
            continue;
        }
        const usize material = source->Items[i].MaterialSlot;
        if (material != NO_MATERIAL &&
            (mState->Materials[material].HasBake || mState->Materials[material].ShadowSlot != NO_SHADOW))
        {
            return Status::Unsupported;
        }
    }
    core::UniquePtr<ReferenceJob> scene;
    SceneShaderDiagnostics localDiagnostics;
    SceneShaderDiagnostics* diagnosticsOrNull =
        graphSceneOrNull != nullptr && graphSceneOrNull->DiagnosticsOrNull != nullptr
            ? graphSceneOrNull->DiagnosticsOrNull
            : &localDiagnostics;
    if (graphSceneOrNull != nullptr)
    {
        status =
            internal::BuildMaterialGraphRayScene(*graphSceneOrNull,
                                                 description,
                                                 mState->Reference->Options.CpuByteBudget - mState->Reference->CpuBytes,
                                                 scene);
        if (status == Status::Ready && ReferenceFingerprint(*scene,
                                                            mState->Reference->Options.CpuByteBudget -
                                                                mState->Reference->CpuBytes - scene->Info.CpuBytes,
                                                            diagnosticsOrNull) == false)
        {
            return diagnosticsOrNull != nullptr && diagnosticsOrNull->ExceededBudget != SceneMemoryBudget::None
                       ? Status::CapacityExceeded
                       : Status::OutOfMemory;
        }
    }
    else if (state.IsGraphRayScene == true)
    {
        Array<internal::GraphRaySceneInput> inputs;
        if (inputs.TryEnsureCapacity(source->Count) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; source != nullptr && i < source->Count; ++i)
        {
            const State::Item& item = source->Items[i];
            if (item.Input.Pass != Layer::Opaque || item.MaterialSlot != NO_MATERIAL ||
                mState->MeshRecords[item.MeshSlot].Reference.Get() == nullptr)
            {
                return Status::Unsupported;
            }
            const math::Vector4 color = item.Input.Material.Color;
            if (inputs.TryAdd({mState->MeshRecords[item.MeshSlot].Reference.Get(),
                               item.Input.Transform,
                               {color.X, color.Y, color.Z},
                               item.Input.SourceId}) == false)
            {
                return Status::OutOfMemory;
            }
        }
        const usize remaining = mState->Reference->Options.CpuByteBudget - mState->Reference->CpuBytes;
        const usize inputBytes = inputs.GetCapacity() * sizeof(internal::GraphRaySceneInput);
        if (inputBytes > remaining)
        {
            return Status::CapacityExceeded;
        }
        status = internal::BuildGraphRayScene({inputs.GetData(), inputs.GetSize()},
                                              description,
                                              remaining - inputBytes,
                                              scene);
        if (status == Status::Ready &&
            ReferenceFingerprint(*scene, remaining - inputBytes - scene->Info.CpuBytes) == false)
        {
            return Status::OutOfMemory;
        }
    }
    else
    {
        status = mState->BuildReferenceJob(Access::Slot(snapshot), description, scene, true);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    // Frozen geometry/BVH and input texels are CPU setup data; no host path state exists.
    const foundation::usize cpuRemaining = mState->Reference->Options.CpuByteBudget - mState->Reference->CpuBytes;
    if (scene->Info.CpuBytes > cpuRemaining)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull,
                                           SceneMemoryBudget::CpuScene,
                                           scene->Info.CpuBytes,
                                           cpuRemaining);
    }
    const usize scratch = cpuRemaining - scene->Info.CpuBytes;
    Array<math::Vector4> data, texels;
    internal::GpuPreviewUniform uniform;
    status = PackGpuScene(*scene,
                          mState->GpuPreview->HasVisibilityBuffer,
                          mState->GpuPreview->HasNativeRayQueries,
                          scratch,
                          data,
                          texels,
                          uniform,
                          diagnosticsOrNull);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize pixels = usize(description.Width) * description.Height;
    const usize dataBytes = data.GetSize() * sizeof(math::Vector4);
    const usize texelBytes = texels.GetSize() * sizeof(math::Vector4);
    const usize uploadBytes = dataBytes + texelBytes;
    const bool isSceneShared = sharedScene != internal::NO_GPU_PREVIEW_SCENE;
    if (isSceneShared == true)
    {
        // The caller vouches that both jobs packed the same scene description; a different
        // packed layout means it did not, so reject rather than bind mismatched offsets.
        const internal::GpuPreviewSceneRecord& record = state.Scenes[sharedScene];
        if (record.SceneData != scene->SceneData || record.DataBytes != dataBytes || record.TexelBytes != texelBytes ||
            record.FingerprintLow != scene->Scene.FingerprintLow ||
            record.FingerprintHigh != scene->Scene.FingerprintHigh)
        {
            return Status::InvalidDescription;
        }
    }
    // A shared scene is already uploaded and charged by its record.
    const usize sceneUploadBytes = isSceneShared == true ? 0 : uploadBytes;
    usize gpuBytes = 0;
    if (foundation::TryMultiply(pixels, GPU_PREVIEW_BYTES_PER_PIXEL, gpuBytes) == false ||
        foundation::TryAdd(gpuBytes, sceneUploadBytes, gpuBytes) == false ||
        foundation::TryAdd(gpuBytes, sizeof(uniform) + sizeof(math::Vector4), gpuBytes) == false)
    {
        return Status::CapacityExceeded;
    }
    for (usize bufferIndex = 0; state.HasPersistentBuffers == true && bufferIndex < internal::GPU_GRAPH_BUFFER_COUNT;
         ++bufferIndex)
    {
        const bool shouldCreatePrivateBuffer = invocationOrNull == nullptr ||
                                               invocationOrNull->Buffers.IsEmpty() == true ||
                                               (invocationOrNull->Buffers[bufferIndex].Buffer == rhi::BufferHandle{} &&
                                                invocationOrNull->Buffers[bufferIndex].Size == 0);
        if (shouldCreatePrivateBuffer == true &&
            foundation::TryAdd(gpuBytes, usize{LUDUS_GPU_PREVIEW_VECTOR_BYTES}, gpuBytes) == false)
        {
            return Status::CapacityExceeded;
        }
    }
    const usize gpuRemaining = state.Options.GpuByteBudget - state.GpuBytes;
    if (gpuBytes > gpuRemaining)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull, SceneMemoryBudget::GpuPreview, gpuBytes, gpuRemaining);
    }
    const uint64 requiredScratch = uint64{uploadBytes} + uint64{pixels} * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES;
    if (requiredScratch > scratch)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull, SceneMemoryBudget::CpuScratch, requiredScratch, scratch);
    }
    rhi::ComputeCapabilities computeCaps;
    rhi::StorageUsage usage;
    status = rhi::GetStorageUsage(mState->Device, usage);
    if (status != Status::Ready)
    {
        return status;
    }
    rhi::DeviceInfo deviceInformation;
    if (rhi::GetDeviceInfo(mState->Device, deviceInformation) != rhi::DeviceStatus::Ready)
    {
        return Status::NotReady;
    }
    const uint64 resourceBudget = deviceInformation.Profile.Storage.ResourceBytes;
    const uint64 resourceRemaining = resourceBudget - math::Min(resourceBudget, usage.ResourceBytes);
    if (gpuBytes > resourceRemaining)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull,
                                           SceneMemoryBudget::RhiResources,
                                           gpuBytes,
                                           resourceRemaining);
    }
    rhi::FormatCapabilities formatCaps;
    status = rhi::GetComputeCapabilities(mState->Device, computeCaps);
    if (status == Status::Ready)
    {
        status = rhi::GetFormatCapabilities(mState->Device, rhi::RasterFormat::Rgba8Unorm, formatCaps);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    const usize largestStorageBuffer =
        math::Max(math::Max(dataBytes, texelBytes), pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES);
    if (largestStorageBuffer > computeCaps.MaxStorageRange)
    {
        return internal::RejectSceneMemory(diagnosticsOrNull,
                                           SceneMemoryBudget::DeviceStorageRange,
                                           largestStorageBuffer,
                                           computeCaps.MaxStorageRange);
    }
    if ((description.Width - 1) / state.WorkgroupWidth + 1 > computeCaps.MaxDispatch[0] ||
        (description.Height - 1) / state.WorkgroupHeight + 1 > computeCaps.MaxDispatch[1] ||
        description.Width > formatCaps.MaxAttachmentDimension2D ||
        description.Height > formatCaps.MaxAttachmentDimension2D)
    {
        return Status::Unsupported;
    }
    Array<uint8> zeros;
    if (zeros.TryResize(pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES) == false)
    {
        return Status::OutOfMemory;
    }
    GpuPreviewRecord& job = state.Jobs[slot];
    job.Used = job.Owned = true;
    job.Setup = Status::Pending;
    job.Info = {};
    job.HasImage = false;
    job.GpuBytes = gpuBytes - sceneUploadBytes;
    state.GpuBytes += gpuBytes;
    job.DataBytes = dataBytes;
    job.TexelBytes = texelBytes;
    if (isSceneShared == true)
    {
        internal::GpuPreviewSceneRecord& record = state.Scenes[sharedScene];
        record.References += 1;
        job.Scene = sharedScene;
        job.Data = record.Data;
        job.Texels = record.Texels;
        job.NativeGeometry = record.NativeGeometry;
        job.HasNativeGeometry = record.HasNativeGeometry;
    }
    else
    {
        // Each job holds at most one record, so a free record exists for this free job slot.
        usize sceneSlot = 0;
        while (sceneSlot < state.Scenes.GetSize() && state.Scenes[sceneSlot].References != 0)
        {
            ++sceneSlot;
        }
        LUDUS_ASSERT(sceneSlot < state.Scenes.GetSize());
        internal::GpuPreviewSceneRecord& record = state.Scenes[sceneSlot];
        record.References = 1;
        record.DataBytes = dataBytes;
        record.TexelBytes = texelBytes;
        record.GpuBytes = sceneUploadBytes;
        record.SceneData = scene->SceneData;
        record.FingerprintLow = scene->Scene.FingerprintLow;
        record.FingerprintHigh = scene->Scene.FingerprintHigh;
        job.Scene = sceneSlot;
    }
    job.Width = description.Width;
    job.Height = description.Height;
    job.Info.TargetSamples = description.TargetSamples;
    job.Info.SceneFingerprintLow = scene->Scene.FingerprintLow;
    job.Info.SceneFingerprintHigh = scene->Scene.FingerprintHigh;
    if (invocationOrNull != nullptr)
    {
        uniform.Sampling[2] |= invocationOrNull->Program << LUDUS_GRAPH_PASS_INDEX_SHIFT;
        constexpr usize SAMPLE_LIMIT_COMPONENT = 3;
        uniform.Sampling[SAMPLE_LIMIT_COMPONENT] = invocationOrNull->AccumulationSamples;
    }
    uniform.Options.Y = invocationOrNull != nullptr ? invocationOrNull->ToneMappingExposure : state.Options.Exposure;
    job.HasNativeRayInvocation = invocationOrNull != nullptr && invocationOrNull->HasNativeRayInvocation == true;
    job.DenoisingQuality = invocationOrNull != nullptr ? invocationOrNull->DenoisingQuality : 0;
    job.RayPathCapture = hasRayPathCapture == true ? invocationOrNull->RayPathCapture : GpuPreviewGraphBuffer{};
    uniform.Sampling[2] |= job.DenoisingQuality != 0 ? LUDUS_GPU_PREVIEW_DENOISING_FLAG : 0;
    job.Uniform = uniform;
    const math::Vector4 dimensions{float32(job.Width),
                                   float32(job.Height),
                                   job.DenoisingQuality != 0 ? uniform.Options.Y : 0,
                                   0};
    if (isSceneShared == false)
    {
        internal::GpuPreviewSceneRecord& record = state.Scenes[job.Scene];
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Storage, dataBytes},
                                   {reinterpret_cast<const uint8*>(data.GetData()), dataBytes},
                                   record.Data);
        if (Accepted(status))
        {
            status = rhi::CreateBuffer(mState->Device,
                                       {rhi::BufferRole::Storage, texelBytes},
                                       {reinterpret_cast<const uint8*>(texels.GetData()), texelBytes},
                                       record.Texels);
        }
        job.Data = record.Data;
        job.Texels = record.Texels;
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Uniform, sizeof(uniform)},
                                   {reinterpret_cast<const uint8*>(&uniform), sizeof(uniform)},
                                   job.Parameters);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Storage, pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES},
                                   {zeros.GetData(), pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES},
                                   job.Accumulation);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Storage, pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES},
                                   {zeros.GetData(), pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES},
                                   job.Colors);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Uniform, sizeof(dimensions)},
                                   {reinterpret_cast<const uint8*>(&dimensions), sizeof(dimensions)},
                                   job.ImageParameters);
    }
    for (internal::GpuFrameRecord& frame : job.Frames)
    {
        if (Accepted(status))
        {
            status = rhi::CreateTexture(mState->Device,
                                        {job.Width, job.Height, rhi::RasterFormat::Rgba8Unorm, true},
                                        {},
                                        frame.Texture);
        }
    }
    if (Accepted(status) == true)
    {
        status = job.Storage.Initialize(mState->Device,
                                        state.Options.StorageBuffers,
                                        job.Width,
                                        job.Height,
                                        {scratch - uploadBytes - pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES,
                                         state.Options.GpuByteBudget - state.GpuBytes});
        job.GpuBytes += job.Storage.GetBytes();
        state.GpuBytes += job.Storage.GetBytes();
    }
    if (Accepted(status) == true && state.HasNativeRayQueries == true && isSceneShared == false)
    {
        internal::GpuPreviewSceneRecord& record = state.Scenes[job.Scene];
        rhi::RayAllocation allocation;
        status =
            internal::CreateGpuPreviewRayGeometry(mState->Device,
                                                  scene->Scene,
                                                  scratch - uploadBytes - pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES,
                                                  state.Options.GpuByteBudget - state.GpuBytes,
                                                  record.NativeGeometry,
                                                  allocation);
        record.HasNativeGeometry = scene->Scene.Order.GetSize() != 0;
        job.NativeGeometry = record.NativeGeometry;
        job.HasNativeGeometry = record.HasNativeGeometry;
        if (Accepted(status) == true)
        {
            const usize nativeBytes = allocation.ResidentBytes + allocation.ScratchBytes;
            record.GpuBytes += nativeBytes;
            state.GpuBytes += nativeBytes;
        }
    }
    for (usize bufferIndex = 0; Accepted(status) == true && state.HasPersistentBuffers == true &&
                                bufferIndex < internal::GPU_GRAPH_BUFFER_COUNT;
         ++bufferIndex)
    {
        GpuPreviewGraphBuffer buffer = invocationOrNull != nullptr && invocationOrNull->Buffers.IsEmpty() == false
                                           ? invocationOrNull->Buffers[bufferIndex]
                                           : GpuPreviewGraphBuffer{};
        if (buffer.Buffer == rhi::BufferHandle{} && buffer.Size == 0)
        {
            static constexpr StaticArray<uint8, LUDUS_GPU_PREVIEW_VECTOR_BYTES> ZERO_WORD{};
            status = rhi::CreateBuffer(mState->Device,
                                       {rhi::BufferRole::Storage, ZERO_WORD.GetSize()},
                                       ZERO_WORD.AsSpan(),
                                       buffer.Buffer);
            buffer.Size = ZERO_WORD.GetSize();
            job.HasOwnedGraphBuffers[bufferIndex] = Accepted(status);
        }
        else if (buffer.Size == 0 || buffer.Size % LUDUS_GPU_PREVIEW_VECTOR_BYTES != 0 ||
                 buffer.Size > computeCaps.MaxStorageRange)
        {
            status = Status::InvalidDescription;
        }
        job.GraphBuffers[bufferIndex] = buffer;
    }
    if (Accepted(status))
    {
        status = PollGpuJob(mState->Device, state, job);
    }
    if (Accepted(status) == false)
    {
        mState->DropGpuPreview(slot);
        return status;
    }
    outOutput = Access::Make<renderer::GpuPreview>({mState->Owner, job.Generation, slot});
    return status;
}
Status Renderer::PrepareGpuPreviewGraph(renderer::GpuPreview job,
                                        rhi::TextureHandle target,
                                        rhi::OrderedGraph& outGraph,
                                        const VisibilityRasterPass* visibilityOrNull) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    Status status = RendererAdmission(mState->Device);
    if (status != Status::Ready)
    {
        return status;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    constexpr usize JOB_COUNT = internal::GPU_PREVIEW_JOB_COUNT;
    internal::GpuPreviewRecord* jobOrNull = Resolve(state.Jobs, JOB_COUNT, mState->Owner, job);
    if (jobOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    status = PollGpuJob(mState->Device, state, *jobOrNull);
    if (status != Status::Ready)
    {
        return status;
    }
    rhi::TextureDescription description;
    status = rhi::GetTextureDescription(mState->Device, target, description);
    if (status != Status::Ready)
    {
        return status;
    }
    if (description.Width != jobOrNull->Width || description.Height != jobOrNull->Height ||
        description.Attachment == false || description.Format != rhi::RasterFormat::Rgba8Unorm ||
        description.SampleCount != 1)
    {
        return Status::InvalidDescription;
    }
    if (state.HasVisibilityBuffer != (visibilityOrNull != nullptr))
    {
        return Status::InvalidDescription;
    }
    if (visibilityOrNull != nullptr)
    {
        status = PrepareVisibilityInput(mState->Device, state, *jobOrNull, *visibilityOrNull);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    // Build directly into the caller's null graph: CreateOrderedGraph rejects occupied outputs.
    // On later failure discard only the graph allocated by this invocation.
    status = rhi::CreateOrderedGraph(mState->Device, outGraph);
    if (status != Status::Ready)
    {
        return status;
    }
    status = internal::BuildGpuGraph(mState->Device, state, *jobOrNull, 0, outGraph, &target, visibilityOrNull);
    if (status != Status::Ready)
    {
        static_cast<void>(rhi::DiscardOrderedGraph(mState->Device, outGraph));
    }
    return status;
}
Status Renderer::PrepareGpuPreviewGraphSequence(BorrowedView<const renderer::GpuPreview> jobs,
                                                BorrowedView<const rhi::TextureHandle> targets,
                                                rhi::OrderedGraph& outGraph,
                                                const VisibilityRasterPass* visibilityOrNull) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    Status status = RendererAdmission(mState->Device);
    if (status != Status::Ready)
    {
        return status;
    }
    if (jobs.IsValid() == false || targets.IsValid() == false || jobs.IsEmpty() == true ||
        jobs.GetSize() != targets.GetSize() || jobs.GetSize() > content::render_graph_format::MAX_RAY_PASSES)
    {
        return Status::InvalidDescription;
    }
    if (mState->GpuPreview->HasVisibilityBuffer != (visibilityOrNull != nullptr))
    {
        return Status::InvalidDescription;
    }
    StaticArray<const GpuPreviewRecord*, content::render_graph_format::MAX_RAY_PASSES> records{};
    GpuPreviewState& state = *mState->GpuPreview;
    for (usize index = 0; index < jobs.GetSize(); ++index)
    {
        GpuPreviewRecord* jobOrNull = Resolve(state.Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, jobs[index]);
        if (jobOrNull == nullptr)
        {
            return Status::InvalidHandle;
        }
        for (usize earlier = 0; earlier < index; ++earlier)
        {
            if (records[earlier] == jobOrNull)
            {
                return Status::InvalidDescription;
            }
        }
        status = PollGpuJob(mState->Device, state, *jobOrNull);
        if (status == Status::Ready && visibilityOrNull != nullptr)
        {
            status = PrepareVisibilityInput(mState->Device, state, *jobOrNull, *visibilityOrNull);
        }
        if (status != Status::Ready)
        {
            return status;
        }
        rhi::TextureDescription description;
        status = rhi::GetTextureDescription(mState->Device, targets[index], description);
        if (status != Status::Ready)
        {
            return status;
        }
        if (description.Width != jobOrNull->Width || description.Height != jobOrNull->Height ||
            description.Attachment == false || description.Format != rhi::RasterFormat::Rgba8Unorm ||
            description.SampleCount != 1 ||
            (visibilityOrNull != nullptr && targets[index] == visibilityOrNull->Attachment.Color))
        {
            return Status::InvalidDescription;
        }
        records[index] = jobOrNull;
    }
    return internal::BuildGpuGraphSequence(mState->Device,
                                           state,
                                           {records.GetData(), jobs.GetSize()},
                                           targets,
                                           outGraph,
                                           visibilityOrNull);
}
Status Renderer::AdvanceGpuPreview(renderer::GpuPreview handle,
                                   rhi::SurfaceHandle surface,
                                   rhi::SubmissionToken& outCompletion) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    Status status = RendererAdmission(mState->Device);
    if (status != Status::Ready)
    {
        return status;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, handle);
    if (job == nullptr)
    {
        return Status::InvalidHandle;
    }
    status = PollGpuJob(mState->Device, state, *job);
    if (status != Status::Ready)
    {
        return status == Status::Pending ? Status::NotReady : status;
    }
    if (job->Info.Complete)
    {
        return Status::InvalidState;
    }
    uint32 frame = job->HasImage ? 1 - job->LatestFrame : 0;
    if (job->Frames[frame].Owned || job->Frames[frame].Generation == LAST_ID)
    {
        frame = 1 - frame;
    }
    if (job->Frames[frame].Owned)
    {
        return Status::NotReady;
    }
    if (job->Frames[frame].Generation == LAST_ID)
    {
        return Status::IdentityExhausted;
    }
    rhi::OrderedGraph graph;
    status = internal::BuildGpuGraph(mState->Device, state, *job, frame, graph);
    if (status == Status::Ready)
    {
        status = rhi::ExecuteOrderedGraph(mState->Device, surface, graph, outCompletion);
    }
    static_cast<void>(rhi::DiscardOrderedGraph(mState->Device, graph));
    if (status != Status::Ready)
    {
        return status;
    }
    job->PendingFrame = frame;
    job->Frames[frame].Producer = outCompletion;
    job->Info.Pending = true;
    if (job->HasImage && job->LatestFrame == frame)
    {
        job->HasImage = false;
    }
    return Status::Ready;
}
Status Renderer::GetGpuPreviewInfo(renderer::GpuPreview handle, GpuPreviewInfo& output) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, handle);
    if (job == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status status = PollGpuJob(mState->Device, state, *job);
    if (Accepted(status) == false)
    {
        return status;
    }
    output = job->Info;
    return status;
}
Status
Renderer::AcquireGpuPreviewFrame(renderer::GpuPreview handle, GpuPreviewFrame& frame, GpuPreviewImage& output) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    if (Access::Owner(frame) != 0)
    {
        return Status::InvalidState;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, handle);
    if (job == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status status = PollGpuJob(mState->Device, state, *job);
    if (Accepted(status) == false)
    {
        return status;
    }
    if (job->HasImage == false || job->Frames[job->LatestFrame].Owned)
    {
        return Status::NotReady;
    }
    internal::GpuFrameRecord& image = job->Frames[job->LatestFrame];
    if (image.Generation == LAST_ID)
    {
        return Status::IdentityExhausted;
    }
    image.Owned = true;
    frame =
        Access::Make<GpuPreviewFrame>({mState->Owner, image.Generation, Access::Slot(handle) * 2 + job->LatestFrame});
    output = {image.Texture, image.Producer, job->Width, job->Height, image.Samples};
    return Status::Ready;
}
Status Renderer::Release(renderer::GpuPreview& handle) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    internal::GpuPreviewRecord* job =
        Resolve(mState->GpuPreview->Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, handle);
    if (job == nullptr)
    {
        return Status::InvalidHandle;
    }
    job->Owned = false;
    mState->RetireGpuPreviews();
    handle = {};
    return Status::Ready;
}
Status Renderer::Release(GpuPreviewFrame& handle) noexcept
{
    if (mState.Get() == nullptr || mState->GpuPreview.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    const usize slot = Access::Slot(handle);
    if (Access::Owner(handle) != mState->Owner || slot >= internal::GPU_PREVIEW_JOB_COUNT * 2)
    {
        return Status::InvalidHandle;
    }
    GpuPreviewRecord& job = mState->GpuPreview->Jobs[slot / 2];
    internal::GpuFrameRecord& frame = job.Frames[slot % 2];
    if (job.Used == false || frame.Owned == false || frame.Generation != Access::Generation(handle))
    {
        return Status::InvalidHandle;
    }
    frame.Owned = false;
    ++frame.Generation;
    handle = {};
    mState->RetireGpuPreviews();
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
