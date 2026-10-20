#include "internal/gpu_pass_timing_buffer.h"
#include <ludus/graphics/rhi/gpu_scope.h>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;

TEST_CASE("GPU scopes validate and own bounded UTF-8 names", "[rhi][gpu-scope]")
{
    rhi::GpuScopeDescription scope;
    constexpr char NAME[] = "Geometry/\"opaque\"";
    REQUIRE(rhi::MakeGpuScope(1, {NAME, sizeof(NAME) - 1}, scope) == true);
    CHECK(std::strcmp(scope.Name.GetData(), NAME) == 0);
    CHECK(rhi::MakeGpuScope(0, {NAME, sizeof(NAME) - 1}, scope) == false);
    CHECK(rhi::MakeGpuScope(1, {}, scope) == false);
    CHECK(rhi::MakeGpuScope(1, {nullptr, 1}, scope) == false);
    constexpr char EMBEDDED_ZERO[] = "a\0b";
    CHECK(rhi::MakeGpuScope(1, {EMBEDDED_ZERO, sizeof(EMBEDDED_ZERO) - 1}, scope) == false);
    constexpr char INVALID_UTF8[] = "\xC0\xAF";
    CHECK(rhi::MakeGpuScope(1, {INVALID_UTF8, sizeof(INVALID_UTF8) - 1}, scope) == false);
    StaticArray<char, rhi::GPU_SCOPE_NAME_BYTES> oversized;
    for (char& character : oversized)
    {
        character = 'a';
    }
    CHECK(rhi::MakeGpuScope(1, {oversized.GetData(), oversized.GetSize()}, scope) == false);
    CHECK(std::strcmp(scope.Name.GetData(), NAME) == 0);
    CHECK(scope.Identity == 1);
}

TEST_CASE("GPU pass batch keeps physical identities and pending storage bounded", "[rhi][gpu-scope]")
{
    rhi::internal::GpuPassTimingBuffer buffer;
    rhi::GpuScopeDescription scope;
    REQUIRE(rhi::MakeGpuScope(2, {"Merged", sizeof("Merged") - 1}, scope, 2) == true);
    buffer.Arm();
    CHECK(buffer.CanArm() == false);
    CHECK(buffer.GetReport().State == rhi::GpuTimingState::Armed);
    // No acquired frame: remains armed, including skipped acquisitions.
    CHECK(buffer.AddPass(rhi::GpuPassKind::Raster) == rhi::MAX_GPU_TIMED_PASSES);
    buffer.BeginFrame();
    REQUIRE(buffer.BeginScope(scope) == true);
    CHECK(buffer.BeginScope(scope) == false);
    for (usize index = 0; index < rhi::MAX_GPU_TIMED_PASSES; ++index)
    {
        REQUIRE(buffer.AddPass(rhi::GpuPassKind::Raster) == index);
    }
    CHECK(buffer.AddPass(rhi::GpuPassKind::Compute) == rhi::MAX_GPU_TIMED_PASSES);
    CHECK(buffer.EndScope() == true);
    CHECK(buffer.EndScope() == false);
    buffer.Submit();
    CHECK(buffer.CanArm() == false);
    const rhi::GpuPassTimings& report = buffer.GetReport();
    CHECK(report.State == rhi::GpuTimingState::Pending);
    CHECK(report.Count == rhi::MAX_GPU_TIMED_PASSES);
    CHECK(report.DroppedPasses == 1);
    CHECK(report.Passes[1].Scope.Identity == 2);
    CHECK(report.Passes[1].Scope.LogicalPassCount == 2);
    CHECK(report.Passes[1].ScopePassIndex == 1);
    // A later frame cannot write the pending batch, even with a new scope.
    buffer.BeginFrame();
    REQUIRE(buffer.BeginScope(scope) == true);
    CHECK(buffer.AddPass(rhi::GpuPassKind::Compute) == rhi::MAX_GPU_TIMED_PASSES);
    buffer.Submit();
    CHECK(report.Count == rhi::MAX_GPU_TIMED_PASSES);
    CHECK(report.DroppedPasses == 1);
    buffer.GetReport().State = rhi::GpuTimingState::Available;
    CHECK(buffer.CanArm() == true);
    CHECK(buffer.GetReport().Count == rhi::MAX_GPU_TIMED_PASSES);
    buffer.Arm();
    CHECK(buffer.GetReport().Count == 0);
    buffer.BeginFrame();
    buffer.Submit();
    CHECK(buffer.GetReport().State == rhi::GpuTimingState::Available);
    buffer.Reset();
    CHECK(buffer.GetReport().State == rhi::GpuTimingState::Empty);
}
