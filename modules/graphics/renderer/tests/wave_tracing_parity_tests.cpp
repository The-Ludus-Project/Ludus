#include "wave_tracing_parity.h"
#include "wave_tracing_parity_layout.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/wave_tracing.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::renderer;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;
constexpr uint64 TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize CASE_COUNT = LUDUS_WAVE_PARITY_CASE_COUNT;
constexpr usize WORDS_PER_RECORD = 4;
constexpr usize CASE_WORDS = CASE_COUNT * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
constexpr usize RESULT_WORDS = CASE_COUNT * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
constexpr usize CELL_COUNT = usize{LUDUS_WAVE_PARITY_RESOLUTION} * LUDUS_WAVE_PARITY_RESOLUTION;
constexpr usize CASE_BYTES = CASE_WORDS * sizeof(uint32);
constexpr usize RESULT_BYTES = RESULT_WORDS * sizeof(uint32);
template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    RasterStatus status = rhi::GetStatus(device, handle);
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}

bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}

bool RunParityKernel(const Array<uint32>& caseWords, Array<uint32>& outResults)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = CASE_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = CASE_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(outResults.TryResize(RESULT_WORDS, uint32{0}) == true);
    rhi::BufferHandle cases;
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = CASE_BYTES },
                                         {reinterpret_cast<const uint8*>(caseWords.GetData()), CASE_BYTES},
                                         cases)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {reinterpret_cast<const uint8*>(outResults.GetData()), RESULT_BYTES},
                                         results)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::wave_tracing_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, cases);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    constexpr usize BINDING_COUNT = 2;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = 0, .Buffer = cases, .Size = CASE_BYTES },
        { .Binding = 1, .Buffer = results, .Size = RESULT_BYTES },
    }};
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources.AsSpan(), set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion casesOriginal;
    rhi::GraphVersion casesVersion;
    rhi::GraphVersion resultsOriginal;
    rhi::GraphVersion resultsVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, cases, casesOriginal) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, casesOriginal, casesVersion) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, resultsOriginal) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, resultsOriginal, resultsVersion) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {casesVersion, rhi::GraphAccessMode::StorageReadWrite, 0, CASE_BYTES, rhi::RasterVisibility::Compute},
        {resultsVersion, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(CASE_COUNT / CASE_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "wave tracing parity";
    pass.Uses = uses.GetData();
    pass.UseCount = static_cast<uint32>(uses.GetSize());
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    rhi::GraphReport graphReport;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, graphReport) == RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    RasterStatus executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    const uint64 beginning = time::NowTicks();
    while (executed == RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == RasterStatus::Ready);
    WaitReady(device, completion);
    rhi::ReadbackTicket ticket;
    RasterStatus status = rhi::RequestBufferReadback(device, results, 0, RESULT_BYTES, ticket);
    const uint64 start = time::NowTicks();
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(outResults.GetData()), RESULT_BYTES) ==
            RasterStatus::Ready);
    REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    return true;
}

void WriteFloat(usize index, Array<uint32>& outWords, float64 value)
{
    const float32 narrowed = static_cast<float32>(value);
    std::memcpy(&outWords[index], &narrowed, sizeof(narrowed));
}
float64 ReadFloat(const Array<uint32>& words, usize index)
{
    float32 value = 0;
    std::memcpy(&value, &words[index], sizeof(value));
    return static_cast<float64>(value);
}
} // namespace

