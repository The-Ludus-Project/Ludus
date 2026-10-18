#pragma once

#include <ludus/foundation/base/byte_order.hpp>

#include <cstring>
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
constexpr rhi::RasterBinding GPU_COMPUTE_BINDINGS[]{
    {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Compute, sizeof(internal::GpuPreviewUniform)},
    {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, 16},
    {2, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, 16},
    {3, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, 16},
    {4, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, 16}};
constexpr rhi::RasterBinding GPU_IMAGE_BINDINGS[]{
    {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Vertex, 16}};
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
                    usize budget,
                    Array<math::Vector4>& data,
                    Array<math::Vector4>& texels,
                    internal::GpuPreviewUniform& uniform) noexcept
{
    const internal::ReferenceSceneData& scene = job.Scene;
    const uint64 nodesWide = uint64(scene.Triangles.GetSize()) * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    const uint64 orderWide = nodesWide + uint64(scene.Nodes.GetSize()) * 3;
    const uint64 emittersWide = orderWide + scene.Order.GetSize();
    const uint64 surfacesWide = emittersWide + scene.Emitters.GetSize();
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
    const auto getImage = [&scene](usize index) noexcept -> const rhi::TextureCpuContent& {
        const internal::ReferenceInstance& instance = scene.Instances[index / LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE];
        return index % LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE == 0   ? instance.Texture
               : index % LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE == 1 ? instance.SurfaceMap
                                                                     : instance.EmissionMap;
    };
    if (textureCount * sizeof(usize) > budget || textures.TryResize(textureCount) == false)
    {
        return Status::CapacityExceeded;
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
    if (size > ~uint32{0} ||
        uint64(size) + dataSize > (budget - textures.GetSize() * sizeof(usize)) / sizeof(math::Vector4))
    {
        return Status::CapacityExceeded;
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
        out[8] = GpuVector(t.GeometricNormal);
        out[9] = GpuVector(t.Error);
        for (usize corner = 0; corner < 3; ++corner)
        {
            out[10 + corner] = t.Tangents[corner];
            out[13 + corner] = t.Colors[corner];
        }
    }
    for (usize i = 0; i < scene.Nodes.GetSize(); ++i)
    {
        const internal::ReferenceBvhNode& n = scene.Nodes[i];
        math::Vector4* out = data.GetData() + nodes + i * 3;
        out[0] = GpuVector(n.Bounds.Min, GpuBits(n.Left));
        out[1] = GpuVector(n.Bounds.Max, GpuBits(n.Right));
        out[2] = {GpuBits(n.First), GpuBits(n.Count), 0, 0};
    }
    for (usize i = 0; i < scene.Order.GetSize(); ++i)
    {
        data[order + i].X = GpuBits(scene.Order[i]);
    }
    for (usize i = 0; i < scene.Emitters.GetSize(); ++i)
    {
        data[emitters + i].X = GpuBits(scene.Emitters[i]);
    }
    for (usize i = 0; i < scene.InstanceCount; ++i)
    {
        const internal::ReferenceInstance& instance = scene.Instances[i];
        const internal::ReferenceSurfaceData& s = *instance.Surface;
        math::Vector4* out = data.GetData() + surfaces + i * LUDUS_GPU_PREVIEW_SURFACE_WORDS;
        out[0] = GpuVector(s.Bsdf.BaseColor, s.Bsdf.Metallic);
        out[1] = {s.Bsdf.Roughness, s.AlphaCutoff, GpuBits(instance.Texture.Width), GpuBits(instance.Texture.Height)};
        out[2] = s.Tint;
        out[3] = s.UvTransform;
        out[4] = GpuVector(s.Emission, GpuBits(s.HasTexture ? 1U : 0U));
        const uint32 flags = (s.Sampler.Filter == rhi::RasterFilter::Linear ? 1U : 0U) |
                             (s.Sampler.U == rhi::RasterAddress::Repeat ? 2U : 0U) |
                             (s.Sampler.V == rhi::RasterAddress::Repeat ? 4U : 0U) |
                             (instance.Texture.Format == rhi::RasterFormat::Rgba8Srgb ? 8U : 0U);
        out[5] = {GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE])),
                  GpuBits(flags),
                  GpuBits(s.Alpha == MaterialAlpha::Masked ? 1U : 0U),
                  GpuBits(s.IsEmissionTwoSided == true ? 1U : 0U)};
        out[6] = {s.NormalScale,
                  GpuBits(instance.SurfaceMap.Width),
                  GpuBits(instance.SurfaceMap.Height),
                  GpuBits(s.HasSurfaceMap ? 1U : 0U)};
        out[7] = {GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE + 1])),
                  GpuBits(flags & 7U),
                  GpuBits(s.AreAuthoredInputsEnabled ? 1U : 0U),
                  0};
        out[8] = {GpuBits(static_cast<uint32>(textures[i * LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE + 2])),
                  GpuBits((flags & 7U) | (instance.EmissionMap.Format == rhi::RasterFormat::Rgba8Srgb ? 8U : 0U)),
                  GpuBits(instance.EmissionMap.Width),
                  GpuBits(instance.EmissionMap.Height)};
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
                if (foundation::TryReadLittleEndian({image.Bytes + y * image.RowPitch + x * 4, 4}, word) == false)
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
        outLight[3] = {light.InnerCos, 0, 0, 0};
    }
    uniform.ClipToWorld = job.ClipToWorld;
    uniform.CameraPositionWorld =
        GpuVector(job.Description.CameraPosition, job.Description.OrthographicCamera ? 1.0F : 0.0F);
    uniform.Dimensions[0] = job.Description.Width;
    uniform.Dimensions[1] = job.Description.Height;
    uniform.Dimensions[2] = job.Description.MaxDepth;
    uniform.Dimensions[3] = job.Description.RouletteDepth;
    uniform.Offsets[0] = static_cast<uint32>(nodes);
    uniform.Offsets[1] = static_cast<uint32>(order);
    uniform.Offsets[2] = static_cast<uint32>(emitters);
    uniform.Offsets[3] = static_cast<uint32>(surfaces);
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
        uniform.SurfaceLayout[3] =
            static_cast<uint32>(environmentWide) + 1; // Zero denotes absent; offset zero remains representable.
    }
    uniform.Counts[0] = static_cast<uint32>(scene.Triangles.GetSize());
    uniform.Counts[1] = static_cast<uint32>(scene.Nodes.GetSize());
    uniform.Counts[2] = static_cast<uint32>(scene.Emitters.GetSize());
    uniform.Counts[3] = static_cast<uint32>(job.Description.LightCount);
    uniform.Sampling[0] = static_cast<uint32>(job.Key.Value);
    uniform.Sampling[1] = static_cast<uint32>(job.Key.Value >> 32);
    uniform.Sampling[2] = (job.Description.ReverseZ ? 1U : 0U) | (job.Description.InfiniteFar ? 2U : 0U) |
                          (job.Description.CenterRays ? 4U : 0U);
    uniform.Sampling[3] = job.Description.TargetSamples;
    uniform.Environment = GpuVector(job.Description.Environment);
    uniform.SurfaceLayout[0] = LUDUS_GPU_PREVIEW_SURFACE_WORDS;
    uniform.SurfaceLayout[1] = static_cast<uint32>(lightsWide);
    uniform.SurfaceLayout[2] = LUDUS_GPU_PREVIEW_KERNEL_VERSION;
    uniform.Options.X = job.Description.SecondaryDistance;
    uniform.BoundsMinimumWorld = GpuVector(scene.Bounds.Min);
    uniform.BoundsMaximumWorld = GpuVector(scene.Bounds.Max);
    return Status::Ready;
}
Status PollGpuJob(rhi::DeviceHandle device, const GpuPreviewState& state, GpuPreviewRecord& job) noexcept
{
    if (job.Setup == Status::Pending)
    {
        const rhi::BufferHandle buffers[]{job.Data,
                                          job.Texels,
                                          job.Parameters,
                                          job.Accumulation,
                                          job.Colors,
                                          job.ImageParameters};
        for (rhi::BufferHandle buffer : buffers)
        {
            const Status status = rhi::GetStatus(device, buffer);
            if (status != Status::Ready)
            {
                return Accepted(status) ? Status::Pending : (job.Setup = status);
            }
        }
        if (job.ComputeBindingsCreated == false)
        {
            const usize pixels = usize(job.Width) * job.Height;
            const rhi::RasterBindingResource bindings[]{{0, job.Parameters, 0, sizeof(internal::GpuPreviewUniform)},
                                                        {1, job.Data, 0, job.DataBytes},
                                                        {2, job.Accumulation, 0, pixels * 32},
                                                        {3, job.Colors, 0, pixels * 16},
                                                        {4, job.Texels, 0, job.TexelBytes}};
            const Status created = rhi::CreateBindingSet(device, state.ComputeLayout, bindings, job.ComputeBindings);
            if (Accepted(created) == false)
            {
                return job.Setup = created;
            }
            job.ComputeBindingsCreated = true;
        }
        if (job.ImageBindingsCreated == false)
        {
            const rhi::RasterBindingResource binding{0, job.ImageParameters, 0, 16};
            const Status created = rhi::CreateBindingSet(device, state.ImageLayout, {&binding, 1}, job.ImageBindings);
            if (Accepted(created) == false)
            {
                return job.Setup = created;
            }
            job.ImageBindingsCreated = true;
        }
        Status status = rhi::GetStatus(device, job.ComputeBindings);
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, job.ImageBindings);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, job.Frames[0].Texture);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(device, job.Frames[1].Texture);
        }
        if (status != Status::Ready)
        {
            return Accepted(status) ? Status::Pending : (job.Setup = status);
        }
        job.Setup = Status::Ready;
    }
    if (job.Setup != Status::Ready)
    {
        return job.Setup;
    }
    if (job.Info.Pending)
    {
        const Status status = rhi::GetStatus(device, job.Frames[job.PendingFrame].Producer);
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            job.Setup = status;
            return status;
        }
        job.Info.Pending = false;
        ++job.Info.CompletedSamples;
        job.LatestFrame = job.PendingFrame;
        job.HasImage = true;
        job.Frames[job.LatestFrame].Samples = job.Info.CompletedSamples;
        job.Info.Complete = job.Info.CompletedSamples == job.Info.TargetSamples;
    }
    return Status::Ready;
}
} // namespace
void Renderer::State::DropGpuPreview(usize slot) noexcept
{
    GpuPreviewRecord& job = GpuPreview->Jobs[slot];
    static_cast<void>(rhi::Destroy(Device, job.ComputeBindings));
    static_cast<void>(rhi::Destroy(Device, job.ImageBindings));
    static_cast<void>(rhi::Destroy(Device, job.Data));
    static_cast<void>(rhi::Destroy(Device, job.Texels));
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
    for (usize i = 0; i < 4; ++i)
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
    for (usize i = 0; i < 4; ++i)
    {
        if (GpuPreview->Jobs[i].Used)
        {
            DropGpuPreview(i);
        }
    }
    internal::GpuPreviewState& state = *GpuPreview;
    static_cast<void>(rhi::Destroy(Device, state.ComputePipeline));
    static_cast<void>(rhi::Destroy(Device, state.ImagePipeline));
    static_cast<void>(rhi::Destroy(Device, state.ComputeLayout));
    static_cast<void>(rhi::Destroy(Device, state.ImageLayout));
    static_cast<void>(rhi::Destroy(Device, state.ComputeShader));
    static_cast<void>(rhi::Destroy(Device, state.VertexShader));
    static_cast<void>(rhi::Destroy(Device, state.FragmentShader));
    static_cast<void>(rhi::Destroy(Device, state.Quad));
    static_cast<void>(rhi::Destroy(Device, state.Indices));
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
    if (options.CpuByteBudget <= sizeof(GpuPreviewState) || options.GpuByteBudget < 44 ||
        math::IsFinite(options.Exposure) == false || options.Exposure <= 0 || options.Exposure > 10000)
    {
        return Status::InvalidDescription;
    }
    rhi::ComputeCapabilities caps;
    status = rhi::GetComputeCapabilities(mState->Device, caps);
    if (status != Status::Ready)
    {
        return status;
    }
    // Frozen kernel ABI: consume independently cooked target reflection, never invent it.
    if (compute.Bindings.size() != 5 || compute.WorkgroupSize[0] != 64 || compute.WorkgroupSize[1] != 1 ||
        compute.WorkgroupSize[2] != 1)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < 5; ++i)
    {
        const ludus::graphics::rhi::RasterBinding& a = compute.Bindings[i];
        const rhi::RasterBinding& b = GPU_COMPUTE_BINDINGS[i];
        if (a.Binding != b.Binding || a.Kind != b.Kind || a.Visibility != b.Visibility || a.MinSize != b.MinSize)
        {
            return Status::InvalidDescription;
        }
    }
    if (vertex.Inputs.size() != 2 || vertex.Inputs[0].Location != 0 ||
        vertex.Inputs[0].Format != rhi::RasterVertexFormat::Float2 || vertex.Inputs[1].Location != 1 ||
        vertex.Inputs[1].Format != rhi::RasterVertexFormat::Float4 || vertex.Bindings.size() != 1 ||
        vertex.Bindings[0].Binding != 0 || vertex.Bindings[0].Kind != rhi::RasterBindingKind::UniformBuffer ||
        vertex.Bindings[0].MinSize != 16 || vertex.Bindings[0].Visibility != rhi::RasterVisibility::Vertex ||
        fragment.Bindings.empty() == false)
    {
        return Status::InvalidDescription;
    }
    GpuPreviewState* allocation = new (std::nothrow) GpuPreviewState;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<GpuPreviewState> state(allocation);
    state->Options = options;
    status = EnableReferenceRendering(
        {},
        {ReferenceTraversal::CpuOracle, options.CpuByteBudget - sizeof(GpuPreviewState), 0, 1});
    if (status != Status::Ready)
    {
        return status;
    }
    mState->GpuPreview = core::Move(state);
    internal::GpuPreviewState& target = *mState->GpuPreview;
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
    const rhi::RasterVertexStream streams[]{{8, false}, {16, true}};
    const rhi::RasterVertexAttribute attributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float2},
                                                  {1, 1, 0, rhi::RasterVertexFormat::Float4}};
    for (;;)
    {
        bool created = false;
        switch (state.Phase)
        {
            case 0:
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
                    status = rhi::CreateBindingLayout(mState->Device, GPU_COMPUTE_BINDINGS, state.ComputeLayout);
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
                if (Accepted(status))
                {
                    status = rhi::GetStatus(mState->Device, state.Quad);
                }
                if (status == Status::Ready)
                {
                    status = rhi::GetStatus(mState->Device, state.Indices);
                }
                if (status == Status::Ready)
                {
                    state.GpuBytes = sizeof(corners) + sizeof(indices);
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
                                 renderer::GpuPreview& output) noexcept
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
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    State::SnapshotRecord* source = Resolve(mState->SnapshotRecords, mState->Owner, snapshot);
    if (source == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (description.MaxDepth > LUDUS_GPU_PREVIEW_MAX_DEPTH || description.TargetSamples > 4096 ||
        uint64(description.Width) * description.Height > (uint64{1} << 24))
    {
        return Status::Unsupported;
    }
    internal::GpuPreviewState& state = *mState->GpuPreview;
    usize slot = 0;
    status = FreeSlot(state.Jobs, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    for (usize i = 0; i < source->Count; ++i)
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
    status = mState->BuildReferenceJob(Access::Slot(snapshot), description, scene, true);
    if (status != Status::Ready)
    {
        return status;
    }
    // Frozen geometry/BVH and input texels are CPU setup data; no host path state exists.
    const foundation::usize cpuRemaining = mState->Reference->Options.CpuByteBudget - mState->Reference->CpuBytes;
    if (scene->Info.CpuBytes > cpuRemaining)
    {
        return Status::CapacityExceeded;
    }
    const usize scratch = cpuRemaining - scene->Info.CpuBytes;
    Array<math::Vector4> data, texels;
    internal::GpuPreviewUniform uniform;
    status = PackGpuScene(*scene, scratch, data, texels, uniform);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize pixels = usize(description.Width) * description.Height;
    const usize dataBytes = data.GetSize() * sizeof(math::Vector4);
    const usize texelBytes = texels.GetSize() * sizeof(math::Vector4);
    const usize uploadBytes = dataBytes + texelBytes;
    usize gpuBytes = 0;
    if (foundation::TryMultiply(pixels, usize{64}, gpuBytes) == false ||
        foundation::TryAdd(gpuBytes, uploadBytes, gpuBytes) == false ||
        foundation::TryAdd(gpuBytes, sizeof(uniform) + sizeof(math::Vector4), gpuBytes) == false ||
        gpuBytes > state.Options.GpuByteBudget - state.GpuBytes || uploadBytes > scratch ||
        pixels > (scratch - uploadBytes) / 32)
    {
        return Status::CapacityExceeded;
    }
    rhi::ComputeCapabilities computeCaps;
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
    if ((description.Width + 63) / 64 > computeCaps.MaxDispatch[0] || description.Height > computeCaps.MaxDispatch[1] ||
        dataBytes > computeCaps.MaxStorageRange || texelBytes > computeCaps.MaxStorageRange ||
        pixels * 32 > computeCaps.MaxStorageRange || description.Width > formatCaps.MaxAttachmentDimension2D ||
        description.Height > formatCaps.MaxAttachmentDimension2D)
    {
        return Status::Unsupported;
    }
    Array<uint8> zeros;
    if (zeros.TryResize(pixels * 32) == false)
    {
        return Status::OutOfMemory;
    }
    GpuPreviewRecord& job = state.Jobs[slot];
    job.Used = job.Owned = true;
    job.Setup = Status::Pending;
    job.Info = {};
    job.HasImage = false;
    job.GpuBytes = gpuBytes;
    state.GpuBytes += gpuBytes;
    job.DataBytes = dataBytes;
    job.TexelBytes = texelBytes;
    job.Width = description.Width;
    job.Height = description.Height;
    job.Info.TargetSamples = description.TargetSamples;
    job.Info.SceneFingerprintLow = scene->Scene.FingerprintLow;
    job.Info.SceneFingerprintHigh = scene->Scene.FingerprintHigh;
    uniform.Options.Y = state.Options.Exposure;
    const math::Vector4 dimensions{float32(job.Width), float32(job.Height), 0, 0};
    status = rhi::CreateBuffer(mState->Device,
                               {rhi::BufferRole::Storage, dataBytes},
                               {reinterpret_cast<const uint8*>(data.GetData()), dataBytes},
                               job.Data);
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::Storage, texelBytes},
                                   {reinterpret_cast<const uint8*>(texels.GetData()), texelBytes},
                                   job.Texels);
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
                                   {rhi::BufferRole::Storage, pixels * 32},
                                   {zeros.GetData(), pixels * 32},
                                   job.Accumulation);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(mState->Device,
                                   {rhi::BufferRole::StorageVertex, pixels * 16},
                                   {zeros.GetData(), pixels * 16},
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
    if (Accepted(status))
    {
        status = PollGpuJob(mState->Device, state, job);
    }
    if (Accepted(status) == false)
    {
        mState->DropGpuPreview(slot);
        return status;
    }
    output = Access::Make<renderer::GpuPreview>({mState->Owner, job.Generation, slot});
    return status;
}
namespace
{
Status BuildGpuGraph(rhi::DeviceHandle device,
                     const GpuPreviewState& state,
                     const GpuPreviewRecord& job,
                     uint32 image,
                     rhi::OrderedGraph& graph) noexcept
{
    Status status = rhi::CreateOrderedGraph(device, graph);
    if (status != Status::Ready)
    {
        return status;
    }
    const rhi::BufferHandle buffers[]{job.Data,
                                      job.Parameters,
                                      job.Accumulation,
                                      job.Colors,
                                      state.Quad,
                                      state.Indices,
                                      job.ImageParameters,
                                      job.Texels};
    rhi::GraphVersion versions[8];
    for (usize i = 0; i < 8; ++i)
    {
        status = rhi::ImportGraphBuffer(device, graph, buffers[i], versions[i]);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    rhi::RasterTextureState textureState;
    status = rhi::GetTextureState(device, job.Frames[image].Texture, textureState);
    if (status != Status::Ready)
    {
        return status;
    }
    rhi::GraphVersion texture, output, accumulation, colors;
    status = rhi::ImportGraphTexture(device,
                                     graph,
                                     job.Frames[image].Texture,
                                     textureState,
                                     rhi::RasterTextureUse::SampledFragment,
                                     texture);
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, texture, output);
    }
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, versions[2], accumulation);
    }
    if (status == Status::Ready)
    {
        status = rhi::NextGraphVersion(device, graph, versions[3], colors);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    const usize pixels = usize(job.Width) * job.Height;
    const rhi::GraphUse computeUses[]{
        {versions[0], rhi::GraphAccessMode::StorageRead, 0, job.DataBytes, rhi::RasterVisibility::Compute},
        {versions[1],
         rhi::GraphAccessMode::Uniform,
         0,
         sizeof(internal::GpuPreviewUniform),
         rhi::RasterVisibility::Compute},
        {accumulation, rhi::GraphAccessMode::StorageReadWrite, 0, pixels * 32, rhi::RasterVisibility::Compute},
        {colors, rhi::GraphAccessMode::StorageReadWrite, 0, pixels * 16, rhi::RasterVisibility::Compute},
        {versions[7], rhi::GraphAccessMode::StorageRead, 0, job.TexelBytes, rhi::RasterVisibility::Compute}};
    const rhi::ComputeDispatch dispatch{state.ComputePipeline,
                                        job.ComputeBindings,
                                        {(job.Width + 63) / 64, job.Height, 1}};
    rhi::GraphPassDescription compute;
    compute.Name = "GPU preview transport";
    compute.Uses = computeUses;
    compute.UseCount = 5;
    compute.Dispatch = &dispatch;
    status = rhi::AddGraphPass(device, graph, compute);
    if (status != Status::Ready)
    {
        return status;
    }
    const rhi::RasterVertexSlice slices[]{{state.Quad, 0, 32}, {job.Colors, 0, pixels * 16}};
    const rhi::RasterDraw
        draw{state.ImagePipeline, job.ImageBindings, slices, state.Indices, 0, 6, 4, static_cast<uint32>(pixels)};
    const rhi::GraphUse imageUses[]{{versions[4], rhi::GraphAccessMode::Vertex, 0, 32},
                                    {colors, rhi::GraphAccessMode::Vertex, 0, pixels * 16},
                                    {versions[5], rhi::GraphAccessMode::Index, 0, 12},
                                    {versions[6], rhi::GraphAccessMode::Uniform, 0, 16, rhi::RasterVisibility::Vertex},
                                    {output, rhi::GraphAccessMode::ColorWrite}};
    rhi::GraphPassDescription present;
    present.Name = "GPU preview image";
    present.Attachment.Color = job.Frames[image].Texture;
    present.Uses = imageUses;
    present.UseCount = 5;
    present.Draws = &draw;
    present.DrawCount = 1;
    status = rhi::AddGraphPass(device, graph, present);
    if (status == Status::Ready)
    {
        status = rhi::AddGraphRoot(device, graph, accumulation, rhi::GraphRoot::History);
    }
    if (status == Status::Ready)
    {
        status = rhi::AddGraphRoot(device, graph, output, rhi::GraphRoot::Export);
    }
    rhi::GraphReport report;
    if (status == Status::Ready)
    {
        status = rhi::CompileOrderedGraph(device, graph, report);
    }
    return status;
}
} // namespace
Status Renderer::AdvanceGpuPreview(renderer::GpuPreview handle,
                                   rhi::SurfaceHandle surface,
                                   rhi::SubmissionToken& completion) noexcept
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
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, 4, mState->Owner, handle);
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
    status = BuildGpuGraph(mState->Device, state, *job, frame, graph);
    if (status == Status::Ready)
    {
        status = rhi::ExecuteOrderedGraph(mState->Device, surface, graph, completion);
    }
    static_cast<void>(rhi::DiscardOrderedGraph(mState->Device, graph));
    if (status != Status::Ready)
    {
        return status;
    }
    job->PendingFrame = frame;
    job->Frames[frame].Producer = completion;
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
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, 4, mState->Owner, handle);
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
    internal::GpuPreviewRecord* job = Resolve(state.Jobs, 4, mState->Owner, handle);
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
    internal::GpuPreviewRecord* job = Resolve(mState->GpuPreview->Jobs, 4, mState->Owner, handle);
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
    if (Access::Owner(handle) != mState->Owner || slot >= 8)
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
