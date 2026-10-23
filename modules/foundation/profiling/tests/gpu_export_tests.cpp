#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ludus::foundation;
using namespace ludus::foundation::profiling;

#if LUDUS_PROFILING_ENABLED
TEST_CASE("GPU duration export retains submission time identity and sampling gaps", "[profiling][gpu-export]")
{
    const GpuTraceSample sample
    {
        .SubmissionTicks = 1234,
        .CollectionTicks = 9876,
        .DurationNanoseconds = 0,
        .FrameId = 5,
        .DeviceSession = 3,
    };
    const GpuTraceCoverage coverage
    {
        .IsGpuRequested = true,
        .EligibleFrames = 7,
        .InvalidSamples = 1,
        .DroppedSamples = 2,
        .UnresolvedSamples = 1,
    };
    Array<uint8> bytes;
    REQUIRE(SerializePerfettoTrace(bytes, {&sample, 1}, coverage, false) == true);
    const std::string json(reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize());
    CHECK(json.find("\"ts\":1.234") != std::string::npos);
    CHECK(json.find("\"value\":0") != std::string::npos);
    CHECK(json.find("\"collectionNanoseconds\":9876") != std::string::npos);
    CHECK(json.find("\"frameId\":5") != std::string::npos);
    CHECK(json.find("\"unresolvedSamples\":1") != std::string::npos);
    CHECK(json.find("\"cpuScopesIncluded\":false") != std::string::npos);
    CHECK(json.find("\"ph\":\"X\"") == std::string::npos);
}

TEST_CASE("invalid GPU export preserves bytes and unexported CPU events", "[profiling][gpu-export]")
{
    REQUIRE(BeginCapture(1) == true);
    {
        LUDUS_PROFILE_SCOPE(PreservedCpuTrace);
    }
    EndCapture();
    REQUIRE(HasUnexportedTrace() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryAdd(42) == true);
    const GpuTraceSample invalid
    {
        .SubmissionTicks = 10,
        .CollectionTicks = 1,
        .FrameId = 1,
        .DeviceSession = 1,
    };
    CHECK(SerializePerfettoTrace(bytes, {&invalid, 1}) == false);
    REQUIRE(bytes.GetSize() == 1);
    CHECK(bytes[0] == 42);
    CHECK(HasUnexportedTrace() == true);
    REQUIRE(SerializePerfettoTrace(bytes) == true);
    CHECK(HasUnexportedTrace() == false);
    CHECK(SerializePerfettoTrace(bytes, {}, {}, false) == false);
    CHECK(bytes.GetSize() > 1);
}

TEST_CASE("named GPU pass counters escape names and retain physical group identity", "[profiling][gpu-export]")
{
    constexpr char NAME[] = "Scene/\"Opaque\"\n";
    GpuTraceSample sample
    {
        .SubmissionTicks = 1000,
        .CollectionTicks = 2000,
        .DurationNanoseconds = 123,
        .FrameId = 1,
        .DeviceSession = 2,
        .PassName = {NAME, sizeof(NAME) - 1},
        .ScopeIdentity = 3,
        .ScopePassIndex = 1,
        .LogicalPassCount = 2,
    };
    Array<uint8> bytes;
    const GpuTraceCoverage coverage{ .IsGpuRequested = true, .IsPassTimingSupported = true, .QueryDroppedPasses = 4 };
    REQUIRE(SerializePerfettoTrace(bytes, {&sample, 1}, coverage, false) == true);
    const std::string json(reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize());
    CHECK(json.find("GPU/Pass/Scene/\\\"Opaque\\\"\\n [3:1:Raster]/DurationNanoseconds") != std::string::npos);
    CHECK(json.find("\"passSampleCount\":1") != std::string::npos);
    CHECK(json.find("\"frameSampleCount\":0") != std::string::npos);
    CHECK(json.find("\"logicalPassCount\":2") != std::string::npos);
    CHECK(json.find("\"queryDroppedPasses\":4") != std::string::npos);
    CHECK(json.find("\"ph\":\"X\"") == std::string::npos);
    const usize originalSize = bytes.GetSize();
    sample.ScopeIdentity = 0;
    CHECK(SerializePerfettoTrace(bytes, {&sample, 1}, coverage, false) == false);
    CHECK(bytes.GetSize() == originalSize);
    sample.ScopeIdentity = 1;
    sample.PassName = {nullptr, 1};
    CHECK(SerializePerfettoTrace(bytes, {&sample, 1}, coverage, false) == false);
    CHECK(bytes.GetSize() == originalSize);
}

