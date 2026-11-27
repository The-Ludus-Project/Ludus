#include "baseline_renderer_light_transformer_decoder.h"
#include "renderer_light_transformer_decoder.h"
#include "renderer_light_transformer_decoder_slots.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_transformer.hpp>

#include <bit>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
using Status = rhi::RasterStatus;
constexpr usize FIXTURE_HEADER_WORDS = 3;
constexpr usize MAXIMUM_FIXTURE_BYTES = usize{16} * 1024 * 1024;
constexpr usize BUDGET = usize{16} * 1024 * 1024;
constexpr uint64 TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
constexpr float32 RELATIVE_TOLERANCE = 0.0001F;
constexpr float32 ABSOLUTE_TOLERANCE = 0.00001F;
namespace decoder_slots = ludus::shaders::renderer_light_transformer_decoder::slots;
struct Fixture final
{
    Array<float32> Weights;
    Array<renderer::LightTransformerAnchor> Anchors;
    Array<renderer::LightTransformerQuery> Queries;
    Array<math::Vector4> Expected;
};
bool ReadFixture(Fixture& outFixture)
{
    const char* pathOrNull = std::getenv("LUDUS_LIGHT_TRANSFORMER_TEST_FIXTURE");
    const char* path = pathOrNull == nullptr ? LUDUS_LIGHT_TRANSFORMER_TEST_FIXTURE : pathOrNull;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (stream.is_open() == false)
    {
        return false;
    }
    const std::streampos length = stream.tellg();
    if (length < 0 || static_cast<usize>(length) > MAXIMUM_FIXTURE_BYTES)
    {
        return false;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(static_cast<usize>(length)) == false || bytes.GetSize() < FIXTURE_HEADER_WORDS * sizeof(uint32))
    {
        return false;
    }
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.GetData()), static_cast<std::streamsize>(bytes.GetSize()));
    if (stream.good() == false)
    {
        return false;
    }
    StaticArray<uint32, FIXTURE_HEADER_WORDS> counts{};
    for (usize i = 0; i < counts.GetSize(); ++i)
    {
        if (TryReadLittleEndian<uint32>({bytes.GetData() + i * sizeof(uint32), sizeof(uint32)}, counts[i]) == false)
        {
            return false;
        }
    }
    if (counts[0] != renderer::LIGHT_TRANSFORMER_DECODER_WEIGHTS || counts[1] < LUDUS_LIGHT_TRANSFORMER_NEIGHBORS)
    {
        return false;
    }
    usize cursor = FIXTURE_HEADER_WORDS * sizeof(uint32);
    const auto read = [&bytes, &cursor]<typename Element>(uint32 count, Array<Element>& outValues) {
        const usize size = usize{count} * sizeof(Element);
        if (size > bytes.GetSize() - cursor || outValues.TryResize(count) == false)
        {
            return false;
        }
        std::memcpy(outValues.GetData(), bytes.GetData() + cursor, size);
        cursor += size;
        return true;
    };
    return read(counts[0], outFixture.Weights) == true && read(counts[1], outFixture.Anchors) == true &&
           read(counts[2], outFixture.Queries) == true && read(counts[2], outFixture.Expected) == true &&
           cursor == bytes.GetSize();
}
Array<uint8> MakeWeightAsset(const Fixture& fixture)
{
    Array<uint8> bytes;
    constexpr usize HEADER_WORDS = LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS;
    if (bytes.TryResize(HEADER_WORDS * sizeof(uint32) + fixture.Weights.GetSize() * sizeof(float32)) == false)
    {
        return bytes;
    }
    const StaticArray<uint32, HEADER_WORDS> header{{LUDUS_LIGHT_TRANSFORMER_FILE_MAGIC,
                                                    LUDUS_LIGHT_TRANSFORMER_FILE_VERSION,
                                                    static_cast<uint32>(fixture.Weights.GetSize()),
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    0}};
    for (usize i = 0; i < header.GetSize(); ++i)
    {
        REQUIRE(TryWriteLittleEndian(header[i], {bytes.GetData() + i * sizeof(uint32), sizeof(uint32)}) == true);
    }
    std::memcpy(bytes.GetData() + HEADER_WORDS * sizeof(uint32),
                fixture.Weights.GetData(),
                fixture.Weights.GetSize() * sizeof(float32));
    return bytes;
}
struct RhiSession final
{
    RhiSession() noexcept
    {
        rhi::Shutdown();
    }
    ~RhiSession() noexcept
    {
        rhi::Shutdown();
    }
    RhiSession(const RhiSession&) = delete;
    RhiSession& operator=(const RhiSession&) = delete;
};
// A compute-capable 8x8 device, or SKIP without a Metal adapter.
void StartComputeDevice(rhi::DeviceHandle& outDevice, rhi::SurfaceHandle& outSurface)
{
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, outDevice, outSurface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
}
// Whether two results hold the same bits in every component, so equal NaN payloads compare equal.
bool AreBitsEqual(const math::Vector4& first, const math::Vector4& second) noexcept
{
    return std::bit_cast<uint32>(first.X) == std::bit_cast<uint32>(second.X) &&
           std::bit_cast<uint32>(first.Y) == std::bit_cast<uint32>(second.Y) &&
           std::bit_cast<uint32>(first.Z) == std::bit_cast<uint32>(second.Z) &&
           std::bit_cast<uint32>(first.W) == std::bit_cast<uint32>(second.W);
}
// Decode the fixture's queries with `shader` on `device` and read the query-major results back.
void DecodeQueries(rhi::DeviceHandle device,
                   rhi::SurfaceHandle surface,
                   const Fixture& fixture,
                   const rhi::RasterShaderDescription& shader,
                   Array<math::Vector4>& outResults)
{
    rhi::ComputeWorkload workload;
    Status status = renderer::PrepareLightTransformerDecoder({fixture.Weights.GetData(), fixture.Weights.GetSize()},
                                                             {fixture.Anchors.GetData(), fixture.Anchors.GetSize()},
                                                             {fixture.Queries.GetData(), fixture.Queries.GetSize()},
                                                             device,
                                                             shader,
                                                             BUDGET,
                                                             workload);
    REQUIRE((status == Status::Ready || status == Status::Pending));
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = workload.Poll();
    }
    REQUIRE(status == Status::Ready);
    rhi::OrderedGraph graph;
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {8, 8, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (status == Status::NotReady && time::NowTicks() - start < TIMEOUT)
    {
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(status == Status::Ready);
    rhi::BufferHandle resultBuffer;
    REQUIRE(workload.GetBuffer(renderer::LIGHT_TRANSFORMER_RESULT_BUFFER, resultBuffer) == Status::Ready);
    REQUIRE(outResults.TryResize(fixture.Queries.GetSize()) == true);
    rhi::ReadbackTicket ticket;
    status = rhi::RequestBufferReadback(device, resultBuffer, 0, outResults.GetSize() * sizeof(math::Vector4), ticket);
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == Status::Ready);
    REQUIRE(rhi::CopyReadback(device,
                              ticket,
                              reinterpret_cast<uint8*>(outResults.GetData()),
                              outResults.GetSize() * sizeof(math::Vector4)) == Status::Ready);
    REQUIRE(rhi::Release(device, ticket) == Status::Ready);
}
} // namespace

