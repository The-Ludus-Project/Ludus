#include "internal/d3d12_device.h"
#include "internal/d3d12_diagnostics.h"
#include "internal/d3d12_raster.h"
#include "internal/gpu_pass_timing_buffer.h"
#include "internal/gpu_scope_backend.h"
#include "internal/measurement.h"
#include "internal/owner.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>

// Thanks to Microsoft, "Timing (Direct3D 12 Graphics)", for the direct-queue
// bottom-of-pipe timestamp pairs, 64-bit resolve data and queue frequency:
// https://learn.microsoft.com/windows/win32/direct3d12/timing
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
using namespace d3d12;

constexpr uint32 TIMESTAMPS_PER_INTERVAL = 2;
constexpr uint32 FRAME_QUERY_COUNT = TIMESTAMPS_PER_INTERVAL;
constexpr uint64 UNKNOWN_SUBMISSION_FENCE = ~uint64{0};
constexpr uint32 PASS_QUERY_COUNT = static_cast<uint32>(MAX_GPU_TIMED_PASSES) * TIMESTAMPS_PER_INTERVAL;

// Each probe owns one heap and one readback buffer for the session. An armed
// probe records into them once; no later frame writes them until the fence
// completes and the result is copied. Frame and pass probes can overlap.
struct QueryStorage final
{
    ComOwner<ID3D12QueryHeap> Heap;
    ComOwner<ID3D12Resource> Readback;
    ComOwner<ID3D12Fence> CompletionFence;
    uint64 Frequency = 0;
    uint64 StartNanoseconds = 0;
    uint64 Fence = 0;
};