TEST_CASE("Scalar wave transport and beam propagation match the CPU oracle on GPU",
          "[renderer][gpu][wave-tracing][parity]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    Array<uint32> words;
    REQUIRE(words.TryResize(CASE_WORDS, uint32{0}) == true);
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
        WriteFloat(base, words, 0.02 + 0.002 * static_cast<float64>(index));
        WriteFloat(base + 1, words, 0.01);
        WriteFloat(base + 2, words, 0.02);
        WriteFloat(base + 3, words, 0.3);
        WriteFloat(base + 4, words, 0.4);
        words[base + 5] = LUDUS_WAVE_PARITY_RESOLUTION;
        WriteFloat(base + 6, words, 0.02 * static_cast<float64>(index));
        WriteFloat(base + 7, words, -0.01 * static_cast<float64>(index));
        WriteFloat(base + 8, words, 4);
        WriteFloat(base + 9, words, 0.001 * static_cast<float64>(index));
        WriteFloat(base + 10, words, 2);
        for (usize cell = 0; cell < CELL_COUNT; ++cell)
        {
            const usize x = cell % LUDUS_WAVE_PARITY_RESOLUTION;
            const bool isOpen = index % 4 == 0 || (index % 4 == 1 && x < 12) ||
                                (index % 4 == 2 && ((x >= 8 && x < 12) || (x >= 20 && x < 24)));
            WriteFloat(base + LUDUS_WAVE_PARITY_HEADER_RECORDS * WORDS_PER_RECORD + cell,
                       words,
                       isOpen == true ? 1 : 0);
        }
    }
    Array<uint32> results;
    if (RunParityKernel(words, results) == false)
    {
        SKIP("No native GPU adapter");
    }
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        CAPTURE(index);
        const usize base = index * LUDUS_WAVE_PARITY_CASE_RECORDS * WORDS_PER_RECORD;
        const WaveAperture aperture{ReadFloat(words, base),
                                    ReadFloat(words, base + 1),
                                    ReadFloat(words, base + 2),
                                    ReadFloat(words, base + 3),
                                    ReadFloat(words, base + 4),
                                    LUDUS_WAVE_PARITY_RESOLUTION};
        const math::Vector3d receiverApertureWorld{ReadFloat(words, base + 6),
                                                   ReadFloat(words, base + 7),
                                                   ReadFloat(words, base + 8)};
        Array<float64> transmission;
        REQUIRE(transmission.TryResize(CELL_COUNT) == true);
        for (usize cell = 0; cell < transmission.GetSize(); ++cell)
        {
            transmission[cell] = ReadFloat(words, base + LUDUS_WAVE_PARITY_HEADER_RECORDS * WORDS_PER_RECORD + cell);
        }
        WaveField field;
        REQUIRE(TryEvaluateWaveAperture(aperture,
                                        {transmission.GetData(), transmission.GetSize()},
                                        receiverApertureWorld,
                                        field) == true);
        WaveBeam propagated;
        WaveMoments moments;
        REQUIRE(TryPropagateWaveBeam({aperture.WavelengthWorld,
                                      aperture.SigmaXWorldSquared,
                                      aperture.SigmaYWorldSquared,
                                      ReadFloat(words, base + 9)},
                                     ReadFloat(words, base + 10),
                                     propagated,
                                     moments) == true);
        const usize resultBase = index * LUDUS_WAVE_PARITY_RESULT_RECORDS * WORDS_PER_RECORD;
        CHECK(math::Abs(ReadFloat(results, resultBase) - field.Real) < 1e-5);
        CHECK(math::Abs(ReadFloat(results, resultBase + 1) - field.Imaginary) < 1e-5);
        CHECK(math::Abs(ReadFloat(results, resultBase + 2) -
                        (field.Real * field.Real + field.Imaginary * field.Imaginary)) < 1e-5);
        CHECK(ReadFloat(results, resultBase + 3) == 1);
        CHECK(math::Abs(ReadFloat(results, resultBase + 4) - moments.VarianceXWorldSquared) < 1e-7);
        CHECK(math::Abs(ReadFloat(results, resultBase + 5) - moments.VarianceYWorldSquared) < 1e-7);
        // float32 tau and Metal reciprocal rounding contribute up to three ulps to the covariance.
        CHECK(math::Abs(ReadFloat(results, resultBase + 6) - moments.CovarianceX) < 2e-7);
        CHECK(math::Abs(ReadFloat(results, resultBase + 7) - moments.CovarianceY) < 2e-7);
    }
}