TEST_CASE("Light Transformer decoder assets reject corruption and preserve accepted weights",
          "[renderer][light-transformer]")
{
    Fixture fixture;
    REQUIRE(ReadFixture(fixture) == true);
    Array<uint8> bytes = MakeWeightAsset(fixture);
    Array<float32> weights;
    REQUIRE(renderer::ReadLightTransformerDecoderWeights({bytes.GetData(), bytes.GetSize()}, weights) == Status::Ready);
    REQUIRE(weights.GetSize() == fixture.Weights.GetSize());
    for (usize i = 0; i < weights.GetSize(); ++i)
    {
        if (weights[i] != fixture.Weights[i])
        {
            FAIL("Decoder asset changed a binary32 parameter");
        }
    }
    const float32 first = weights[0];
    const auto reject = [&weights, &bytes, first]() {
        CHECK(renderer::ReadLightTransformerDecoderWeights({bytes.GetData(), bytes.GetSize()}, weights) ==
              Status::InvalidDescription);
        CHECK(weights.GetSize() == renderer::LIGHT_TRANSFORMER_DECODER_WEIGHTS);
        CHECK(weights[0] == first);
    };
    bytes[0] ^= 1;
    reject();
    bytes[0] ^= 1;
    bytes[sizeof(uint32)] ^= 1;
    reject();
    bytes[sizeof(uint32)] ^= 1;
    bytes[2 * sizeof(uint32)] ^= 1;
    reject();
    bytes[2 * sizeof(uint32)] ^= 1;
    bytes[3 * sizeof(uint32)] = 1;
    reject();
    bytes[3 * sizeof(uint32)] = 0;
    REQUIRE(TryWriteLittleEndian(uint32{0x7fc00000},
                                 {bytes.GetData() + LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS * sizeof(uint32),
                                  sizeof(uint32)}) == true);
    reject();
    CHECK(renderer::ReadLightTransformerDecoderWeights({bytes.GetData(), bytes.GetSize() - 1}, weights) ==
          Status::InvalidDescription);
    CHECK(renderer::ReadLightTransformerDecoderWeights({nullptr, bytes.GetSize()}, weights) ==
          Status::InvalidDescription);
    rhi::ComputeWorkload empty;
    CHECK(renderer::PrepareLightTransformerDecoder({fixture.Weights.GetData(), fixture.Weights.GetSize() - 1},
                                                   {fixture.Anchors.GetData(), fixture.Anchors.GetSize()},
                                                   {fixture.Queries.GetData(), fixture.Queries.GetSize()},
                                                   {},
                                                   {},
                                                   BUDGET,
                                                   empty) == Status::InvalidDescription);
    CHECK(renderer::PrepareLightTransformerDecoder({fixture.Weights.GetData(), fixture.Weights.GetSize()},
                                                   {fixture.Anchors.GetData(), fixture.Anchors.GetSize()},
                                                   {fixture.Queries.GetData(), fixture.Queries.GetSize()},
                                                   {},
                                                   {},
                                                   1,
                                                   empty) == Status::CapacityExceeded);
}

