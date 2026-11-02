#include "../shaders/renderer_gpu_preview_layout.h"
#include "../src/internal/render_graph_ray_path.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// Host decoding of the ray path capture buffer (render-graph-authoring.md#ray-path-inspection-proposed),
// written here word by word as a capture-variant kernel would.
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace
{
/// A two-pass plan whose ray passes use callback sets 1 (plan pass 2) and 0 (plan pass 3).
renderer::internal::RayPathPassTable MakePasses() noexcept
{
    constexpr uint32 FIRST_RAY_PASS = 2;
    renderer::internal::RayPathPassTable passes{};
    for (uint32& pass : passes)
    {
        pass = format::ABSENT;
    }
    passes[1] = FIRST_RAY_PASS;
    passes[0] = FIRST_RAY_PASS + 1;
    passes[format::MAX_RAY_PROGRAMS - 1] = FIRST_RAY_PASS + 2;
    return passes;
}
/// Capture buffer bytes with a written header, edited word by word.
struct CaptureBytes final
{
    renderer::RayPathCaptureDescription Description{};
    Array<uint8> Bytes;
    explicit CaptureBytes(const renderer::RayPathCaptureDescription& description) : Description(description)
    {
        REQUIRE(Bytes.TryResize(renderer::internal::GetRayPathCaptureBytes(description), 0) == true);
        REQUIRE(renderer::internal::WriteRayPathCaptureHeader(description, {Bytes.GetData(), Bytes.GetSize()}) == true);
    }
    void SetWord(usize word, uint32 value)
    {
        REQUIRE(
            TryWriteLittleEndian(value, std::span<uint8>{Bytes.GetData() + word * sizeof(uint32), sizeof(uint32)}) ==
            true);
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): word index, then its value
    void SetFloat(usize word, float32 value)
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        SetWord(word, bits);
    }
    [[nodiscard]] usize GetSummaryWord(uint32 sample, uint32 program) const noexcept
    {
        return LUDUS_RAY_PATH_HEADER_WORDS +
               (usize{sample} * LUDUS_RAY_PATH_CAPTURE_PASS_COUNT + program) * LUDUS_RAY_PATH_SUMMARY_WORDS;
    }
    [[nodiscard]] usize GetRecordWord(uint32 record) const noexcept
    {
        return GetSummaryWord(Description.SampleCount, 0) + usize{record} * LUDUS_RAY_PATH_RECORD_WORDS;
    }
    void WriteSummary(uint32 sample, uint32 program, uint32 eventCount, bool hasFailed = false)
    {
        const usize base = GetSummaryWord(sample, program);
        SetWord(base + LUDUS_RAY_PATH_SUMMARY_SAMPLE_WORD, sample);
        SetWord(base + LUDUS_RAY_PATH_SUMMARY_PASS_WORD, program);
        SetWord(base + LUDUS_RAY_PATH_SUMMARY_EVENT_COUNT_WORD, eventCount);
        SetFloat(base + LUDUS_RAY_PATH_SUMMARY_RADIANCE_WORD, static_cast<float32>(sample) + .5F);
        SetWord(base + LUDUS_RAY_PATH_SUMMARY_FLAGS_WORD,
                LUDUS_RAY_PATH_SUMMARY_WRITTEN_FLAG | (hasFailed == true ? LUDUS_RAY_PATH_SUMMARY_FAILED_FLAG : 0U));
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): mirrors the record word order
    void WriteRecord(uint32 record, uint32 sample, uint32 program, uint32 sequence, uint32 kind, uint32 outcome)
    {
        const usize base = GetRecordWord(record);
        SetWord(base + LUDUS_RAY_PATH_RECORD_PIXEL_X_WORD, Description.PixelX);
        SetWord(base + LUDUS_RAY_PATH_RECORD_PIXEL_Y_WORD, Description.PixelY);
        SetWord(base + LUDUS_RAY_PATH_RECORD_SAMPLE_WORD, sample);
        SetWord(base + LUDUS_RAY_PATH_RECORD_PASS_WORD, program);
        SetWord(base + LUDUS_RAY_PATH_RECORD_SEQUENCE_WORD, sequence);
        SetWord(base + LUDUS_RAY_PATH_RECORD_KIND_WORD, kind);
        SetWord(base + LUDUS_RAY_PATH_RECORD_OUTCOME_WORD, outcome);
        SetWord(base + LUDUS_RAY_PATH_RECORD_REJECTED_WORD, sequence + 1);
        SetFloat(base + LUDUS_RAY_PATH_RECORD_ORIGIN_WORD + 2, -1);
        SetFloat(base + LUDUS_RAY_PATH_RECORD_DIRECTION_WORD + 1, 1);
        SetFloat(base + LUDUS_RAY_PATH_RECORD_MAX_DISTANCE_WORD, 4);
        SetFloat(base + LUDUS_RAY_PATH_RECORD_HIT_DISTANCE_WORD, 3);
        SetWord(base + LUDUS_RAY_PATH_RECORD_TRIANGLE_WORD, 7);
        SetWord(base + LUDUS_RAY_PATH_RECORD_INSTANCE_WORD, 5);
        SetWord(base + LUDUS_RAY_PATH_RECORD_PRIMITIVE_WORD, 6);
        SetWord(base + LUDUS_RAY_PATH_RECORD_SOURCE_LOW_WORD, 9);
        SetWord(base + LUDUS_RAY_PATH_RECORD_SOURCE_HIGH_WORD, 1);
    }
    rhi::RasterStatus Decode(Array<renderer::RayPathEvent>& inoutEvents,
                             Array<renderer::RayPathSummary>& inoutSummaries,
                             renderer::RayPathCaptureReport& outReport) const
    {
        return renderer::internal::DecodeRayPathCapture({Bytes.GetData(), Bytes.GetSize()},
                                                        MakePasses(),
                                                        inoutEvents,
                                                        inoutSummaries,
                                                        outReport);
    }
};
} // namespace

