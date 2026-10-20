#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

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
#endif
