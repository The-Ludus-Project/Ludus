#include "internal/render_graph_program_cache.h"

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/byte_order.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/device.h>

#include <new>
#include <string_view>

// Shares immutable program dependencies by their complete canonical description and leaves native
// pipeline coalescing to the RHI request service. Contract and acknowledgements:
// docs/architecture/renderer-systems.md, "First migration and acceptance".
namespace ludus::graphics::renderer
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize VERTEX_COMPONENT_COUNT = 4;
constexpr usize VERTEX_BYTES = QUAD_VERTEX_COUNT * VERTEX_COMPONENT_COUNT * sizeof(float32);
constexpr usize INDEX_BYTES = QUAD_INDEX_COUNT * sizeof(uint32);
constexpr usize WORKGROUP_DIMENSIONS = 3;
// WriteShader and WriteStage name every field; extend them whenever these descriptions change.
// Pointer-sized members change the layout of 32-bit targets, where the check is vacuous.
constexpr usize EXPECTED_SHADER_DESCRIPTION_BYTES = 184;
constexpr usize EXPECTED_RASTER_SHADER_DESCRIPTION_BYTES = 832;
constexpr usize EXPECTED_RASTER_BINDING_BYTES = 32;
constexpr usize EXPECTED_SHADER_INPUT_BYTES = 8;
constexpr usize EXPECTED_UNIFORM_MEMBER_BYTES = 40;
static_assert(sizeof(usize) != sizeof(uint64) || sizeof(rhi::ShaderDescription) == EXPECTED_SHADER_DESCRIPTION_BYTES,
              "encode every ShaderDescription field in the program cache");
static_assert(sizeof(usize) != sizeof(uint64) ||
                  sizeof(rhi::RasterShaderDescription) == EXPECTED_RASTER_SHADER_DESCRIPTION_BYTES,
              "encode every RasterShaderDescription field in the program cache");
static_assert(sizeof(usize) != sizeof(uint64) || sizeof(rhi::RasterBinding) == EXPECTED_RASTER_BINDING_BYTES,
              "encode every RasterBinding field in the program cache");
static_assert(sizeof(rhi::RasterShaderInput) == EXPECTED_SHADER_INPUT_BYTES,
              "encode every RasterShaderInput field in the program cache");
static_assert(sizeof(usize) != sizeof(uint64) || sizeof(rhi::RasterUniformMember) == EXPECTED_UNIFORM_MEMBER_BYTES,
              "encode every RasterUniformMember field in the program cache");

/// Appends a canonical, unambiguous byte encoding: fixed-width little-endian integers and
/// length-prefixed byte strings. Equal encodings mean equal descriptions; no pointer, hash or
/// padding byte participates.
class CanonicalWriter final
{
public:
    explicit CanonicalWriter(Array<uint8>& inoutBytes) noexcept : mBytes(inoutBytes) {}
    template <core::UnsignedInteger ValueType>
    void WriteInteger(ValueType value) noexcept
    {
        const usize offset = mBytes.GetSize();
        if (mHasFailed == true || mBytes.TryResize(offset + sizeof(ValueType)) == false)
        {
            mHasFailed = true;
            return;
        }
        const bool isWritten = core::TryWriteLittleEndian(value, {mBytes.GetData() + offset, sizeof(ValueType)});
        mHasFailed = isWritten == false;
    }
    void WriteFlag(bool isSet) noexcept
    {
        WriteInteger(static_cast<uint8>(isSet == true ? 1 : 0));
    }
    template <typename EnumType>
    void WriteEnum(EnumType value) noexcept
    {
        WriteInteger(static_cast<uint64>(value));
    }
    void WriteText(std::string_view text) noexcept
    {
        WriteInteger(static_cast<uint64>(text.size()));
        for (const char character : text)
        {
            WriteInteger(static_cast<uint8>(character));
        }
    }
    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }

