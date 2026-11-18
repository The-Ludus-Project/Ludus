#pragma once
// Thanks to Khronos WebGL Working Group, EXT_disjoint_timer_query_webgl2, rev. 4:
// https://registry.khronos.org/webgl/extensions/EXT_disjoint_timer_query_webgl2/
// Reject every pending result when the extension reports GPU_DISJOINT_EXT.
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
{
namespace
{
/// @brief Frame timer-query sample state of one device owner (R14c).
struct MeasurementWebGlState final
{
    GpuTiming Timing;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<MeasurementWebGlState, MAX_DEVICE_OWNERS> gMeasurementWebGlStates{};
/// MeasurementWebGlState of the bound owner slot.
MeasurementWebGlState& GetMeasurementWebGl() noexcept
{
    return gMeasurementWebGlStates[internal::GetBoundOwnerSlot()];
}
// clang-format off
EM_JS(void, TimingGlAdapter, (char* name, uint32 capacity), {
    const gl = GL.currentContext?.GLctx;
    if (!gl) return;
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    stringToUTF8(ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER), name, capacity);
});
// R14c: each owner keeps its own query, keyed by its owner slot in Module.ludusTimings.
EM_JS(int32, TimingGlArm, (uint32 owner), {
    const gl = GL.currentContext?.GLctx;
    if (!gl) return 0;
    const ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    if (!ext) return 0;
    const bits = gl.getQuery(ext.TIME_ELAPSED_EXT, ext.QUERY_COUNTER_BITS_EXT);
    if (!bits) return 0;
    const query = gl.createQuery();
    if (!query) return -1;
    const timings = Module.ludusTimings || (Module.ludusTimings = []);
    if (timings[owner]) timings[owner].gl.deleteQuery(timings[owner].query);
    // Clear an earlier disjoint notification before beginning this sample.
    gl.getParameter(ext.GPU_DISJOINT_EXT);
    timings[owner] = {gl, ext, query, open:false}; return bits;
});
EM_JS(void, TimingGlBegin, (uint32 owner), {
    const t = Module.ludusTimings[owner]; t.gl.beginQuery(t.ext.TIME_ELAPSED_EXT, t.query); t.open=true;
});
EM_JS(void, TimingGlEnd, (uint32 owner), {
    const t = Module.ludusTimings[owner]; t.gl.endQuery(t.ext.TIME_ELAPSED_EXT); t.open=false;
});
EM_JS(int32, TimingGlPoll, (uint32 owner, foundation::float64* duration), {
    const t = Module.ludusTimings?.[owner];
    if (!t || t.gl.isContextLost()) return 3;
    if (t.gl.getParameter(t.ext.GPU_DISJOINT_EXT)) return 2;
    if (!t.gl.getQueryParameter(t.query, t.gl.QUERY_RESULT_AVAILABLE)) return 0;
    const value = t.gl.getQueryParameter(t.query, t.gl.QUERY_RESULT);
    if (!Number.isSafeInteger(value) || value < 0) return 2;
    HEAPF64[duration/8] = value; return 1;
});
EM_JS(void, TimingGlReset, (uint32 owner), {
    const t = Module.ludusTimings?.[owner];
    if (t) { if(t.open && !t.gl.isContextLost()) t.gl.endQuery(t.ext.TIME_ELAPSED_EXT); t.gl.deleteQuery(t.query); }
    if (t) delete Module.ludusTimings[owner];
});
// clang-format on
/// Key of the bound owner's timer query in Module.ludusTimings.
uint32 GetTimingOwner() noexcept
{
    return static_cast<uint32>(internal::GetBoundOwnerSlot());
}
} // namespace
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    MeasurementAdapter output;
    TimingGlAdapter(output.Name, sizeof(output.Name));
    return output;
}
void TimingReset() noexcept
{
    TimingGlReset(GetTimingOwner());
    GetMeasurementWebGl().Timing = {};
}
GpuTiming TimingPoll() noexcept
{
    if (GetMeasurementWebGl().Timing.State == GpuTimingState::Pending)
    {
        const int32 result = TimingGlPoll(GetTimingOwner(), &GetMeasurementWebGl().Timing.DurationNanoseconds);
        if (result == 1)
        {
            GetMeasurementWebGl().Timing.State = GpuTimingState::Available;
        }
        else if (result == 2)
        {
            GetMeasurementWebGl().Timing.State = GpuTimingState::Disjoint;
        }
        else if (result == 3)
        {
            GetMeasurementWebGl().Timing.State = GpuTimingState::Failed;
        }
    }
    return GetMeasurementWebGl().Timing;
}
RasterStatus TimingArm() noexcept
{
    const auto state = TimingPoll().State;
    if (state == GpuTimingState::Armed || state == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    const int32 result = TimingGlArm(GetTimingOwner());
    if (result < 0)
    {
        return RasterStatus::OutOfMemory;
    }
    GetMeasurementWebGl().Timing = { .State = result > 0 ? GpuTimingState::Armed : GpuTimingState::Unavailable };
    if (result > 0)
    {
        GetMeasurementWebGl().Timing.ValidBits = static_cast<uint32>(result);
    }
    return result > 0 ? RasterStatus::Ready : result == 0 ? RasterStatus::Unsupported : RasterStatus::OutOfMemory;
}
void TimingBegin() noexcept
{
    if (GetMeasurementWebGl().Timing.State == GpuTimingState::Armed)
    {
        TimingGlBegin(GetTimingOwner());
    }
}
void TimingEnd() noexcept
{
    if (GetMeasurementWebGl().Timing.State == GpuTimingState::Armed)
    {
        TimingGlEnd(GetTimingOwner());
        GetMeasurementWebGl().Timing.State = GpuTimingState::Pending;
    }
}
void TimingSubmitted() noexcept {}
bool RasterAllocationBytes(internal::RasterKind, usize, uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
