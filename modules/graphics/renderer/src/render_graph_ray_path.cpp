#include "internal/render_graph_ray_path.h"
#include "../shaders/renderer_gpu_preview_layout.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>

#include <cstring>

// Diagnostic ray path capture owner; the design and its limits are in
// docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed.
namespace ludus::graphics::renderer::internal
{
namespace
{
namespace format = content::render_graph_format;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
using Status = rhi::RasterStatus;

static_assert(LUDUS_RAY_PATH_CAPTURE_PASS_COUNT == format::MAX_RAY_PROGRAMS);
static_assert(((LUDUS_GRAPH_PROGRAM_COUNT - 1) << LUDUS_GRAPH_PASS_INDEX_SHIFT) <
              (1U << LUDUS_GPU_PREVIEW_HISTORY_SHIFT));
static_assert(LUDUS_RAY_PATH_CAPTURE_WORD_BYTES == sizeof(uint32));
static_assert(RAY_PATH_INVALID_INDEX == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE);
/// The first slice selects one pixel; the buffer layout already admits a rectangle.
constexpr uint32 SELECTED_PIXEL_EXTENT = 1;

usize GetSummaryCount(const RayPathCaptureDescription& description) noexcept
{
    return usize{SELECTED_PIXEL_EXTENT} * SELECTED_PIXEL_EXTENT * description.SampleCount *
           LUDUS_RAY_PATH_CAPTURE_PASS_COUNT;
}
usize GetRecordsWord(usize summaryCount) noexcept
{
    return LUDUS_RAY_PATH_HEADER_WORDS + summaryCount * LUDUS_RAY_PATH_SUMMARY_WORDS;
}
uint32 ReadWord(std::span<const uint8> bytes, usize word) noexcept
{
    uint32 value = 0;
    static_cast<void>(
        foundation::TryReadLittleEndian(bytes.subspan(word * LUDUS_RAY_PATH_CAPTURE_WORD_BYTES, sizeof(uint32)),
                                        value));
    return value;
}
foundation::float32 ReadFloat(std::span<const uint8> bytes, usize word) noexcept
{
    const uint32 bits = ReadWord(bytes, word);
    foundation::float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
foundation::math::Vector3 ReadVector(std::span<const uint8> bytes, usize word) noexcept
{
    return {ReadFloat(bytes, word), ReadFloat(bytes, word + 1), ReadFloat(bytes, word + 2)};
}
/// Order by pixel row, pixel column, sample and pass; events then by sequence.
bool IsBefore(const RayPathSummary& left, const RayPathSummary& right) noexcept
{
    if (left.PixelY != right.PixelY)
    {
        return left.PixelY < right.PixelY;
    }
    if (left.PixelX != right.PixelX)
    {
        return left.PixelX < right.PixelX;
    }
    if (left.Sample != right.Sample)
    {
        return left.Sample < right.Sample;
    }
    return left.Pass < right.Pass;
}
RayPathSummary GetKey(const RayPathEvent& event) noexcept
{
    RayPathSummary key;
    key.PixelX = event.PixelX;
    key.PixelY = event.PixelY;
    key.Sample = event.Sample;
    key.Pass = event.Pass;
    return key;
}
bool IsBefore(const RayPathEvent& left, const RayPathEvent& right) noexcept
{
    const RayPathSummary leftKey = GetKey(left);
    const RayPathSummary rightKey = GetKey(right);
    if (IsBefore(leftKey, rightKey) == true || IsBefore(rightKey, leftKey) == true)
    {
        return IsBefore(leftKey, rightKey);
    }
    return left.Sequence < right.Sequence;
}
bool IsSameInvocation(const RayPathEvent& event, const RayPathSummary& summary) noexcept
{
    return event.PixelX == summary.PixelX && event.PixelY == summary.PixelY && event.Sample == summary.Sample &&
           event.Pass == summary.Pass;
}
} // namespace

usize GetRayPathCaptureBytes(const RayPathCaptureDescription& description) noexcept
{
    const usize words =
        GetRecordsWord(GetSummaryCount(description)) + usize{description.RecordCapacity} * LUDUS_RAY_PATH_RECORD_WORDS;
    return words * LUDUS_RAY_PATH_CAPTURE_WORD_BYTES;
}
bool WriteRayPathCaptureHeader(const RayPathCaptureDescription& description, std::span<uint8> outBytes) noexcept
{
    if (outBytes.size() != GetRayPathCaptureBytes(description))
    {
        return false;
    }
    const foundation::StaticArray<uint32, LUDUS_RAY_PATH_HEADER_WORDS> header{{
        LUDUS_RAY_PATH_CAPTURE_VERSION,
        description.PixelX,
        description.PixelY,
        SELECTED_PIXEL_EXTENT,
        SELECTED_PIXEL_EXTENT,
        0,
        description.SampleCount,
        description.RecordCapacity,
        0,
        0,
        0,
        0,
    }};
    for (usize word = 0; word < header.GetSize(); ++word)
    {
        static_cast<void>(foundation::TryWriteLittleEndian(
            header[word],
            outBytes.subspan(word * LUDUS_RAY_PATH_CAPTURE_WORD_BYTES, sizeof(uint32))));
    }
    return true;
}
Status DecodeRayPathCapture(std::span<const uint8> bytes,
                            const RayPathPassTable& passes,
                            foundation::Array<RayPathEvent>& inoutEvents,
                            foundation::Array<RayPathSummary>& inoutSummaries,
                            RayPathCaptureReport& outReport) noexcept
{
    constexpr usize HEADER_BYTES = usize{LUDUS_RAY_PATH_HEADER_WORDS} * LUDUS_RAY_PATH_CAPTURE_WORD_BYTES;
    if (bytes.size() < HEADER_BYTES ||
        ReadWord(bytes, LUDUS_RAY_PATH_HEADER_VERSION_WORD) != LUDUS_RAY_PATH_CAPTURE_VERSION)
    {
        return Status::InvalidDescription;
    }
    RayPathCaptureDescription description;
    description.PixelX = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_PIXEL_X_WORD);
    description.PixelY = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_PIXEL_Y_WORD);
    description.SampleCount = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_SAMPLE_COUNT_WORD);
    description.RecordCapacity = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_RECORD_CAPACITY_WORD);
    if (ReadWord(bytes, LUDUS_RAY_PATH_HEADER_WIDTH_WORD) != SELECTED_PIXEL_EXTENT ||
        ReadWord(bytes, LUDUS_RAY_PATH_HEADER_HEIGHT_WORD) != SELECTED_PIXEL_EXTENT ||
        ReadWord(bytes, LUDUS_RAY_PATH_HEADER_FIRST_SAMPLE_WORD) != 0 || description.SampleCount == 0 ||
        description.SampleCount > MAX_RAY_PATH_SAMPLES || description.RecordCapacity == 0 ||
        description.RecordCapacity > MAX_RAY_PATH_RECORDS || bytes.size() != GetRayPathCaptureBytes(description))
    {
        return Status::InvalidDescription;
    }
    const uint32 appended = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_APPENDED_WORD);
    const uint32 stored = appended < description.RecordCapacity ? appended : description.RecordCapacity;
    const usize summaryCount = GetSummaryCount(description);
    foundation::Array<RayPathSummary> summaries;
    foundation::Array<RayPathEvent> events;
    if (summaries.TryEnsureCapacity(summaryCount) == false || events.TryResize(stored) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize index = 0; index < summaryCount; ++index)
    {
        const usize base = LUDUS_RAY_PATH_HEADER_WORDS + index * LUDUS_RAY_PATH_SUMMARY_WORDS;
        const uint32 flags = ReadWord(bytes, base + LUDUS_RAY_PATH_SUMMARY_FLAGS_WORD);
        if ((flags & LUDUS_RAY_PATH_SUMMARY_WRITTEN_FLAG) == 0)
        {
            continue;
        }
        const uint32 program = ReadWord(bytes, base + LUDUS_RAY_PATH_SUMMARY_PASS_WORD);
        if (program >= passes.GetSize() || passes[program] == format::ABSENT)
        {
            return Status::InvalidDescription;
        }
        RayPathSummary summary;
        summary.PixelX = description.PixelX;
        summary.PixelY = description.PixelY;
        summary.Sample = ReadWord(bytes, base + LUDUS_RAY_PATH_SUMMARY_SAMPLE_WORD);
        summary.Pass = passes[program];
        summary.EventCount = ReadWord(bytes, base + LUDUS_RAY_PATH_SUMMARY_EVENT_COUNT_WORD);
        summary.Radiance = ReadVector(bytes, base + LUDUS_RAY_PATH_SUMMARY_RADIANCE_WORD);
        summary.HasFailed = (flags & LUDUS_RAY_PATH_SUMMARY_FAILED_FLAG) != 0;
        static_cast<void>(summaries.TryAdd(summary));
    }
    const usize recordsWord = GetRecordsWord(summaryCount);
    for (usize record = 0; record < stored; ++record)
    {
        const usize base = recordsWord + record * LUDUS_RAY_PATH_RECORD_WORDS;
        const uint32 program = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_PASS_WORD);
        const uint32 kind = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_KIND_WORD);
        const uint32 outcome = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_OUTCOME_WORD);
        if (program >= passes.GetSize() || passes[program] == format::ABSENT ||
            kind > LUDUS_RAY_PATH_KIND_RASTER_FIRST_HIT || outcome > LUDUS_RAY_PATH_OUTCOME_FAILURE)
        {
            return Status::InvalidDescription;
        }
        RayPathEvent& event = events[record];
        event.PixelX = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_PIXEL_X_WORD);
        event.PixelY = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_PIXEL_Y_WORD);
        event.Sample = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_SAMPLE_WORD);
        event.Pass = passes[program];
        event.Sequence = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_SEQUENCE_WORD);
        event.Kind = static_cast<RayPathEventKind>(kind);
        event.Outcome = static_cast<RayPathOutcome>(outcome);
        event.RejectedCandidates = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_REJECTED_WORD);
        event.OriginWorld = ReadVector(bytes, base + LUDUS_RAY_PATH_RECORD_ORIGIN_WORD);
        event.DirectionWorld = ReadVector(bytes, base + LUDUS_RAY_PATH_RECORD_DIRECTION_WORD);
        event.MinDistanceWorld = ReadFloat(bytes, base + LUDUS_RAY_PATH_RECORD_MIN_DISTANCE_WORD);
        event.MaxDistanceWorld = ReadFloat(bytes, base + LUDUS_RAY_PATH_RECORD_MAX_DISTANCE_WORD);
        event.HitDistanceWorld = ReadFloat(bytes, base + LUDUS_RAY_PATH_RECORD_HIT_DISTANCE_WORD);
        event.Triangle = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_TRIANGLE_WORD);
        event.Instance = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_INSTANCE_WORD);
        event.Primitive = ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_PRIMITIVE_WORD);
        event.SourceId =
            uint64{ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_SOURCE_LOW_WORD)} |
            (uint64{ReadWord(bytes, base + LUDUS_RAY_PATH_RECORD_SOURCE_HIGH_WORD)} << LUDUS_GRAPH_SOURCE_HIGH_SHIFT);
    }
    foundation::Sort(summaries.GetData(),
                     summaries.GetSize(),
                     [](const RayPathSummary& left, const RayPathSummary& right) { return IsBefore(left, right); });
    foundation::Sort(events.GetData(), events.GetSize(), [](const RayPathEvent& left, const RayPathEvent& right) {
        return IsBefore(left, right);
    });
    // Both tables share one order, so one merge finds each invocation's recorded sequences.
    RayPathCaptureReport report;
    report.RecordCapacity = description.RecordCapacity;
    report.StoredRecords = stored;
    report.DroppedRecords = ReadWord(bytes, LUDUS_RAY_PATH_HEADER_DROPPED_WORD);
    report.WrittenSummaries = static_cast<uint32>(summaries.GetSize());
    usize event = 0;
    for (RayPathSummary& summary : summaries)
    {
        while (event < events.GetSize() && IsBefore(GetKey(events[event]), summary) == true)
        {
            ++event;
        }
        uint32 expected = 0;
        while (event < events.GetSize() && IsSameInvocation(events[event], summary) == true)
        {
            expected += events[event].Sequence == expected ? 1 : 0;
            ++event;
        }
        summary.IsTruncated = expected != summary.EventCount;
        report.TruncatedSummaries += summary.IsTruncated == true ? 1 : 0;
    }
    inoutEvents = foundation::core::Move(events);
    inoutSummaries = foundation::core::Move(summaries);
    outReport = report;
    return Status::Ready;
}

