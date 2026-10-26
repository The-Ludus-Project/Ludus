#pragma once
// Included only by renderer.cpp: captured jobs retain the existing snapshot,
// mesh and material incarnations, while RHI retains accepted native work.

#include "reference_data.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/hash/hash.hpp>

#include <cstring>

namespace ludus::graphics::renderer
{
namespace
{
using internal::ReferenceJob;
using internal::ReferenceStage;
using internal::ReferenceTriangle;
// Padding-free fingerprint schema: binary32/u32 = 4 bytes, u64 = 8 bytes.
// Triangle: positions/normals, UVs, geometric normal/error, area and two IDs.
// Instance: source ID, color/emission pitches and hashes, 14 scalars, 14 enum/image words including emitter sidedness.
// Map extension: pitch/hash, normal strength, tag/format/dimensions/reserved.
// Light: source ID/kind, RGB/intensity, position/direction/range/cone scalars.
constexpr usize FINGERPRINT_TRIANGLE_BYTES = sizeof(float32) * 3 * 3 * 2 + sizeof(float32) * 3 * 2 +
                                             sizeof(float32) * 2 * 3 + sizeof(float32) + 2 * sizeof(uint32) +
                                             sizeof(float32) * 3 * 2 * 4;
constexpr usize FINGERPRINT_INSTANCE_BYTES = 7 * sizeof(uint64) + 14 * sizeof(float32) + 14 * sizeof(uint32);
constexpr usize FINGERPRINT_SURFACE_MAP_BYTES = 3 * sizeof(uint64) + sizeof(float32) + 5 * sizeof(uint32);
constexpr usize FINGERPRINT_LIGHT_BYTES = sizeof(uint64) + sizeof(uint32) + 13 * sizeof(float32);
constexpr usize FINGERPRINT_ENVIRONMENT_BYTES = 3 * sizeof(float32);
constexpr uint32 FINGERPRINT_SURFACE_MAP_TAG = 0x53555246; // Numeric SURF tag; encoded little-endian.
/// Largest admitted RT1 tile, in pixels.
constexpr uint32 MAXIMUM_REFERENCE_TILE_PIXELS = 2048;
/// LUHDR01 flags bit identifying a GPU-compute capture.
constexpr uint32 HDR_GPU_COMPUTE_FLAG = 64;

usize GetReferenceGpuBytes(const ReferenceJob& job) noexcept
{
    return job.Compute.Get() != nullptr ? job.Compute->GpuBytes : 0;
}
// Pack the frozen scene with the GPU preview's layout and create the job's device buffers.
Status PrepareReferenceCompute(ReferenceJob& inoutJob, usize cpuRemaining, usize gpuRemaining) noexcept
{
    Array<math::Vector4> data;
    Array<math::Vector4> texels;
    internal::GpuPreviewUniform uniform;
    Status status = PackGpuScene(inoutJob, false, cpuRemaining, data, texels, uniform, nullptr);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize packedBytes = (data.GetSize() + texels.GetSize()) * sizeof(math::Vector4);
    const usize pixels = usize(inoutJob.Description.Width) * inoutJob.Description.Height;
    usize zeroBytes = 0;
    if (foundation::TryMultiply(pixels, internal::REFERENCE_COMPUTE_PIXEL_BYTES, zeroBytes) == false ||
        packedBytes > cpuRemaining || zeroBytes > cpuRemaining - packedBytes)
    {
        return Status::CapacityExceeded;
    }
    zeroBytes = math::Max(zeroBytes, internal::REFERENCE_COMPUTE_STATUS_BYTES);
    Array<uint8> zeros;
    if (zeros.TryResize(zeroBytes) == false)
    {
        return Status::OutOfMemory;
    }
    std::memset(zeros.GetData(), 0, zeros.GetSize());
    internal::ReferenceComputeInputs inputs;
    inputs.Scene = {data.GetData(), data.GetSize()};
    inputs.Texels = {texels.GetData(), texels.GetSize()};
    inputs.Parameters = {reinterpret_cast<const uint8*>(&uniform), sizeof(uniform)};
    inputs.Zeros = {zeros.GetData(), zeros.GetSize()};
    inputs.CpuByteLimit = cpuRemaining - packedBytes - math::Min(zeroBytes, cpuRemaining - packedBytes);
    inputs.GpuByteLimit = gpuRemaining;
    return internal::CreateReferenceComputeJob(inputs, inoutJob);
}

bool SameReferenceLight(const DirectLight& a, const DirectLight& b) noexcept
{
    return a.SourceId == b.SourceId && a.Kind == b.Kind && a.Color == b.Color && a.Intensity == b.Intensity &&
           a.Position == b.Position && a.Direction == b.Direction && a.Range == b.Range && a.InnerCos == b.InnerCos &&
           a.OuterCos == b.OuterCos;
}
bool SameReferenceIntegral(const ReferenceJob& a,
                           const ReferenceCaptureDescription& b,
                           const LightSelection& lights) noexcept
{
    const auto& d = a.Description;
    if (d.WorldToClip != b.WorldToClip || d.CameraPosition != b.CameraPosition ||
        d.OrthographicCamera != b.OrthographicCamera || d.ReverseZ != b.ReverseZ || d.InfiniteFar != b.InfiniteFar ||
        d.Width != b.Width || d.Height != b.Height || d.MaxDepth != b.MaxDepth || d.RouletteDepth != b.RouletteDepth ||
        d.SecondaryDistance != b.SecondaryDistance || d.Environment != b.Environment ||
        a.EnvironmentOrNull != Access::GetEnvironment(b.EnvironmentTextureOrNull) || d.TextureMip != b.TextureMip ||
        d.Seed != b.Seed || d.CenterRays != b.CenterRays || d.LightCount != lights.Count)
    {
        return false;
    }
    for (usize i = 0; i < lights.Count; ++i)
    {
        if (SameReferenceLight(a.Lights[i], lights.Lights[i]) == false)
        {
            return false;
        }
    }
    return true;
}
Status ValidateReferenceDescription(const ReferenceCaptureDescription& d, LightSelection& lights) noexcept
{
    if (d.Width == 0 || d.Height == 0 || d.Width > 16384 || d.Height > 16384 || d.TargetSamples == 0 ||
        d.TargetSamples >= (uint32{1} << 24) || d.MaxDepth == 0 || d.MaxDepth > LUDUS_REFERENCE_MAX_DEPTH ||
        d.RouletteDepth == 0 || d.RouletteDepth > 64 || math::IsFinite(d.SecondaryDistance) == false ||
        d.SecondaryDistance <= 0 || d.SecondaryDistance > 2000000 ||
        internal::ReferenceColor(d.Environment, 10000) == false || d.TextureMip >= 16)
    {
        return Status::InvalidDescription;
    }
    return NormalizeDirectLights(d.Lights, d.LightCount, lights);
}
void Put32(uint8* output, uint32 value) noexcept
{
    static_cast<void>(foundation::TryWriteLittleEndian(value, {output, 4}));
}
void Put64(uint8* output, uint64 value) noexcept
{
    static_cast<void>(foundation::TryWriteLittleEndian(value, {output, 8}));
}
void PutFloat(uint8* output, float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Put32(output, bits);
}
bool ReferenceFingerprint(ReferenceJob& inoutJob, usize budget, SceneShaderDiagnostics* diagnosticsOrNull) noexcept
{
    // One bounded cold scratch stream, serialized without padding or host byte order.
    // Hashes are diagnostics; exact retained identities/parameters control resets.
    const usize triangleBytes = inoutJob.Scene.Triangles.GetSize() * FINGERPRINT_TRIANGLE_BYTES;
    usize bytes = triangleBytes + inoutJob.Scene.InstanceCount * FINGERPRINT_INSTANCE_BYTES +
                  inoutJob.Description.LightCount * FINGERPRINT_LIGHT_BYTES + FINGERPRINT_ENVIRONMENT_BYTES +
                  (inoutJob.EnvironmentOrNull != nullptr ? 2 * sizeof(uint64) + 2 * sizeof(uint32) : 0);
    for (usize i = 0; i < inoutJob.Scene.InstanceCount; ++i)
    {
        if (inoutJob.Scene.Instances[i].Surface->HasSurfaceMap)
        {
            bytes += FINGERPRINT_SURFACE_MAP_BYTES;
        }
    }
    if (bytes > budget)
    {
        static_cast<void>(internal::RejectSceneMemory(diagnosticsOrNull, SceneMemoryBudget::CpuScratch, bytes, budget));
        return false;
    }
    Array<uint8> stream;
    if (stream.TryResize(bytes) == false)
    {
        return false;
    }
    usize offset = 0;
    const auto word = [&stream, &offset](uint32 value) noexcept {
        Put32(stream.GetData() + offset, value);
        offset += 4;
    };
    const auto scalar = [&stream, &offset](float32 value) noexcept {
        PutFloat(stream.GetData() + offset, value);
        offset += 4;
    };
    const auto vector = [&scalar](math::Vector3 value) noexcept {
        scalar(value.X);
        scalar(value.Y);
        scalar(value.Z);
    };
    for (const ReferenceTriangle& triangle : inoutJob.Scene.Triangles)
    {
        for (auto p : triangle.Positions)
        {
            vector(p);
        }
        for (auto n : triangle.Normals)
        {
            vector(n);
        }
        for (auto uv : triangle.Uvs)
        {
            scalar(uv.X);
            scalar(uv.Y);
        }
        vector(triangle.GeometricNormal);
        vector(triangle.Error);
        scalar(triangle.Area);
        word(triangle.Instance);
        word(triangle.Primitive);
        for (usize corner = 0; corner < 3; ++corner)
        {
            vector({triangle.Tangents[corner].X, triangle.Tangents[corner].Y, triangle.Tangents[corner].Z});
            scalar(triangle.Tangents[corner].W);
            vector({triangle.Colors[corner].X, triangle.Colors[corner].Y, triangle.Colors[corner].Z});
            scalar(triangle.Colors[corner].W);
        }
    }
    for (usize i = 0; i < inoutJob.Scene.InstanceCount; ++i)
    {
        const internal::ReferenceInstance& instance = inoutJob.Scene.Instances[i];
        const auto& surface = *instance.Surface;
        Put64(stream.GetData() + offset, instance.SourceId);
        offset += 8;
        vector(surface.Bsdf.BaseColor);
        scalar(surface.Bsdf.Metallic);
        scalar(surface.Bsdf.Roughness);
        vector(surface.Emission);
        scalar(surface.Tint.W);
        scalar(surface.UvTransform.X);
        scalar(surface.UvTransform.Y);
        scalar(surface.UvTransform.Z);
        scalar(surface.UvTransform.W);
        scalar(surface.AlphaCutoff);
        word(static_cast<uint32>(surface.Alpha));
        word(surface.AreAuthoredInputsEnabled ? 1U : 0U);
        word(surface.MaterialUvSet);
        word(surface.HasEmissionMap ? 1U : 0U);
        word(surface.IsEmissionTwoSided == true ? 1U : 0U);
        word(instance.EmissionMap.Width);
        word(instance.EmissionMap.Height);
        word(static_cast<uint32>(instance.EmissionMap.Format));
        Put64(stream.GetData() + offset, instance.EmissionMap.RowPitch);
        offset += sizeof(uint64);
        const foundation::Fingerprint128 emissionFingerprint =
            foundation::StableFingerprint128({instance.EmissionMap.Bytes, instance.EmissionMap.Size});
        Put64(stream.GetData() + offset, emissionFingerprint.Low);
        offset += sizeof(uint64);
        Put64(stream.GetData() + offset, emissionFingerprint.High);
        offset += sizeof(uint64);
        word(static_cast<uint32>(surface.Sampler.Filter));
        word(static_cast<uint32>(surface.Sampler.U));
        word(static_cast<uint32>(surface.Sampler.V));
        word(static_cast<uint32>(instance.Texture.Format));
        word(instance.Texture.Width);
        word(instance.Texture.Height);
        Put64(stream.GetData() + offset, instance.Texture.RowPitch);
        offset += 8;
        const auto textureFingerprint =
            foundation::StableFingerprint128({instance.Texture.Bytes, instance.Texture.Size});
        Put64(stream.GetData() + offset, textureFingerprint.Low);
        offset += 8;
        Put64(stream.GetData() + offset, textureFingerprint.High);
        offset += 8;
        if (surface.HasSurfaceMap)
        {
            word(FINGERPRINT_SURFACE_MAP_TAG);
            scalar(surface.NormalScale);
            word(static_cast<uint32>(instance.SurfaceMap.Format));
            word(instance.SurfaceMap.Width);
            word(instance.SurfaceMap.Height);
            Put64(stream.GetData() + offset, instance.SurfaceMap.RowPitch);
            offset += 8;
            const foundation::Fingerprint128 fingerprint =
                foundation::StableFingerprint128({instance.SurfaceMap.Bytes, instance.SurfaceMap.Size});
            Put64(stream.GetData() + offset, fingerprint.Low);
            offset += 8;
            Put64(stream.GetData() + offset, fingerprint.High);
            offset += 8;
            word(0);
        }
    }
    for (usize i = 0; i < inoutJob.Description.LightCount; ++i)
    {
        const DirectLight& light = inoutJob.Lights[i];
        Put64(stream.GetData() + offset, light.SourceId);
        offset += 8;
        word(static_cast<uint32>(light.Kind));
        vector(light.Color);
        scalar(light.Intensity);
        vector(light.Position);
        vector(light.Direction);
        scalar(light.Range);
        scalar(light.InnerCos);
        scalar(light.OuterCos);
    }
    vector(inoutJob.Description.Environment);
    if (inoutJob.EnvironmentOrNull != nullptr)
    {
        Put64(stream.GetData() + offset, inoutJob.EnvironmentOrNull->FingerprintLow);
        offset += sizeof(uint64);
        Put64(stream.GetData() + offset, inoutJob.EnvironmentOrNull->FingerprintHigh);
        offset += sizeof(uint64);
        word(inoutJob.EnvironmentOrNull->Width);
        word(inoutJob.EnvironmentOrNull->Height);
    }
    if (offset != bytes)
    {
        return false;
    }
    const foundation::Fingerprint128 fingerprint = foundation::StableFingerprint128({stream.GetData(), offset});
    inoutJob.Scene.FingerprintLow = fingerprint.Low;
    inoutJob.Scene.FingerprintHigh = fingerprint.High;
    return true;
}
} // namespace
usize Renderer::State::RemainingReferenceRayBytes(Status& status) noexcept
{
    if (Reference->Options.Traversal != ReferenceTraversal::NativeQueries)
    {
        status = Status::Ready;
        return 0;
    }
    rhi::RayResourceStats stats;
    status = rhi::GetRayResourceStats(Device, stats);
    if (status != Status::Ready)
    {
        return 0;
    }
    const auto budget = Reference->Options.RayByteBudget;
    const usize query =
        usize(Reference->Options.TilePixels) * 2 * (sizeof(rhi::RayInput) + sizeof(rhi::RayResult)) + 16;
    if (stats.ResidentBytes > budget || stats.BuildScratchBytes > budget - stats.ResidentBytes ||
        query > budget - stats.ResidentBytes - stats.BuildScratchBytes)
    {
        status = Status::CapacityExceeded;
        return 0;
    }
    return budget - stats.ResidentBytes - stats.BuildScratchBytes - query;
}
Status Renderer::State::PrepareReferenceMesh(const MeshDescription& input,
                                             const struct Vertex* vertices,
                                             core::UniquePtr<internal::ReferenceMeshData>& output) noexcept
{
    for (usize i = 0; i < input.VertexCount; ++i)
    {
        const auto p = vertices[i].Position;
        const auto uv = vertices[i].Uv;
        if (math::Abs(p.X) > 1000000 || math::Abs(p.Y) > 1000000 || math::Abs(p.Z) > 1000000 ||
            math::Abs(uv.X) > 1000000 || math::Abs(uv.Y) > 1000000)
        {
            return Status::Unsupported;
        }
    }
    const usize bytesPerVertex = sizeof(math::Vector3) * 2 + sizeof(math::Vector2) * 2 + sizeof(math::Vector4) * 2;
    const auto remaining = Reference->Options.CpuByteBudget - Reference->CpuBytes;
    if (input.VertexCount > remaining / bytesPerVertex)
    {
        return Status::CapacityExceeded;
    }
    const auto vertexBytes = input.VertexCount * bytesPerVertex;
    if (input.IndexCount > (remaining - vertexBytes) / sizeof(uint32) ||
        sizeof(internal::ReferenceMeshData) > remaining - vertexBytes - input.IndexCount * sizeof(uint32))
    {
        return Status::CapacityExceeded;
    }
    auto* allocation = new (std::nothrow) internal::ReferenceMeshData;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<internal::ReferenceMeshData> data(allocation);
    data->Device = Device;
    if (data->Positions.TryResize(input.VertexCount) == false || data->Normals.TryResize(input.VertexCount) == false ||
        data->MaterialUvs1.TryResize(input.VertexCount) == false ||
        data->Tangents.TryResize(input.VertexCount) == false || data->Colors.TryResize(input.VertexCount) == false ||
        data->Uvs.TryResize(input.VertexCount) == false ||
        data->Indices.TryAddRange({input.Indices, input.IndexCount}) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < input.VertexCount; ++i)
    {
        data->Positions[i] = vertices[i].Position;
        data->Normals[i] = vertices[i].Normal;
        data->Uvs[i] = vertices[i].Uv;
        data->MaterialUvs1[i] = vertices[i].MaterialUv1;
        data->Tangents[i] = vertices[i].Tangent;
        data->Colors[i] = vertices[i].Color;
    }
    data->CpuBytes = sizeof(internal::ReferenceMeshData) + vertexBytes + input.IndexCount * sizeof(uint32);
    data->NativeStatus = Status::Ready;
    if (Reference->Options.Traversal == ReferenceTraversal::NativeQueries)
    {
        Status status = Status::Ready;
        const auto budget = RemainingReferenceRayBytes(status);
        if (status != Status::Ready)
        {
            return status;
        }
        const rhi::RayGeometryDescription description{data->Positions.GetData(),
                                                      input.VertexCount,
                                                      data->Indices.GetData(),
                                                      input.IndexCount,
                                                      data->Uvs.GetData(),
                                                      budget};
        data->NativeStatus = rhi::CreateRayGeometry(Device, description, data->Geometry);
        if (Accepted(data->NativeStatus) == false)
        {
            return data->NativeStatus;
        }
    }
    output = core::Move(data);
    return Status::Ready;
}
Status Renderer::State::PrepareReferenceSurface(const MaterialDescription& surface,
                                                const LitMaterialDescription* lighting,
                                                core::UniquePtr<internal::ReferenceSurfaceData>& output) noexcept
{
    if (lighting == nullptr)
    {
        return Status::Ready;
    }
    const auto uv = surface.UvTransform;
    if (math::Abs(uv.X) > 1000000 || math::Abs(uv.Y) > 1000000 || math::Abs(uv.Z) > 1000000 ||
        math::Abs(uv.W) > 1000000)
    {
        return Status::Unsupported;
    }
    if (sizeof(internal::ReferenceSurfaceData) > Reference->Options.CpuByteBudget - Reference->CpuBytes)
    {
        return Status::CapacityExceeded;
    }
    auto* data = new (std::nothrow) internal::ReferenceSurfaceData;
    if (data == nullptr)
    {
        return Status::OutOfMemory;
    }
    data->Bsdf = {{surface.Tint.X, surface.Tint.Y, surface.Tint.Z}, lighting->Metallic, lighting->Roughness};
    data->Tint = surface.Tint;
    data->UvTransform = surface.UvTransform;
    data->Sampler = surface.Sampler;
    data->Alpha = surface.Alpha;
    data->AlphaCutoff = surface.AlphaCutoff;
    data->Emission = lighting->Emission;
    data->IsEmissionTwoSided = lighting->IsEmissionTwoSided;
    data->HasTexture = rhi::IsNull(surface.Texture) == false;
    data->HasSurfaceMap = rhi::IsNull(lighting->SurfaceMap) == false;
    data->HasEmissionMap = rhi::IsNull(lighting->EmissionMap) == false;
    data->AreAuthoredInputsEnabled = lighting->AreAuthoredInputsEnabled;
    data->MaterialUvSet = lighting->MaterialUvSet;
    data->NormalScale = lighting->NormalScale;
    output.Reset(data);
    return Status::Ready;
}
void Renderer::State::DropReferenceCapture(usize index) noexcept
{
    auto& record = Reference->Captures[index];
    if (record.Used == false)
    {
        return;
    }
    const auto snapshot = record.SnapshotSlot;
    Reference->CpuBytes -= record.Job->Info.CpuBytes;
    Reference->GpuBytes -= GetReferenceGpuBytes(*record.Job);
    record.Job.Reset();
    record.Used = record.Owned = false;
    ++record.Generation;
    --SnapshotRecords[snapshot].References;
    DropSnapshot(snapshot);
}
void Renderer::State::ClearReference() noexcept
{
    if (Reference.Get() == nullptr)
    {
        return;
    }
    for (usize i = 0; i < 4; ++i)
    {
        DropReferenceCapture(i);
    }
    for (auto& mesh : MeshRecords)
    {
        mesh.Reference.Reset();
    }
    for (auto& material : Materials)
    {
        material.Reference.Reset();
    }
    static_cast<void>(rhi::Destroy(Device, Reference->Program));
    internal::DestroyReferenceComputePipelines(Device, Reference->Compute);
    Reference.Reset();
}
Status Renderer::EnableReferenceRendering(const rhi::ShaderDescription& shader,
                                          const ReferenceRenderingOptions& options) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (mState->Reference)
    {
        return Status::InvalidState;
    }
    for (const auto& mesh : mState->MeshRecords)
    {
        if (mesh.Used)
        {
            return Status::InvalidState;
        }
    }
    for (const auto& material : mState->Materials)
    {
        if (material.Used)
        {
            return Status::InvalidState;
        }
    }
    if (static_cast<uint8>(options.Traversal) > 1 || options.TilePixels == 0 || options.TilePixels > 2048 ||
        options.CpuByteBudget < sizeof(internal::ReferenceState))
    {
        return Status::InvalidDescription;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    if (options.Traversal == ReferenceTraversal::NativeQueries)
    {
        rhi::RayCapabilities caps;
        const auto status = rhi::GetRayCapabilities(mState->Device, caps);
        if (status != Status::Ready)
        {
            return status;
        }
        if (options.RayByteBudget == 0 || usize(options.TilePixels) * 2 > caps.MaxRays)
        {
            return Status::InvalidDescription;
        }
    }
    auto* allocation = new (std::nothrow) internal::ReferenceState;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<internal::ReferenceState> state(allocation);
    state->Options = options;
    state->CpuBytes = sizeof(internal::ReferenceState);
    if (options.Traversal == ReferenceTraversal::NativeQueries)
    {
        const auto status = rhi::CreateRayQueryProgram(mState->Device, shader, state->Program);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    mState->Reference = core::Move(state);
    return Status::Ready;
}
Status Renderer::EnableReferenceRendering(const rhi::RasterShaderDescription& trace,
                                          const rhi::RasterShaderDescription& reduce,
                                          const ReferenceRenderingOptions& options) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (mState->Reference)
    {
        return Status::InvalidState;
    }
    for (const State::MeshRecord& mesh : mState->MeshRecords)
    {
        if (mesh.Used)
        {
            return Status::InvalidState;
        }
    }
    for (const State::MaterialRecord& material : mState->Materials)
    {
        if (material.Used)
        {
            return Status::InvalidState;
        }
    }
    if (options.Traversal != ReferenceTraversal::GpuCompute || options.TilePixels == 0 ||
        options.TilePixels > MAXIMUM_REFERENCE_TILE_PIXELS ||
        options.CpuByteBudget < sizeof(internal::ReferenceState) || options.GpuByteBudget == 0)
    {
        return Status::InvalidDescription;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    internal::ReferenceState* allocation = new (std::nothrow) internal::ReferenceState;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<internal::ReferenceState> state(allocation);
    state->Options = options;
    state->CpuBytes = sizeof(internal::ReferenceState);
    Status status = internal::CreateReferenceComputePipelines(mState->Device, trace, reduce, state->Compute);
    if (status != Status::Ready)
    {
        return status;
    }
    mState->Reference = core::Move(state);
    status = internal::PollReferenceComputePipelines(mState->Device, mState->Reference->Compute);
    if (Accepted(status) == false)
    {
        mState->ClearReference();
        return status;
    }
    return Status::Ready;
}
Status Renderer::DisableReferenceRendering() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    if (mState->GpuPreview)
    {
        return Status::InvalidState;
    }
    mState->ClearReference();
    return Status::Ready;
}
} // namespace ludus::graphics::renderer

