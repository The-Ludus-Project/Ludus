#include "internal/backend.h"
#include <ludus/foundation/base/checked_integer.hpp>

#include "internal/gpu_scope_backend.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"

#include <ludus/graphics/rhi/graph.h>

#include "internal/lifecycle.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <new>

#include <cstring>

// Thanks to Khronos, "WebGL 2.0 Specification", sections 3.7.3 (buffer objects),
// 3.7.6 (textures) and 3.7.9 (drawing buffers), for the deliberately explicit
// range/packing and mesh-local indexed-draw boundaries:
// https://registry.khronos.org/webgl/specs/latest/2.0/
// Ownership/retention follows docs/architecture/rhi-gdi.md; this is the RHI's
// single portable registry. Backend objects never become public identities.
namespace ludus::graphics::rhi
{
namespace internal
{
struct RasterIdentity final
{
    foundation::uint64 Owner;
    foundation::usize Slot;
    foundation::uint64 Generation;
};
struct BindingArrayLocation final
{
    foundation::uint32 Binding = 0;
    foundation::uint32 Element = 0;
};
struct RasterAccess final
{
    static const BindingSetHandle& GetIndexSet(const RasterBindingIndex& index) noexcept
    {
        return index.mSet;
    }
    static foundation::uint32 GetIndexBinding(const RasterBindingIndex& index) noexcept
    {
        return index.mBinding;
    }
    static foundation::uint32 GetIndexElement(const RasterBindingIndex& index) noexcept
    {
        return index.mElement;
    }
    static void
    SetIndex(const BindingSetHandle& set, const BindingArrayLocation& location, RasterBindingIndex& outIndex) noexcept
    {
        outIndex.mSet = set;
        outIndex.mBinding = location.Binding;
        outIndex.mElement = location.Element;
    }
    template <typename T>
    static foundation::uint64 Owner(T handle) noexcept
    {
        return handle.Owner;
    }
    template <typename T>
    static foundation::uint64 Generation(T handle) noexcept
    {
        return handle.Generation;
    }
    template <typename T>
    static foundation::usize Slot(T handle) noexcept
    {
        return handle.Slot == 0 ? RASTER_INVALID : handle.Slot - 1;
    }
    template <typename T>
    static void Set(T& handle, const RasterIdentity& identity) noexcept
    {
        handle.Owner = identity.Owner;
        handle.Slot = static_cast<foundation::uint32>(identity.Slot + 1);
        handle.Generation = identity.Generation;
    }
};
} // namespace internal

namespace
{
using namespace foundation;
using namespace internal;
constexpr usize KINDS = static_cast<usize>(RasterKind::Count);
struct CpuTextureRecord final
{
    StorageArray<uint8> Bytes;
    TextureMipUpload Mips[16];
};
struct RayRecordInfo final
{
    StorageArray<usize> GeometrySlots;
    StorageArray<float32> TriangleUvs;
    usize Scene = 0, Program = 0, Count = 0;
    RayAllocation Allocation;
};
struct Record final
{
    core::UniquePtr<RayRecordInfo> Ray;
    core::UniquePtr<CpuTextureRecord> CpuTexture;
    uint64 Owner = 0;
    uint64 Generation = 1;
    uint64 LastUse = 0;
    uint32 Request = 0;
    uint32 Callbacks = 0;
    usize References = 0;
    usize ExternalReads = 0;
    RasterStatus Status = RasterStatus::Failed;
    bool Published = false;
    bool InFrame = false;
    bool ServiceOwned = false;
    bool QueuedPipeline = false;
    BufferDescription Buffer;
    uint64 BufferRevision = 1;
    bool FrameBufferWritten = false;
    ComputePipelineInfo Compute;
    TextureDescription Texture;
    uint64 TextureRevision = 1;
    uint32 InitializedMips = 0;
    RasterTextureUse TextureUse = RasterTextureUse::Undefined;
    RasterTextureUse FrameTextureUse = RasterTextureUse::Undefined;
    bool ColorDefined = false;
    bool DepthDefined = false;
    bool FrameColorDefined = false;
    bool FrameDepthDefined = false;
    bool StencilDefined = false, FrameStencilDefined = false;
    StorageArray<uint8> AttachmentState, FrameAttachmentState;
    usize TextureSlot = 0;
    TextureViewDescription View;
    RasterShaderInfo Shader;
    RasterLayout Layout;
    RasterSet Set;
    RasterPipelineInfo Pipeline;
    StorageArray<uint8> Indices;
    uint64 StorageBytes = 0;
    usize AuxiliaryHostBytes = 0;
};
constexpr usize RASTER_KINDS = static_cast<usize>(RasterKind::RayGeometry);
StorageTable<Record> gRecords[KINDS];
RasterSequence<uint32> gRequests;
RasterSequence<uint64> gSubmissions;
uint64 gFrameSubmission = 0;
uint64 gFirstAcceptedSubmission = 0;
uint64 gLastAcceptedSubmission = 0;
usize gDrawCount = 0;
RasterPassInfo gRasterPass;
bool gSurfaceColorDefined = true;
bool gSurfaceDepthDefined = false;
bool gGraphExecuting = false;
void ResetGraphRecords() noexcept;
RasterStatus ValidatePacketPass(const RasterPacket&, const RasterPassInfo&, bool) noexcept;
RasterStatus ComputeAdmission(DeviceHandle, bool = true) noexcept;
bool StorageRole(BufferRole role) noexcept
{
    return role == BufferRole::Storage || role == BufferRole::StorageVertex || role == BufferRole::StorageIndirect;
}
bool BufferBinding(RasterBindingKind kind) noexcept
{
    return kind == RasterBindingKind::UniformBuffer || kind == RasterBindingKind::StorageRead ||
           kind == RasterBindingKind::StorageReadWrite;
}
void RetainPacketTextures(const RasterPacket&) noexcept;

Record& At(RasterKind kind, usize slot) noexcept
{
    return gRecords[static_cast<usize>(kind)][slot];
}
RasterStatus Admission(DeviceHandle device, bool mutation = true) noexcept
{
    DeviceInfo info;
    switch (GetDeviceInfo(device, info))
    {
        case DeviceStatus::InvalidHandle:
            return RasterStatus::InvalidHandle;
        case DeviceStatus::Ready:
            break;
        case DeviceStatus::Pending:
            return RasterStatus::NotReady;
        case DeviceStatus::DeviceLost:
            return RasterStatus::DeviceLost;
        default:
            return RasterStatus::Failed;
    }
    if (info.Enabled.PortableRaster == false)
    {
        return RasterStatus::Unsupported;
    }
    return mutation && FrameOpen() ? RasterStatus::InvalidState : RasterStatus::Ready;
}
template <typename T>
Record* Resolve(T handle, RasterKind kind, bool published = true) noexcept
{
    const auto slot = RasterAccess::Slot(handle);
    if (slot >= gRecords[static_cast<usize>(kind)].GetSize() || RasterAccess::Owner(handle) == 0)
    {
        return nullptr;
    }
    auto& record = At(kind, slot);
    return record.Owner == DeviceOwner() && record.Owner == RasterAccess::Owner(handle) &&
                   record.Generation == RasterAccess::Generation(handle) && (published == false || record.Published)
               ? &record
               : nullptr;
}
void Retain(RasterKind kind, usize slot) noexcept
{
    ++At(kind, slot).References;
}
void Release(RasterKind kind, usize slot) noexcept
{
    --At(kind, slot).References;
}
void Dependencies(Record& record, RasterKind kind, bool retain) noexcept
{
    const auto reference = [retain](RasterKind dependency, usize slot) noexcept {
        if (retain)
        {
            Retain(dependency, slot);
        }
        else
        {
            Release(dependency, slot);
        }
    };
    if (kind == RasterKind::View)
    {
        reference(RasterKind::Texture, record.TextureSlot);
    }
    else if (kind == RasterKind::Pipeline)
    {
        reference(RasterKind::Shader, record.Pipeline.Vertex);
        reference(RasterKind::Shader, record.Pipeline.Fragment);
        reference(RasterKind::Layout, record.Pipeline.Layout);
    }
    else if (kind == RasterKind::ComputePipeline)
    {
        reference(RasterKind::Shader, record.Compute.Shader);
        reference(RasterKind::Layout, record.Compute.Layout);
    }
    else if (kind == RasterKind::RayScene)
    {
        for (auto slot : record.Ray->GeometrySlots)
        {
            reference(RasterKind::RayGeometry, slot);
        }
    }
    else if (kind == RasterKind::RayBatch)
    {
        reference(RasterKind::RayProgram, record.Ray->Program);
        reference(RasterKind::RayScene, record.Ray->Scene);
    }
    else if (kind == RasterKind::Set)
    {
        const RasterLayout& layout = record.Set.Description;
        for (usize i = 0; i < layout.DescriptorCount; ++i)
        {
            const RasterBindingKind type = layout.GetDescriptorBinding(i).Kind;
            reference(BufferBinding(type)    ? RasterKind::Buffer
                      : TextureBinding(type) ? RasterKind::View
                                             : RasterKind::Sampler,
                      record.Set.Slots[i]);
        }
        reference(RasterKind::Layout, record.Set.Layout);
    }
}
void Collect() noexcept
{
    const auto completed = backend::RasterCompleted();
    for (usize kind = static_cast<usize>(RasterKind::RayGeometry); kind < KINDS; ++kind)
    {
        auto& pool = gRecords[kind];
        for (usize slot = 0; slot < pool.GetSize(); ++slot)
        {
            auto& record = gRecords[kind][slot];
            if (record.Owner != 0 && record.Status == RasterStatus::Pending)
            {
                record.Status = backend::RayPoll(static_cast<RasterKind>(kind), slot);
            }
        }
    }
    // Acyclic dependencies can expose another detached record in each pass.
    for (usize pass = 0; pass < KINDS; ++pass)
    {
        for (usize kind = KINDS; kind-- > 0;)
        {
            for (usize slot = 0; slot < gRecords[kind].GetSize(); ++slot)
            {
                auto& record = gRecords[kind][slot];
                if (record.Owner == 0 || record.References != 0 || record.Status == RasterStatus::Pending ||
                    record.Callbacks != 0 || record.LastUse > completed)
                {
                    continue;
                }
                backend::RasterDestroy(static_cast<RasterKind>(kind), slot);
                ReleaseResourceStorage(record.StorageBytes);
                Dependencies(record, static_cast<RasterKind>(kind), false);
                const auto generation = RasterNextGeneration(record.Generation);
                ReleaseHostStorage(record.AuxiliaryHostBytes);
                record.CpuTexture.Reset();
                record.Ray.Reset();
                record = {};
                record.Generation = generation;
            }
        }
    }
}
RasterStatus Reserve(RasterKind kind, usize& slot) noexcept
{
    Collect();
    if (GetStartup().State != StartupState::Ready)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    auto& records = gRecords[static_cast<usize>(kind)];
    bool exhausted = false;
    const auto limit = static_cast<usize>(kind) < RASTER_KINDS ? usize{CurrentDeviceProfile().Raster.ResourcesPerKind}
                                                               : usize{CurrentStorageLimits().RayResourcesPerKind};
    for (slot = 0; slot < limit; ++slot)
    {
        if (slot == records.GetSize())
        {
            const auto remaining = limit - slot;
            const auto size = slot + (remaining < 64 ? remaining : 64);
            if (records.EnsureSlots(size) == false)
            {
                return StorageFailure();
            }
        }
        auto& record = records[slot];
        if (record.Owner != 0)
        {
            continue;
        }
        if (record.Generation == 0)
        {
            exhausted = true;
            continue;
        }
        if (gRequests.Next == 0)
        {
            return RasterStatus::IdentityExhausted;
        }
        const auto grown =
            static_cast<usize>(kind) < RASTER_KINDS ? backend::RasterGrow(kind, slot + 1) : RasterStatus::Ready;
        if (grown != RasterStatus::Ready)
        {
            return grown;
        }
        record.Owner = DeviceOwner();
        record.References = 1;
        record.Request = gRequests.Take();
        record.Status = RasterStatus::Pending;
        return RasterStatus::Ready;
    }
    return exhausted ? RasterStatus::IdentityExhausted : RasterStatus::CapacityExceeded;
}
template <typename T, typename Create>
RasterStatus Publish(RasterKind kind, T& output, Create create, uint64 storageBytes = 0) noexcept
{
    if (RasterAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    usize slot = 0;
    Collect();
    if (GetStartup().State != StartupState::Ready)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    if (ReserveResourceStorage(storageBytes) == false)
    {
        return StorageFailure();
    }
    const auto reservation = Reserve(kind, slot);
    if (reservation != RasterStatus::Ready)
    {
        ReleaseResourceStorage(storageBytes);
        return reservation;
    }
    auto& record = At(kind, slot);
    record.StorageBytes = storageBytes;
    const auto result = create(slot, record);
    if (record.Owner == 0)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    // Spontaneous error-scope callbacks may have updated Status inside create.
    if (result != RasterStatus::Ready && result != RasterStatus::Pending)
    {
        backend::RasterDestroy(kind, slot);
        ReleaseResourceStorage(record.StorageBytes);
        const auto generation = record.Generation;
        ReleaseHostStorage(record.AuxiliaryHostBytes);
        record.CpuTexture.Reset();
        record.Ray.Reset();
        record = {};
        record.Generation = generation;
        return result;
    }
    if (record.Status == RasterStatus::Pending)
    {
        record.Status = result;
    }
    Dependencies(record, kind, true);
    record.Published = true;
    RasterAccess::Set(output, {record.Owner, slot, record.Generation});
    return result;
}
bool ValidBinding(const RasterBinding& binding) noexcept
{
    const auto visibility = static_cast<uint8>(binding.Visibility);
    if (binding.Binding >= RASTER_BINDINGS || visibility < 1 || visibility > 4 ||
        (binding.HasDynamicOffset == true && BufferBinding(binding.Kind) == false) || binding.Count == 0 ||
        (binding.Count != 1 && binding.HasDynamicOffset == true) ||
        (binding.Count > 1 &&
         (binding.Kind == RasterBindingKind::TextureDepth2D || binding.Kind == RasterBindingKind::TextureStencil2D)) ||
        (binding.IsRuntimeSized == true &&
         (TextureBinding(binding.Kind) == false || binding.Kind == RasterBindingKind::TextureDepth2D ||
          binding.Kind == RasterBindingKind::TextureStencil2D)) ||
        (binding.HasPartialBinding == true && binding.Count == 1 && binding.IsRuntimeSized == false) ||
        static_cast<uint8>(binding.Indexing) > static_cast<uint8>(RasterBindingIndexing::NonUniform) ||
        (binding.Count == 1 && binding.IsRuntimeSized == false &&
         binding.Indexing != RasterBindingIndexing::Constant) ||
        (binding.Count > 1 && TextureBinding(binding.Kind) == false &&
         binding.Indexing != RasterBindingIndexing::Constant))
    {
        return false;
    }
    if (binding.Kind == RasterBindingKind::UniformBuffer)
    {
        return binding.MinSize > 0 && binding.MinSize <= 16384;
    }
    if (binding.Kind == RasterBindingKind::StorageRead || binding.Kind == RasterBindingKind::StorageReadWrite)
    {
        const ComputeCapabilities limits = backend::ComputeLimits();
        return visibility == 4 && binding.MinSize > 0 && binding.MinSize % 4 == 0 &&
               binding.MinSize <= limits.MaxStorageRange;
    }
    return visibility != 4 && (TextureBinding(binding.Kind) == true || binding.Kind == RasterBindingKind::Sampler) &&
           binding.MinSize == 0;
}
bool CopyLayout(std::span<const RasterBinding> entries, RasterLayout& outLayout) noexcept
{
    if (entries.size() > RASTER_BINDINGS)
    {
        return false;
    }
    outLayout.Count = entries.size();
    outLayout.DescriptorCount = 0;
    for (usize i = 0; i < entries.size(); ++i)
    {
        if (ValidBinding(entries[i]) == false)
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (entries[i].Binding == entries[j].Binding)
            {
                return false;
            }
        }
        if (TryAdd(outLayout.DescriptorCount, usize{entries[i].Count}, outLayout.DescriptorCount) == false)
        {
            return false;
        }
        if (entries[i].IsRuntimeSized == true)
        {
            for (const RasterBinding& candidate : entries)
            {
                if (candidate.Binding > entries[i].Binding ||
                    (candidate.IsRuntimeSized == true && candidate.Binding != entries[i].Binding))
                {
                    return false;
                }
            }
        }
        outLayout.Entries[i] = entries[i];
    }
    for (usize entry = 0; entry < entries.size(); ++entry)
    {
        if (entries[entry].IsRuntimeSized == false)
        {
            continue;
        }
        if (entries[entry].Binding != RUNTIME_BINDING_ARRAY_SLOT)
        {
            return false;
        }
        bool hasLengthBinding = false;
        for (usize candidate = 0; candidate < outLayout.Count; ++candidate)
        {
            const RasterBinding& binding = outLayout.Entries[candidate];
            if (binding.Binding != RUNTIME_BINDING_LENGTH_SLOT)
            {
                continue;
            }
            hasLengthBinding = true;
            if (binding.Kind != RasterBindingKind::UniformBuffer || binding.MinSize != RUNTIME_BINDING_LENGTH_BYTES ||
                binding.Count != 1 || binding.HasDynamicOffset == true || binding.HasPartialBinding == true ||
                (static_cast<uint8>(binding.Visibility) & static_cast<uint8>(entries[entry].Visibility)) !=
                    static_cast<uint8>(entries[entry].Visibility))
            {
                return false;
            }
        }
        if (hasLengthBinding == false)
        {
            if (outLayout.Count == RASTER_BINDINGS)
            {
                return false;
            }
            outLayout.Entries[outLayout.Count++] =
            {
                .Binding = RUNTIME_BINDING_LENGTH_SLOT,
                .Kind = RasterBindingKind::UniformBuffer,
                .Visibility = entries[entry].Visibility,
                .MinSize = RUNTIME_BINDING_LENGTH_BYTES,
            };
            if (TryAdd(outLayout.DescriptorCount, usize{1}, outLayout.DescriptorCount) == false)
            {
                return false;
            }
        }
    }
    return true;
}
RasterStatus ValidateLayoutCapabilities(const RasterLayout& layout) noexcept
{
    const BindingCapabilities capabilities = backend::BindingLimits();
    if (layout.DescriptorCount > capabilities.MaxDescriptorsPerSet || layout.Count > capabilities.MaxBindings)
    {
        return RasterStatus::Unsupported;
    }
    uint32 totalUniforms = 0;
    uint32 totalSamplers = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        totalUniforms +=
            layout.Entries[entry].Kind == RasterBindingKind::UniformBuffer ? layout.Entries[entry].Count : 0U;
        totalSamplers += layout.Entries[entry].Kind == RasterBindingKind::Sampler ? layout.Entries[entry].Count : 0U;
    }
    if (totalUniforms > capabilities.MaxUniformBuffersPerSet || totalSamplers > capabilities.MaxSamplersPerSet)
    {
        return RasterStatus::Unsupported;
    }
    for (uint32 stage = 1; stage <= 4; stage <<= 1U)
    {
        uint32 uniforms = 0;
        uint32 sampled = 0;
        uint32 samplers = 0;
        uint32 storage = 0;
        for (usize entry = 0; entry < layout.Count; ++entry)
        {
            const RasterBinding& binding = layout.Entries[entry];
            if (binding.Binding >= capabilities.MaxBindings)
            {
                return RasterStatus::Unsupported;
            }
            if ((static_cast<uint32>(binding.Visibility) & stage) == 0)
            {
                continue;
            }
            uniforms += binding.Kind == RasterBindingKind::UniformBuffer ? binding.Count : 0U;
            sampled += TextureBinding(binding.Kind) == true ? binding.Count : 0U;
            samplers += binding.Kind == RasterBindingKind::Sampler ? binding.Count : 0U;
            storage +=
                binding.Kind == RasterBindingKind::StorageRead || binding.Kind == RasterBindingKind::StorageReadWrite
                    ? binding.Count
                    : 0U;
        }
        if (uniforms > capabilities.MaxUniformBuffersPerStage || sampled > capabilities.MaxSampledTexturesPerStage ||
            samplers > capabilities.MaxSamplersPerStage || storage > backend::ComputeLimits().MaxStorageBindings)
        {
            return RasterStatus::Unsupported;
        }
    }
    uint32 dynamicUniforms = 0;
    uint32 dynamicStorage = 0;
    uint32 textures = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& binding = layout.Entries[entry];
        if (binding.HasDynamicOffset == true)
        {
            if (binding.Kind == RasterBindingKind::UniformBuffer)
            {
                ++dynamicUniforms;
            }
            else
            {
                ++dynamicStorage;
            }
        }
        if (TextureBinding(binding.Kind) == true)
        {
            textures += binding.Count;
        }
        if ((binding.Count > 1 && TextureBinding(binding.Kind) == true &&
             binding.Count > capabilities.MaxTextureArrayElements) ||
            (binding.Count > 1 && BufferBinding(binding.Kind) == true &&
             capabilities.HasBufferDescriptorArrays == false) ||
            (binding.Count > 1 && binding.Kind == RasterBindingKind::Sampler &&
             capabilities.HasSamplerDescriptorArrays == false) ||
            (binding.IsRuntimeSized == true && capabilities.HasRuntimeTextureArrays == false) ||
            (binding.HasPartialBinding == true && capabilities.HasPartiallyBoundArrays == false) ||
            (binding.Indexing == RasterBindingIndexing::Uniform && capabilities.HasUniformTextureIndexing == false) ||
            (binding.Indexing == RasterBindingIndexing::NonUniform && capabilities.HasBindlessTextureIndexing == false))
        {
            return RasterStatus::Unsupported;
        }
        if (backend::Kind() == Backend::Metal &&
            (TextureBinding(binding.Kind) == true || binding.Kind == RasterBindingKind::Sampler))
        {
            const uint32 registerLimit = binding.Kind == RasterBindingKind::Sampler
                                             ? capabilities.MaxSamplersPerStage
                                             : capabilities.MaxTextureArrayElements;
            if (binding.Count > registerLimit || binding.Binding > registerLimit - binding.Count)
            {
                return RasterStatus::Unsupported;
            }
            for (usize previous = 0; previous < entry; ++previous)
            {
                const RasterBinding& other = layout.Entries[previous];
                if (((TextureBinding(binding.Kind) == true && TextureBinding(other.Kind) == true) ||
                     (binding.Kind == RasterBindingKind::Sampler && other.Kind == RasterBindingKind::Sampler)) &&
                    binding.Binding < other.Binding + other.Count && other.Binding < binding.Binding + binding.Count)
                {
                    return RasterStatus::InvalidDescription;
                }
            }
        }
    }
    if (dynamicUniforms > capabilities.MaxDynamicUniformBuffers ||
        dynamicStorage > capabilities.MaxDynamicStorageBuffers ||
        (capabilities.MaxTextureArrayElements > 1 && textures > capabilities.MaxTextureArrayElements))
    {
        return RasterStatus::Unsupported;
    }
    return RasterStatus::Ready;
}
bool CopyName(std::string_view name, char (&output)[64]) noexcept
{
    if (name.size() >= 64)
    {
        return false;
    }
    if (name.empty())
    {
        return true;
    }
    for (const char c : name)
    {
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') && c != '_')
        {
            return false;
        }
    }
    std::memcpy(output, name.data(), name.size());
    return true;
}
bool Fits(usize offset, usize size, usize total) noexcept
{
    return offset <= total && size <= total - offset;
}
RasterStatus ValidateBindingOffsets(const RasterSet& set, const RasterBindingOffsets& offsets) noexcept
{
    const RasterLayout& layout = set.Description;
    const RasterCapabilities raster = backend::RasterLimits();
    const ComputeCapabilities compute = backend::ComputeLimits();
    for (usize bindingNumber = 0; bindingNumber < RASTER_BINDINGS; ++bindingNumber)
    {
        const uint32 offset = offsets.Bytes[bindingNumber];
        bool hasDynamicBinding = false;
        for (usize entry = 0; entry < layout.DescriptorCount; ++entry)
        {
            const RasterBinding& binding = layout.GetDescriptorBinding(entry);
            if (binding.Binding != bindingNumber || binding.HasDynamicOffset == false)
            {
                continue;
            }
            hasDynamicBinding = true;
            const usize alignment = binding.Kind == RasterBindingKind::UniformBuffer ? raster.UniformOffsetAlignment
                                                                                     : compute.StorageOffsetAlignment;
            usize effectiveOffset = 0;
            if (alignment == 0 || offset % alignment != 0 ||
                TryAdd(set.Offsets[entry], usize{offset}, effectiveOffset) == false ||
                Fits(effectiveOffset, set.Sizes[entry], At(RasterKind::Buffer, set.Slots[entry]).Buffer.Size) == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        if (hasDynamicBinding == false && offset != 0)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    return RasterStatus::Ready;
}
constexpr uint8 COLOR_DEFINED = 1, DEPTH_DEFINED = 2, STENCIL_DEFINED = 4;
bool AllDefined(const StorageArray<uint8>& states, uint8 aspect) noexcept
{
    if (states.GetSize() == 0)
    {
        return false;
    }
    for (auto state : states)
    {
        if ((state & aspect) == 0)
        {
            return false;
        }
    }
    return true;
}
usize SubresourceIndex(const TextureDescription& texture, uint32 mip, uint32 layer) noexcept
{
    return static_cast<usize>(mip) * texture.DepthOrLayers + layer;
}
bool ViewDefined(const Record& texture, const TextureViewDescription& view, const StorageArray<uint8>& states) noexcept
{
    if (texture.Texture.Attachment == false)
    {
        return true;
    }
    const uint8 aspect = view.Aspect == TextureAspect::Depth     ? DEPTH_DEFINED
                         : view.Aspect == TextureAspect::Stencil ? STENCIL_DEFINED
                                                                 : COLOR_DEFINED;
    for (uint32 mip = view.BaseMip; mip < view.BaseMip + view.MipCount; ++mip)
    {
        for (uint32 layer = view.BaseLayer; layer < view.BaseLayer + view.LayerCount; ++layer)
        {
            if ((states[SubresourceIndex(texture.Texture, mip, layer)] & aspect) == 0)
            {
                return false;
            }
        }
    }
    return true;
}
void FrameRetain(RasterKind kind, usize slot) noexcept
{
    auto& record = At(kind, slot);
    if (record.InFrame == false)
    {
        record.InFrame = true;
        record.FrameTextureUse = record.TextureUse;
        record.FrameColorDefined = record.ColorDefined;
        record.FrameDepthDefined = record.DepthDefined;
        record.FrameStencilDefined = record.StencilDefined;
        for (usize i = 0; i < record.AttachmentState.GetSize(); ++i)
        {
            record.FrameAttachmentState[i] = record.AttachmentState[i];
        }
        ++record.References;
    }
}
bool ShaderFits(const RasterShaderInfo& shader, const RasterLayout& layout) noexcept
{
    for (usize i = 0; i < shader.Layout.Count; ++i)
    {
        const auto& need = shader.Layout.Entries[i];
        bool found = false;
        const auto stage = shader.Stage == ShaderStage::Vertex ? 1U : shader.Stage == ShaderStage::Fragment ? 2U : 4U;
        for (usize j = 0; j < layout.Count; ++j)
        {
            const auto& entry = layout.Entries[j];
            if (entry.Binding == need.Binding && entry.Kind == need.Kind && entry.MinSize >= need.MinSize &&
                entry.IsRuntimeSized == need.IsRuntimeSized &&
                (need.IsRuntimeSized == true ? entry.Count >= need.Count : entry.Count == need.Count) &&
                static_cast<uint8>(entry.Indexing) >= static_cast<uint8>(need.Indexing) &&
                (static_cast<uint8>(entry.Visibility) & stage) != 0)
            {
                found = true;
            }
        }
        if (found == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace
RasterStatus GetRasterCapabilities(DeviceHandle device, RasterCapabilities& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission == RasterStatus::InvalidHandle)
    {
        return admission;
    }
    output = admission == RasterStatus::Ready ? internal::CurrentDeviceProfile().Raster : RasterCapabilities{};
    return admission;
}
RasterStatus CreateBuffer(DeviceHandle device,
                          const BufferDescription& description,
                          std::span<const uint8> bytes,
                          BufferHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto limits = backend::RasterLimits();
    if (description.Size == 0 || bytes.size() != description.Size || description.Size > limits.MaxBufferSize ||
        static_cast<uint8>(description.Role) > static_cast<uint8>(BufferRole::StorageIndirect))
    {
        return RasterStatus::InvalidDescription;
    }
    if (StorageRole(description.Role))
    {
        const auto compute = ComputeAdmission(device);
        if (compute != RasterStatus::Ready)
        {
            return compute;
        }
    }
    const usize alignment = description.Role == BufferRole::Index16   ? 2
                            : description.Role == BufferRole::Uniform ? 16
                                                                      : 4;
    if (description.Size % alignment != 0)
    {
        return RasterStatus::InvalidDescription;
    }
    return Publish(
        RasterKind::Buffer,
        output,
        [&](usize slot, Record& record) noexcept {
            record.Buffer = description;
            if (description.Role == BufferRole::Index16 || description.Role == BufferRole::Index32)
            {
                if (record.Indices.TryResize(bytes.size()) == false)
                {
                    return StorageFailure();
                }
                std::memcpy(record.Indices.GetData(), bytes.data(), bytes.size());
            }
            return backend::RasterCreateBuffer(slot, description, bytes, record.Request);
        },
        description.Size);
}
RasterStatus GetFormatCapabilities(DeviceHandle device, RasterFormat format, FormatCapabilities& output) noexcept
{
    if (static_cast<uint8>(format) > static_cast<uint8>(RasterFormat::Bc5RgUnorm))
    {
        return RasterStatus::InvalidDescription;
    }
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        if (admission != RasterStatus::InvalidHandle)
        {
            output = {};
        }
        return admission;
    }
    output = internal::CurrentDeviceProfile().Formats[static_cast<usize>(format)];
    return output.Uses.Sampled ? RasterStatus::Ready : RasterStatus::Unsupported;
}
RasterStatus CreateTexture(DeviceHandle device,
                           const TextureDescription& description,
                           const TextureUpload& upload,
                           TextureHandle& output) noexcept
{
    const RasterStatus admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const RasterCapabilities textureLimits = backend::RasterLimits();
    if (description.Width == 0 || description.Height == 0 || description.DepthOrLayers == 0 ||
        static_cast<uint8>(description.Dimension) > static_cast<uint8>(TextureDimension::D3) ||
        static_cast<uint8>(description.Format) > static_cast<uint8>(RasterFormat::Bc5RgUnorm) ||
        (description.Dimension == TextureDimension::D2 && description.DepthOrLayers != 1) ||
        (description.Dimension == TextureDimension::Cube &&
         (description.DepthOrLayers != 6 || description.Width != description.Height)) ||
        (description.Stencil && description.Attachment == false) ||
        (description.SampleCount != 1 && description.SampleCount != 4) ||
        (description.SampleCount != 1 &&
         (description.Attachment == false || description.Dimension != TextureDimension::D2 ||
          description.MipLevels != 1)) ||
        (description.Attachment &&
         (description.Dimension == TextureDimension::D3 || description.Format == RasterFormat::R8Unorm ||
          description.Format == RasterFormat::Rg8Unorm || CompressedTexture(description.Format))) ||
        (description.RetainCpuContent &&
         (description.Attachment || description.Dimension != TextureDimension::D2 ||
          description.Format == RasterFormat::Rgba16Float || CompressedTexture(description.Format))))
    {
        return RasterStatus::InvalidDescription;
    }
    const FormatCapabilities format = internal::CurrentDeviceProfile().Formats[static_cast<usize>(description.Format)];
    const uint32 dimension = description.Attachment && description.Dimension == TextureDimension::Cube
                                 ? format.MaxAttachmentCubeDimension
                             : description.Attachment                          ? format.MaxAttachmentDimension2D
                             : description.Dimension == TextureDimension::Cube ? format.MaxCubeDimension
                             : description.Dimension == TextureDimension::D3   ? format.MaxDimension3D
                                                                               : format.MaxSampledDimension2D;
    if (format.Uses.Sampled == false || format.Uses.Filterable == false ||
        (description.Attachment && format.Uses.ColorAttachment == false) || description.Width > dimension ||
        description.Height > dimension || description.MipLevels > format.MaxMipLevels ||
        (description.Attachment && (description.MipLevels > format.MaxAttachmentMipLevels ||
                                    description.DepthOrLayers > format.MaxAttachmentLayers ||
                                    (format.SampleCounts & description.SampleCount) == 0 ||
                                    (description.Stencil && format.StencilAttachment == false))) ||
        (description.Dimension == TextureDimension::D2Array && description.DepthOrLayers > format.MaxArrayLayers) ||
        (description.Dimension == TextureDimension::D3 && description.DepthOrLayers > dimension))
    {
        return RasterStatus::Unsupported;
    }
    if (CompressedTexture(description.Format) == true && format.HasUnalignedBlockExtents == false &&
        (description.Width % 4 != 0 || description.Height % 4 != 0))
    {
        return RasterStatus::Unsupported;
    }
    uint32 levels = 1;
    const uint32 largest2D = description.Width > description.Height ? description.Width : description.Height;
    const uint32 largest = description.Dimension == TextureDimension::D3 && description.DepthOrLayers > largest2D
                               ? description.DepthOrLayers
                               : largest2D;
    for (uint32 extent = largest; extent > 1; extent /= 2)
    {
        ++levels;
    }
    if (description.MipLevels == 0 || description.MipLevels > levels || description.MipLevels > 16)
    {
        return RasterStatus::InvalidDescription;
    }
    uint64 storageBytes = 0;
    uint32 storageWidth = description.Width;
    uint32 storageHeight = description.Height;
    const uint64 storageChannels = TextureBlockBytes(description.Format);
    for (uint32 mip = 0; mip < description.MipLevels; ++mip)
    {
        uint64 pixels = 0;
        uint64 bytes = 0;
        if (foundation::TryMultiply(uint64{TextureBlockRows(description.Format, storageWidth)},
                                    uint64{TextureBlockRows(description.Format, storageHeight)},
                                    pixels) == false ||
            foundation::TryMultiply(pixels, uint64{TextureSlices(description, mip)}, pixels) == false ||
            foundation::TryMultiply(pixels, storageChannels * description.SampleCount, bytes) == false ||
            foundation::TryAdd(storageBytes, bytes, storageBytes) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        if (description.Attachment)
        {
            if (backend::Kind() == Backend::WebGL2 &&
                (foundation::TryMultiply(pixels, storageChannels, bytes) == false ||
                 foundation::TryAdd(storageBytes, bytes, storageBytes) == false))
            {
                return RasterStatus::InvalidDescription;
            }
            if (foundation::TryMultiply(pixels,
                                        uint64{description.Stencil ? 8U : 4U} * description.SampleCount,
                                        bytes) == false ||
                foundation::TryAdd(storageBytes, bytes, storageBytes) == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        storageWidth = storageWidth > 1 ? storageWidth / 2 : 1;
        storageHeight = storageHeight > 1 ? storageHeight / 2 : 1;
    }
    if (description.DeferredInitialization &&
        (description.Attachment || description.RetainCpuContent || description.Format != RasterFormat::Rgba8Unorm ||
         description.Dimension != TextureDimension::D2))
    {
        return RasterStatus::InvalidDescription;
    }
    if (description.Attachment || description.DeferredInitialization)
    {
        if (upload.Bytes.empty() == false || upload.RowPitch != 0 || upload.Mips.empty() == false)
        {
            return RasterStatus::InvalidDescription;
        }
        return Publish(
            RasterKind::Texture,
            output,
            [&](usize slot, Record& record) noexcept {
                record.Texture = description;
                if (description.Attachment &&
                    (record.AttachmentState.TryResize(static_cast<usize>(description.MipLevels) *
                                                      description.DepthOrLayers) == false ||
                     record.FrameAttachmentState.TryResize(record.AttachmentState.GetSize()) == false))
                {
                    return StorageFailure();
                }
                return backend::RasterCreateTexture(slot, description, upload, record.Request);
            },
            storageBytes);
    }
    if ((upload.Mips.empty() && description.MipLevels != 1) ||
        (upload.Mips.empty() == false && (upload.Mips.size() != description.MipLevels || upload.RowPitch != 0)))
    {
        return RasterStatus::InvalidDescription;
    }
    usize occupied = 0;
    uint32 width = description.Width;
    uint32 height = description.Height;
    const usize channels = TextureBlockBytes(description.Format);
    for (usize level = 0; level < description.MipLevels; ++level)
    {
        const usize pitch = upload.Mips.empty() ? upload.RowPitch : upload.Mips[level].RowPitch;
        const usize offset = upload.Mips.empty() ? usize{0} : upload.Mips[level].Offset;
        const usize rows = TextureBlockRows(description.Format, height);
        const usize slices = TextureSlices(description, static_cast<uint32>(level));
        if (pitch < static_cast<usize>(TextureBlockRows(description.Format, width)) * channels ||
            pitch % (channels > 4 ? channels : 4) != 0 || offset != occupied || occupied > upload.Bytes.size() ||
            pitch > (upload.Bytes.size() - occupied) / rows / slices)
        {
            return RasterStatus::InvalidDescription;
        }
        occupied += pitch * rows * slices;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    if (upload.Bytes.data() == nullptr || occupied != upload.Bytes.size() ||
        occupied > textureLimits.MaxTextureUploadBytes)
    {
        return RasterStatus::InvalidDescription;
    }
    return Publish(
        RasterKind::Texture,
        output,
        [&](usize slot, Record& record) noexcept {
            record.Texture = description;
            if (description.RetainCpuContent)
            {
                if (ReserveHostStorage(sizeof(CpuTextureRecord)) == false)
                {
                    return StorageFailure();
                }
                record.AuxiliaryHostBytes = sizeof(CpuTextureRecord);
                CpuTextureRecord* copy = new (std::nothrow) CpuTextureRecord;
                if (copy == nullptr)
                {
                    return RasterStatus::OutOfMemory;
                }
                record.CpuTexture.Reset(copy);
                if (copy->Bytes.TryResize(upload.Bytes.size()) == false)
                {
                    return StorageFailure();
                }
                std::memcpy(copy->Bytes.GetData(), upload.Bytes.data(), upload.Bytes.size());
                for (usize level = 0; level < description.MipLevels; ++level)
                {
                    copy->Mips[level] = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
                }
            }
            record.ColorDefined = true;
            record.InitializedMips = (uint32{1} << description.MipLevels) - 1;
            record.TextureUse = RasterTextureUse::SampledBoth;
            return backend::RasterCreateTexture(slot, description, upload, record.Request);
        },
        storageBytes);
}
bool IsNull(TextureHandle handle) noexcept
{
    return RasterAccess::Owner(handle) == 0;
}
RasterStatus GetTextureDescription(DeviceHandle device, TextureHandle handle, TextureDescription& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* record = Resolve(handle, RasterKind::Texture);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Status != RasterStatus::Ready)
    {
        return record->Status == RasterStatus::Pending ? RasterStatus::NotReady : record->Status;
    }
    output = record->Texture;
    return RasterStatus::Ready;
}
RasterStatus CreateTextureView(DeviceHandle device, TextureHandle texture, TextureViewHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* source = Resolve(texture, RasterKind::Texture);
    if (source == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    TextureViewDescription view;
    view.Dimension = source->Texture.Dimension;
    return CreateTextureView(device, texture, view, output);
}
RasterStatus CreateTextureView(DeviceHandle device,
                               TextureHandle texture,
                               const TextureViewDescription& description,
                               TextureViewHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* source = Resolve(texture, RasterKind::Texture);
    if (source == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (source->Status != RasterStatus::Ready)
    {
        return source->Status == RasterStatus::Pending ? RasterStatus::NotReady : source->Status;
    }
    if (static_cast<uint8>(description.Aspect) > static_cast<uint8>(TextureAspect::Stencil))
    {
        return RasterStatus::InvalidDescription;
    }
    if ((description.Attachment &&
         (source->Texture.Attachment == false || description.Aspect != TextureAspect::Color)) ||
        (description.Attachment == false && source->Texture.SampleCount != 1))
    {
        return RasterStatus::InvalidDescription;
    }
    if (description.Aspect != TextureAspect::Color &&
        (source->Texture.Attachment == false ||
         (description.Aspect == TextureAspect::Stencil && source->Texture.Stencil == false)))
    {
        return RasterStatus::InvalidDescription;
    }
    const auto& allocation = source->Texture;
    auto view = description;
    const uint32 layers = allocation.Dimension == TextureDimension::D3 ? 1U : allocation.DepthOrLayers;
    if (view.BaseMip >= allocation.MipLevels || view.BaseLayer >= layers ||
        static_cast<uint8>(view.Dimension) > static_cast<uint8>(TextureDimension::D3))
    {
        return RasterStatus::InvalidDescription;
    }
    if (view.MipCount == 0)
    {
        view.MipCount = allocation.MipLevels - view.BaseMip;
    }
    if (view.LayerCount == 0)
    {
        view.LayerCount = layers - view.BaseLayer;
    }
    const bool compatibleDimension = view.Dimension == allocation.Dimension ||
                                     (view.Dimension == TextureDimension::D2 &&
                                      allocation.Dimension != TextureDimension::D3 && view.LayerCount == 1);
    if (view.MipCount > allocation.MipLevels - view.BaseMip || view.LayerCount > layers - view.BaseLayer ||
        compatibleDimension == false ||
        (view.Dimension == TextureDimension::Cube && (view.BaseLayer != 0 || view.LayerCount != 6)))
    {
        return RasterStatus::InvalidDescription;
    }
    const bool partial = view.BaseMip != 0 || view.MipCount != allocation.MipLevels || view.BaseLayer != 0 ||
                         view.LayerCount != layers || view.Dimension != allocation.Dimension;
    const auto caps = internal::CurrentDeviceProfile().Formats[static_cast<usize>(allocation.Format)];
    if ((view.Attachment && (caps.AttachmentViews == false || view.Dimension != TextureDimension::D2 ||
                             view.MipCount != 1 || view.LayerCount != 1)) ||
        (view.Aspect == TextureAspect::Depth &&
         (caps.DepthSampled == false || view.Dimension != TextureDimension::D2 || view.LayerCount != 1)) ||
        (view.Aspect == TextureAspect::Stencil &&
         (caps.StencilSampled == false || view.Dimension != TextureDimension::D2 || view.LayerCount != 1)) ||
        (partial && view.Attachment == false && caps.PartialViews == false))
    {
        return RasterStatus::Unsupported;
    }
    const uint32 initialized = ((uint32{1} << view.MipCount) - 1) << view.BaseMip;
    if (allocation.Attachment == false && (source->InitializedMips & initialized) != initialized)
    {
        return RasterStatus::InvalidState;
    }
    // GLES has no native texture views: its partial views copy a sampled snapshot.
    // Mutation exclusion retains the source; logical copy storage is budgeted here.
    uint64 bytes = 0;
    if (backend::Kind() == Backend::WebGL2 && partial && view.Attachment == false)
    {
        for (uint32 mip = view.BaseMip; mip < view.BaseMip + view.MipCount; ++mip)
        {
            const uint64 pixels =
                uint64{TextureMipExtent(allocation.Width, mip)} * TextureMipExtent(allocation.Height, mip);
            const uint64 slices =
                allocation.Dimension == TextureDimension::D3 ? TextureSlices(allocation, mip) : view.LayerCount;
            bytes += pixels * slices * TextureBlockBytes(allocation.Format);
        }
    }
    return Publish(
        RasterKind::View,
        output,
        [&](usize slot, Record& record) noexcept {
            record.TextureSlot = RasterAccess::Slot(texture);
            record.View = view;
            return backend::RasterCreateView(slot, {record.TextureSlot, view}, record.Request);
        },
        bytes);
}
RasterStatus CreateSampler(DeviceHandle device, const SamplerDescription& description, SamplerHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    // Positive range predicates reject NaN as well as infinities without a math dependency.
    const bool validLod = description.MinLod >= 0 && description.MinLod <= 15 &&
                          description.MaxLod >= description.MinLod && description.MaxLod <= 15;
    if (static_cast<uint8>(description.Filter) > 1 || static_cast<uint8>(description.U) > 1 ||
        static_cast<uint8>(description.V) > 1 || static_cast<uint8>(description.W) > 1 ||
        static_cast<uint8>(description.MipFilter) > 2 || validLod == false ||
        (description.MipFilter == RasterMipFilter::None && description.MinLod != 0))
    {
        return RasterStatus::InvalidDescription;
    }
    return Publish(RasterKind::Sampler, output, [&](usize slot, Record& record) noexcept {
        return backend::RasterCreateSampler(slot, description, record.Request);
    });
}
RasterStatus
CreateRasterShader(DeviceHandle device, const RasterShaderDescription& description, RasterShaderHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    RasterShaderInfo info;
    info.ShouldPrepareAsynchronously = description.ShouldPrepareAsynchronously;
    info.Stage = description.Artifact.Stage;
    if ((info.Stage != ShaderStage::Vertex && info.Stage != ShaderStage::Fragment &&
         info.Stage != ShaderStage::Compute) ||
        CopyLayout(description.Bindings, info.Layout) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize binding = 0; binding < info.Layout.Count; ++binding)
    {
        if (description.HasVerifiedArrayBounds == false &&
            (info.Layout.Entries[binding].Count > 1 || info.Layout.Entries[binding].IsRuntimeSized == true))
        {
            return RasterStatus::InvalidDescription;
        }
    }
    const RasterStatus bindingStatus = ValidateLayoutCapabilities(info.Layout);
    if (bindingStatus != RasterStatus::Ready)
    {
        return bindingStatus;
    }
    if (info.Stage == ShaderStage::Compute)
    {
        const auto compute = ComputeAdmission(device);
        if (compute != RasterStatus::Ready)
        {
            return compute;
        }
        const auto limits = backend::ComputeLimits();
        uint32 product = 1;
        for (usize i = 0; i < 3; ++i)
        {
            const auto size = description.WorkgroupSize[i];
            if (size == 0 || size > limits.MaxWorkgroupSize[i] || size > limits.MaxWorkgroupInvocations / product)
            {
                return RasterStatus::InvalidDescription;
            }
            product *= size;
            info.WorkgroupSize[i] = size;
        }
    }
    for (const auto& binding : description.Bindings)
    {
        if (static_cast<uint8>(binding.Visibility) != (info.Stage == ShaderStage::Compute  ? 4
                                                       : info.Stage == ShaderStage::Vertex ? 1
                                                                                           : 2) &&
            (info.Stage == ShaderStage::Compute || binding.Visibility != RasterVisibility::Both))
        {
            return RasterStatus::InvalidDescription;
        }
    }
    if (description.UniformMembers.size() > 64)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < description.UniformMembers.size(); ++i)
    {
        const auto& member = description.UniformMembers[i];
        char name[64]{};
        bool valid = false;
        for (const auto& binding : description.Bindings)
        {
            valid |= binding.Binding == member.Binding && binding.Kind == RasterBindingKind::UniformBuffer &&
                     member.Offset < binding.MinSize;
        }
        if (valid == false || member.Offset % 4 != 0 || member.Name.empty() || CopyName(member.Name, name) == false ||
            static_cast<uint8>(member.Shape) > static_cast<uint8>(RasterUniformShape::Float4x4Array))
        {
            return RasterStatus::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            const auto& previous = description.UniformMembers[j];
            if (previous.Binding == member.Binding &&
                (previous.Name == member.Name || previous.Offset == member.Offset))
            {
                return RasterStatus::InvalidDescription;
            }
        }
    }
    for (usize i = 0; i < RASTER_BINDINGS; ++i)
    {
        bool loadOnly = false;
        for (usize b = 0; b < info.Layout.Count; ++b)
        {
            loadOnly |= info.Layout.Entries[b].Binding == i &&
                        (info.Layout.Entries[b].Kind == RasterBindingKind::TextureDepth2D ||
                         info.Layout.Entries[b].Kind == RasterBindingKind::TextureStencil2D);
        }
        if (CopyName(description.UniformBlocks[i], info.UniformBlocks[i]) == false ||
            CopyName(description.TextureNames[i], info.TextureNames[i]) == false ||
            (description.TextureSamplers[i] >= RASTER_BINDINGS &&
             (loadOnly == false || description.TextureSamplers[i] != ~uint32{0})))
        {
            return RasterStatus::InvalidDescription;
        }
        info.TextureSamplers[i] = description.TextureSamplers[i];
    }
    if (description.Inputs.size() > backend::RasterLimits().MaxVertexAttributes ||
        (info.Stage != ShaderStage::Vertex && description.Inputs.empty() == false))
    {
        return RasterStatus::InvalidDescription;
    }
    info.InputCount = description.Inputs.size();
    for (usize i = 0; i < info.InputCount; ++i)
    {
        const auto& input = description.Inputs[i];
        if (input.Location >= backend::RasterLimits().MaxVertexAttributes || static_cast<uint8>(input.Format) > 2)
        {
            return RasterStatus::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (info.Inputs[j].Location == input.Location)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        info.Inputs[i] = input;
    }
    const auto& artifact = description.Artifact;
    const auto backendKind = GetStartup().SelectedBackend;
    if (backendKind == Backend::WebGL2)
    {
        for (usize i = 0; i < info.Layout.Count; ++i)
        {
            const auto& binding = info.Layout.Entries[i];
            if (TextureBinding(binding.Kind) == false)
            {
                continue;
            }
            bool samplerFound = false;
            for (usize j = 0; j < info.Layout.Count; ++j)
            {
                const auto& candidate = info.Layout.Entries[j];
                samplerFound |= candidate.Kind == RasterBindingKind::Sampler &&
                                candidate.Binding == info.TextureSamplers[binding.Binding];
            }
            if (samplerFound == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
    }
    const auto entry = backendKind == Backend::Metal    ? artifact.MslEntry
                       : backendKind == Backend::WebGPU ? artifact.WgslEntry
                       : backendKind == Backend::WebGL2 ? artifact.GlslEsEntry
                                                        : artifact.SpirvEntry;
    char name[64]{};
    if (entry.empty() || CopyName(entry, name) == false ||
        (backendKind == Backend::Metal    ? artifact.Msl.empty()
         : backendKind == Backend::WebGPU ? artifact.Wgsl.empty()
         : backendKind == Backend::WebGL2 ? artifact.GlslEs.empty()
                                          : artifact.Spirv.size() < 5 || artifact.Spirv[0] != 0x07230203U))
    {
        return RasterStatus::InvalidDescription;
    }
    return Publish(RasterKind::Shader, output, [&](usize slot, Record& record) noexcept {
        record.Shader = info;
        return backend::RasterCreateShader(slot, artifact, info, record.Request);
    });
}
RasterStatus
CreateBindingLayout(DeviceHandle device, std::span<const RasterBinding> entries, BindingLayoutHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    RasterLayout layout;
    if (CopyLayout(entries, layout) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    const RasterStatus bindingStatus = ValidateLayoutCapabilities(layout);
    if (bindingStatus != RasterStatus::Ready)
    {
        return bindingStatus;
    }
    return Publish(RasterKind::Layout, output, [&](usize slot, Record& record) noexcept {
        record.Layout = layout;
        return backend::RasterCreateLayout(slot, layout, record.Request);
    });
}
namespace
{
uint64 CountBindingDescriptors() noexcept
{
    uint64 count = 0;
    for (const Record& record : gRecords[static_cast<usize>(RasterKind::Set)])
    {
        if (record.Owner != 0)
        {
            count += record.Set.Description.DescriptorCount;
        }
    }
    return count;
}
} // namespace
namespace
{
RasterStatus CreateBindingSnapshot(DeviceHandle device,
                                   BindingLayoutHandle handle,
                                   std::span<const RasterBindingResource> entries,
                                   uint32 runtimeCount,
                                   BindingSetHandle& outSet) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const Record* layoutRecordOrNull = Resolve(handle, RasterKind::Layout);
    if (layoutRecordOrNull == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (layoutRecordOrNull->Status != RasterStatus::Ready)
    {
        return layoutRecordOrNull->Status == RasterStatus::Pending ? RasterStatus::NotReady
                                                                   : layoutRecordOrNull->Status;
    }
    RasterLayout layout = layoutRecordOrNull->Layout;
    bool hasRuntimeBinding = false;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        RasterBinding& binding = layout.Entries[entry];
        if (binding.IsRuntimeSized == true)
        {
            hasRuntimeBinding = true;
            if (runtimeCount != 0)
            {
                if (runtimeCount > binding.Count)
                {
                    return RasterStatus::InvalidDescription;
                }
                layout.DescriptorCount -= binding.Count - runtimeCount;
                binding.Count = runtimeCount;
            }
        }
    }
    if ((runtimeCount != 0 && hasRuntimeBinding == false) || entries.size() > layout.DescriptorCount)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize entry = 0; entry < entries.size(); ++entry)
    {
        if (hasRuntimeBinding == true && entries[entry].Binding == RUNTIME_BINDING_LENGTH_SLOT)
        {
            return RasterStatus::InvalidDescription;
        }
        bool hasLocation = false;
        for (usize binding = 0; binding < layout.Count; ++binding)
        {
            hasLocation = hasLocation == true || (entries[entry].Binding == layout.Entries[binding].Binding &&
                                                  entries[entry].ArrayElement < layout.Entries[binding].Count);
        }
        if (hasLocation == false)
        {
            return RasterStatus::InvalidDescription;
        }
        for (usize previous = 0; previous < entry; ++previous)
        {
            if (entries[entry].Binding == entries[previous].Binding &&
                entries[entry].ArrayElement == entries[previous].ArrayElement)
            {
                return RasterStatus::InvalidDescription;
            }
        }
    }
    Collect();
    const uint64 descriptorCount = CountBindingDescriptors();
    const uint64 descriptorLimit = CurrentStorageLimits().BindingDescriptors;
    if (descriptorCount > descriptorLimit || layout.DescriptorCount > descriptorLimit - descriptorCount)
    {
        return RasterStatus::CapacityExceeded;
    }
    RasterSet set;
    if (set.TryResize(layout.DescriptorCount) == false)
    {
        return StorageFailure();
    }
    set.Layout = RasterAccess::Slot(handle);
    set.Description = layout;
    const RasterCapabilities limits = backend::RasterLimits();
    usize lengthDescriptor = RASTER_INVALID;
    uint32 allocatedRuntimeCount = 0;
    for (usize binding = 0; binding < layout.Count; ++binding)
    {
        if (layout.Entries[binding].IsRuntimeSized == true)
        {
            allocatedRuntimeCount = layout.Entries[binding].Count;
        }
    }
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        if (hasRuntimeBinding == true && layout.GetDescriptorBinding(i).Binding == RUNTIME_BINDING_LENGTH_SLOT)
        {
            lengthDescriptor = i;
            set.IsPopulated[i] = true;
            set.Offsets[i] = 0;
            set.Sizes[i] = RUNTIME_BINDING_LENGTH_BYTES;
            continue;
        }
        const RasterBindingResource* entryOrNull = nullptr;
        for (const auto& candidate : entries)
        {
            if (candidate.Binding != layout.GetDescriptorBinding(i).Binding ||
                candidate.ArrayElement != layout.GetArrayElement(i))
            {
                continue;
            }
            if (entryOrNull != nullptr)
            {
                return RasterStatus::InvalidDescription;
            }
            entryOrNull = &candidate;
        }
        set.IsPopulated[i] = entryOrNull != nullptr;
        if (entryOrNull == nullptr && layout.GetDescriptorBinding(i).HasPartialBinding == true)
        {
            for (const RasterBindingResource& candidate : entries)
            {
                if (candidate.Binding == layout.GetDescriptorBinding(i).Binding && candidate.ArrayElement == 0)
                {
                    entryOrNull = &candidate;
                }
            }
        }
        if (entryOrNull == nullptr)
        {
            return RasterStatus::InvalidDescription;
        }
        const RasterBindingKind kind = layout.GetDescriptorBinding(i).Kind;
        const Record* resourceOrNull = BufferBinding(kind)    ? Resolve(entryOrNull->Buffer, RasterKind::Buffer)
                                       : TextureBinding(kind) ? Resolve(entryOrNull->Texture, RasterKind::View)
                                                              : Resolve(entryOrNull->Sampler, RasterKind::Sampler);
        if (resourceOrNull == nullptr)
        {
            return RasterStatus::InvalidHandle;
        }
        if (resourceOrNull->Status != RasterStatus::Ready)
        {
            return resourceOrNull->Status == RasterStatus::Pending ? RasterStatus::NotReady : resourceOrNull->Status;
        }
        const RasterBindingKind textureType = resourceOrNull->View.Aspect == TextureAspect::Depth
                                                  ? RasterBindingKind::TextureDepth2D
                                              : resourceOrNull->View.Aspect == TextureAspect::Stencil
                                                  ? RasterBindingKind::TextureStencil2D
                                                  : TextureBindingType(resourceOrNull->View.Dimension);
        if (TextureBinding(kind) == true && (resourceOrNull->View.Attachment == true || textureType != kind))
        {
            return RasterStatus::InvalidDescription;
        }
        if (kind == RasterBindingKind::UniformBuffer &&
            (resourceOrNull->Buffer.Role != BufferRole::Uniform || limits.UniformOffsetAlignment == 0 ||
             entryOrNull->Offset % limits.UniformOffsetAlignment != 0 || entryOrNull->Size % 16 != 0 ||
             entryOrNull->Size < layout.GetDescriptorBinding(i).MinSize || entryOrNull->Size > limits.MaxUniformRange ||
             Fits(entryOrNull->Offset, entryOrNull->Size, resourceOrNull->Buffer.Size) == false))
        {
            return RasterStatus::InvalidDescription;
        }
        if (kind == RasterBindingKind::StorageRead || kind == RasterBindingKind::StorageReadWrite)
        {
            const auto compute = backend::ComputeLimits();
            if (StorageRole(resourceOrNull->Buffer.Role) == false || compute.StorageOffsetAlignment == 0 ||
                entryOrNull->Offset % compute.StorageOffsetAlignment != 0 || entryOrNull->Size % 4 != 0 ||
                entryOrNull->Size < layout.GetDescriptorBinding(i).MinSize ||
                entryOrNull->Size > compute.MaxStorageRange ||
                Fits(entryOrNull->Offset, entryOrNull->Size, resourceOrNull->Buffer.Size) == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        set.Slots[i] = BufferBinding(kind)    ? RasterAccess::Slot(entryOrNull->Buffer)
                       : TextureBinding(kind) ? RasterAccess::Slot(entryOrNull->Texture)
                                              : RasterAccess::Slot(entryOrNull->Sampler);
        set.Offsets[i] = entryOrNull->Offset;
        set.Sizes[i] = entryOrNull->Size;
    }
    BufferHandle lengthBuffer;
    if (lengthDescriptor != RASTER_INVALID)
    {
        const StaticArray<uint32, RUNTIME_BINDING_LENGTH_WORDS> length{allocatedRuntimeCount, 0, 0, 0};
        const RasterStatus lengthStatus =
            CreateBuffer(device,
                         { .Role = BufferRole::Uniform, .Size = RUNTIME_BINDING_LENGTH_BYTES },
                         {reinterpret_cast<const uint8*>(length.GetData()), RUNTIME_BINDING_LENGTH_BYTES},
                         lengthBuffer);
        if (lengthStatus != RasterStatus::Ready)
        {
            if (RasterAccess::Owner(lengthBuffer) != 0)
            {
                (void)Destroy(device, lengthBuffer);
            }
            return lengthStatus == RasterStatus::Pending ? RasterStatus::NotReady : lengthStatus;
        }
        set.Slots[lengthDescriptor] = RasterAccess::Slot(lengthBuffer);
    }
    const RasterStatus result = Publish(RasterKind::Set, outSet, [&](usize slot, Record& inoutRecord) noexcept {
        inoutRecord.Set = Move(set);
        return backend::RasterCreateSet(slot, layout, inoutRecord.Set, inoutRecord.Request);
    });
    if (RasterAccess::Owner(lengthBuffer) != 0)
    {
        // The accepted snapshot owns the buffer through Dependencies; on failure
        // this releases the temporary buffer. No public metadata handle escapes.
        (void)Destroy(device, lengthBuffer);
    }
    return result;
}
} // namespace
RasterStatus CreateBindingSet(DeviceHandle device,
                              BindingLayoutHandle layout,
                              std::span<const RasterBindingResource> entries,
                              BindingSetHandle& outSet) noexcept
{
    return CreateBindingSnapshot(device, layout, entries, 0, outSet);
}
RasterStatus CreateRuntimeBindingSet(DeviceHandle device,
                                     BindingLayoutHandle layout,
                                     BorrowedView<const RasterBindingResource> entries,
                                     uint32 runtimeCount,
                                     BindingSetHandle& outSet) noexcept
{
    if (runtimeCount == 0 || entries.IsValid() == false)
    {
        return RasterStatus::InvalidDescription;
    }
    return CreateBindingSnapshot(device, layout, {entries.GetDataOrNull(), entries.GetSize()}, runtimeCount, outSet);
}
RasterStatus PublishBindingSetUpdate(DeviceHandle device,
                                     BindingSetHandle& inoutCurrent,
                                     BindingSetHandle& inoutReplacement) noexcept
{
    const RasterStatus admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const Record* currentOrNull = Resolve(inoutCurrent, RasterKind::Set);
    const Record* replacementOrNull = Resolve(inoutReplacement, RasterKind::Set);
    if (currentOrNull == nullptr || replacementOrNull == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (currentOrNull == replacementOrNull || currentOrNull->Set.Layout != replacementOrNull->Set.Layout)
    {
        return RasterStatus::InvalidDescription;
    }
    constexpr usize SNAPSHOT_COUNT = 2;
    const StaticArray<const Record*, SNAPSHOT_COUNT> candidates{currentOrNull, replacementOrNull};
    for (const Record* candidate : candidates)
    {
        if (candidate->Status != RasterStatus::Ready)
        {
            return candidate->Status == RasterStatus::Pending ? RasterStatus::NotReady : candidate->Status;
        }
    }
    // Publish a new immutable identity without mutating an already encoded set.
    // In-frame publication cannot allocate or collect. Concrete packet leases
    // retain old metadata and native descriptors until submission completion.
    Record& previous = At(RasterKind::Set, RasterAccess::Slot(inoutCurrent));
    previous.Published = false;
    --previous.References;
    inoutCurrent = inoutReplacement;
    inoutReplacement = {};
    return RasterStatus::Ready;
}
RasterStatus GetBindingIndex(DeviceHandle device,
                             BindingSetHandle handle,
                             uint32 binding,
                             uint32 element,
                             RasterBindingIndex& outIndex) noexcept
{
    const RasterStatus admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const Record* set = Resolve(handle, RasterKind::Set);
    if (set == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (set->Status != RasterStatus::Ready)
    {
        return set->Status == RasterStatus::Pending ? RasterStatus::NotReady : set->Status;
    }
    const RasterLayout& layout = set->Set.Description;
    usize descriptor = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& description = layout.Entries[entry];
        if (description.Binding == binding && (description.Count > 1 || description.IsRuntimeSized == true) &&
            element < description.Count && set->Set.IsPopulated[descriptor + element] == true)
        {
            RasterAccess::SetIndex(handle, { .Binding = binding, .Element = element }, outIndex);
            return RasterStatus::Ready;
        }
        descriptor += description.Count;
    }
    return RasterStatus::InvalidDescription;
}
RasterStatus GetBindingArrayElement(DeviceHandle device,
                                    BindingSetHandle handle,
                                    const RasterBindingIndex& index,
                                    uint32& outElement) noexcept
{
    const RasterStatus admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const BindingSetHandle& source = RasterAccess::GetIndexSet(index);
    if (RasterAccess::Owner(source) != RasterAccess::Owner(handle) ||
        RasterAccess::Slot(source) != RasterAccess::Slot(handle) ||
        RasterAccess::Generation(source) != RasterAccess::Generation(handle))
    {
        return RasterStatus::InvalidHandle;
    }
    RasterBindingIndex validated;
    const RasterStatus result = GetBindingIndex(device,
                                                handle,
                                                RasterAccess::GetIndexBinding(index),
                                                RasterAccess::GetIndexElement(index),
                                                validated);
    if (result == RasterStatus::Ready)
    {
        outElement = RasterAccess::GetIndexElement(index);
    }
    return result;
}
namespace
{
RasterStatus ValidateReachableBindings(DeviceHandle device, const RasterDraw& draw, const RasterSet& set) noexcept
{
    const RasterLayout& layout = set.Description;
    usize required = 0;
    usize descriptor = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& binding = layout.Entries[entry];
        const usize firstDescriptor = descriptor;
        descriptor += binding.Count;
        if (binding.Indexing != RasterBindingIndexing::NonUniform)
        {
            continue;
        }
        for (uint32 element = 0; element < binding.Count; ++element)
        {
            if (set.IsPopulated[firstDescriptor + element] == false)
            {
                continue;
            }
            ++required;
            usize matches = 0;
            for (const RasterBindingIndex& index : draw.ReachableBindings)
            {
                uint32 checkedElement = 0;
                const RasterStatus status = GetBindingArrayElement(device, draw.Bindings, index, checkedElement);
                if (status != RasterStatus::Ready)
                {
                    return status;
                }
                if (RasterAccess::GetIndexBinding(index) == binding.Binding && checkedElement == element)
                {
                    ++matches;
                }
            }
            if (matches != 1)
            {
                return RasterStatus::InvalidDescription;
            }
        }
    }
    return draw.ReachableBindings.size() == required ? RasterStatus::Ready : RasterStatus::InvalidDescription;
}
RasterStatus
ValidatePipeline(DeviceHandle device, const RasterPipelineDescription& description, RasterPipelineInfo& info) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* vertex = Resolve(description.Vertex, RasterKind::Shader);
    const auto* fragment = Resolve(description.Fragment, RasterKind::Shader);
    const auto* layout = Resolve(description.Layout, RasterKind::Layout);
    if (vertex == nullptr || fragment == nullptr || layout == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const Record* dependencies[]{vertex, fragment, layout};
    for (const auto* dependency : dependencies)
    {
        if (dependency->Status != RasterStatus::Ready)
        {
            return dependency->Status == RasterStatus::Pending ? RasterStatus::NotReady : dependency->Status;
        }
    }
    if (vertex->Shader.Stage != ShaderStage::Vertex || fragment->Shader.Stage != ShaderStage::Fragment ||
        ShaderFits(vertex->Shader, layout->Layout) == false || ShaderFits(fragment->Shader, layout->Layout) == false ||
        description.Streams.size() > 2 || description.Attributes.size() > backend::RasterLimits().MaxVertexAttributes ||
        description.Attributes.size() != vertex->Shader.InputCount)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < layout->Layout.Count; ++i)
    {
        if (layout->Layout.Entries[i].Visibility == RasterVisibility::Compute)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    info.ShouldPrepareAsynchronously = description.ShouldPrepareAsynchronously == true ||
                                       vertex->Shader.ShouldPrepareAsynchronously == true ||
                                       fragment->Shader.ShouldPrepareAsynchronously == true;
    info.Vertex = RasterAccess::Slot(description.Vertex);
    info.Fragment = RasterAccess::Slot(description.Fragment);
    info.Layout = RasterAccess::Slot(description.Layout);
    info.Depth = description.Depth;
    if (static_cast<uint8>(description.Target) > static_cast<uint8>(RasterTarget::Rgba16Float))
    {
        return RasterStatus::InvalidDescription;
    }
    if (description.Target != RasterTarget::Surface)
    {
        const auto format = description.Target == RasterTarget::Rgba16Float  ? RasterFormat::Rgba16Float
                            : description.Target == RasterTarget::Rgba8Unorm ? RasterFormat::Rgba8Unorm
                                                                             : RasterFormat::Rgba8Srgb;
        const auto capabilities = internal::CurrentDeviceProfile().Formats[static_cast<usize>(format)];
        if (capabilities.Uses.ColorAttachment == false || (description.Blend && capabilities.Uses.Blendable == false))
        {
            return RasterStatus::Unsupported;
        }
    }
    info.Blend = description.Blend;
    info.Target = description.Target;
    if (static_cast<uint8>(description.DepthCompare) > static_cast<uint8>(RasterDepthCompare::Always))
    {
        return RasterStatus::InvalidDescription;
    }
    info.DepthWrite = description.DepthWrite;
    info.DepthCompare = description.DepthCompare;
    if (description.ColorFormats.size() > 4 ||
        description.ColorFormats.size() > internal::CurrentDeviceProfile().Interface.MaxColorAttachments ||
        (description.SampleCount != 1 && description.SampleCount != 4) ||
        (description.SampleCount != 1 && description.ColorFormats.empty()) ||
        (description.Stencil && description.ColorFormats.empty()) || description.StencilReadMask > 255 ||
        description.StencilWriteMask > 255 || description.StencilReference > 255)
    {
        return RasterStatus::InvalidDescription;
    }
    info.ColorCount = description.ColorFormats.size();
    info.SampleCount = description.SampleCount;
    info.Stencil = description.Stencil;
    info.StencilFront = description.StencilFront;
    info.StencilBack = description.StencilBack;
    info.StencilReadMask = description.StencilReadMask;
    info.StencilWriteMask = description.StencilWriteMask;
    info.StencilReference = description.StencilReference;
    const RasterStencilFace faces[]{description.StencilFront, description.StencilBack};
    for (const auto& face : faces)
    {
        if (static_cast<uint8>(face.Compare) > 7 || static_cast<uint8>(face.Fail) > 7 ||
            static_cast<uint8>(face.DepthFail) > 7 || static_cast<uint8>(face.Pass) > 7)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        const auto format = description.ColorFormats[i];
        if (static_cast<uint8>(format) > static_cast<uint8>(RasterFormat::Bc5RgUnorm))
        {
            return RasterStatus::InvalidDescription;
        }
        const auto caps = internal::CurrentDeviceProfile().Formats[static_cast<usize>(format)];
        if (caps.Uses.ColorAttachment == false || (caps.SampleCounts & info.SampleCount) == 0 ||
            (info.Blend && caps.Uses.Blendable == false) || (info.Stencil && caps.StencilAttachment == false))
        {
            return RasterStatus::Unsupported;
        }
        info.ColorFormats[i] = format;
    }
    info.StreamCount = description.Streams.size();
    info.AttributeCount = description.Attributes.size();
    for (usize i = 0; i < info.StreamCount; ++i)
    {
        const auto& stream = description.Streams[i];
        if (stream.Stride == 0 || stream.Stride > internal::CurrentDeviceProfile().Interface.MaxVertexStride ||
            stream.Stride % 4 != 0)
        {
            return RasterStatus::InvalidDescription;
        }
        info.Streams[i] = stream;
    }
    for (usize i = 0; i < info.AttributeCount; ++i)
    {
        const auto& attribute = description.Attributes[i];
        const auto format = static_cast<uint8>(attribute.Format);
        const usize size = (static_cast<usize>(format) + 2) * 4;
        if (format > 2 || attribute.Location >= backend::RasterLimits().MaxVertexAttributes ||
            attribute.Stream >= info.StreamCount || attribute.Offset % 4 != 0 ||
            Fits(attribute.Offset, size, info.Streams[attribute.Stream].Stride) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (attribute.Location == info.Attributes[j].Location)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        bool matched = false;
        for (usize j = 0; j < vertex->Shader.InputCount; ++j)
        {
            if (vertex->Shader.Inputs[j].Location == attribute.Location &&
                vertex->Shader.Inputs[j].Format == attribute.Format)
            {
                matched = true;
            }
        }
        if (matched == false)
        {
            return RasterStatus::InvalidDescription;
        }
        info.Attributes[i] = attribute;
    }
    return RasterStatus::Ready;
}
} // namespace
RasterStatus CreateRasterPipeline(DeviceHandle device,
                                  const RasterPipelineDescription& description,
                                  RasterPipelineHandle& output) noexcept
{
    RasterPipelineInfo info;
    const auto valid = ValidatePipeline(device, description, info);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    return Publish(RasterKind::Pipeline, output, [&](usize slot, Record& record) noexcept {
        record.Pipeline = info;
        return backend::RasterCreatePipeline(slot, info, record.Request);
    });
}
namespace
{
RasterStatus ValidateDraw(DeviceHandle device, const RasterDraw& draw, RasterPacket& packet) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* pipeline = Resolve(draw.Pipeline, RasterKind::Pipeline);
    const auto* set = Resolve(draw.Bindings, RasterKind::Set);
    const auto* indices = Resolve(draw.Indices, RasterKind::Buffer);
    if (pipeline == nullptr || set == nullptr || indices == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const Record* dependencies[]{pipeline, set, indices};
    for (const auto* dependency : dependencies)
    {
        if (dependency->Status != RasterStatus::Ready)
        {
            return dependency->Status == RasterStatus::Pending ? RasterStatus::NotReady : dependency->Status;
        }
    }
    const RasterStatus offsetsStatus = ValidateBindingOffsets(set->Set, draw.BindingOffsets);
    if (offsetsStatus != RasterStatus::Ready)
    {
        return offsetsStatus;
    }
    const RasterStatus reachableStatus = ValidateReachableBindings(device, draw, set->Set);
    if (reachableStatus != RasterStatus::Ready)
    {
        return reachableStatus;
    }
    packet.BindingOffsets = draw.BindingOffsets;
    packet.DynamicOffsetCount = CollectDynamicOffsets(At(RasterKind::Layout, set->Set.Layout).Layout,
                                                      draw.BindingOffsets,
                                                      packet.DynamicOffsets);
    const auto& info = pipeline->Pipeline;
    if (set->Set.Layout != info.Layout ||
        ShaderFits(At(RasterKind::Shader, info.Vertex).Shader, set->Set.Description) == false ||
        ShaderFits(At(RasterKind::Shader, info.Fragment).Shader, set->Set.Description) == false ||
        draw.Vertices.size() != info.StreamCount || draw.IndexCount == 0 || draw.IndexCount % 3 != 0 ||
        draw.VertexCount == 0 || draw.InstanceCount == 0 || draw.InstanceCount > 0x7fffffffU ||
        (indices->Buffer.Role != BufferRole::Index16 && indices->Buffer.Role != BufferRole::Index32))
    {
        return RasterStatus::InvalidDescription;
    }
    const usize width = indices->Buffer.Role == BufferRole::Index16 ? 2 : 4;
    if (draw.IndexOffset % width != 0 || draw.IndexCount > ~usize{0} / width ||
        Fits(draw.IndexOffset, static_cast<usize>(draw.IndexCount) * width, indices->Buffer.Size) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < draw.IndexCount; ++i)
    {
        // Byte copies avoid alignment assumptions; index upload uses native-endian
        // unsigned integers, as do the selected native/browser graphics APIs.
        uint32 index = 0;
        if (width == 2)
        {
            uint16 value = 0;
            std::memcpy(&value, indices->Indices.GetData() + draw.IndexOffset + i * width, width);
            index = value;
        }
        else
        {
            std::memcpy(&index, indices->Indices.GetData() + draw.IndexOffset + i * width, width);
        }
        if (index >= draw.VertexCount || index == (width == 2 ? 0xffffU : 0xffffffffU))
        {
            return RasterStatus::InvalidDescription;
        }
    }
    if (RasterAccess::Owner(draw.Indirect) != 0)
    {
        const auto compute = ComputeAdmission(device, false);
        if (compute != RasterStatus::Ready)
        {
            return compute;
        }
        const auto* indirect = Resolve(draw.Indirect, RasterKind::Buffer);
        if (indirect == nullptr)
        {
            return RasterStatus::InvalidHandle;
        }
        if (indirect->Status != RasterStatus::Ready)
        {
            return indirect->Status == RasterStatus::Pending ? RasterStatus::NotReady : indirect->Status;
        }
        if (indirect->Buffer.Role != BufferRole::StorageIndirect || draw.IndirectOffset % 4 != 0 ||
            Fits(draw.IndirectOffset, sizeof(IndexedIndirectArguments), indirect->Buffer.Size) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        packet.Indirect = RasterAccess::Slot(draw.Indirect);
        packet.IndirectOffset = draw.IndirectOffset;
    }
    else if (draw.IndirectOffset != 0)
    {
        return RasterStatus::InvalidDescription;
    }
    packet.Pipeline = RasterAccess::Slot(draw.Pipeline);
    packet.Set = RasterAccess::Slot(draw.Bindings);
    packet.Indices = RasterAccess::Slot(draw.Indices);
    packet.IndexOffset = draw.IndexOffset;
    packet.IndexCount = draw.IndexCount;
    packet.InstanceCount = draw.InstanceCount;
    packet.Index32 = width == 4;
    for (usize i = 0; i < info.StreamCount; ++i)
    {
        const auto& slice = draw.Vertices[i];
        const auto* buffer = Resolve(slice.Buffer, RasterKind::Buffer);
        if (buffer == nullptr)
        {
            return RasterStatus::InvalidHandle;
        }
        if (buffer->Status != RasterStatus::Ready)
        {
            return buffer->Status == RasterStatus::Pending ? RasterStatus::NotReady : buffer->Status;
        }
        const usize count = info.Streams[i].PerInstance ? draw.InstanceCount : draw.VertexCount;
        if ((buffer->Buffer.Role != BufferRole::Vertex && buffer->Buffer.Role != BufferRole::StorageVertex) ||
            slice.Offset % 4 != 0 || Fits(slice.Offset, slice.Size, buffer->Buffer.Size) == false ||
            count > slice.Size / info.Streams[i].Stride)
        {
            return RasterStatus::InvalidDescription;
        }
        packet.Vertices[i] = RasterAccess::Slot(slice.Buffer);
        packet.Offsets[i] = slice.Offset;
        packet.Sizes[i] = slice.Size;
    }
    return RasterStatus::Ready;
}
RasterStatus ReserveFrameSubmission() noexcept
{
    if (gFrameSubmission != 0)
    {
        return RasterStatus::Ready;
    }
    if (gSubmissions.Next == 0)
    {
        return RasterStatus::IdentityExhausted;
    }
    const auto result = backend::RasterReserveSubmission();
    if (result == RasterStatus::Ready)
    {
        gFrameSubmission = gSubmissions.Next;
    }
    return result;
}
RasterStatus EncodePacket(const RasterPacket& packet) noexcept
{
    if (gDrawCount >= CurrentDeviceProfile().Raster.DrawsPerFrame)
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto reservation = ReserveFrameSubmission();
    if (reservation != RasterStatus::Ready)
    {
        return reservation;
    }
    if (ClaimRasterFrame() == false)
    {
        return RasterStatus::InvalidState;
    }
    FrameRetain(RasterKind::Pipeline, packet.Pipeline);
    FrameRetain(RasterKind::Set, packet.Set);
    FrameRetain(RasterKind::Buffer, packet.Indices);
    if (packet.Indirect != RASTER_INVALID)
    {
        FrameRetain(RasterKind::Buffer, packet.Indirect);
    }
    for (usize i = 0; i < At(RasterKind::Pipeline, packet.Pipeline).Pipeline.StreamCount; ++i)
    {
        FrameRetain(RasterKind::Buffer, packet.Vertices[i]);
    }
    RetainPacketTextures(packet);
    ++gDrawCount;
    return backend::RasterDraw(packet);
}
} // namespace
RasterStatus DrawIndexed(DeviceHandle device, const RasterDraw& draw) noexcept
{
    if (FrameOpen() == false)
    {
        const auto admission = Admission(device, false);
        return admission == RasterStatus::Ready ? RasterStatus::InvalidState : admission;
    }
    RasterPacket packet;
    const auto valid = ValidateDraw(device, draw, packet);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    const auto compatible = ValidatePacketPass(packet, gRasterPass, true);
    return compatible == RasterStatus::Ready ? EncodePacket(packet) : compatible;
}

namespace internal
{
void RasterExpect(uint32 request, RasterCallbacks callbacks) noexcept
{
    for (auto& records : gRecords)
    {
        for (auto& record : records)
        {
            if (record.Owner != 0 && record.Request == request)
            {
                record.Callbacks = static_cast<uint32>(callbacks);
                return;
            }
        }
    }
}
bool IsRasterRequestCurrent(RasterRequestIdentity identity) noexcept
{
    const StorageTable<Record>& records = gRecords[static_cast<usize>(identity.Kind)];
    if (identity.ResourceSlot >= records.GetSize())
    {
        return false;
    }
    const Record& record = records[identity.ResourceSlot];
    return record.Owner != 0 && record.Request == identity.Request && record.Callbacks != 0;
}
void RasterComplete(uint32 request, RasterStatus status) noexcept
{
    for (auto& records : gRecords)
    {
        for (auto& record : records)
        {
            if (record.Owner != 0 && record.Request == request && record.Callbacks != 0)
            {
                --record.Callbacks;
                if (status != RasterStatus::Ready)
                {
                    record.Status = status;
                }
                else if (record.Callbacks == 0 && record.Status == RasterStatus::Pending)
                {
                    record.Status = RasterStatus::Ready;
                }
                return;
            }
        }
    }
}
void RasterFail(uint32 request, RasterStatus status) noexcept
{
    for (auto& records : gRecords)
    {
        for (auto& record : records)
        {
            if (record.Owner != 0 && record.Request == request)
            {
                record.Status = status;
                return;
            }
        }
    }
}
bool PrepareRasterFrame() noexcept
{
    return backend::RasterPrepareStorage() == RasterStatus::Ready;
}
void RasterBeginFrame() noexcept
{
    gFrameSubmission = 0;
    gDrawCount = 0;
    gRasterPass = {};
    gGraphExecuting = false;
    gSurfaceColorDefined = true;
    gSurfaceDepthDefined = false;
    Collect();
}
void RasterEndFrame(bool accepted) noexcept
{
    if (gFrameSubmission != 0 && accepted)
    {
        (void)gSubmissions.Take();
        if (gFirstAcceptedSubmission == 0)
        {
            gFirstAcceptedSubmission = gFrameSubmission;
        }
        gLastAcceptedSubmission = gFrameSubmission;
        backend::RasterSubmit(gFrameSubmission);
    }
    if (accepted == false)
    {
        backend::RasterDiscardSubmission();
    }
    for (auto& records : gRecords)
    {
        for (auto& record : records)
        {
            if (record.InFrame == false)
            {
                continue;
            }
            if (accepted)
            {
                record.LastUse = gFrameSubmission;
                if (&records == &gRecords[static_cast<usize>(RasterKind::Texture)])
                {
                    record.TextureUse = record.FrameTextureUse;
                    record.ColorDefined = record.FrameColorDefined;
                    record.DepthDefined = record.FrameDepthDefined;
                    record.StencilDefined = record.FrameStencilDefined;
                    for (usize i = 0; i < record.AttachmentState.GetSize(); ++i)
                    {
                        record.AttachmentState[i] = record.FrameAttachmentState[i];
                    }
                    ++record.TextureRevision;
                }
            }
            if (accepted && record.FrameBufferWritten)
            {
                ++record.BufferRevision;
            }
            record.FrameBufferWritten = false;
            record.InFrame = false;
            --record.References;
        }
    }
    gFrameSubmission = 0;
    Collect();
}
void ReleaseRasterResources() noexcept
{
    // Shutdown owns all physical release. Native reset waits or drops the lost
    // device; browser callbacks retain their stable bookkeeping until delivery.
    backend::RayDrain();
    backend::RasterReset();
    backend::TimingReset();
    backend::ResetPassTimings();
    backend::LifetimeReset();
    ResetLifetimeRecords();
    ResetGraphRecords();
    for (usize kind = KINDS; kind-- > 0;)
    {
        for (usize slot = 0; slot < gRecords[kind].GetSize(); ++slot)
        {
            auto& record = gRecords[kind][slot];
            if (record.Owner == 0)
            {
                continue;
            }
            backend::RasterDestroy(static_cast<RasterKind>(kind), slot);
            ReleaseResourceStorage(record.StorageBytes);
            const auto generation = RasterNextGeneration(record.Generation);
            ReleaseHostStorage(record.AuxiliaryHostBytes);
            record.CpuTexture.Reset();
            record.Ray.Reset();
            record = {};
            record.Generation = generation;
        }
    }
    backend::RasterShutdown();
    gFirstAcceptedSubmission = 0;
    gLastAcceptedSubmission = 0;
    gFrameSubmission = 0;
    gDrawCount = 0;
}
} // namespace internal
RasterStatus GetAllocationTelemetry(DeviceHandle device, AllocationTelemetry& output) noexcept
{
    const auto status = Admission(device, false);
    if (status == RasterStatus::InvalidHandle)
    {
        return status;
    }
    output = {};
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    output.BindingDescriptors = CountBindingDescriptors();
    for (const Record& record : gRecords[static_cast<usize>(RasterKind::Set)])
    {
        if (record.Owner != 0 && record.Published == false)
        {
            output.RetiringBindingDescriptors += record.Set.Description.DescriptorCount;
        }
    }
    internal::DescriptorArenaUsage arenaUsage;
    output.DescriptorArenasAvailable = backend::GetDescriptorArenaUsage(arenaUsage);
    output.DescriptorArenaPools = arenaUsage.Pools;
    output.ReservedArenaDescriptors = arenaUsage.ReservedDescriptors;
    static uint64 lastOwner = 0, peak = 0, blockPeak = 0;
    const auto owner = internal::DeviceOwner();
    if (lastOwner != owner)
    {
        lastOwner = owner;
        peak = 0;
        blockPeak = 0;
    }
    output.ApiBytesAvailable = backend::Kind() == Backend::Metal || backend::Kind() == Backend::Vulkan;
    for (usize kind = 0; kind < 2; ++kind)
    {
        for (usize slot = 0; slot < gRecords[kind].GetSize(); ++slot)
        {
            const auto& record = gRecords[kind][slot];
            if (record.Owner == 0)
            {
                continue;
            }
            output.LogicalBytes += record.StorageBytes;
            uint64 bytes = 0;
            if (backend::RasterAllocationBytes(static_cast<RasterKind>(kind), slot, bytes) == false)
            {
                output.ApiBytesAvailable = false;
                continue;
            }
            if (foundation::TryAdd(output.ApiBytes, bytes, output.ApiBytes) == false ||
                (record.Published == false &&
                 foundation::TryAdd(output.RetiringApiBytes, bytes, output.RetiringApiBytes) == false))
            {
                output = {};
                return RasterStatus::CapacityExceeded;
            }
        }
    }
    if (output.ApiBytesAvailable == false)
    {
        output.ApiBytes = 0;
        output.RetiringApiBytes = 0;
    }
    else if (output.ApiBytes > peak)
    {
        peak = output.ApiBytes;
    }
    output.PeakObservedApiBytes = peak;
    output.AllocatorBlockBytesAvailable = backend::AllocatorBlockBytes(output.AllocatorBlockBytes);
    if (output.AllocatorBlockBytesAvailable && output.AllocatorBlockBytes > blockPeak)
    {
        blockPeak = output.AllocatorBlockBytes;
    }
    output.PeakObservedAllocatorBlockBytes = output.AllocatorBlockBytesAvailable ? blockPeak : 0;
    return RasterStatus::Ready;
}
RasterStatus GetStorageUsage(DeviceHandle device, StorageUsage& output) noexcept
{
    const auto status = Admission(device, false);
    if (status == RasterStatus::InvalidHandle)
    {
        return status;
    }
    output = {};
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    output = CurrentStorageUsage();
    for (auto& records : gRecords)
    {
        for (const auto& record : records)
        {
            if (record.Owner == 0)
            {
                continue;
            }
            if (record.Published)
            {
                ++output.PublishedRecords;
            }
            else
            {
                ++output.DetachedRecords;
            }
            if (record.Status == RasterStatus::Pending || record.Callbacks != 0)
            {
                ++output.PendingRecords;
            }
        }
    }
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, BufferHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Buffer);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, BufferHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Buffer);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, TextureHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Texture);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, TextureHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Texture);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->ServiceOwned)
    {
        return RasterStatus::InvalidState;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, TextureViewHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::View);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, TextureViewHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::View);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, SamplerHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Sampler);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, SamplerHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Sampler);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, RasterShaderHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Shader);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, RasterShaderHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Shader);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, BindingLayoutHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Layout);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, BindingLayoutHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Layout);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, BindingSetHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Set);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, BindingSetHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Set);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, RasterPipelineHandle handle) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::Pipeline);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, RasterPipelineHandle& handle) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::Pipeline);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->ServiceOwned)
    {
        return RasterStatus::InvalidState;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
} // namespace ludus::graphics::rhi