RenderGraphRayPathCapture::~RenderGraphRayPathCapture() noexcept
{
    Reset();
}
Status RenderGraphRayPathCapture::Request(rhi::DeviceHandle device,
                                          const RayPathCaptureDescription& description,
                                          const RayPathPassTable& passes) noexcept
{
    if (IsRequested() == true)
    {
        return Status::InvalidState;
    }
    if (description.SampleCount == 0 || description.SampleCount > MAX_RAY_PATH_SAMPLES ||
        description.RecordCapacity == 0 || description.RecordCapacity > MAX_RAY_PATH_RECORDS ||
        description.PixelX >= format::MAX_TARGET_EXTENT || description.PixelY >= format::MAX_TARGET_EXTENT)
    {
        return Status::InvalidDescription;
    }
    rhi::LifetimeCapabilities capabilities;
    Status status = rhi::GetLifetimeCapabilities(device, capabilities);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize bytes = GetRayPathCaptureBytes(description);
    if (bytes > capabilities.TransferBytes)
    {
        return Status::CapacityExceeded;
    }
    foundation::Array<uint8> contents;
    if (contents.TryResize(bytes, 0) == false)
    {
        return Status::OutOfMemory;
    }
    static_cast<void>(WriteRayPathCaptureHeader(description, {contents.GetData(), contents.GetSize()}));
    rhi::BufferHandle buffer;
    status =
        rhi::CreateBuffer(device, {rhi::BufferRole::Storage, bytes}, {contents.GetData(), contents.GetSize()}, buffer);
    if (status != Status::Ready && status != Status::Pending)
    {
        return status;
    }
    mDevice = device;
    mBuffer = buffer;
    mPasses = passes;
    mBytes = foundation::core::Move(contents);
    return status;
}
Status RenderGraphRayPathCapture::Poll(RayPathCaptureReport& outReport) noexcept
{
    if (IsRequested() == false)
    {
        return Status::InvalidState;
    }
    if (mIsReading == false)
    {
        const Status requested = rhi::RequestBufferReadback(mDevice, mBuffer, 0, mBytes.GetSize(), mReadback);
        if (requested != Status::Ready && requested != Status::Pending)
        {
            return requested;
        }
        mIsReading = true;
        return Status::Pending;
    }
    Status status = rhi::GetStatus(mDevice, mReadback);
    if (status == Status::Pending)
    {
        return status;
    }
    if (status == Status::Ready)
    {
        status = rhi::CopyReadback(mDevice, mReadback, mBytes.GetData(), mBytes.GetSize());
    }
    static_cast<void>(rhi::Release(mDevice, mReadback));
    mIsReading = false;
    if (status != Status::Ready)
    {
        return status;
    }
    return DecodeRayPathCapture({mBytes.GetData(), mBytes.GetSize()}, mPasses, mEvents, mSummaries, outReport);
}
void RenderGraphRayPathCapture::Reset() noexcept
{
    if (mIsReading == true)
    {
        static_cast<void>(rhi::Release(mDevice, mReadback));
        mIsReading = false;
    }
    static_cast<void>(rhi::Destroy(mDevice, mBuffer));
    mBuffer = {};
    mDevice = {};
    mBytes.Clear();
    mEvents.Clear();
    mSummaries.Clear();
}
} // namespace ludus::graphics::renderer::internal