private:
    Array<uint8>& mBytes;
    bool mHasFailed = false;
};
void WriteShader(const rhi::ShaderDescription& shader, CanonicalWriter& inoutWriter) noexcept
{
    inoutWriter.WriteEnum(shader.Stage);
    inoutWriter.WriteInteger(static_cast<uint64>(shader.UniformSize));
    const usize words = shader.Spirv.IsValid() == true ? shader.Spirv.GetSize() : 0;
    inoutWriter.WriteInteger(static_cast<uint64>(words));
    for (usize i = 0; i < words; ++i)
    {
        inoutWriter.WriteInteger(shader.Spirv[i]);
    }
    inoutWriter.WriteText(shader.Wgsl);
    inoutWriter.WriteText(shader.GlslEs);
    inoutWriter.WriteText(shader.SpirvEntry);
    inoutWriter.WriteText(shader.WgslEntry);
    inoutWriter.WriteText(shader.GlslEsEntry);
    inoutWriter.WriteText(shader.Msl);
    inoutWriter.WriteText(shader.MslEntry);
    const usize dxilBytes = shader.Dxil.IsValid() == true ? shader.Dxil.GetSize() : 0;
    inoutWriter.WriteInteger(static_cast<uint64>(dxilBytes));
    for (usize i = 0; i < dxilBytes; ++i)
    {
        inoutWriter.WriteInteger(shader.Dxil[i]);
    }
    inoutWriter.WriteText(shader.DxilEntry);
    inoutWriter.WriteFlag(shader.HasNativeRayStages);
}
/// Every field that reaches device state. ShouldPrepareAsynchronously only schedules preparation of
/// the same artifact, so stages differing only by that hint share one set.
void WriteStage(const rhi::RasterShaderDescription& stage, CanonicalWriter& inoutWriter) noexcept
{
    WriteShader(stage.Artifact, inoutWriter);
    const usize inputs = stage.Inputs.IsValid() == true ? stage.Inputs.GetSize() : 0;
    inoutWriter.WriteInteger(static_cast<uint64>(inputs));
    for (usize i = 0; i < inputs; ++i)
    {
        inoutWriter.WriteInteger(stage.Inputs[i].Location);
        inoutWriter.WriteEnum(stage.Inputs[i].Format);
    }
    const usize bindings = stage.Bindings.IsValid() == true ? stage.Bindings.GetSize() : 0;
    inoutWriter.WriteInteger(static_cast<uint64>(bindings));
    for (usize i = 0; i < bindings; ++i)
    {
        const rhi::RasterBinding& binding = stage.Bindings[i];
        inoutWriter.WriteInteger(binding.Binding);
        inoutWriter.WriteEnum(binding.Kind);
        inoutWriter.WriteEnum(binding.Visibility);
        inoutWriter.WriteInteger(static_cast<uint64>(binding.MinSize));
        inoutWriter.WriteFlag(binding.HasDynamicOffset);
        inoutWriter.WriteInteger(binding.Count);
        inoutWriter.WriteEnum(binding.Indexing);
        inoutWriter.WriteFlag(binding.IsRuntimeSized);
        inoutWriter.WriteFlag(binding.HasPartialBinding);
        inoutWriter.WriteEnum(binding.StorageFormat);
    }
    for (usize i = 0; i < rhi::RASTER_BINDING_COUNT; ++i)
    {
        inoutWriter.WriteText(stage.UniformBlocks[i]);
        inoutWriter.WriteText(stage.TextureNames[i]);
        inoutWriter.WriteInteger(stage.TextureSamplers[i]);
    }
    for (usize i = 0; i < WORKGROUP_DIMENSIONS; ++i)
    {
        inoutWriter.WriteInteger(stage.WorkgroupSize[i]);
    }
    const usize members = stage.UniformMembers.IsValid() == true ? stage.UniformMembers.GetSize() : 0;
    inoutWriter.WriteInteger(static_cast<uint64>(members));
    for (usize i = 0; i < members; ++i)
    {
        const rhi::RasterUniformMember& member = stage.UniformMembers[i];
        inoutWriter.WriteInteger(member.Binding);
        inoutWriter.WriteText(member.Name);
        inoutWriter.WriteInteger(static_cast<uint64>(member.Offset));
        inoutWriter.WriteEnum(member.Shape);
    }
    inoutWriter.WriteFlag(stage.HasVerifiedArrayBounds);
}
[[nodiscard]] bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right) noexcept
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
void DestroyDependencies(internal::RenderGraphUpscaleDependencies& inoutDependencies) noexcept
{
    // RHI retains every incarnation still referenced by a pipeline or accepted work until completion.
    const rhi::DeviceHandle device = inoutDependencies.Device;
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Layout));
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Sampler));
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Vertices));
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Indices));
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Vertex));
    static_cast<void>(rhi::Destroy(device, inoutDependencies.Fragment));
    inoutDependencies.Canonical.Clear();
    inoutDependencies = internal::RenderGraphUpscaleDependencies{};
}
/// Create every dependency; failure leaves the handles created so far for DestroyDependencies.
[[nodiscard]] Status CreateDependencies(const rhi::RasterShaderDescription& vertex,
                                        const rhi::RasterShaderDescription& fragment,
                                        internal::RenderGraphUpscaleDependencies& inoutDependencies) noexcept
{
    const rhi::DeviceHandle device = inoutDependencies.Device;
    Status status = rhi::CreateRasterShader(device, vertex, inoutDependencies.Vertex);
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, fragment, inoutDependencies.Fragment);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, fragment.Bindings, inoutDependencies.Layout);
    }
    rhi::SamplerDescription sampler;
    sampler.Filter = rhi::RasterFilter::Linear;
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateSampler(device, sampler, inoutDependencies.Sampler);
    }
    // Reuse the installed renderer_composite recipe's canonical top-left UV mapping.
    const StaticArray<math::Vector4, QUAD_VERTEX_COUNT> vertices{
        {{-1, 1, 0, 0}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 2, 3, 0}};
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Vertex, VERTEX_BYTES},
                                   {reinterpret_cast<const uint8*>(vertices.GetData()), VERTEX_BYTES},
                                   inoutDependencies.Vertices);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Index32, INDEX_BYTES},
                                   {reinterpret_cast<const uint8*>(indices.GetData()), INDEX_BYTES},
                                   inoutDependencies.Indices);
    }
    return status;
}
} // namespace
struct RenderGraphProgramCache::State final
{
    rhi::DeviceHandle Device{};
    StaticArray<internal::RenderGraphUpscaleDependencies, RENDER_GRAPH_PROGRAM_CACHE_CAPACITY> Upscale{};
    RenderGraphProgramCacheReport Report{};
};
RenderGraphProgramCache::RenderGraphProgramCache() noexcept = default;
RenderGraphProgramCache::~RenderGraphProgramCache() noexcept
{
    Reset();
}
Status RenderGraphProgramCache::Initialize(rhi::DeviceHandle device) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    rhi::DeviceInfo information;
    const rhi::DeviceStatus admitted = rhi::GetDeviceInfo(device, information);
    if (admitted != rhi::DeviceStatus::Ready)
    {
        return admitted == rhi::DeviceStatus::InvalidHandle ? Status::InvalidHandle : Status::NotReady;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    mState.Swap(candidate);
    return Status::Ready;
}
RenderGraphProgramCacheReport RenderGraphProgramCache::GetReport() const noexcept
{
    return mState.Get() != nullptr ? mState->Report : RenderGraphProgramCacheReport{};
}
void RenderGraphProgramCache::Reset() noexcept
{
    if (mState.Get() == nullptr)
    {
        return;
    }
    LUDUS_ASSERT(mState->Report.Leases == 0, "a RenderGraph still holds programs from this cache");
    for (internal::RenderGraphUpscaleDependencies& dependencies : mState->Upscale)
    {
        if (dependencies.IsLive == true)
        {
            DestroyDependencies(dependencies);
        }
    }
    mState.Reset();
}
namespace internal
{
bool RenderGraphProgramCacheAccess::IsUsable(const RenderGraphProgramCache& cache, rhi::DeviceHandle device) noexcept
{
    // The RHI owns one device record at a time; a cache left over from a destroyed device fails admission.
    rhi::DeviceInfo information;
    return cache.mState.Get() != nullptr && cache.mState->Device == device &&
           rhi::GetDeviceInfo(device, information) == rhi::DeviceStatus::Ready;
}
rhi::DeviceHandle RenderGraphProgramCacheAccess::GetDevice(const RenderGraphProgramCache& cache) noexcept
{
    return cache.mState.Get() != nullptr ? cache.mState->Device : rhi::DeviceHandle{};
}
Status RenderGraphProgramCacheAccess::AcquireUpscale(RenderGraphProgramCache& inoutCache,
                                                     const rhi::RasterShaderDescription& vertex,
                                                     const rhi::RasterShaderDescription& fragment,
                                                     RenderGraphUpscaleDependencies*& outDependencies) noexcept
{
    RenderGraphProgramCache::State& state = *inoutCache.mState;
    Array<uint8> canonical;
    CanonicalWriter writer{canonical};
    WriteStage(vertex, writer);
    WriteStage(fragment, writer);
    if (writer.HasFailed() == true)
    {
        return Status::OutOfMemory;
    }
    RenderGraphUpscaleDependencies* freeOrNull = nullptr;
    for (RenderGraphUpscaleDependencies& dependencies : state.Upscale)
    {
        if (dependencies.IsLive == false)
        {
            freeOrNull = freeOrNull == nullptr ? &dependencies : freeOrNull;
            continue;
        }
        // A failed set stays with its leaseholders; a new acquisition prepares a fresh set.
        if (IsAccepted(dependencies.Status) == true && AreBytesEqual(dependencies.Canonical, canonical) == true)
        {
            ++dependencies.Leases;
            ++state.Report.Leases;
            ++state.Report.SharedLeases;
            outDependencies = &dependencies;
            return dependencies.Status;
        }
    }
    if (freeOrNull == nullptr)
    {
        return Status::CapacityExceeded;
    }
    RenderGraphUpscaleDependencies& created = *freeOrNull;
    created.Device = state.Device;
    const Status status = CreateDependencies(vertex, fragment, created);
    if (IsAccepted(status) == false)
    {
        DestroyDependencies(created);
        return status;
    }
    created.Canonical = Move(canonical);
    created.Leases = 1;
    created.Status = Status::Pending;
    created.IsLive = true;
    ++state.Report.LiveEntries;
    ++state.Report.Leases;
    ++state.Report.CreatedEntries;
    outDependencies = &created;
    return Poll(created);
}
Status RenderGraphProgramCacheAccess::Poll(RenderGraphUpscaleDependencies& inoutDependencies) noexcept
{
    if (inoutDependencies.Status != Status::Pending)
    {
        return inoutDependencies.Status;
    }
    const rhi::DeviceHandle device = inoutDependencies.Device;
    Status status = rhi::GetStatus(device, inoutDependencies.Vertex);
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutDependencies.Fragment);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutDependencies.Layout);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutDependencies.Sampler);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutDependencies.Vertices);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(device, inoutDependencies.Indices);
    }
    inoutDependencies.Status = status;
    return status;
}
void RenderGraphProgramCacheAccess::Release(RenderGraphProgramCache& inoutCache,
                                            RenderGraphUpscaleDependencies*& inoutDependenciesOrNull) noexcept
{
    if (inoutDependenciesOrNull == nullptr || inoutCache.mState.Get() == nullptr)
    {
        inoutDependenciesOrNull = nullptr;
        return;
    }
    RenderGraphProgramCache::State& state = *inoutCache.mState;
    RenderGraphUpscaleDependencies& dependencies = *inoutDependenciesOrNull;
    inoutDependenciesOrNull = nullptr;
    LUDUS_ASSERT(dependencies.Leases != 0 && state.Report.Leases != 0, "released a program this cache did not lease");
    --dependencies.Leases;
    --state.Report.Leases;
    if (dependencies.Leases == 0)
    {
        DestroyDependencies(dependencies);
        --state.Report.LiveEntries;
        ++state.Report.ReleasedEntries;
    }
}
} // namespace internal
} // namespace ludus::graphics::renderer
