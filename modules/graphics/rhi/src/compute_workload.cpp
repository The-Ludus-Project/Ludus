#include <ludus/graphics/rhi/compute_workload.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <new>

namespace ludus::graphics::rhi
{
using namespace foundation;
namespace internal
{
// Bounded packing workspace; negotiated descriptor/graph limits still apply.
constexpr usize WORKLOAD_BINDINGS = 16;
constexpr usize WORKLOAD_NAME_BYTES = 64;
constexpr uint32 OWN_PROGRAM = ~uint32{0};
struct WorkloadBuffer final
{
    BufferHandle Handle{};
    usize Size = 0;
    bool IsReadOnly = false;
    bool IsBorrowed = false;
};
struct WorkloadKernel final
{
    StaticString<WORKLOAD_NAME_BYTES - 1> Name;
    Array<ComputeWorkloadBinding> Bindings;
    RasterShaderHandle Shader{};
    BindingLayoutHandle Layout{};
    BindingSetHandle Set{};
    ComputePipelineHandle Pipeline{};
    uint32 GroupsX = 1;
    uint32 GroupsY = 1;
    uint32 GroupsZ = 1;
    uint32 UseProgramFrom = OWN_PROGRAM;
};
struct ComputeWorkloadState final
{
    DeviceHandle Device{};
    Array<WorkloadBuffer> Buffers;
    Array<WorkloadKernel> Kernels;
    usize BufferBytes = 0;
    RasterStatus Status = RasterStatus::Pending;
    ~ComputeWorkloadState() noexcept
    {
        for (WorkloadKernel& kernel : Kernels)
        {
            static_cast<void>(Destroy(Device, kernel.Set));
            if (kernel.UseProgramFrom == OWN_PROGRAM)
            {
                static_cast<void>(Destroy(Device, kernel.Pipeline));
                static_cast<void>(Destroy(Device, kernel.Layout));
                static_cast<void>(Destroy(Device, kernel.Shader));
            }
        }
        for (WorkloadBuffer& buffer : Buffers)
        {
            if (buffer.IsBorrowed == false)
            {
                static_cast<void>(Destroy(Device, buffer.Handle));
            }
        }
    }
};
} // namespace internal
namespace
{
using internal::ComputeWorkloadState;
using internal::WorkloadBuffer;
using internal::WorkloadKernel;
bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}
RasterStatus
ValidateDescription(DeviceHandle device, const ComputeWorkloadDescription& description, usize& outBytes) noexcept
{
    if (description.Buffers.IsValid() == false || description.Kernels.IsValid() == false ||
        description.Buffers.IsEmpty() == true || description.Kernels.IsEmpty() == true)
    {
        return RasterStatus::InvalidDescription;
    }
    ComputeCapabilities capabilities;
    RasterStatus status = GetComputeCapabilities(device, capabilities);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    usize bytes = 0;
    for (usize bufferIndex = 0; bufferIndex < description.Buffers.GetSize(); ++bufferIndex)
    {
        const ComputeWorkloadBuffer& buffer = description.Buffers[bufferIndex];
        if (buffer.InitialBytes.IsValid() == false || buffer.Size == 0 ||
            (buffer.InitialBytes.IsEmpty() == false &&
             (buffer.InitialBytes.GetSize() != buffer.Size || buffer.External != BufferHandle{})))
        {
            return RasterStatus::InvalidDescription;
        }
        if (buffer.External != BufferHandle{})
        {
            for (usize earlier = 0; earlier < bufferIndex; ++earlier)
            {
                if (description.Buffers[earlier].External == buffer.External)
                {
                    return RasterStatus::InvalidDescription;
                }
            }
        }
        if (buffer.Size > capabilities.MaxStorageRange || TryAdd(bytes, buffer.Size, bytes) == false ||
            bytes > description.MaximumBufferBytes)
        {
            return RasterStatus::CapacityExceeded;
        }
    }
    for (usize kernelIndex = 0; kernelIndex < description.Kernels.GetSize(); ++kernelIndex)
    {
        const ComputeWorkloadKernel& kernel = description.Kernels[kernelIndex];
        if (kernel.UseProgramFrom != internal::OWN_PROGRAM &&
            (kernel.UseProgramFrom >= kernelIndex ||
             description.Kernels[kernel.UseProgramFrom].UseProgramFrom != internal::OWN_PROGRAM))
        {
            return RasterStatus::InvalidDescription;
        }
        const RasterShaderDescription& shader = kernel.UseProgramFrom == internal::OWN_PROGRAM
                                                    ? kernel.Shader
                                                    : description.Kernels[kernel.UseProgramFrom].Shader;
        if (kernel.NameOrNull == nullptr || kernel.NameOrNull[0] == '\0' || kernel.Bindings.IsValid() == false ||
            kernel.Bindings.GetSize() > internal::WORKLOAD_BINDINGS || shader.Artifact.Stage != ShaderStage::Compute ||
            shader.Bindings.size() != kernel.Bindings.GetSize())
        {
            return RasterStatus::InvalidDescription;
        }
        if (kernel.GroupsX == 0 || kernel.GroupsY == 0 || kernel.GroupsZ == 0 ||
            kernel.GroupsX > capabilities.MaxDispatch[0] || kernel.GroupsY > capabilities.MaxDispatch[1] ||
            kernel.GroupsZ > capabilities.MaxDispatch[2])
        {
            return RasterStatus::CapacityExceeded;
        }
        for (usize i = 0; i < kernel.Bindings.GetSize(); ++i)
        {
            const ComputeWorkloadBinding& binding = kernel.Bindings[i];
            if (binding.Buffer >= description.Buffers.GetSize() || binding.Size == 0)
            {
                return RasterStatus::InvalidDescription;
            }
            const ComputeWorkloadBuffer& buffer = description.Buffers[binding.Buffer];
            if (binding.Offset > buffer.Size || binding.Size > buffer.Size - binding.Offset ||
                (buffer.IsReadOnly == true && binding.IsWritable == true))
            {
                return RasterStatus::InvalidDescription;
            }
            const RasterBindingKind kind =
                binding.IsWritable == true ? RasterBindingKind::StorageReadWrite : RasterBindingKind::StorageRead;
            usize matches = 0;
            for (const RasterBinding& reflected : shader.Bindings)
            {
                if (reflected.Binding == binding.Slot && reflected.Kind == kind &&
                    reflected.Visibility == RasterVisibility::Compute)
                {
                    ++matches;
                }
            }
            if (matches != 1)
            {
                return RasterStatus::InvalidDescription;
            }
            for (usize earlier = 0; earlier < i; ++earlier)
            {
                // Whole-buffer hazards require one physical allocation per pass entry.
                if (kernel.Bindings[earlier].Buffer == binding.Buffer || kernel.Bindings[earlier].Slot == binding.Slot)
                {
                    return RasterStatus::InvalidDescription;
                }
            }
        }
    }
    outBytes = bytes;
    return RasterStatus::Ready;
}
RasterStatus Prepare(ComputeWorkloadState& inoutState) noexcept
{
    for (const WorkloadBuffer& buffer : inoutState.Buffers)
    {
        const RasterStatus status = GetStatus(inoutState.Device, buffer.Handle);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
    }
    for (WorkloadKernel& kernel : inoutState.Kernels)
    {
        WorkloadKernel& program =
            kernel.UseProgramFrom == internal::OWN_PROGRAM ? kernel : inoutState.Kernels[kernel.UseProgramFrom];
        RasterStatus status = GetStatus(inoutState.Device, program.Shader);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
        status = GetStatus(inoutState.Device, program.Layout);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
        if (GetStatus(inoutState.Device, kernel.Set) == RasterStatus::InvalidHandle)
        {
            StaticArray<RasterBindingResource, internal::WORKLOAD_BINDINGS> resources{};
            for (usize i = 0; i < kernel.Bindings.GetSize(); ++i)
            {
                const ComputeWorkloadBinding& binding = kernel.Bindings[i];
                resources[i] = {binding.Slot, inoutState.Buffers[binding.Buffer].Handle, binding.Offset, binding.Size};
            }
            status = CreateBindingSet(inoutState.Device,
                                      program.Layout,
                                      {resources.GetData(), kernel.Bindings.GetSize()},
                                      kernel.Set);
            if (IsAccepted(status) == false)
            {
                return status;
            }
        }
        status = GetStatus(inoutState.Device, kernel.Set);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
        if (GetStatus(inoutState.Device, program.Pipeline) == RasterStatus::InvalidHandle)
        {
            status = CreateComputePipeline(inoutState.Device, {program.Shader, program.Layout}, program.Pipeline);
            if (IsAccepted(status) == false)
            {
                return status;
            }
        }
        status = GetStatus(inoutState.Device, program.Pipeline);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
    }
    return RasterStatus::Ready;
}
} // namespace
ComputeWorkload::ComputeWorkload() noexcept = default;
ComputeWorkload::~ComputeWorkload() noexcept = default;
ComputeWorkload::ComputeWorkload(ComputeWorkload&& other) noexcept = default;
ComputeWorkload& ComputeWorkload::operator=(ComputeWorkload&& other) noexcept = default;
RasterStatus ComputeWorkload::Initialize(DeviceHandle device, const ComputeWorkloadDescription& description) noexcept
{
    if (mState.Get() != nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    usize bytes = 0;
    RasterStatus status = ValidateDescription(device, description, bytes);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    ComputeWorkloadState* allocationOrNull = new (std::nothrow) ComputeWorkloadState;
    core::UniquePtr<ComputeWorkloadState> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return RasterStatus::OutOfMemory;
    }
    candidate->Device = device;
    candidate->BufferBytes = bytes;
    if (candidate->Buffers.TryResize(description.Buffers.GetSize()) == false ||
        candidate->Kernels.TryResize(description.Kernels.GetSize()) == false)
    {
        return RasterStatus::OutOfMemory;
    }
    Array<uint8> zeros;
    for (usize i = 0; i < description.Buffers.GetSize(); ++i)
    {
        const ComputeWorkloadBuffer& source = description.Buffers[i];
        WorkloadBuffer& buffer = candidate->Buffers[i];
        buffer.Size = source.Size;
        buffer.IsReadOnly = source.IsReadOnly;
        buffer.IsBorrowed = source.External != BufferHandle{};
        if (buffer.IsBorrowed == true)
        {
            buffer.Handle = source.External;
            status = GetStatus(device, buffer.Handle);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            continue;
        }
        BorrowedView<const uint8> initial = source.InitialBytes;
        if (initial.IsEmpty() == true)
        {
            if (zeros.TryResize(source.Size, uint8{0}) == false)
            {
                return RasterStatus::OutOfMemory;
            }
            initial = {zeros.GetData(), source.Size};
        }
        status = CreateBuffer(device,
                              {BufferRole::Storage, source.Size},
                              {initial.GetDataOrNull(), initial.GetSize()},
                              buffer.Handle);
        if (IsAccepted(status) == false)
        {
            return status;
        }
    }
    for (usize i = 0; i < description.Kernels.GetSize(); ++i)
    {
        const ComputeWorkloadKernel& source = description.Kernels[i];
        WorkloadKernel& kernel = candidate->Kernels[i];
        if (kernel.Name.TryAssign(source.NameOrNull) != StringStatus::Ok)
        {
            return RasterStatus::InvalidDescription;
        }
        if (kernel.Bindings.TryAddRange({source.Bindings.GetDataOrNull(), source.Bindings.GetSize()}) == false)
        {
            return RasterStatus::OutOfMemory;
        }
        kernel.GroupsX = source.GroupsX;
        kernel.GroupsY = source.GroupsY;
        kernel.GroupsZ = source.GroupsZ;
        kernel.UseProgramFrom = source.UseProgramFrom;
        if (kernel.UseProgramFrom != internal::OWN_PROGRAM)
        {
            continue;
        }
        status = CreateBindingLayout(device, source.Shader.Bindings, kernel.Layout);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        status = CreateRasterShader(device, source.Shader, kernel.Shader);
        if (IsAccepted(status) == false)
        {
            return status;
        }
    }
    status = Prepare(*candidate);
    if (IsAccepted(status) == false)
    {
        return status;
    }
    candidate->Status = status;
    mState = Move(candidate);
    return status;
}
RasterStatus ComputeWorkload::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return RasterStatus::NotReady;
    }
    if (mState->Status == RasterStatus::Pending)
    {
        mState->Status = Prepare(*mState);
    }
    return mState->Status;
}
RasterStatus ComputeWorkload::BuildGraph(OrderedGraph& outGraph,
                                         BorrowedView<GraphVersion> outVersions,
                                         bool shouldRootBuffers) const noexcept
{
    if (mState.Get() == nullptr || mState->Status != RasterStatus::Ready)
    {
        return mState.Get() == nullptr ? RasterStatus::NotReady : mState->Status;
    }
    if (outVersions.IsValid() == false ||
        (outVersions.IsEmpty() == false && outVersions.GetSize() != mState->Buffers.GetSize()))
    {
        return RasterStatus::InvalidDescription;
    }
    Array<GraphVersion> versions;
    if (versions.TryResize(mState->Buffers.GetSize()) == false)
    {
        return RasterStatus::OutOfMemory;
    }
    const DeviceHandle device = mState->Device;
    OrderedGraph candidate;
    RasterStatus status = CreateOrderedGraph(device, outGraph);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    // CreateOrderedGraph validates the caller's empty output. Keep subsequent
    // preparation private so even a loss-time discard failure preserves that output.
    candidate = outGraph;
    outGraph = {};
    status = [&]() noexcept -> RasterStatus {
        for (usize i = 0; i < versions.GetSize(); ++i)
        {
            const RasterStatus imported = ImportGraphBuffer(device, candidate, mState->Buffers[i].Handle, versions[i]);
            if (imported != RasterStatus::Ready)
            {
                return imported;
            }
        }
        for (const WorkloadKernel& kernel : mState->Kernels)
        {
            // GraphUse's default member initializer supplies RasterVisibility::Both;
            // Clang 23 incorrectly diagnoses aggregate-array initialization as enum zero.
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            StaticArray<GraphUse, internal::WORKLOAD_BINDINGS> uses{};
            for (usize i = 0; i < kernel.Bindings.GetSize(); ++i)
            {
                const ComputeWorkloadBinding& binding = kernel.Bindings[i];
                if (binding.IsWritable == true)
                {
                    GraphVersion next;
                    const RasterStatus advanced = NextGraphVersion(device, candidate, versions[binding.Buffer], next);
                    if (advanced != RasterStatus::Ready)
                    {
                        return advanced;
                    }
                    versions[binding.Buffer] = next;
                }
                uses[i] = {versions[binding.Buffer],
                           binding.IsWritable == true ? GraphAccessMode::StorageReadWrite
                                                      : GraphAccessMode::StorageRead,
                           binding.Offset,
                           binding.Size,
                           RasterVisibility::Compute};
            }
            const WorkloadKernel& program =
                kernel.UseProgramFrom == internal::OWN_PROGRAM ? kernel : mState->Kernels[kernel.UseProgramFrom];
            const ComputeDispatch dispatch{program.Pipeline,
                                           kernel.Set,
                                           {kernel.GroupsX, kernel.GroupsY, kernel.GroupsZ}};
            GraphPassDescription pass;
            pass.Name = kernel.Name.GetData();
            pass.Dispatch = &dispatch;
            pass.Uses = uses.GetData();
            pass.UseCount = kernel.Bindings.GetSize();
            const RasterStatus added = AddGraphPass(device, candidate, pass);
            if (added != RasterStatus::Ready)
            {
                return added;
            }
        }
        for (usize i = 0; i < versions.GetSize(); ++i)
        {
            if (shouldRootBuffers == true && mState->Buffers[i].IsReadOnly == false)
            {
                const RasterStatus rooted = AddGraphRoot(device, candidate, versions[i], GraphRoot::History);
                if (rooted != RasterStatus::Ready)
                {
                    return rooted;
                }
            }
        }
        return RasterStatus::Ready;
    }();
    if (status != RasterStatus::Ready)
    {
        static_cast<void>(DiscardOrderedGraph(device, candidate));
        return status;
    }
    for (usize i = 0; i < outVersions.GetSize(); ++i)
    {
        outVersions[i] = versions[i];
    }
    outGraph = candidate;
    return RasterStatus::Ready;
}
RasterStatus ComputeWorkload::GetBuffer(usize index, BufferHandle& outBuffer) const noexcept
{
    if (mState.Get() == nullptr || mState->Status != RasterStatus::Ready)
    {
        return mState.Get() == nullptr ? RasterStatus::NotReady : mState->Status;
    }
    if (index >= mState->Buffers.GetSize())
    {
        return RasterStatus::InvalidDescription;
    }
    outBuffer = mState->Buffers[index].Handle;
    return RasterStatus::Ready;
}
usize ComputeWorkload::GetBufferCount() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->Buffers.GetSize();
}
usize ComputeWorkload::GetBufferBytes() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->BufferBytes;
}
void ComputeWorkload::Reset() noexcept
{
    mState.Reset();
}
} // namespace ludus::graphics::rhi