#include "internal/lifetime_services.h"
#include "internal/mutable_transfers.h"

#include "internal/compute_services.h"

#include "internal/ordered_graph.h"

#include "internal/ray_services.h"

namespace ludus::graphics::rhi
{
namespace
{
RasterStatus CpuTextureContent(const Record& record, uint32 mip, TextureCpuContent& output) noexcept
{
    if (record.Status != RasterStatus::Ready)
    {
        return record.Status == RasterStatus::Pending ? RasterStatus::NotReady : record.Status;
    }
    if (record.CpuTexture.Get() == nullptr)
    {
        return RasterStatus::Unsupported;
    }
    if (mip >= record.Texture.MipLevels)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto& source = *record.CpuTexture;
    uint32 width = record.Texture.Width, height = record.Texture.Height;
    for (uint32 level = 0; level < mip; ++level)
    {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    const auto& range = source.Mips[mip];
    output = {source.Bytes.GetData() + range.Offset,
              range.RowPitch * height,
              range.RowPitch,
              width,
              height,
              record.Texture.Format};
    return RasterStatus::Ready;
}
} // namespace
RasterStatus
GetTextureCpuContent(DeviceHandle device, TextureHandle handle, uint32 mip, TextureCpuContent& output) noexcept
{
    const auto status = Admission(device, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    Collect();
    const auto* record = Resolve(handle, RasterKind::Texture);
    return record == nullptr ? RasterStatus::InvalidHandle : CpuTextureContent(*record, mip, output);
}
RasterStatus
GetTextureCpuContent(DeviceHandle device, TextureViewHandle handle, uint32 mip, TextureCpuContent& output) noexcept
{
    const auto status = Admission(device, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    Collect();
    const auto* record = Resolve(handle, RasterKind::View);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Status != RasterStatus::Ready)
    {
        return record->Status == RasterStatus::Pending ? RasterStatus::NotReady : record->Status;
    }
    if (mip >= record->View.MipCount)
    {
        return RasterStatus::InvalidDescription;
    }
    return CpuTextureContent(At(RasterKind::Texture, record->TextureSlot), record->View.BaseMip + mip, output);
}
} // namespace ludus::graphics::rhi

#include "internal/native_interop_services.h"