TEST_CASE("calibrated GPU passes export named slices on non-overlapping lanes", "[profiling][gpu-export]")
{
    constexpr char TRACE[] = "Main/raytracing/\"trace\"";
    constexpr char BLIT[] = "Main/raytracing/blit";
    const auto makePass = [](const char* name, usize length, uint64 start, uint64 end, bool isCompute) {
        return GpuTraceSample
        {
            .SubmissionTicks = 500,
            .CollectionTicks = 9000,
            .DurationNanoseconds = end - start,
            .FrameId = 4,
            .DeviceSession = 1,
            .PassName = {name, length},
            .ScopeIdentity = 7,
            .ScopePassIndex = isCompute == true ? 0U : 1U,
            .IsCompute = isCompute,
            .HasExecutionInterval = true,
            .ExecutionStartTicks = start,
            .ExecutionEndTicks = end,
            .ClockUncertaintyNanoseconds = 3,
        };
    };
    StaticArray<GpuTraceSample, 4> samples{
        makePass(TRACE, sizeof(TRACE) - 1, 1000, 5000, true),
        makePass(BLIT, sizeof(BLIT) - 1, 2000, 6000, false),
        makePass(BLIT, sizeof(BLIT) - 1, 6000, 7000, false),
        GpuTraceSample
        {
            .SubmissionTicks = 500,
            .CollectionTicks = 9000,
            .DurationNanoseconds = 6500,
            .FrameId = 4,
            .DeviceSession = 1,
            .PassName = {},
        },
    };
    samples[2].ClockUncertaintyNanoseconds = 9;
    Array<uint8> bytes;
    const GpuTraceCoverage coverage{ .IsGpuRequested = true, .IsPassTimingSupported = true };
    REQUIRE(SerializePerfettoTrace(bytes, {samples.GetData(), samples.GetSize()}, coverage, false) == true);
    const std::string json(reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize());
    CHECK(json.find("{\"pid\":2,\"tid\":2,\"ph\":\"X\",\"cat\":\"gpu.compute\",\"name\":\"Main/raytracing/"
                    "\\\"trace\\\" (Compute)\",\"ts\":1.000,\"dur\":4.000") != std::string::npos);
    // The overlapping raster pass moves to a second lane; the later one reuses the first.
    CHECK(json.find("{\"pid\":2,\"tid\":3,\"ph\":\"X\",\"cat\":\"gpu.raster\",\"name\":\"Main/raytracing/blit "
                    "(Raster)\",\"ts\":2.000,\"dur\":4.000") != std::string::npos);
    CHECK(json.find("{\"pid\":2,\"tid\":2,\"ph\":\"X\",\"cat\":\"gpu.raster\",\"name\":\"Main/raytracing/blit "
                    "(Raster)\",\"ts\":6.000,\"dur\":1.000") != std::string::npos);
    CHECK(json.find("\"args\":{\"name\":\"GPU execution (calibrated)\"}") != std::string::npos);
    CHECK(json.find("\"args\":{\"name\":\"GPU execution (calibrated, overlap 1)\"}") != std::string::npos);
    CHECK(json.find("\"name\":\"process_name\",\"args\":{\"name\":\"GPU\"}") != std::string::npos);
    CHECK(json.find("\"executionSliceCount\":3") != std::string::npos);
    CHECK(json.find("\"executionLaneCount\":2") != std::string::npos);
    CHECK(json.find("\"maxClockUncertaintyNanoseconds\":9") != std::string::npos);
    // The frame probe has no calibrated interval and stays a duration counter only.
    CHECK(json.find("GPU/FrameDurationNanoseconds") != std::string::npos);
    CHECK(json.find("\"frameSampleCount\":1") != std::string::npos);

    const usize originalSize = bytes.GetSize();
    samples[1].ExecutionEndTicks = samples[1].ExecutionStartTicks - 1;
    CHECK(SerializePerfettoTrace(bytes, {samples.GetData(), samples.GetSize()}, coverage, false) == false);
    samples[1].ExecutionEndTicks = 6000;
    samples[3].HasExecutionInterval = true;
    CHECK(SerializePerfettoTrace(bytes, {samples.GetData(), samples.GetSize()}, coverage, false) == false);
    CHECK(bytes.GetSize() == originalSize);
}
#endif