/// Frame and pass timestamp probes of one device owner (R14c).
struct MeasurementD3D12State final
{
    QueryStorage FrameQueries;
    QueryStorage PassQueries;
    GpuTiming FrameTiming;
    internal::GpuPassTimingBuffer PassTimings;
    usize ActivePass = MAX_GPU_TIMED_PASSES;
    /// Untimed logical-pass events open inside the current raster pass.
    uint32 PassLabelDepth = 0;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<MeasurementD3D12State, MAX_DEVICE_OWNERS> gMeasurementD3D12States{};
/// MeasurementD3D12State of the bound owner slot.
MeasurementD3D12State& GetMeasurementD3D12() noexcept
{
    return gMeasurementD3D12States[internal::GetBoundOwnerSlot()];
}

RasterStatus GetQueryFailure(HRESULT result) noexcept
{
    if (CheckDeviceRemoval() == true)
    {
        return RasterStatus::DeviceLost;
    }
    return result == E_OUTOFMEMORY ? RasterStatus::OutOfMemory : RasterStatus::Failed;
}

bool CanReleaseQueries(const QueryStorage& storage) noexcept
{
    return storage.Fence == 0 || storage.CompletionFence->GetCompletedValue() >= storage.Fence;
}

RasterStatus PrepareQueries(uint32 count, QueryStorage& inoutStorage) noexcept
{
    ID3D12Fence* fence = GetFenceOrNull();
    LUDUS_ASSERT(fence != nullptr);
    if (inoutStorage.Heap.Get() != nullptr && inoutStorage.CompletionFence.Get() != fence)
    {
        // A timed-out device may still be using this probe's bounded storage.
        // A new session can render and retry arming once the old fence retires.
        if (CanReleaseQueries(inoutStorage) == false)
        {
            return RasterStatus::Failed;
        }
        inoutStorage = {};
    }
    uint64 frequency = 0;
    const HRESULT frequencyResult = GetQueueOrNull()->GetTimestampFrequency(&frequency);
    if (FAILED(frequencyResult) == true || frequency == 0)
    {
        if (CheckDeviceRemoval() == true)
        {
            return RasterStatus::DeviceLost;
        }
        return frequencyResult == DXGI_ERROR_UNSUPPORTED || SUCCEEDED(frequencyResult) == true
                   ? RasterStatus::Unsupported
                   : RasterStatus::Failed;
    }
    if (inoutStorage.Heap.Get() == nullptr)
    {
        ID3D12Device* device = GetDeviceOrNull();
        const D3D12_QUERY_HEAP_DESC query{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = count };
        ID3D12QueryHeap* heapOrNull = nullptr;
        const HRESULT queryResult =
            device->CreateQueryHeap(&query, IID_ID3D12QueryHeap, GetInterfaceOutput(heapOrNull));
        if (FAILED(queryResult) == true)
        {
            return GetQueryFailure(queryResult);
        }
        ComOwner<ID3D12QueryHeap> heap(heapOrNull);
        const D3D12_HEAP_PROPERTIES properties{ .Type = D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = count * sizeof(uint64);
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* resourceOrNull = nullptr;
        const HRESULT bufferResult = device->CreateCommittedResource(&properties,
                                                                     D3D12_HEAP_FLAG_NONE,
                                                                     &description,
                                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                                     nullptr,
                                                                     GetInterfaceId(resourceOrNull),
                                                                     GetInterfaceOutput(resourceOrNull));
        if (FAILED(bufferResult) == true)
        {
            return GetQueryFailure(bufferResult);
        }
        inoutStorage.Heap = Move(heap);
        inoutStorage.Readback = ComOwner<ID3D12Resource>(resourceOrNull);
        fence->AddRef();
        inoutStorage.CompletionFence = ComOwner<ID3D12Fence>(fence);
    }
    inoutStorage.Frequency = frequency;
    inoutStorage.Fence = 0;
    return RasterStatus::Ready;
}

void WriteTimestamp(ID3D12GraphicsCommandList& inoutCommands, const QueryStorage& storage, uint32 index) noexcept
{
    inoutCommands.EndQuery(storage.Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void ResolveTimestamps(ID3D12GraphicsCommandList& inoutCommands, const QueryStorage& storage, uint32 count) noexcept
{
    inoutCommands.ResolveQueryData(storage.Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, storage.Readback.Get(), 0);
}

GpuTimingState ReadTimestamps(const QueryStorage& storage, BorrowedView<uint64> output) noexcept
{
    if (CheckDeviceRemoval() == true)
    {
        return GpuTimingState::Failed;
    }
    // GetCompletedFence detects the device-removed sentinel and may finalize
    // the session. In that case do not touch the released query resources.
    const uint64 fence = storage.Fence;
    if (GetCompletedFence() < fence)
    {
        return GetDeviceOrNull() == nullptr ? GpuTimingState::Failed : GpuTimingState::Pending;
    }
    const D3D12_RANGE read{ .Begin = 0, .End = output.GetSize() * sizeof(uint64) };
    void* dataOrNull = nullptr;
    const HRESULT result = storage.Readback->Map(0, &read, &dataOrNull);
    if (FAILED(result) == true)
    {
        static_cast<void>(CheckDeviceRemoval());
        return GpuTimingState::Failed;
    }
    const uint64* data = static_cast<const uint64*>(dataOrNull);
    for (usize index = 0; index < output.GetSize(); ++index)
    {
        output[index] = data[index];
    }
    const D3D12_RANGE written{};
    storage.Readback->Unmap(0, &written);
    return GpuTimingState::Available;
}

GpuTimingState ValidateFrequency(const QueryStorage& storage) noexcept
{
    uint64 frequency = 0;
    const HRESULT result = GetQueueOrNull()->GetTimestampFrequency(&frequency);
    if (FAILED(result) == true)
    {
        return GpuTimingState::Failed;
    }
    return frequency == storage.Frequency ? GpuTimingState::Available : GpuTimingState::Disjoint;
}

GpuTiming MeasureInterval(const QueryStorage& storage, uint64 start, uint64 end, uint64 now) noexcept
{
    constexpr float64 NANOSECONDS_PER_SECOND = internal::MAX_TIMESTAMP_TICK_NANOSECONDS;
    const float64 tick = NANOSECONDS_PER_SECOND / static_cast<float64>(storage.Frequency);
    constexpr uint32 TIMESTAMP_BITS = internal::MAX_TIMESTAMP_COUNTER_BITS;
    float64 duration = 0;
    if (now < storage.StartNanoseconds || end < start ||
        internal::TimestampDuration(start, end, {TIMESTAMP_BITS, tick}, now - storage.StartNanoseconds, duration) ==
            false)
    {
        return { .State = GpuTimingState::Disjoint };
    }
    return
    {
        .State = GpuTimingState::Available,
        .DurationNanoseconds = duration,
        .TickNanoseconds = tick,
        .ValidBits = TIMESTAMP_BITS,
    };
}
} // namespace

RasterStatus TimingArm() noexcept
{
    const GpuTimingState state = TimingPoll().State;
    if (state == GpuTimingState::Armed || state == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    if (GetDeviceOrNull() == nullptr)
    {
        return RasterStatus::DeviceLost;
    }
    const RasterStatus status = PrepareQueries(FRAME_QUERY_COUNT, GetMeasurementD3D12().FrameQueries);
    if (status == RasterStatus::Ready)
    {
        GetMeasurementD3D12().FrameTiming = { .State = GpuTimingState::Armed };
    }
    else if (status == RasterStatus::Unsupported)
    {
        GetMeasurementD3D12().FrameTiming = { .State = GpuTimingState::Unavailable };
    }
    return status;
}

GpuTiming TimingPoll() noexcept
{
    if (GetMeasurementD3D12().FrameTiming.State == GpuTimingState::Pending)
    {
        StaticArray<uint64, FRAME_QUERY_COUNT> timestamps;
        const GpuTimingState state =
            ReadTimestamps(GetMeasurementD3D12().FrameQueries, {timestamps.GetData(), timestamps.GetSize()});
        if (state == GpuTimingState::Available)
        {
            const GpuTimingState clock = ValidateFrequency(GetMeasurementD3D12().FrameQueries);
            GetMeasurementD3D12().FrameTiming = clock == GpuTimingState::Available
                                                    ? MeasureInterval(GetMeasurementD3D12().FrameQueries,
                                                                      timestamps[0],
                                                                      timestamps[1],
                                                                      time::NowTicks())
                                                    : GpuTiming{ .State = clock };
            static_cast<void>(CheckDeviceRemoval());
        }
        else if (state == GpuTimingState::Failed && GetDeviceOrNull() != nullptr)
        {
            GetMeasurementD3D12().FrameTiming = { .State = state };
        }
    }
    return GetMeasurementD3D12().FrameTiming;
}

void TimingReset() noexcept
{
    GetMeasurementD3D12().FrameTiming = {};
}

RasterStatus ArmPassTimings() noexcept
{
    static_cast<void>(PollPassTimings());
    if (GetDeviceOrNull() == nullptr)
    {
        return RasterStatus::DeviceLost;
    }
    if (GetMeasurementD3D12().PassTimings.CanArm() == false)
    {
        return RasterStatus::InvalidState;
    }
    const RasterStatus status = PrepareQueries(PASS_QUERY_COUNT, GetMeasurementD3D12().PassQueries);
    if (status == RasterStatus::Ready)
    {
        GetMeasurementD3D12().PassTimings.Arm();
    }
    else if (status == RasterStatus::Unsupported)
    {
        GetMeasurementD3D12().PassTimings.GetReport() = { .State = GpuTimingState::Unavailable };
    }
    return status;
}

GpuPassTimings PollPassTimings() noexcept
{
    GpuPassTimings& report = GetMeasurementD3D12().PassTimings.GetReport();
    if (report.State != GpuTimingState::Pending)
    {
        return report;
    }
    StaticArray<uint64, PASS_QUERY_COUNT> timestamps;
    const GpuTimingState state = ReadTimestamps(GetMeasurementD3D12().PassQueries,
                                                {timestamps.GetData(), report.Count * TIMESTAMPS_PER_INTERVAL});
    if (state == GpuTimingState::Pending || GetDeviceOrNull() == nullptr)
    {
        return report;
    }
    const GpuTimingState clock =
        state == GpuTimingState::Available ? ValidateFrequency(GetMeasurementD3D12().PassQueries) : state;
    report.State = clock == GpuTimingState::Failed ? GpuTimingState::Failed : GpuTimingState::Available;
    const uint64 now = time::NowTicks();
    for (usize index = 0; index < report.Count; ++index)
    {
        const usize first = index * TIMESTAMPS_PER_INTERVAL;
        report.Passes[index].Timing =
            clock == GpuTimingState::Available
                ? MeasureInterval(GetMeasurementD3D12().PassQueries, timestamps[first], timestamps[first + 1], now)
                : GpuTiming{ .State = clock };
    }
    static_cast<void>(CheckDeviceRemoval());
    return report;
}

RasterStatus BeginScope(const GpuScopeDescription& scope) noexcept
{
    ID3D12GraphicsCommandList* commandsOrNull = GetCommandsOrNull();
    if (commandsOrNull == nullptr)
    {
        return RasterStatus::InvalidState;
    }
    if (GetMeasurementD3D12().PassTimings.BeginScope(scope) == false)
    {
        return RasterStatus::CapacityExceeded;
    }
    BeginPixEvent(*commandsOrNull, scope.Name.GetData());
    return RasterStatus::Ready;
}

RasterStatus EndScope() noexcept
{
    ID3D12GraphicsCommandList* commandsOrNull = GetCommandsOrNull();
    if (commandsOrNull == nullptr || GetMeasurementD3D12().PassTimings.EndScope() == false)
    {
        return RasterStatus::InvalidState;
    }
    EndPixEvent(*commandsOrNull);
    return RasterStatus::Ready;
}

internal::GpuScopePosition GetScopePosition() noexcept
{
    return GetMeasurementD3D12().PassTimings.GetPosition();
}

void BeginPassLabel(const char* name) noexcept
{
    ID3D12GraphicsCommandList* commandsOrNull = GetCommandsOrNull();
    if (commandsOrNull != nullptr)
    {
        BeginPixEvent(*commandsOrNull, name);
        ++GetMeasurementD3D12().PassLabelDepth;
    }
}

void EndPassLabel() noexcept
{
    ID3D12GraphicsCommandList* commandsOrNull = GetCommandsOrNull();
    if (GetMeasurementD3D12().PassLabelDepth != 0 && commandsOrNull != nullptr)
    {
        --GetMeasurementD3D12().PassLabelDepth;
        EndPixEvent(*commandsOrNull);
    }
}

void ResetPassTimings() noexcept
{
    GetMeasurementD3D12().PassTimings.Reset();
    GetMeasurementD3D12().ActivePass = MAX_GPU_TIMED_PASSES;
    GetMeasurementD3D12().PassLabelDepth = 0;
}

namespace d3d12
{
void BeginTimingFrame(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    BeginPixEvent(inoutCommands, "Ludus/Frame");
    GetMeasurementD3D12().PassLabelDepth = 0;
    GetMeasurementD3D12().PassTimings.BeginFrame();
    if (GetMeasurementD3D12().PassTimings.GetReport().State == GpuTimingState::Armed)
    {
        GetMeasurementD3D12().PassQueries.StartNanoseconds = time::NowTicks();
    }
    if (GetMeasurementD3D12().FrameTiming.State == GpuTimingState::Armed)
    {
        GetMeasurementD3D12().FrameQueries.StartNanoseconds = time::NowTicks();
        WriteTimestamp(inoutCommands, GetMeasurementD3D12().FrameQueries, 0);
    }
}

void ClosePassLabels(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    for (; GetMeasurementD3D12().PassLabelDepth != 0; --GetMeasurementD3D12().PassLabelDepth)
    {
        EndPixEvent(inoutCommands);
    }
}

void CloseTimingScopes(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    // Scopes never span submission: end every event an early return left open, then the frame.
    ClosePassLabels(inoutCommands);
    for (uint32 closed = GetMeasurementD3D12().PassTimings.CloseScopes(); closed != 0; --closed)
    {
        EndPixEvent(inoutCommands);
    }
    EndPixEvent(inoutCommands);
}

void EndTimingFrame(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    CloseTimingScopes(inoutCommands);
    if (GetMeasurementD3D12().FrameTiming.State == GpuTimingState::Armed)
    {
        WriteTimestamp(inoutCommands, GetMeasurementD3D12().FrameQueries, 1);
        ResolveTimestamps(inoutCommands, GetMeasurementD3D12().FrameQueries, FRAME_QUERY_COUNT);
    }
    const GpuPassTimings& report = GetMeasurementD3D12().PassTimings.GetReport();
    if (report.State == GpuTimingState::Armed && report.Count != 0)
    {
        ResolveTimestamps(inoutCommands,
                          GetMeasurementD3D12().PassQueries,
                          static_cast<uint32>(report.Count) * TIMESTAMPS_PER_INTERVAL);
    }
}

void MarkTimingsQueued() noexcept
{
    // ExecuteCommandLists can enqueue work even when the later Present or
    // Signal fails. A drain or the removed-device sentinel then proves safety.
    if (GetMeasurementD3D12().FrameTiming.State == GpuTimingState::Armed)
    {
        GetMeasurementD3D12().FrameQueries.Fence = UNKNOWN_SUBMISSION_FENCE;
    }
    if (GetMeasurementD3D12().PassTimings.GetReport().State == GpuTimingState::Armed &&
        GetMeasurementD3D12().PassTimings.GetReport().Count != 0)
    {
        GetMeasurementD3D12().PassQueries.Fence = UNKNOWN_SUBMISSION_FENCE;
    }
}

void SubmitTimings(uint64 fence) noexcept
{
    if (GetMeasurementD3D12().FrameTiming.State == GpuTimingState::Armed)
    {
        GetMeasurementD3D12().FrameQueries.Fence = fence;
        GetMeasurementD3D12().FrameTiming.State = GpuTimingState::Pending;
    }
    if (GetMeasurementD3D12().PassQueries.Fence == UNKNOWN_SUBMISSION_FENCE &&
        GetMeasurementD3D12().PassTimings.GetReport().State == GpuTimingState::Armed)
    {
        GetMeasurementD3D12().PassQueries.Fence = fence;
    }
    GetMeasurementD3D12().PassTimings.Submit();
}

void ReleaseTimings(bool isQueueDrained) noexcept
{
    TimingReset();
    ResetPassTimings();
    const auto release = [isQueueDrained](QueryStorage& inoutStorage) noexcept {
        if (CanReleaseQueries(inoutStorage) == true ||
            (isQueueDrained == true && inoutStorage.CompletionFence.Get() == GetFenceOrNull()))
        {
            inoutStorage = {};
        }
    };
    release(GetMeasurementD3D12().FrameQueries);
    release(GetMeasurementD3D12().PassQueries);
}

void BeginPhysicalPassTiming(ID3D12GraphicsCommandList& inoutCommands, GpuPassKind kind) noexcept
{
    GetMeasurementD3D12().ActivePass = GetMeasurementD3D12().PassTimings.AddPass(kind);
    if (GetMeasurementD3D12().ActivePass < MAX_GPU_TIMED_PASSES)
    {
        WriteTimestamp(inoutCommands,
                       GetMeasurementD3D12().PassQueries,
                       static_cast<uint32>(GetMeasurementD3D12().ActivePass) * TIMESTAMPS_PER_INTERVAL);
    }
}

void EndPhysicalPassTiming(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    if (GetMeasurementD3D12().ActivePass < MAX_GPU_TIMED_PASSES)
    {
        WriteTimestamp(inoutCommands,
                       GetMeasurementD3D12().PassQueries,
                       static_cast<uint32>(GetMeasurementD3D12().ActivePass) * TIMESTAMPS_PER_INTERVAL + 1);
        GetMeasurementD3D12().ActivePass = MAX_GPU_TIMED_PASSES;
    }
}
} // namespace d3d12
} // namespace ludus::graphics::rhi::backend
