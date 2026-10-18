#include "internal/uniform_set_table.h"

#include <ludus/foundation/base/checked_integer.hpp>

// Per-draw uniform sets for authored render-graph techniques; see the RG
// execution profile in docs/architecture/render-graph-authoring.md. Values are
// copied once into immutable buffers and selected per packet; they never become
// shader configurations.
namespace ludus::graphics::renderer::internal
{
namespace
{
using Status = rhi::RasterStatus;
constexpr uint64 LAST_GENERATION = ~uint64{0};
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool AlignUp(usize value, usize alignment, usize& outValue) noexcept
{
    usize sum = 0;
    if (TryAdd(value, alignment - 1, sum) == false)
    {
        return false;
    }
    outValue = sum / alignment * alignment;
    return true;
}
bool IsAdmittedUniform(const rhi::RasterBinding& binding) noexcept
{
    return binding.Kind == rhi::RasterBindingKind::UniformBuffer && binding.Binding < rhi::RASTER_BINDING_COUNT &&
           binding.MinSize != 0 && binding.HasDynamicOffset == false && binding.Count == 1 &&
           binding.Indexing == rhi::RasterBindingIndexing::Constant && binding.IsRuntimeSized == false &&
           binding.HasPartialBinding == false;
}
} // namespace
bool MergeFlatUniformLayout(std::span<const rhi::RasterBinding> vertex,
                            std::span<const rhi::RasterBinding> fragment,
                            usize alignment,
                            usize maxRange,
                            FlatUniformLayout& outLayout) noexcept
{
    if (vertex.empty() == true && fragment.empty() == true)
    {
        outLayout = {};
        return true;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    {
        return false;
    }
    StaticArray<bool, rhi::RASTER_BINDING_COUNT> isVertex{};
    StaticArray<bool, rhi::RASTER_BINDING_COUNT> isFragment{};
    StaticArray<usize, rhi::RASTER_BINDING_COUNT> sizes{};
    for (usize stage = 0; stage < 2; ++stage)
    {
        const std::span<const rhi::RasterBinding> bindings = stage == 0 ? vertex : fragment;
        StaticArray<bool, rhi::RASTER_BINDING_COUNT>& isDeclared = stage == 0 ? isVertex : isFragment;
        const rhi::RasterVisibility own = stage == 0 ? rhi::RasterVisibility::Vertex : rhi::RasterVisibility::Fragment;
        for (const rhi::RasterBinding& binding : bindings)
        {
            if (IsAdmittedUniform(binding) == false || binding.MinSize > maxRange ||
                (binding.Visibility != own && binding.Visibility != rhi::RasterVisibility::Both) ||
                isDeclared[binding.Binding] == true ||
                (sizes[binding.Binding] != 0 && sizes[binding.Binding] != binding.MinSize))
            {
                return false;
            }
            isDeclared[binding.Binding] = true;
            sizes[binding.Binding] = binding.MinSize;
        }
    }
    FlatUniformLayout layout;
    for (usize slot = 0; slot < rhi::RASTER_BINDING_COUNT; ++slot)
    {
        if (sizes[slot] == 0)
        {
            continue;
        }
        rhi::RasterBinding& entry = layout.Bindings[layout.Count];
        entry.Binding = static_cast<uint32>(slot);
        entry.Kind = rhi::RasterBindingKind::UniformBuffer;
        entry.Visibility = isVertex[slot] == true && isFragment[slot] == true ? rhi::RasterVisibility::Both
                           : isVertex[slot] == true                           ? rhi::RasterVisibility::Vertex
                                                                              : rhi::RasterVisibility::Fragment;
        layout.Sizes[layout.Count] = sizes[slot];
        if (AlignUp(sizes[slot], UNIFORM_RANGE_GRANULE, entry.MinSize) == false || entry.MinSize > maxRange ||
            AlignUp(layout.BufferBytes, alignment, layout.Offsets[layout.Count]) == false ||
            TryAdd(layout.Offsets[layout.Count], entry.MinSize, layout.BufferBytes) == false ||
            TryAdd(layout.CallerBytes, sizes[slot], layout.CallerBytes) == false)
        {
            return false;
        }
        ++layout.Count;
    }
    outLayout = layout;
    return true;
}
Status CreateFlatUniformBindings(rhi::DeviceHandle device,
                                 const FlatUniformLayout& layout,
                                 rhi::BufferHandle buffer,
                                 rhi::BindingSetHandle& outBindings) noexcept
{
    StaticArray<rhi::RasterBindingResource, rhi::RASTER_BINDING_COUNT> resources{};
    for (usize i = 0; i < layout.Count; ++i)
    {
        resources[i].Binding = layout.Bindings[i].Binding;
        resources[i].Buffer = buffer;
        resources[i].Offset = layout.Offsets[i];
        resources[i].Size = layout.Bindings[i].MinSize;
    }
    return rhi::CreateBindingSet(device, layout.Handle, {resources.GetData(), layout.Count}, outBindings);
}
usize UniformSetTable::GetHostBytes(usize capacity, usize stagingBytes) noexcept
{
    usize records = 0;
    usize total = 0;
    if (TryMultiply(capacity, sizeof(Record), records) == false || TryAdd(records, stagingBytes, total) == false)
    {
        return ~usize{0};
    }
    return total;
}
bool UniformSetTable::Reserve(usize capacity, usize stagingBytes) noexcept
{
    if (mRecords.TryResize(capacity) == false || mStaging.TryResize(stagingBytes) == false)
    {
        mRecords.Clear();
        mStaging.Clear();
        return false;
    }
    return true;
}
Status UniformSetTable::Create(rhi::DeviceHandle device,
                               const FlatUniformLayout& layout,
                               BorrowedView<const uint8> bytes,
                               UniformSetIdentity& outIdentity) noexcept
{
    if (layout.Count == 0 || bytes.IsValid() == false || bytes.GetSize() != layout.CallerBytes ||
        mStaging.GetSize() < layout.BufferBytes)
    {
        return Status::InvalidDescription;
    }
    usize slot = mRecords.GetSize();
    bool isExhausted = false;
    for (usize i = 0; i < mRecords.GetSize() && slot == mRecords.GetSize(); ++i)
    {
        if (mRecords[i].IsUsed == false)
        {
            if (mRecords[i].Generation != LAST_GENERATION)
            {
                slot = i;
            }
            else
            {
                isExhausted = true;
            }
        }
    }
    if (slot == mRecords.GetSize())
    {
        return isExhausted == true ? Status::IdentityExhausted : Status::CapacityExceeded;
    }
    for (usize i = 0; i < layout.BufferBytes; ++i)
    {
        mStaging[i] = 0;
    }
    usize source = 0;
    for (usize binding = 0; binding < layout.Count; ++binding)
    {
        for (usize i = 0; i < layout.Sizes[binding]; ++i)
        {
            mStaging[layout.Offsets[binding] + i] = bytes[source + i];
        }
        source += layout.Sizes[binding];
    }
    Record& record = mRecords[slot];
    const Status created = rhi::CreateBuffer(device,
                                             {rhi::BufferRole::Uniform, layout.BufferBytes},
                                             {mStaging.GetData(), layout.BufferBytes},
                                             record.Buffer);
    if (IsAccepted(created) == false)
    {
        return created;
    }
    record.IsUsed = true;
    record.IsOwned = true;
    record.HasBindings = false;
    record.References = 0;
    record.Setup = Status::Pending;
    const uint64 generation = record.Generation;
    const Status status = GetStatus(device, layout, slot, generation);
    if (IsAccepted(status) == false)
    {
        Detach(device, slot);
        return status;
    }
    outIdentity = {slot, generation};
    return status;
}
Status UniformSetTable::CreateDefault(rhi::DeviceHandle device,
                                      const FlatUniformLayout& layout,
                                      rhi::BufferHandle& outBuffer) noexcept
{
    if (layout.Count == 0 || mStaging.GetSize() < layout.BufferBytes)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < layout.BufferBytes; ++i)
    {
        mStaging[i] = 0;
    }
    return rhi::CreateBuffer(device,
                             {rhi::BufferRole::Uniform, layout.BufferBytes},
                             {mStaging.GetData(), layout.BufferBytes},
                             outBuffer);
}
Status UniformSetTable::GetStatus(rhi::DeviceHandle device,
                                  const FlatUniformLayout& layout,
                                  usize slot,
                                  uint64 generation) noexcept
{
    if (IsOwned(slot, generation) == false)
    {
        return Status::InvalidHandle;
    }
    Record& record = mRecords[slot];
    if (record.Setup != Status::Pending)
    {
        return record.Setup;
    }
    const Status buffer = rhi::GetStatus(device, record.Buffer);
    if (buffer != Status::Ready)
    {
        if (buffer != Status::Pending)
        {
            record.Setup = buffer;
        }
        return buffer;
    }
    if (record.HasBindings == false)
    {
        const Status created = CreateFlatUniformBindings(device, layout, record.Buffer, record.Bindings);
        if (IsAccepted(created) == false)
        {
            record.Setup = created;
            return created;
        }
        record.HasBindings = true;
    }
    const Status ready = rhi::GetStatus(device, record.Bindings);
    if (ready != Status::Pending)
    {
        record.Setup = ready;
    }
    return ready;
}
bool UniformSetTable::IsOwned(usize slot, uint64 generation) const noexcept
{
    return slot < mRecords.GetSize() && mRecords[slot].IsUsed == true && mRecords[slot].IsOwned == true &&
           mRecords[slot].Generation == generation;
}
void UniformSetTable::Retain(usize slot) noexcept
{
    ++mRecords[slot].References;
}
void UniformSetTable::Unretain(rhi::DeviceHandle device, usize slot) noexcept
{
    --mRecords[slot].References;
    dropIfUnused(device, slot);
}
void UniformSetTable::Detach(rhi::DeviceHandle device, usize slot) noexcept
{
    mRecords[slot].IsOwned = false;
    dropIfUnused(device, slot);
}
rhi::BindingSetHandle UniformSetTable::GetBindings(usize slot) const noexcept
{
    return mRecords[slot].Bindings;
}
void UniformSetTable::Clear(rhi::DeviceHandle device) noexcept
{
    for (usize slot = 0; slot < mRecords.GetSize(); ++slot)
    {
        if (mRecords[slot].IsUsed == true)
        {
            mRecords[slot].IsOwned = false;
            mRecords[slot].References = 0;
            dropIfUnused(device, slot);
        }
    }
}
void UniformSetTable::dropIfUnused(rhi::DeviceHandle device, usize slot) noexcept
{
    Record& record = mRecords[slot];
    if (record.IsOwned == true || record.References != 0)
    {
        return;
    }
    static_cast<void>(rhi::Destroy(device, record.Bindings));
    static_cast<void>(rhi::Destroy(device, record.Buffer));
    const uint64 generation = record.Generation + 1;
    record = {};
    record.Generation = generation;
}
} // namespace ludus::graphics::renderer::internal
