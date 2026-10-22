#include "internal/gpu_preview_camera.h"

#include "internal/gpu_preview_data.h"
#include "internal/reference_data.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <cstring>
#include <new>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
struct GpuPreviewCameraUpdate::State final
{
    rhi::DeviceHandle Device{};
    GpuPreviewUniform Uniform{};
    rhi::BufferHandle Parameters{};
    rhi::BindingSetHandle Bindings{};
    rhi::BindingLayoutHandle Layout{};
    StaticArray<rhi::RasterBindingResource,
                LUDUS_GRAPH_VISIBILITY_BINDING_COUNT + content::render_graph_format::MAX_STORAGE_BUFFERS>
        Resources{};
    usize BindingCount = 0;
    bool HasBindings = false;
    ~State() noexcept
    {
        static_cast<void>(rhi::Destroy(Device, Bindings));
        static_cast<void>(rhi::Destroy(Device, Parameters));
    }
};
GpuPreviewCameraUpdate::GpuPreviewCameraUpdate() noexcept = default;
GpuPreviewCameraUpdate::~GpuPreviewCameraUpdate() noexcept = default;
Status GpuPreviewCameraUpdate::Prepare(rhi::DeviceHandle device,
                                       const GpuPreviewUniform& uniform,
                                       const ViewDescription& camera,
                                       uint64 revision,
                                       rhi::BindingLayoutHandle layout,
                                       BorrowedView<const rhi::RasterBindingResource> bindings) noexcept
{
    if (camera.Depth != DepthConvention::Conventional || camera.InfiniteFar == true || bindings.IsValid() == false ||
        bindings.GetSize() < LUDUS_GPU_PREVIEW_BINDING_COUNT ||
        bindings.GetSize() > LUDUS_GRAPH_VISIBILITY_BINDING_COUNT + content::render_graph_format::MAX_STORAGE_BUFFERS)
    {
        return Status::InvalidDescription;
    }
    const uint32 epoch = uniform.Sampling[2] >> LUDUS_GPU_PREVIEW_HISTORY_SHIFT;
    if (epoch == LUDUS_GPU_PREVIEW_MAX_HISTORY_EPOCH)
    {
        return Status::IdentityExhausted; // A full extraction can restart the bounded history identity.
    }
    ReferenceJob validation;
    validation.IsCpuOnly = true;
    validation.Description.WorldToClip = camera.WorldToClip;
    validation.Description.CameraPosition = camera.CameraPosition;
    validation.Description.OrthographicCamera = camera.OrthographicCamera;
    if (PrepareReferenceCamera(validation) != Status::Ready)
    {
        return Status::InvalidDescription;
    }
    math::RandomKey key;
    if (math::TryMakeRandomKey(revision, REFERENCE_RANDOM_DOMAIN, key) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    candidate->Uniform = uniform;
    candidate->Uniform.ClipToWorld = validation.ClipToWorld;
    candidate->Uniform.CameraPositionWorld = {camera.CameraPosition.X,
                                              camera.CameraPosition.Y,
                                              camera.CameraPosition.Z,
                                              camera.OrthographicCamera == true ? 1.F : 0.F};
    candidate->Uniform.Sampling[0] = static_cast<uint32>(key.Value);
    candidate->Uniform.Sampling[1] = static_cast<uint32>(key.Value >> LUDUS_GRAPH_SOURCE_HIGH_SHIFT);
    candidate->Uniform.Sampling[2] =
        (uniform.Sampling[2] & LUDUS_GPU_PREVIEW_FLAGS_MASK) | ((epoch + 1) << LUDUS_GPU_PREVIEW_HISTORY_SHIFT);
    candidate->Uniform.BoundsMinimumWorld.W = 0;
    candidate->Uniform.BoundsMaximumWorld.W = 0;
    // Source generation is exposed as raw uint bits to scene callbacks.
    const uint32 generationLow = static_cast<uint32>(revision);
    const uint32 generationHigh = static_cast<uint32>(revision >> LUDUS_GRAPH_SOURCE_HIGH_SHIFT);
    std::memcpy(&candidate->Uniform.BoundsMinimumWorld.W, &generationLow, sizeof(generationLow));
    std::memcpy(&candidate->Uniform.BoundsMaximumWorld.W, &generationHigh, sizeof(generationHigh));
    candidate->Layout = layout;
    candidate->BindingCount = bindings.GetSize();
    for (usize i = 0; i < bindings.GetSize(); ++i)
    {
        candidate->Resources[i] = bindings[i];
    }
    const Status created =
        rhi::CreateBuffer(device,
                          {rhi::BufferRole::Uniform, sizeof(GpuPreviewUniform)},
                          {reinterpret_cast<const uint8*>(&candidate->Uniform), sizeof(GpuPreviewUniform)},
                          candidate->Parameters);
    if (created != Status::Ready && created != Status::Pending)
    {
        return created;
    }
    candidate->Resources[0].Buffer = candidate->Parameters;
    mState.Swap(candidate);
    return Poll();
}
Status GpuPreviewCameraUpdate::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    Status status = rhi::GetStatus(state.Device, state.Parameters);
    if (status == Status::Ready && state.HasBindings == false)
    {
        status = rhi::CreateBindingSet(state.Device,
                                       state.Layout,
                                       {state.Resources.GetData(), state.BindingCount},
                                       state.Bindings);
        state.HasBindings = status == Status::Ready || status == Status::Pending;
    }
    return status == Status::Ready ? rhi::GetStatus(state.Device, state.Bindings) : status;
}
void GpuPreviewCameraUpdate::Commit(GpuPreviewUniform& outUniform,
                                    rhi::BufferHandle& outParameters,
                                    rhi::BindingSetHandle& outBindings) noexcept
{
    LUDUS_REQUIRE(mState.Get() != nullptr && mState->HasBindings == true);
    outUniform = mState->Uniform;
    outParameters = mState->Parameters;
    outBindings = mState->Bindings;
    mState->Parameters = {};
    mState->Bindings = {};
    mState.Reset();
}
void GpuPreviewCameraUpdate::Reset() noexcept
{
    mState.Reset();
}
} // namespace ludus::graphics::renderer::internal