namespace ludus::graphics::renderer
{
Status Renderer::State::BuildReferenceJob(usize snapshotSlot,
                                          const ReferenceCaptureDescription& description,
                                          core::UniquePtr<internal::ReferenceJob>& output,
                                          bool gpuSetup) noexcept
{
    LightSelection lights;
    auto status = ValidateReferenceDescription(description, lights);
    if (status != Status::Ready)
    {
        return status;
    }
    internal::EnvironmentData* environmentOrNull = Access::GetEnvironment(description.EnvironmentTextureOrNull);
    if (description.EnvironmentTextureOrNull != nullptr)
    {
        if (environmentOrNull == nullptr || rhi::GetStatus(Device, environmentOrNull->Texture) == Status::InvalidHandle)
        {
            return Status::InvalidDescription;
        }
        const Status ready = description.EnvironmentTextureOrNull->GetStatus();
        if (ready != Status::Ready)
        {
            return ready == Status::Pending ? Status::NotReady : ready;
        }
    }
    const auto& snapshot = SnapshotRecords[snapshotSlot];
    usize triangles = 0;
    usize instances = 0;
    usize emitters = 0;
    usize surfaceMaps = 0;
    uint64 alphaScalars = 0;
    for (usize i = 0; i < snapshot.Count; ++i)
    {
        const auto& item = snapshot.Items[i];
        if (item.Input.Pass == Layer::Overlay)
        {
            continue;
        }
        if (item.MaterialSlot == NO_MATERIAL || Materials[item.MaterialSlot].Lit == false ||
            Materials[item.MaterialSlot].Reference.Get() == nullptr ||
            MeshRecords[item.MeshSlot].Reference.Get() == nullptr)
        {
            return Status::Unsupported;
        }
        if (Reference->Options.Traversal == ReferenceTraversal::NativeQueries && gpuSetup == false &&
            Materials[item.MaterialSlot].Reference->AreAuthoredInputsEnabled &&
            Materials[item.MaterialSlot].Reference->Alpha == MaterialAlpha::Masked)
        {
            return Status::Unsupported; // Native opacity has no authored per-vertex alpha/UV routing yet.
        }
        const auto& transform = item.Input.Transform;
        for (auto column : transform.Columns)
        {
            if (math::Abs(column.X) > 1000000 || math::Abs(column.Y) > 1000000 || math::Abs(column.Z) > 1000000)
            {
                return Status::Unsupported;
            }
        }
        if (math::Abs(transform.Translation.X) > 1000000 || math::Abs(transform.Translation.Y) > 1000000 ||
            math::Abs(transform.Translation.Z) > 1000000)
        {
            return Status::Unsupported;
        }
        const auto& mesh = *MeshRecords[item.MeshSlot].Reference;
        const auto& material = Materials[item.MaterialSlot];
        const auto count = mesh.Indices.GetSize() / 3;
        if (count > (~uint32{0}) / 2 - triangles)
        {
            return Status::CapacityExceeded;
        }
        triangles += count;
        const auto& emission = material.Reference->Emission;
        if (emission.X > 0 || emission.Y > 0 || emission.Z > 0)
        {
            emitters += count;
        }
        if (material.Reference->HasTexture)
        {
            rhi::TextureCpuContent texture;
            status = rhi::GetTextureCpuContent(Device, material.Texture, description.TextureMip, texture);
            if (status != Status::Ready)
            {
                return status;
            }
            // RT1/GPU preview texel and alpha lookup admit four-channel bytes only.
            // R9's retained R8/RG8 textures remain legal RHI inputs, but are outside
            // this reference surface profile; never index them as RGBA.
            if (texture.Format != rhi::RasterFormat::Rgba8Unorm && texture.Format != rhi::RasterFormat::Rgba8Srgb)
            {
                return Status::Unsupported;
            }
            if (material.Alpha == MaterialAlpha::Masked)
            {
                alphaScalars += uint64(texture.Width) * texture.Height;
            }
        }
        if (material.Reference->HasSurfaceMap)
        {
            ++surfaceMaps;
            rhi::TextureCpuContent texture;
            status = rhi::GetTextureCpuContent(Device, material.SurfaceTexture, description.TextureMip, texture);
            if (status != Status::Ready)
            {
                return status;
            }
            if (texture.Format != rhi::RasterFormat::Rgba8Unorm)
            {
                return Status::Unsupported;
            }
        }
        ++instances;
    }
    const bool isGpuCompute = gpuSetup == false && Reference->Options.Traversal == ReferenceTraversal::GpuCompute;
    const usize pixels = gpuSetup ? 0 : usize(description.Width) * description.Height;
    const usize tile = gpuSetup || isGpuCompute ? 0 : Reference->Options.TilePixels;
    const bool isOracle = gpuSetup || Reference->Options.Traversal != ReferenceTraversal::NativeQueries;
    const uint64 bvhBytes =
        isOracle ? uint64(triangles) * (2 * sizeof(internal::ReferenceBvhNode) + sizeof(uint32)) : 0;
    const uint64 fixedWide =
        sizeof(ReferenceJob) + uint64(instances) * sizeof(internal::ReferenceInstance) +
        uint64(pixels) * sizeof(internal::ReferenceAccumulator) + uint64(tile) * sizeof(internal::ReferencePath) +
        uint64(tile) * 2 * (sizeof(rhi::RayInput) + sizeof(rhi::RayResult)) +
        uint64(triangles) * sizeof(ReferenceTriangle) + bvhBytes + uint64(emitters) * sizeof(uint32);
    const uint64 signatureBytes =
        uint64(triangles) * FINGERPRINT_TRIANGLE_BYTES + uint64(instances) * FINGERPRINT_INSTANCE_BYTES +
        uint64(surfaceMaps) * FINGERPRINT_SURFACE_MAP_BYTES + uint64(lights.Count) * FINGERPRINT_LIGHT_BYTES +
        FINGERPRINT_ENVIRONMENT_BYTES + (environmentOrNull != nullptr ? 2 * sizeof(uint64) + 2 * sizeof(uint32) : 0);
    const uint64 nativeSetup =
        isOracle ? 0
                 : uint64(instances) * (sizeof(rhi::RayInstanceDescription) + sizeof(Array<float32>)) +
                       alphaScalars * sizeof(float32);
    const uint64 transientWide =
        math::Max(signatureBytes, math::Max(nativeSetup, isOracle ? uint64(triangles) * sizeof(uint32) : uint64{0}));
    usize fixed = 0;
    usize transient = 0;
    // Bounded source counts fit uint64; convert only at the wasm32 allocation boundary.
    if (foundation::TryIntegerCast(fixedWide, fixed) == false ||
        foundation::TryIntegerCast(transientWide, transient) == false)
    {
        return Status::CapacityExceeded;
    }
    const auto remaining = Reference->Options.CpuByteBudget - Reference->CpuBytes;
    if (fixed > remaining || transient > remaining - fixed)
    {
        return Status::CapacityExceeded;
    }
    auto* allocation = new (std::nothrow) ReferenceJob;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<ReferenceJob> job(allocation);
    job->EnvironmentOrNull = environmentOrNull;
    internal::RetainEnvironment(environmentOrNull);
    job->Device = Device;
    job->Description = description;
    job->Description.EnvironmentTextureOrNull = nullptr;
    job->Description.Lights = job->Lights;
    job->Description.LightCount = lights.Count;
    for (usize i = 0; i < lights.Count; ++i)
    {
        job->Lights[i] = lights.Lights[i];
    }
    if (math::TryMakeRandomKey(description.Seed, internal::REFERENCE_RANDOM_DOMAIN, job->Key) !=
        math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    if (job->Scene.Instances.TryEnsureCapacity(instances) == false ||
        job->Scene.Instances.TryResize(instances) == false || job->Pixels.TryEnsureCapacity(pixels) == false ||
        job->Pixels.TryResize(pixels) == false || job->Paths.TryEnsureCapacity(tile) == false ||
        job->Paths.TryResize(tile) == false || job->Rays.TryEnsureCapacity(tile * 2) == false ||
        job->Rays.TryResize(tile * 2) == false || job->Hits.TryEnsureCapacity(tile * 2) == false ||
        job->Hits.TryResize(tile * 2) == false || job->Scene.Triangles.TryEnsureCapacity(triangles) == false ||
        job->Scene.Triangles.TryResize(triangles) == false || job->Scene.Emitters.TryEnsureCapacity(emitters) == false)
    {
        return Status::OutOfMemory;
    }
    job->Scene.Bounds = math::EmptyAabb3();
    usize triangleAt = 0;
    for (usize i = 0; i < snapshot.Count; ++i)
    {
        const auto& item = snapshot.Items[i];
        if (item.Input.Pass == Layer::Overlay)
        {
            continue;
        }
        const auto& mesh = *MeshRecords[item.MeshSlot].Reference;
        const auto& material = Materials[item.MaterialSlot];
        auto& instance = job->Scene.Instances[job->Scene.InstanceCount];
        instance.Surface = material.Reference.Get();
        instance.SourceId = item.Input.SourceId;
        instance.FirstTriangle = triangleAt;
        instance.TriangleCount = mesh.Indices.GetSize() / 3;
        if (instance.Surface->HasTexture)
        {
            status = rhi::GetTextureCpuContent(Device, material.Texture, description.TextureMip, instance.Texture);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        if (instance.Surface->HasSurfaceMap)
        {
            status =
                rhi::GetTextureCpuContent(Device, material.SurfaceTexture, description.TextureMip, instance.SurfaceMap);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        if (instance.Surface->HasEmissionMap)
        {
            status = rhi::GetTextureCpuContent(Device,
                                               material.EmissionTexture,
                                               description.TextureMip,
                                               instance.EmissionMap);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        math::Matrix3 normalMatrix;
        if (math::TryNormalMatrix(item.Input.Transform, {}, normalMatrix) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        for (usize primitive = 0; primitive < instance.TriangleCount; ++primitive)
        {
            auto& triangle = job->Scene.Triangles[triangleAt];
            triangle.Instance = static_cast<uint32>(job->Scene.InstanceCount);
            triangle.Primitive = static_cast<uint32>(primitive);
            for (usize corner = 0; corner < 3; ++corner)
            {
                const auto vertex = mesh.Indices[primitive * 3 + corner];
                triangle.Positions[corner] = math::TransformPoint(item.Input.Transform, mesh.Positions[vertex]);
                triangle.Normals[corner] = internal::ReferenceNormalize(normalMatrix * mesh.Normals[vertex]);
                triangle.Uvs[corner] =
                    instance.Surface->MaterialUvSet == 1 ? mesh.MaterialUvs1[vertex] : mesh.Uvs[vertex];
                const math::Vector4 tangent = mesh.Tangents[vertex];
                const math::Vector3 tangentWorld =
                    math::TransformVector(item.Input.Transform, {tangent.X, tangent.Y, tangent.Z});
                const float32 determinant =
                    math::Dot(item.Input.Transform.Columns[0],
                              math::Cross(item.Input.Transform.Columns[1], item.Input.Transform.Columns[2]));
                triangle.Tangents[corner] = {tangentWorld.X,
                                             tangentWorld.Y,
                                             tangentWorld.Z,
                                             tangent.W * (determinant < 0 ? -1.0F : 1.0F)};
                triangle.Colors[corner] =
                    instance.Surface->AreAuthoredInputsEnabled ? mesh.Colors[vertex] : math::Vector4{1, 1, 1, 1};
                const auto p = triangle.Positions[corner];
                if (math::IsFinite(p) == false || math::Abs(p.X) > internal::MAX_WORLD_COORDINATE ||
                    math::Abs(p.Y) > internal::MAX_WORLD_COORDINATE || math::Abs(p.Z) > internal::MAX_WORLD_COORDINATE)
                {
                    return Status::Unsupported;
                }
                job->Scene.Bounds = math::Merge(job->Scene.Bounds, {p, p});
                // Conservative FP32 affine/barycentric arithmetic allowance, using
                // absolute products before cancellation, not a scene-sized epsilon.
                const math::Vector3 error =
                    internal::ComputeReferencePositionError(item.Input.Transform, mesh.Positions[vertex]);
                triangle.Error = {math::Max(triangle.Error.X, error.X),
                                  math::Max(triangle.Error.Y, error.Y),
                                  math::Max(triangle.Error.Z, error.Z)};
            }
            // Thanks to Dirk Gerrit van Antwerpen, "Solving Self-Intersection
            // Artifacts in DirectX Raytracing", NVIDIA Technical Blog (2023),
            // Error bounds: traversal mixes error across coordinate axes.
            // https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/
            // Adapt the extent term to world-space triangles with this profile's
            // 1e-6 FP32 allowance; this is not the article's RTX-specific bound.
            // A flat triangle at coordinate zero still needs a normal offset.
            float32 extent = 0;
            for (usize corner = 1; corner < 3; ++corner)
            {
                const auto edge = triangle.Positions[corner] - triangle.Positions[0];
                extent =
                    math::Max(extent, math::Max(math::Abs(edge.X), math::Max(math::Abs(edge.Y), math::Abs(edge.Z))));
            }
            const auto traversalError = extent * 2e-6F;
            triangle.Error = triangle.Error + math::Vector3{traversalError, traversalError, traversalError};
            const auto areaVector = math::Cross(triangle.Positions[1] - triangle.Positions[0],
                                                triangle.Positions[2] - triangle.Positions[0]);
            triangle.Area = math::Length(areaVector) * 0.5F;
            const uint32 i0 = mesh.Indices[primitive * 3];
            const uint32 i1 = mesh.Indices[primitive * 3 + 1];
            const uint32 i2 = mesh.Indices[primitive * 3 + 2];
            const auto localNormal =
                math::Cross(mesh.Positions[i1] - mesh.Positions[i0], mesh.Positions[i2] - mesh.Positions[i0]);
            triangle.GeometricNormal = internal::ReferenceNormalize(normalMatrix * localNormal);
            if (math::IsFinite(triangle.Area) == false || triangle.Area <= 0 ||
                math::Length(triangle.GeometricNormal) == 0)
            {
                return Status::Unsupported;
            }
            for (auto normal : triangle.Normals)
            {
                if (math::IsFinite(normal) == false || math::Dot(normal, triangle.GeometricNormal) <= 0)
                {
                    return Status::Unsupported;
                }
            }
            const auto& emission = instance.Surface->Emission;
            if ((emission.X > 0 || emission.Y > 0 || emission.Z > 0) &&
                job->Scene.Emitters.TryAdd(static_cast<uint32>(triangleAt)) == false)
            {
                return Status::OutOfMemory;
            }
            ++triangleAt;
        }
        ++job->Scene.InstanceCount;
    }
    if (isOracle == true)
    {
        status = internal::BuildReferenceBvh(job->Scene);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    status = internal::PrepareReferenceCamera(*job);
    if (status != Status::Ready)
    {
        return status;
    }
    if (ReferenceFingerprint(*job, remaining - fixed) == false)
    {
        return Status::OutOfMemory;
    }
    job->Info.Traversal = Reference->Options.Traversal;
    job->Info.Seed = description.Seed;
    job->Info.Width = description.Width;
    job->Info.Height = description.Height;
    job->Info.TargetSamples = description.TargetSamples;
    job->Info.MaxDepth = description.MaxDepth;
    job->Info.SceneFingerprintLow = job->Scene.FingerprintLow;
    job->Info.SceneFingerprintHigh = job->Scene.FingerprintHigh;
    job->Info.CpuBytes =
        sizeof(ReferenceJob) + job->Scene.Instances.GetCapacity() * sizeof(internal::ReferenceInstance) +
        job->Pixels.GetCapacity() * sizeof(internal::ReferenceAccumulator) +
        job->Paths.GetCapacity() * sizeof(internal::ReferencePath) + job->Rays.GetCapacity() * sizeof(rhi::RayInput) +
        job->Hits.GetCapacity() * sizeof(rhi::RayResult) +
        job->Scene.Triangles.GetCapacity() * sizeof(ReferenceTriangle) +
        job->Scene.Order.GetCapacity() * sizeof(uint32) + job->Scene.Emitters.GetCapacity() * sizeof(uint32) +
        job->Scene.Nodes.GetCapacity() * sizeof(internal::ReferenceBvhNode);
    if (job->Info.CpuBytes > remaining || transient > remaining - job->Info.CpuBytes)
    {
        return Status::CapacityExceeded;
    }
    job->Stage = ReferenceStage::Primary;
    if (isGpuCompute == true)
    {
        status = PrepareReferenceCompute(*job,
                                         remaining - job->Info.CpuBytes,
                                         Reference->Options.GpuByteBudget - Reference->GpuBytes);
        if (Accepted(status) == false)
        {
            return status;
        }
    }
    if (isOracle == false && instances != 0)
    {
        Array<rhi::RayInstanceDescription> native;
        Array<Array<float32>> alpha;
        if (native.TryResize(instances) == false || alpha.TryResize(instances) == false)
        {
            return Status::OutOfMemory;
        }
        usize at = 0;
        for (usize i = 0; i < snapshot.Count; ++i)
        {
            const auto& item = snapshot.Items[i];
            if (item.Input.Pass == Layer::Overlay)
            {
                continue;
            }
            const auto& mesh = *MeshRecords[item.MeshSlot].Reference;
            const auto geometry = rhi::GetStatus(Device, mesh.Geometry);
            if (geometry != Status::Ready)
            {
                return geometry == Status::Pending ? Status::NotReady : geometry;
            }
            auto& target = native[at];
            const auto& source = job->Scene.Instances[at];
            const auto& surface = *source.Surface;
            target.Geometry = mesh.Geometry;
            const auto& transform = item.Input.Transform;
            target.Transform[0] = transform.Columns[0].X;
            target.Transform[1] = transform.Columns[1].X;
            target.Transform[2] = transform.Columns[2].X;
            target.Transform[3] = transform.Translation.X;
            target.Transform[4] = transform.Columns[0].Y;
            target.Transform[5] = transform.Columns[1].Y;
            target.Transform[6] = transform.Columns[2].Y;
            target.Transform[7] = transform.Translation.Y;
            target.Transform[8] = transform.Columns[0].Z;
            target.Transform[9] = transform.Columns[1].Z;
            target.Transform[10] = transform.Columns[2].Z;
            target.Transform[11] = transform.Translation.Z;
            target.AlphaMasked = surface.Alpha == MaterialAlpha::Masked;
            target.Opacity = surface.Tint.W;
            target.AlphaCutoff = surface.AlphaCutoff;
            target.UvTransform[0] = surface.UvTransform.X;
            target.UvTransform[1] = surface.UvTransform.Y;
            target.UvTransform[2] = surface.UvTransform.Z;
            target.UvTransform[3] = surface.UvTransform.W;
            target.LinearFilter = surface.Sampler.Filter == rhi::RasterFilter::Linear;
            target.RepeatU = surface.Sampler.U == rhi::RasterAddress::Repeat;
            target.RepeatV = surface.Sampler.V == rhi::RasterAddress::Repeat;
            if (target.AlphaMasked && surface.HasTexture)
            {
                const auto& image = source.Texture;
                const auto size = usize(image.Width) * image.Height;
                if (alpha[at].TryResize(size) == false)
                {
                    return Status::OutOfMemory;
                }
                for (usize y = 0; y < image.Height; ++y)
                {
                    for (usize x = 0; x < image.Width; ++x)
                    {
                        alpha[at][y * image.Width + x] = float32(image.Bytes[y * image.RowPitch + x * 4 + 3]) / 255;
                    }
                }
                target.Alpha = alpha[at].GetData();
                target.Width = image.Width;
                target.Height = image.Height;
            }
            ++at;
        }
        const auto budget = RemainingReferenceRayBytes(status);
        if (status != Status::Ready)
        {
            return status;
        }
        if (budget == 0)
        {
            return Status::CapacityExceeded;
        }
        status = rhi::CreateRayScene(Device, {native.GetData(), instances, budget}, job->Scene.Native);
        if (Accepted(status) == false)
        {
            return status;
        }
        job->Stage = status == Status::Pending ? ReferenceStage::Setup : ReferenceStage::Primary;
    }
    const auto result = job->Stage == ReferenceStage::Setup ? Status::Pending : Status::Ready;
    output = core::Move(job);
    return result;
}
Status Renderer::BeginReferenceCapture(Snapshot snapshot,
                                       const ReferenceCaptureDescription& description,
                                       ReferenceCapture& output) noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    auto* source = Resolve(mState->SnapshotRecords, mState->Owner, snapshot);
    if (source == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& state = *mState->Reference;
    usize slot = 0;
    auto status = FreeSlot(state.Captures, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    core::UniquePtr<ReferenceJob> job;
    status = mState->BuildReferenceJob(Access::Slot(snapshot), description, job);
    if (Accepted(status) == false)
    {
        return status;
    }
    auto& record = state.Captures[slot];
    record.SnapshotSlot = Access::Slot(snapshot);
    state.CpuBytes += job->Info.CpuBytes;
    state.GpuBytes += GetReferenceGpuBytes(*job);
    record.Job = core::Move(job);
    record.Used = record.Owned = true;
    ++source->References;
    output = Access::Make<ReferenceCapture>({mState->Owner, record.Generation, slot});
    return status;
}
Status Renderer::Release(ReferenceCapture& handle) noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    mState->DropReferenceCapture(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}
Status Renderer::GetStatus(ReferenceCapture handle) const noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status admission = RendererAdmission(mState->Device, false);
    if (admission != Status::Ready)
    {
        return admission;
    }
    const auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (record->Job->Stage == ReferenceStage::Failed)
    {
        return Status::Failed;
    }
    return record->Job->Stage == ReferenceStage::Complete ? Status::Ready : Status::Pending;
}
Status Renderer::GetReferenceCaptureInfo(ReferenceCapture handle, ReferenceCaptureInfo& output) const noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    output = record->Job->Info;
    return Status::Ready;
}
Status Renderer::CopyReferencePixels(ReferenceCapture handle, ReferencePixel* output, usize count) const noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto& pixels = record->Job->Pixels;
    if (output == nullptr || count != pixels.GetSize())
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < count; ++i)
    {
        const auto& p = pixels[i];
        const auto weight = p.Samples == 0 ? 0.0F : 1 / float32(p.Samples);
        const math::Vector3 emission = p.Emission.Value * weight;
        const math::Vector3 direct = p.Direct.Value * weight;
        const math::Vector3 indirect = p.Indirect.Value * weight;
        output[i] = {emission + direct + indirect, emission, direct, indirect, p.Samples};
    }
    return Status::Ready;
}
} // namespace ludus::graphics::renderer

namespace ludus::graphics::renderer
{
namespace
{
Status
ReferenceQuery(ReferenceJob& job, rhi::RayQueryProgram program, ReferenceTraversal traversal, usize rayBudget) noexcept
{
    uint64 closest = 0;
    uint64 visibility = 0;
    for (usize i = 0; i < job.QueryCount; ++i)
    {
        const auto origin = internal::ReferenceOrigin(job.Rays[i]);
        if (math::IsFinite(origin) == false || math::Abs(origin.X) > internal::MAX_WORLD_COORDINATE ||
            math::Abs(origin.Y) > internal::MAX_WORLD_COORDINATE ||
            math::Abs(origin.Z) > internal::MAX_WORLD_COORDINATE)
        {
            ++job.Info.InvalidContributions;
            for (usize path = 0; path < job.PathCount; ++path)
            {
                const auto& candidate = job.Paths[path];
                if ((job.Stage == ReferenceStage::Primary && path == i) || candidate.ShadowQuery == i ||
                    candidate.NextQuery == i)
                {
                    job.Info.FailurePixel = candidate.Pixel;
                    job.Info.FailureDepth = candidate.Depth;
                    break;
                }
            }
            return Status::Failed;
        }
        if (job.Rays[i].VisibilityOnly != 0)
        {
            ++visibility;
        }
        else
        {
            ++closest;
        }
    }
    if (traversal == ReferenceTraversal::CpuOracle || job.Scene.InstanceCount == 0)
    {
        for (usize i = 0; i < job.QueryCount; ++i)
        {
            const auto status = internal::TraceReferenceOracle(job.Scene, job.Rays[i], job.Hits[i]);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        job.Info.ClosestQueries += closest;
        job.Info.VisibilityQueries += visibility;
        return Status::Ready;
    }
    rhi::RayResourceStats stats;
    const auto observed = rhi::GetRayResourceStats(job.Device, stats);
    if (observed != Status::Ready)
    {
        return observed;
    }
    const usize needed = job.QueryCount * (sizeof(rhi::RayInput) + sizeof(rhi::RayResult)) + 16;
    if (stats.ResidentBytes > rayBudget || stats.BuildScratchBytes > rayBudget - stats.ResidentBytes ||
        needed > rayBudget - stats.ResidentBytes - stats.BuildScratchBytes)
    {
        return Status::CapacityExceeded;
    }
    const auto status =
        rhi::SubmitRayQueries(job.Device, program, job.Scene.Native, job.Rays.GetData(), job.QueryCount, job.Batch);
    if (Accepted(status))
    {
        job.Info.ClosestQueries += closest;
        job.Info.VisibilityQueries += visibility;
    }
    return status;
}
Status ReferenceRead(ReferenceJob& job, ReferenceTraversal traversal) noexcept
{
    if (traversal == ReferenceTraversal::CpuOracle || job.Scene.InstanceCount == 0)
    {
        return Status::Ready;
    }
    const auto status = rhi::GetStatus(job.Device, job.Batch);
    if (status != Status::Ready)
    {
        return status;
    }
    const auto read = rhi::ReadRayResults(job.Device, job.Batch, job.Hits.GetData(), job.QueryCount);
    if (read != Status::Ready)
    {
        return read;
    }
    return rhi::Destroy(job.Device, job.Batch);
}
bool FiniteReferenceSum(const internal::ReferenceSum& sum) noexcept
{
    return math::IsFinite(sum.Value) && math::IsFinite(sum.Compensation) && sum.Value.X >= 0 && sum.Value.Y >= 0 &&
           sum.Value.Z >= 0;
}
} // namespace
Status Renderer::AdvanceReferenceCapture(ReferenceCapture handle) noexcept
{
    return AdvanceReferenceCapture(handle, rhi::SurfaceHandle{});
}
Status Renderer::AdvanceReferenceCapture(ReferenceCapture handle, rhi::SurfaceHandle surface) noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    auto& state = *mState->Reference;
    auto* record = Resolve(state.Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& job = *record->Job;
    if (job.Stage == ReferenceStage::Failed)
    {
        return Status::Failed;
    }
    if (job.Stage == ReferenceStage::Complete)
    {
        return Status::Ready;
    }
    const auto fail = [&job](Status status) noexcept {
        job.Stage = ReferenceStage::Failed;
        return status;
    };
    if (state.Options.Traversal == ReferenceTraversal::GpuCompute)
    {
        const Status pipelines = internal::PollReferenceComputePipelines(mState->Device, state.Compute);
        if (pipelines != Status::Ready)
        {
            return pipelines == Status::Pending ? pipelines : fail(pipelines);
        }
        return internal::AdvanceReferenceCompute(state.Compute, surface, job);
    }
    if (job.Stage == ReferenceStage::Setup)
    {
        const auto status = rhi::GetStatus(job.Device, job.Scene.Native);
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            return fail(status);
        }
        job.Stage = ReferenceStage::Primary;
        return Status::Pending;
    }
    if (job.Stage == ReferenceStage::Primary)
    {
        job.PathCount = math::Min(usize(state.Options.TilePixels), job.Pixels.GetSize() - job.FirstPixel);
        job.QueryCount = job.PathCount;
        for (usize i = 0; i < job.PathCount; ++i)
        {
            auto& path = job.Paths[i];
            path = {};
            path.Pixel = static_cast<uint32>(job.FirstPixel + i);
            path.Sample = job.Pixels[path.Pixel].Samples;
            const auto status = internal::GenerateReferencePrimary(job, path, path.Ray);
            if (status != Status::Ready)
            {
                return fail(status);
            }
            job.Rays[i] = path.Ray;
        }
        const auto status = ReferenceQuery(job, state.Program, state.Options.Traversal, state.Options.RayByteBudget);
        if (Accepted(status) == false)
        {
            return fail(status);
        }
        job.Stage = ReferenceStage::WaitPrimary;
        return Status::Pending;
    }
    if (job.Stage == ReferenceStage::WaitPrimary)
    {
        const auto status = ReferenceRead(job, state.Options.Traversal);
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            return fail(status);
        }
        for (usize i = 0; i < job.PathCount; ++i)
        {
            const Status prepared = internal::PrepareReferenceVertex(job, job.Info, job.Paths[i], job.Hits[i]);
            if (prepared != Status::Ready)
            {
                return fail(prepared);
            }
        }
        job.Stage = ReferenceStage::Bounce;
        return Status::Pending;
    }
    if (job.Stage == ReferenceStage::Bounce)
    {
        job.QueryCount = 0;
        for (usize i = 0; i < job.PathCount; ++i)
        {
            auto& path = job.Paths[i];
            path.ShadowQuery = path.NextQuery = ~usize{0};
            if (path.HasShadow)
            {
                path.ShadowQuery = job.QueryCount;
                job.Rays[job.QueryCount++] = path.Shadow;
            }
            if (path.HasNext)
            {
                path.NextQuery = job.QueryCount;
                job.Rays[job.QueryCount++] = path.Next;
            }
        }
        if (job.QueryCount == 0)
        {
            job.Stage = ReferenceStage::Commit;
            return Status::Pending;
        }
        const auto status = ReferenceQuery(job, state.Program, state.Options.Traversal, state.Options.RayByteBudget);
        if (Accepted(status) == false)
        {
            return fail(status);
        }
        job.Stage = ReferenceStage::WaitBounce;
        return Status::Pending;
    }
    if (job.Stage == ReferenceStage::WaitBounce)
    {
        const auto status = ReferenceRead(job, state.Options.Traversal);
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            return fail(status);
        }
        for (usize i = 0; i < job.PathCount; ++i)
        {
            auto& path = job.Paths[i];
            if (path.ShadowQuery != ~usize{0})
            {
                const auto hit = job.Hits[path.ShadowQuery].Hit;
                if (hit > 1)
                {
                    return fail(Status::Failed);
                }
                if (hit == 0)
                {
                    if (path.DirectDepth == 0)
                    {
                        path.Direct = path.Direct + path.PendingDirect;
                    }
                    else
                    {
                        path.Indirect = path.Indirect + path.PendingDirect;
                    }
                }
            }
            path.HasShadow = false;
            if (path.NextQuery != ~usize{0})
            {
                path.Ray = path.Next;
                const Status prepared = internal::PrepareReferenceVertex(job, job.Info, path, job.Hits[path.NextQuery]);
                if (prepared != Status::Ready)
                {
                    return fail(prepared);
                }
            }
            else
            {
                path.HasNext = false;
            }
        }
        job.Stage = ReferenceStage::Bounce;
        return Status::Pending;
    }
    if (job.Stage == ReferenceStage::Commit)
    {
        // Check every candidate sum before publishing any pixel in this tile.
        for (usize i = 0; i < job.PathCount; ++i)
        {
            const auto& path = job.Paths[i];
            auto trial = job.Pixels[path.Pixel];
            trial.Emission.Add(path.Emission);
            trial.Direct.Add(path.Direct);
            trial.Indirect.Add(path.Indirect);
            if (FiniteReferenceSum(trial.Emission) == false || FiniteReferenceSum(trial.Direct) == false ||
                FiniteReferenceSum(trial.Indirect) == false ||
                math::IsFinite(trial.Emission.Value + trial.Direct.Value + trial.Indirect.Value) == false)
            {
                ++job.Info.InvalidContributions;
                job.Info.FailurePixel = path.Pixel;
                job.Info.FailureDepth = path.Depth;
                return fail(Status::Failed);
            }
            if (trial.Samples >= (uint32{1} << 24) - 1)
            {
                return fail(Status::CapacityExceeded);
            }
        }
        for (usize i = 0; i < job.PathCount; ++i)
        {
            const auto& path = job.Paths[i];
            auto& pixel = job.Pixels[path.Pixel];
            pixel.Emission.Add(path.Emission);
            pixel.Direct.Add(path.Direct);
            pixel.Indirect.Add(path.Indirect);
            ++pixel.Samples;
            ++job.Info.CommittedSamples;
        }
        job.FirstPixel += job.PathCount;
        job.Info.MaximumSamples = job.Info.MinimumSamples + 1;
        if (job.FirstPixel == job.Pixels.GetSize())
        {
            job.FirstPixel = 0;
            ++job.Info.MinimumSamples;
            job.Info.MaximumSamples = job.Info.MinimumSamples;
        }
        job.Info.Complete = job.Info.MinimumSamples >= job.Description.TargetSamples;
        job.Stage = job.Info.Complete ? ReferenceStage::Complete : ReferenceStage::Primary;
        return job.Info.Complete ? Status::Ready : Status::Pending;
    }
    return fail(Status::InvalidState);
}
Status Renderer::UpdateReferenceCapture(ReferenceCapture handle,
                                        Snapshot snapshot,
                                        const ReferenceCaptureDescription& description) noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status admission = RendererAdmission(mState->Device);
    if (admission != Status::Ready)
    {
        return admission;
    }
    auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    auto* source = Resolve(mState->SnapshotRecords, mState->Owner, snapshot);
    if (record == nullptr || source == nullptr)
    {
        return Status::InvalidHandle;
    }
    LightSelection lights;
    auto status = ValidateReferenceDescription(description, lights);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& old = *record->Job;
    if (record->SnapshotSlot == Access::Slot(snapshot) && SameReferenceIntegral(old, description, lights))
    {
        old.Description.TargetSamples = description.TargetSamples;
        old.Info.TargetSamples = description.TargetSamples;
        if (old.Compute.Get() != nullptr)
        {
            return internal::ResumeReferenceCompute(old);
        }
        old.Info.Complete = old.Info.MinimumSamples >= description.TargetSamples;
        if (old.Info.Complete)
        {
            static_cast<void>(rhi::Destroy(old.Device, old.Batch));
            old.Stage = ReferenceStage::Complete;
        }
        else if (old.Stage == ReferenceStage::Complete || old.Stage == ReferenceStage::Failed)
        {
            static_cast<void>(rhi::Destroy(old.Device, old.Batch));
            old.Stage = ReferenceStage::Primary;
        }
        return old.Info.Complete ? Status::Ready : Status::Pending;
    }
    if (old.Info.Revision == LAST_ID)
    {
        return Status::IdentityExhausted;
    }
    core::UniquePtr<ReferenceJob> replacement;
    status = mState->BuildReferenceJob(Access::Slot(snapshot), description, replacement);
    if (Accepted(status) == false)
    {
        return status;
    }
    replacement->Info.Revision = old.Info.Revision + 1;
    ++source->References;
    const auto oldSnapshot = record->SnapshotSlot;
    auto& state = *mState->Reference;
    state.CpuBytes -= old.Info.CpuBytes;
    state.GpuBytes -= GetReferenceGpuBytes(old);
    record->Job.Reset();
    state.CpuBytes += replacement->Info.CpuBytes;
    state.GpuBytes += GetReferenceGpuBytes(*replacement);
    record->Job = core::Move(replacement);
    record->SnapshotSlot = Access::Slot(snapshot);
    --mState->SnapshotRecords[oldSnapshot].References;
    mState->DropSnapshot(oldSnapshot);
    return status;
}
Status
Renderer::EncodeReferenceHdr(ReferenceCapture handle, uint8* output, usize capacity, usize& written) const noexcept
{
    if (mState.Get() == nullptr || mState->Reference.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto* record = Resolve(mState->Reference->Captures, mState->Owner, handle);
    if (record == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto& job = *record->Job;
    constexpr usize headerBytes = 256;
    constexpr usize pixelBytes = 16;
    if (job.Pixels.GetSize() > (~usize{0} - headerBytes) / pixelBytes)
    {
        return Status::CapacityExceeded;
    }
    const auto size = headerBytes + job.Pixels.GetSize() * pixelBytes;
    if (output == nullptr)
    {
        return Status::InvalidDescription;
    }
    if (capacity < size)
    {
        return Status::CapacityExceeded;
    }
    std::memset(output, 0, headerBytes);
    const uint8 magic[8]{'L', 'U', 'H', 'D', 'R', '0', '1', 0};
    std::memcpy(output, magic, sizeof(magic));
    Put32(output + 8, 1);
    Put32(output + 12, job.Info.ProfileVersion);
    Put32(output + 16, job.Info.SamplerVersion);
    Put32(output + 20, static_cast<uint32>(headerBytes));
    Put32(output + 24, job.Description.Width);
    Put32(output + 28, job.Description.Height);
    Put32(output + 32, job.Description.TargetSamples);
    Put32(output + 36, job.Description.MaxDepth);
    Put32(output + 40, job.Description.RouletteDepth);
    const uint32 flags = (job.Info.Traversal == ReferenceTraversal::CpuOracle ? 1U : 0U) |
                         (job.Info.Traversal == ReferenceTraversal::GpuCompute ? HDR_GPU_COMPUTE_FLAG : 0U) |
                         (job.Description.CenterRays ? 2U : 0U) | (job.Description.OrthographicCamera ? 4U : 0U) |
                         (job.Description.ReverseZ ? 8U : 0U) | (job.Description.InfiniteFar ? 16U : 0U) |
                         (job.Info.Complete == false ? 32U : 0U);
    Put32(output + 44, flags);
    Put64(output + 48, job.Description.Seed);
    Put64(output + 56, job.Info.Revision);
    Put64(output + 64, job.Scene.FingerprintLow);
    Put64(output + 72, job.Scene.FingerprintHigh);
    PutFloat(output + 80, job.Description.SecondaryDistance);
    Put32(output + 84, job.Description.TextureMip);
    Put32(output + 88, static_cast<uint32>(job.Description.LightCount));
    usize at = 96;
    for (const auto& column : job.Description.WorldToClip.Columns)
    {
        PutFloat(output + at, column.X);
        PutFloat(output + at + 4, column.Y);
        PutFloat(output + at + 8, column.Z);
        PutFloat(output + at + 12, column.W);
        at += 16;
    }
    PutFloat(output + 160, job.Description.CameraPosition.X);
    PutFloat(output + 164, job.Description.CameraPosition.Y);
    PutFloat(output + 168, job.Description.CameraPosition.Z);
    PutFloat(output + 172, job.Description.Environment.X);
    PutFloat(output + 176, job.Description.Environment.Y);
    PutFloat(output + 180, job.Description.Environment.Z);
    for (usize i = 0; i < job.Pixels.GetSize(); ++i)
    {
        const auto& pixel = job.Pixels[i];
        const auto scale = pixel.Samples == 0 ? 0.0F : 1 / float32(pixel.Samples);
        const auto radiance = (pixel.Emission.Value + pixel.Direct.Value + pixel.Indirect.Value) * scale;
        auto* target = output + headerBytes + i * pixelBytes;
        PutFloat(target, radiance.X);
        PutFloat(target + 4, radiance.Y);
        PutFloat(target + 8, radiance.Z);
        Put32(target + 12, pixel.Samples);
    }
    written = size;
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