TEST_CASE("Ray path decoding sorts events, maps passes and reports every field", "[render-graph][ray-path]")
{
    CaptureBytes capture({ .PixelX = 12, .PixelY = 34, .SampleCount = 2, .RecordCapacity = 8 });
    // Atomic reservation leaves records in arbitrary order across invocations.
    capture.WriteSummary(1, 0, 1);
    capture.WriteSummary(0, 1, 2, true);
    capture.WriteRecord(0, 1, 0, 0, LUDUS_RAY_PATH_KIND_CLOSEST, LUDUS_RAY_PATH_OUTCOME_MISS);
    capture.WriteRecord(1, 0, 1, 1, LUDUS_RAY_PATH_KIND_VISIBILITY, LUDUS_RAY_PATH_OUTCOME_FAILURE);
    capture.WriteRecord(2, 0, 1, 0, LUDUS_RAY_PATH_KIND_RASTER_FIRST_HIT, LUDUS_RAY_PATH_OUTCOME_HIT);
    constexpr uint32 APPENDED = 3;
    capture.SetWord(LUDUS_RAY_PATH_HEADER_APPENDED_WORD, APPENDED);
    Array<renderer::RayPathEvent> events;
    Array<renderer::RayPathSummary> summaries;
    renderer::RayPathCaptureReport report;
    REQUIRE(capture.Decode(events, summaries, report) == rhi::RasterStatus::Ready);
    CHECK(report.RecordCapacity == 8);
    CHECK(report.StoredRecords == APPENDED);
    CHECK(report.DroppedRecords == 0);
    CHECK(report.WrittenSummaries == 2);
    CHECK(report.TruncatedSummaries == 0);
    REQUIRE(summaries.GetSize() == 2);
    CHECK(summaries[0].Sample == 0);
    CHECK(summaries[0].Pass == 2);
    CHECK(summaries[0].EventCount == 2);
    CHECK(summaries[0].HasFailed == true);
    CHECK(summaries[0].Radiance.X == .5F);
    CHECK(summaries[1].Sample == 1);
    CHECK(summaries[1].Pass == 3);
    CHECK(summaries[1].HasFailed == false);
    REQUIRE(events.GetSize() == APPENDED);
    CHECK(events[0].Sample == 0);
    CHECK(events[0].Sequence == 0);
    CHECK(events[0].Kind == renderer::RayPathEventKind::RasterFirstHit);
    CHECK(events[0].Outcome == renderer::RayPathOutcome::Hit);
    CHECK(events[1].Sequence == 1);
    CHECK(events[1].Kind == renderer::RayPathEventKind::Visibility);
    CHECK(events[1].Outcome == renderer::RayPathOutcome::Failure);
    CHECK(events[1].RejectedCandidates == 2);
    CHECK(events[2].Sample == 1);
    CHECK(events[2].Pass == 3);
    const renderer::RayPathEvent& event = events[0];
    CHECK(event.PixelX == 12);
    CHECK(event.PixelY == 34);
    CHECK(event.Pass == 2);
    CHECK(event.OriginWorld.Z == -1);
    CHECK(event.DirectionWorld.Y == 1);
    CHECK(event.MaxDistanceWorld == 4);
    CHECK(event.HitDistanceWorld == 3);
    CHECK(event.Triangle == 7);
    CHECK(event.Instance == 5);
    CHECK(event.Primitive == 6);
    CHECK(event.SourceId == ((uint64{1} << 32) | 9));
}