TEST_CASE("Portable trained query decoder agrees with the independent PyTorch oracle",
          "[renderer][gpu][light-transformer]")
{
    const RhiSession session;
    Fixture fixture;
    REQUIRE(ReadFixture(fixture) == true);
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(device, surface);
    Array<math::Vector4> actual;
    DecodeQueries(device, surface, fixture, ludus::shaders::renderer_light_transformer_decoder::Compute(), actual);
    for (usize i = 0; i < actual.GetSize(); ++i)
    {
        const math::Vector4 expected = fixture.Expected[i];
        INFO("query " << i);
        CHECK(math::IsFinite(actual[i]) == true);
        CHECK(math::Abs(actual[i].X - expected.X) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.X));
        CHECK(math::Abs(actual[i].Y - expected.Y) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.Y));
        CHECK(math::Abs(actual[i].Z - expected.Z) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected.Z));
        CHECK(actual[i].W == 0);
    }
}

TEST_CASE("The LUSL query decoder recipe returns its Slang original's results",
          "[renderer][gpu][light-transformer][lusl]")
{
    // LUSL L5d wave 2: the oracle fixture's anchors and queries decoded by renderer_light_transformer_decoder and by
    // its Slang original (shaders/renderer_light_transformer_decoder.slang) on one device; every result word must
    // match. Both bind the workload's buffers through the LUSL recipe's slots, the Slang original's manual layout.
    static_assert(decoder_slots::NODES == 0 && decoder_slots::WEIGHTS == 1 && decoder_slots::FEATURES == 2 &&
                  decoder_slots::RESULTS == renderer::LIGHT_TRANSFORMER_RESULT_BUFFER);
    const RhiSession session;
    Fixture fixture;
    REQUIRE(ReadFixture(fixture) == true);
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(device, surface);
    Array<math::Vector4> slang;
    Array<math::Vector4> lusl;
    DecodeQueries(device,
                  surface,
                  fixture,
                  ludus::shaders::baseline_renderer_light_transformer_decoder::Compute(),
                  slang);
    DecodeQueries(device, surface, fixture, ludus::shaders::renderer_light_transformer_decoder::Compute(), lusl);
    REQUIRE(lusl.GetSize() == slang.GetSize());
    usize differing = 0;
    usize decoded = 0;
    for (usize i = 0; i < lusl.GetSize(); ++i)
    {
        differing += AreBitsEqual(lusl[i], slang[i]) == false ? 1 : 0;
        decoded += math::IsFinite(lusl[i]) == true && (lusl[i].X != 0 || lusl[i].Y != 0 || lusl[i].Z != 0) ? 1 : 0;
    }
    CHECK(differing == 0);
    // Equality is not vacuous: every query found its neighbourhood and decoded a finite, nonzero prediction.
    CHECK(decoded == lusl.GetSize());
}
