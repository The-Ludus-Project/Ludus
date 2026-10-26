#pragma once
// Included only by raster.cpp. Ray resources use the same owner, incarnations,
// dependency graph and completion-gated collection as raster resources.
namespace ludus::graphics::rhi
{
namespace
{
/// Components in one ray geometry vertex position.
constexpr usize RAY_POSITION_COMPONENT_COUNT = 3;
constexpr usize RAY_TRIANGLE_CORNER_COUNT = 3;
constexpr usize RAY_OPACITY_WORD_SCALARS = 4;
constexpr usize RAY_TRIANGLE_UV_SCALARS = RAY_TRIANGLE_CORNER_COUNT * 2;
constexpr usize RAY_TRIANGLE_UV_PADDED_SCALARS = 8;
/// Ray visibility masks use the low eight bits.
constexpr uint32 MAX_RAY_INSTANCE_MASK = 255;
float32 RayScalar(const void* data, usize index) noexcept
{
    float32 value = 0;
    std::memcpy(&value, static_cast<const uint8*>(data) + index * sizeof(float32), sizeof(value));
    return value;
}
bool RayFinite(float32 value) noexcept
{
    return value == value && value < __builtin_huge_valf() && value > -__builtin_huge_valf();
}
bool RayPosition(float32 value) noexcept
{
    return RayFinite(value) && value >= -1000000 && value <= 1000000;
}
bool RayUnitInterval(float32 value) noexcept
{
    return RayFinite(value) && value >= 0 && value <= 1;
}
bool RayTransform(const float32* values) noexcept
{
    for (usize i = 0; i < 12; ++i)
    {
        if (RayPosition(values[i]) == false)
        {
            return false;
        }
    }
    const float64 determinant =
        float64(values[0]) * (float64(values[5]) * float64(values[10]) - float64(values[6]) * float64(values[9])) -
        float64(values[1]) * (float64(values[4]) * float64(values[10]) - float64(values[6]) * float64(values[8])) +
        float64(values[2]) * (float64(values[4]) * float64(values[9]) - float64(values[5]) * float64(values[8]));
    return determinant != 0;
}
bool RayRecord(Record& record) noexcept
{
    if (ReserveHostStorage(sizeof(RayRecordInfo)) == false)
    {
        return false;
    }
    record.AuxiliaryHostBytes = sizeof(RayRecordInfo);
    auto* allocation = new (std::nothrow) RayRecordInfo;
    if (allocation == nullptr)
    {
        StorageAllocationFailed();
    }
    record.Ray = core::UniquePtr<RayRecordInfo>(allocation);
    return static_cast<bool>(record.Ray);
}
template <typename T>
RasterStatus RayStatus(DeviceHandle device, T handle, RasterKind kind) noexcept
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
    const auto* record = Resolve(handle, kind);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
template <typename T>
RasterStatus RayDetach(DeviceHandle device, T& handle, RasterKind kind) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, kind);
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
template <typename T>
RasterStatus RayBytes(DeviceHandle device, T handle, RasterKind kind, RayAllocation& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* record = Resolve(handle, kind);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    output = record->Ray->Allocation;
    return RasterStatus::Ready;
}
bool ValidRay(const RayInput& ray) noexcept
{
    float64 lengthSquared = 0;
    for (usize axis = 0; axis < 3; ++axis)
    {
        if (RayPosition(ray.Origin[axis]) == false || RayFinite(ray.Direction[axis]) == false)
        {
            return false;
        }
        lengthSquared += float64(ray.Direction[axis]) * float64(ray.Direction[axis]);
    }
    return lengthSquared >= 0.99999 && lengthSquared <= 1.00001 && RayFinite(ray.MinDistance) &&
           RayFinite(ray.MaxDistance) && ray.MinDistance >= 0 && ray.MaxDistance >= ray.MinDistance && ray.Mask > 0 &&
           ray.Mask <= 255 && ray.VisibilityOnly <= 1 && ray.Reserved[0] == 0 && ray.Reserved[1] == 0;
}
} // namespace
RasterStatus GetRayCapabilities(DeviceHandle device, RayCapabilities& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission == RasterStatus::InvalidHandle)
    {
        return admission;
    }
    output = {};
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    output = backend::RayLimits();
    return output.StaticTriangles && output.InlineQueries ? RasterStatus::Ready : RasterStatus::Unsupported;
}
RasterStatus
CreateRayQueryProgram(DeviceHandle device, const ShaderDescription& description, RayQueryProgram& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (backend::RayLimits().InlineQueries == false)
    {
        return RasterStatus::Unsupported;
    }
    if (description.Stage != ShaderStage::Compute)
    {
        return RasterStatus::InvalidDescription;
    }
    return Publish(RasterKind::RayProgram, output, [&description](usize slot, Record&) noexcept {
        return backend::RayCreateProgram(slot, description);
    });
}
RasterStatus
CreateRayGeometry(DeviceHandle device, const RayGeometryDescription& description, RayGeometryHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto limits = backend::RayLimits();
    if (limits.StaticTriangles == false)
    {
        return RasterStatus::Unsupported;
    }
    const auto& d = description;
    if (d.Positions == nullptr || d.Indices == nullptr || d.VertexCount == 0 || d.IndexCount == 0 ||
        d.IndexCount % RAY_TRIANGLE_CORNER_COUNT != 0 ||
        d.OpaqueTriangleCount > d.IndexCount / RAY_TRIANGLE_CORNER_COUNT ||
        d.VertexCount > limits.MaxBufferBytes / (RAY_POSITION_COMPONENT_COUNT * sizeof(float32)) ||
        d.IndexCount > limits.MaxBufferBytes / (2 * sizeof(float32)) || d.ByteBudget == 0)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < d.VertexCount; ++i)
    {
        for (usize axis = 0; axis < RAY_POSITION_COMPONENT_COUNT; ++axis)
        {
            if (RayPosition(RayScalar(d.Positions, i * RAY_POSITION_COMPONENT_COUNT + axis)) == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        if (d.Texcoords != nullptr && (RayPosition(RayScalar(d.Texcoords, i * 2)) == false ||
                                       RayPosition(RayScalar(d.Texcoords, i * 2 + 1)) == false))
        {
            return RasterStatus::InvalidDescription;
        }
    }
    for (usize i = 0; i < d.IndexCount; i += 3)
    {
        for (usize corner = 0; corner < 3; ++corner)
        {
            if (d.Indices[i + corner] >= d.VertexCount)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        float64 e1[3]{}, e2[3]{};
        for (usize axis = 0; axis < 3; ++axis)
        {
            const auto base = float64(RayScalar(d.Positions, usize(d.Indices[i]) * 3 + axis));
            e1[axis] = float64(RayScalar(d.Positions, usize(d.Indices[i + 1]) * 3 + axis)) - base;
            e2[axis] = float64(RayScalar(d.Positions, usize(d.Indices[i + 2]) * 3 + axis)) - base;
        }
        const float64 cross[3]{e1[1] * e2[2] - e1[2] * e2[1],
                               e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0]};
        if (cross[0] == 0 && cross[1] == 0 && cross[2] == 0)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    return Publish(RasterKind::RayGeometry, output, [&d](usize slot, Record& record) noexcept {
        if (RayRecord(record) == false || record.Ray->TriangleUvs.TryResize(d.IndexCount * 2) == false)
        {
            return StorageFailure();
        }
        record.Ray->Count = d.IndexCount / RAY_TRIANGLE_CORNER_COUNT;
        record.Ray->OpaqueTriangleCount = d.OpaqueTriangleCount;
        auto& uv = record.Ray->TriangleUvs;
        for (usize i = 0; i < d.IndexCount; ++i)
        {
            uv[i * 2] = d.Texcoords == nullptr ? 0 : RayScalar(d.Texcoords, usize(d.Indices[i]) * 2);
            uv[i * 2 + 1] = d.Texcoords == nullptr ? 0 : RayScalar(d.Texcoords, usize(d.Indices[i]) * 2 + 1);
        }
        return backend::RayCreateGeometry(slot, d, record.Ray->Allocation);
    });
}
RasterStatus
CreateRayScene(DeviceHandle device, const RaySceneDescription& description, RaySceneHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto limits = backend::RayLimits();
    if (limits.StaticTriangles == false)
    {
        return RasterStatus::Unsupported;
    }
    if ((description.Instances == nullptr && description.Count != 0) || description.Count > limits.MaxInstances ||
        description.ByteBudget == 0)
    {
        return RasterStatus::InvalidDescription;
    }
    Collect();
    StorageArray<NativeRayInstance> instances;
    StorageArray<float32> opacity;
    const usize maxScalars =
        (description.ByteBudget < limits.MaxBufferBytes ? description.ByteBudget : limits.MaxBufferBytes) /
        sizeof(float32);
    constexpr usize EMPTY_SCENE_OPACITY_SCALARS = 4;
    constexpr usize INSTANCE_OPACITY_SCALARS = 12;
    if ((description.Count == 0 ? EMPTY_SCENE_OPACITY_SCALARS : description.Count * INSTANCE_OPACITY_SCALARS) >
        maxScalars)
    {
        return RasterStatus::CapacityExceeded;
    }
    const usize opacityScalars =
        description.Count == 0 ? EMPTY_SCENE_OPACITY_SCALARS : description.Count * INSTANCE_OPACITY_SCALARS;
    if (instances.TryResize(description.Count) == false || opacity.TryResize(opacityScalars) == false)
    {
        return StorageFailure();
    }
    StorageArray<usize> uvOffsets;
    if (uvOffsets.TryResize(gRecords[static_cast<usize>(RasterKind::RayGeometry)].GetSize()) == false)
    {
        return StorageFailure();
    }
    for (usize i = 0; i < description.Count; ++i)
    {
        const auto& input = description.Instances[i];
        auto* geometry = Resolve(input.Geometry, RasterKind::RayGeometry);
        if (geometry == nullptr)
        {
            return RasterStatus::InvalidHandle;
        }
        if (geometry->Status != RasterStatus::Ready)
        {
            return geometry->Status == RasterStatus::Pending ? RasterStatus::NotReady : geometry->Status;
        }
        if (RayTransform(input.Transform) == false || input.Mask == 0 || input.Mask > MAX_RAY_INSTANCE_MASK ||
            RayUnitInterval(input.Opacity) == false || RayUnitInterval(input.AlphaCutoff) == false)
        {
            return RasterStatus::InvalidDescription;
        }
        for (auto value : input.UvTransform)
        {
            if (RayPosition(value) == false)
            {
                return RasterStatus::InvalidDescription;
            }
        }
        if ((input.Alpha == nullptr) != (input.Width == 0 && input.Height == 0) ||
            (input.Alpha != nullptr &&
             (input.Width == 0 || input.Height == 0 || input.Width > 16384 || input.Height > 16384)))
        {
            return RasterStatus::InvalidDescription;
        }
        auto& native = instances[i];
        native.Geometry = RasterAccess::Slot(input.Geometry);
        native.Mask = input.Mask;
        native.IsOpaque = input.AlphaMasked == false;
        for (usize component = 0; component < 12; ++component)
        {
            native.Transform[component] = input.Transform[component];
        }
        if (uvOffsets[native.Geometry] == 0)
        {
            const auto& uv = geometry->Ray->TriangleUvs;
            const usize appended = geometry->Ray->Count * RAY_TRIANGLE_UV_PADDED_SCALARS + RAY_OPACITY_WORD_SCALARS;
            const auto begin = opacity.GetSize();
            if (appended > maxScalars - begin)
            {
                return RasterStatus::CapacityExceeded;
            }
            if (opacity.TryResize(begin + appended) == false)
            {
                return StorageFailure();
            }
            // The UV pointer remains valid for existing query shaders. The preceding
            // float4 carries geometry-range identity for the matching partition-aware shader.
            std::memcpy(&opacity[begin], &geometry->Ray->OpaqueTriangleCount, sizeof(uint32));
            uvOffsets[native.Geometry] = begin / RAY_OPACITY_WORD_SCALARS + 1;
            for (usize triangle = 0; triangle < geometry->Ray->Count; ++triangle)
            {
                for (usize component = 0; component < RAY_TRIANGLE_UV_SCALARS; ++component)
                {
                    opacity[begin + RAY_OPACITY_WORD_SCALARS + triangle * RAY_TRIANGLE_UV_PADDED_SCALARS + component] =
                        uv[triangle * RAY_TRIANGLE_UV_SCALARS + component];
                }
            }
        }
        const auto alphaOffset = opacity.GetSize();
        const usize texels = usize(input.Width) * input.Height;
        const usize padded = (texels + 3) / 4 * 4;
        if (padded > maxScalars - alphaOffset)
        {
            return RasterStatus::CapacityExceeded;
        }
        if (opacity.TryResize(alphaOffset + padded) == false)
        {
            return StorageFailure();
        }
        for (usize texel = 0; texel < texels; ++texel)
        {
            if (RayUnitInterval(input.Alpha[texel]) == false)
            {
                return RasterStatus::InvalidDescription;
            }
            opacity[alphaOffset + texel] = input.Alpha[texel];
        }
        const auto base = i * 12;
        opacity[base] = static_cast<float32>(uvOffsets[native.Geometry]);
        opacity[base + 1] = static_cast<float32>(alphaOffset);
        opacity[base + 2] = static_cast<float32>(input.Width);
        opacity[base + 3] = static_cast<float32>(input.Height);
        for (usize component = 0; component < 4; ++component)
        {
            opacity[base + 4 + component] = input.UvTransform[component];
        }
        opacity[base + 8] = input.Opacity;
        opacity[base + 9] = input.AlphaCutoff;
        opacity[base + 10] = input.AlphaMasked ? 1 : 0;
        opacity[base + 11] = static_cast<float32>((input.RepeatU ? 1U : 0U) | (input.RepeatV ? 2U : 0U) |
                                                  (input.LinearFilter ? 4U : 0U));
    }
    return Publish(RasterKind::RayScene,
                   output,
                   [&description, &instances, &opacity](usize slot, Record& record) noexcept {
                       if (RayRecord(record) == false ||
                           record.Ray->GeometrySlots.TryResize(description.Count) == false)
                       {
                           return StorageFailure();
                       }
                       record.Ray->Count = description.Count;
                       for (usize i = 0; i < description.Count; ++i)
                       {
                           record.Ray->GeometrySlots[i] = instances[i].Geometry;
                       }
                       const internal::NativeRaySceneDescription native
                       {
                           .Instances = instances.GetData(),
                           .Count = instances.GetSize(),
                           .Opacity = opacity.GetData(),
                           .ScalarCount = opacity.GetSize(),
                           .ByteBudget = description.ByteBudget,
                       };
                       return backend::RayCreateScene(slot, native, record.Ray->Allocation);
                   });
}
RasterStatus SubmitRayQueries(DeviceHandle device,
                              RayQueryProgram program,
                              RaySceneHandle scene,
                              const RayInput* rays,
                              usize count,
                              RayQueryBatch& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto limits = backend::RayLimits();
    if (limits.InlineQueries == false)
    {
        return RasterStatus::Unsupported;
    }
    if (rays == nullptr || count == 0 || count > limits.MaxRays)
    {
        return RasterStatus::InvalidDescription;
    }
    Collect();
    auto* p = Resolve(program, RasterKind::RayProgram);
    auto* s = Resolve(scene, RasterKind::RayScene);
    if (p == nullptr || s == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (p->Status != RasterStatus::Ready || s->Status != RasterStatus::Ready)
    {
        return p->Status == RasterStatus::Pending || s->Status == RasterStatus::Pending ? RasterStatus::NotReady
                                                                                        : RasterStatus::Failed;
    }
    for (usize i = 0; i < count; ++i)
    {
        if (ValidRay(rays[i]) == false)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    return Publish(RasterKind::RayBatch, output, [&rays, count, program, scene](usize slot, Record& record) noexcept {
        if (RayRecord(record) == false)
        {
            return StorageFailure();
        }
        record.Ray->Scene = RasterAccess::Slot(scene);
        record.Ray->Program = RasterAccess::Slot(program);
        record.Ray->Count = count;
        const internal::NativeRaySubmission submission
        {
            .Program = record.Ray->Program,
            .Scene = record.Ray->Scene,
            .Rays = rays,
            .Count = count,
        };
        return backend::RaySubmit(slot, submission, record.Ray->Allocation);
    });
}
RasterStatus ReadRayResults(DeviceHandle device, RayQueryBatch batch, RayResult* output, usize count) noexcept
{
    const auto status = RayStatus(device, batch, RasterKind::RayBatch);
    if (status != RasterStatus::Ready)
    {
        return status == RasterStatus::Pending ? RasterStatus::NotReady : status;
    }
    const auto* record = Resolve(batch, RasterKind::RayBatch);
    if (output == nullptr || count != record->Ray->Count)
    {
        return RasterStatus::InvalidDescription;
    }
    return backend::RayRead(RasterAccess::Slot(batch), output, count);
}
RasterStatus GetStatus(DeviceHandle d, RayGeometryHandle h) noexcept
{
    return RayStatus(d, h, RasterKind::RayGeometry);
}
RasterStatus GetStatus(DeviceHandle d, RaySceneHandle h) noexcept
{
    return RayStatus(d, h, RasterKind::RayScene);
}
RasterStatus GetStatus(DeviceHandle d, RayQueryProgram h) noexcept
{
    return RayStatus(d, h, RasterKind::RayProgram);
}
RasterStatus GetStatus(DeviceHandle d, RayQueryBatch h) noexcept
{
    return RayStatus(d, h, RasterKind::RayBatch);
}
RasterStatus Destroy(DeviceHandle d, RayGeometryHandle& h) noexcept
{
    return RayDetach(d, h, RasterKind::RayGeometry);
}
RasterStatus Destroy(DeviceHandle d, RaySceneHandle& h) noexcept
{
    return RayDetach(d, h, RasterKind::RayScene);
}
RasterStatus Destroy(DeviceHandle d, RayQueryProgram& h) noexcept
{
    return RayDetach(d, h, RasterKind::RayProgram);
}
RasterStatus Destroy(DeviceHandle d, RayQueryBatch& h) noexcept
{
    return RayDetach(d, h, RasterKind::RayBatch);
}
RasterStatus GetRayAllocation(DeviceHandle d, RayGeometryHandle h, RayAllocation& o) noexcept
{
    return RayBytes(d, h, RasterKind::RayGeometry, o);
}
RasterStatus GetRayAllocation(DeviceHandle d, RaySceneHandle h, RayAllocation& o) noexcept
{
    return RayBytes(d, h, RasterKind::RayScene, o);
}
RasterStatus GetRayAllocation(DeviceHandle d, RayQueryBatch h, RayAllocation& o) noexcept
{
    return RayBytes(d, h, RasterKind::RayBatch, o);
}
} // namespace ludus::graphics::rhi

namespace ludus::graphics::rhi
{
RasterStatus GetRayResourceStats(DeviceHandle device, RayResourceStats& output) noexcept
{
    const auto status = Admission(device, false);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (backend::RayLimits().InlineQueries == false)
    {
        return RasterStatus::Unsupported;
    }
    Collect();
    RayResourceStats result;
    for (usize kind = RASTER_KINDS; kind < KINDS; ++kind)
    {
        const auto& pool = gRecords[kind];
        for (usize slot = 0; slot < pool.GetSize(); ++slot)
        {
            const auto& record = pool[slot];
            if (record.Owner == 0 || record.Ray.Get() == nullptr)
            {
                continue;
            }
            const auto bytes = record.Ray->Allocation.ResidentBytes;
            if (bytes > ~usize{0} - result.ResidentBytes)
            {
                return RasterStatus::CapacityExceeded;
            }
            result.ResidentBytes += bytes;
            if (record.Published == false)
            {
                result.DetachedBytes += bytes;
            }
            if (record.Status == RasterStatus::Pending)
            {
                const auto scratch = record.Ray->Allocation.ScratchBytes;
                if (scratch > ~usize{0} - result.BuildScratchBytes)
                {
                    return RasterStatus::CapacityExceeded;
                }
                result.BuildScratchBytes += scratch;
            }
            if (kind == static_cast<usize>(RasterKind::RayGeometry))
            {
                ++result.Geometries;
            }
            else if (kind == static_cast<usize>(RasterKind::RayScene))
            {
                ++result.Scenes;
            }
            else if (kind == static_cast<usize>(RasterKind::RayBatch))
            {
                ++result.Batches;
            }
        }
    }
    output = result;
    return RasterStatus::Ready;
}
} // namespace ludus::graphics::rhi