TEST_CASE("Ray path decoding marks paths with dropped records as truncated", "[render-graph][ray-path]")
{
    constexpr uint32 CAPACITY = 2;
    CaptureBytes capture({ .SampleCount = 2, .RecordCapacity = CAPACITY });
    capture.WriteSummary(0, 0, 2);
    capture.WriteSummary(1, 0, 2);
    // Sample 1 kept its first event only; sample 0 lost its first event.
    capture.WriteRecord(0, 1, 0, 0, LUDUS_RAY_PATH_KIND_CLOSEST, LUDUS_RAY_PATH_OUTCOME_HIT);
    capture.WriteRecord(1, 0, 0, 1, LUDUS_RAY_PATH_KIND_CLOSEST, LUDUS_RAY_PATH_OUTCOME_MISS);
    constexpr uint32 APPENDED = 4;
    capture.SetWord(LUDUS_RAY_PATH_HEADER_APPENDED_WORD, APPENDED);
    capture.SetWord(LUDUS_RAY_PATH_HEADER_DROPPED_WORD, APPENDED - CAPACITY);
    Array<renderer::RayPathEvent> events;
    Array<renderer::RayPathSummary> summaries;
    renderer::RayPathCaptureReport report;
    REQUIRE(capture.Decode(events, summaries, report) == rhi::RasterStatus::Ready);
    CHECK(report.StoredRecords == CAPACITY);
    CHECK(report.DroppedRecords == APPENDED - CAPACITY);
    CHECK(report.TruncatedSummaries == 2);
    REQUIRE(summaries.GetSize() == 2);
    CHECK(summaries[0].IsTruncated == true);
    CHECK(summaries[1].IsTruncated == true);
    CHECK(events.GetSize() == CAPACITY);
}

TEST_CASE("Ray path decoding rejects malformed buffers and preserves outputs", "[render-graph][ray-path]")
{
    CaptureBytes capture({ .SampleCount = 1, .RecordCapacity = 1 });
    capture.WriteSummary(0, 0, 1);
    capture.WriteRecord(0, 0, 0, 0, LUDUS_RAY_PATH_KIND_CLOSEST, LUDUS_RAY_PATH_OUTCOME_HIT);
    capture.SetWord(LUDUS_RAY_PATH_HEADER_APPENDED_WORD, 1);
    Array<renderer::RayPathEvent> events;
    Array<renderer::RayPathSummary> summaries;
    renderer::RayPathCaptureReport report;
    REQUIRE(capture.Decode(events, summaries, report) == rhi::RasterStatus::Ready);
    REQUIRE(events.GetSize() == 1);
    SECTION("unknown version")
    {
        capture.SetWord(LUDUS_RAY_PATH_HEADER_VERSION_WORD, LUDUS_RAY_PATH_CAPTURE_VERSION + 1);
    }
    SECTION("callback set without a ray pass")
    {
        capture.SetWord(capture.GetRecordWord(0) + LUDUS_RAY_PATH_RECORD_PASS_WORD, 2);
    }
    SECTION("unknown event kind")
    {
        capture.SetWord(capture.GetRecordWord(0) + LUDUS_RAY_PATH_RECORD_KIND_WORD,
                        LUDUS_RAY_PATH_KIND_RASTER_FIRST_HIT + 1);
    }
    SECTION("record capacity differing from the buffer size")
    {
        capture.SetWord(LUDUS_RAY_PATH_HEADER_RECORD_CAPACITY_WORD, 2);
    }
    renderer::RayPathCaptureReport rejected;
    rejected.StoredRecords = 99;
    CHECK(capture.Decode(events, summaries, rejected) == rhi::RasterStatus::InvalidDescription);
    CHECK(rejected.StoredRecords == 99);
    CHECK(events.GetSize() == 1);
    CHECK(summaries.GetSize() == 1);
}

TEST_CASE("Ray path decoding accepts callback sets beyond the active invocation limit", "[render-graph][ray-path]")
{
    constexpr uint32 LAST_CALLBACK = static_cast<uint32>(format::MAX_RAY_PROGRAMS - 1);
    CaptureBytes capture({ .SampleCount = 1, .RecordCapacity = 1 });
    capture.WriteSummary(0, LAST_CALLBACK, 1);
    capture.WriteRecord(0, 0, LAST_CALLBACK, 0, LUDUS_RAY_PATH_KIND_CLOSEST, LUDUS_RAY_PATH_OUTCOME_MISS);
    capture.SetWord(LUDUS_RAY_PATH_HEADER_APPENDED_WORD, 1);
    Array<renderer::RayPathEvent> events;
    Array<renderer::RayPathSummary> summaries;
    renderer::RayPathCaptureReport report;
    REQUIRE(capture.Decode(events, summaries, report) == rhi::RasterStatus::Ready);
    REQUIRE(events.GetSize() == 1);
    REQUIRE(summaries.GetSize() == 1);
    CHECK(LAST_CALLBACK >= format::MAX_RAY_PASSES);
    CHECK(events[0].Pass == MakePasses()[LAST_CALLBACK]);
    CHECK(summaries[0].Pass == events[0].Pass);
    capture.SetWord(capture.GetRecordWord(0) + LUDUS_RAY_PATH_RECORD_PASS_WORD,
                    static_cast<uint32>(format::MAX_RAY_PROGRAMS));
    CHECK(capture.Decode(events, summaries, report) == rhi::RasterStatus::InvalidDescription);
}
