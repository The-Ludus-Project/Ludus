#include "renderer_light_transformer_encoder.h"

#include "../src/internal/light_transformer_topology.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_transformer_encoder.hpp>
#include <ludus/graphics/rhi/profile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
namespace internal = renderer::internal;
using Status = rhi::RasterStatus;
constexpr usize BUDGET = 512 * 1024 * 1024;
constexpr uint64 TIMEOUT = 120 * time::NANOSECONDS_PER_SECOND;
Array<uint8> ReadFile(const char* path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.is_open() == true);
    const std::streampos length = stream.tellg();
    REQUIRE(length > 0);
    REQUIRE(static_cast<usize>(length) < BUDGET);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(length)) == true);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.GetData()), static_cast<std::streamsize>(bytes.GetSize()));
    REQUIRE(stream.good() == true);
    return bytes;
}
} // namespace

TEST_CASE("Portable encoder topology covers sparse duplicates, pooling and partial patches",
          "[renderer][light-transformer]")
{
    internal::EncoderLevel level;
    level.Depth = 12;
    constexpr usize COUNT = 1031;
    REQUIRE(level.CoordinatesGrid.TryResize(COUNT) == true);
    for (usize i = 0; i < COUNT; ++i)
    {
        level.CoordinatesGrid[i] = {static_cast<uint32>(i), 0, 0};
    }
    for (uint32 order = 0; order < 4; ++order)
    {
        Array<uint32> patches;
        REQUIRE(internal::PrepareEncoderPatches(level, order, patches) == Status::Ready);
        for (usize i = 0; i < COUNT; ++i)
        {
            REQUIRE(patches[i * 2 + 1] == 1024);
            const usize begin = patches[i * 2];
            bool hasSelf = false;
            for (usize neighbor = 0; neighbor < 1024; ++neighbor)
            {
                REQUIRE(patches[begin + neighbor] < COUNT);
                hasSelf = hasSelf == true || patches[begin + neighbor] == i;
            }
            REQUIRE(hasSelf == true);
        }
    }
    level.CoordinatesGrid.Resize(3);
    level.CoordinatesGrid[0] = {1, 1, 1};
    level.CoordinatesGrid[1] = {1, 1, 1};
    level.CoordinatesGrid[2] = {2, 1, 1};
    Array<uint32> starts;
    Array<uint32> pairs;
    REQUIRE(internal::PrepareEncoderSparse(level, 3, starts, pairs) == Status::Ready);
    REQUIRE(starts.GetSize() == 4);
    CHECK(starts[0] == 0);
    CHECK(starts[1] == 3);
    CHECK(starts[2] == 6);
    CHECK(starts[3] == 9);
}

TEST_CASE("Portable full encoder agrees with the independent CPU oracle", "[renderer][gpu][light-transformer]")
{
    const char* assetPathOrNull = std::getenv("LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET");
    const char* fixturePathOrNull = std::getenv("LUDUS_LIGHT_TRANSFORMER_ENCODER_FIXTURE");
    const char* assetPath = assetPathOrNull == nullptr ? LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET : assetPathOrNull;
    const char* fixturePath =
        fixturePathOrNull == nullptr ? LUDUS_LIGHT_TRANSFORMER_ENCODER_FIXTURE : fixturePathOrNull;
    Array<uint8> asset = ReadFile(assetPath);
    const Array<uint8> fixture = ReadFile(fixturePath);
    REQUIRE(fixture.GetSize() >= 8);
    uint32 count = 0;
    uint32 anchors = 0;
    REQUIRE(TryReadLittleEndian<uint32>({fixture.GetData(), sizeof(uint32)}, count) == true);
    REQUIRE(TryReadLittleEndian<uint32>({fixture.GetData() + sizeof(uint32), sizeof(uint32)}, anchors) == true);
    REQUIRE(count >= 128);
    REQUIRE(count <= 20000);
    REQUIRE(anchors == count / 4);
    const usize inputBytes = count * sizeof(renderer::LightTransformerScenePoint);
    const usize positionBytes = anchors * sizeof(math::Vector4);
    const usize embeddingBytes = anchors * 128 * sizeof(float32);
    REQUIRE(fixture.GetSize() == 8 + inputBytes + positionBytes + embeddingBytes);
    Array<renderer::LightTransformerScenePoint> points;
    REQUIRE(points.TryResize(count) == true);
    std::memcpy(points.GetData(), fixture.GetData() + 8, inputBytes);
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);

    renderer::LightTransformerEncoding rejected;
    REQUIRE(rejected.AnchorPositionsModel.TryAdd({1, 2, 3}) == true);
    rejected.PacketBytes = 1;
    const auto prepareRejected = [&](BorrowedView<const uint8> bytes, usize budget) {
        return renderer::PrepareLightTransformerEncoder(bytes,
                                                        {points.GetData(), points.GetSize()},
                                                        {0, 0, 0, 1},
                                                        device,
                                                        ludus::shaders::renderer_light_transformer_encoder::Compute(),
                                                        budget,
                                                        rejected);
    };
    CHECK(prepareRejected({asset.GetData(), asset.GetSize() - 1}, BUDGET) == Status::InvalidDescription);
    CHECK(prepareRejected({asset.GetData(), asset.GetSize()}, 1) == Status::CapacityExceeded);
    const uint8 magic = asset[0];
    asset[0] = 0;
    CHECK(prepareRejected({asset.GetData(), asset.GetSize()}, BUDGET) == Status::InvalidDescription);
    asset[0] = magic;
    constexpr usize FIRST_PARAMETER =
        (LUDUS_LIGHT_ENCODER_HEADER_WORDS + LUDUS_LIGHT_ENCODER_LAYER_WORDS * LUDUS_LIGHT_ENCODER_LAYER_COUNT) *
        sizeof(uint32);
    uint32 parameterBits = 0;
    std::memcpy(&parameterBits, asset.GetData() + FIRST_PARAMETER, sizeof(parameterBits));
    constexpr uint32 NAN_BITS = 0x7fc00000;
    REQUIRE(TryWriteLittleEndian<uint32>(NAN_BITS, {asset.GetData() + FIRST_PARAMETER, sizeof(uint32)}) == true);
    CHECK(prepareRejected({asset.GetData(), asset.GetSize()}, BUDGET) == Status::InvalidDescription);
    std::memcpy(asset.GetData() + FIRST_PARAMETER, &parameterBits, sizeof(parameterBits));
    CHECK(rejected.Workload.GetBufferCount() == 0);
    CHECK(rejected.PacketBytes == 1);
    REQUIRE(rejected.AnchorPositionsModel.GetSize() == 1);
    CHECK(rejected.AnchorPositionsModel[0].Z == 3);
    renderer::LightTransformerEncoding encoding;
    Status status =
        renderer::PrepareLightTransformerEncoder({asset.GetData(), asset.GetSize()},
                                                 {points.GetData(), points.GetSize()},
                                                 {0, 0, 0, 1},
                                                 device,
                                                 ludus::shaders::renderer_light_transformer_encoder::Compute(),
                                                 BUDGET,
                                                 encoding);
    REQUIRE((status == Status::Ready || status == Status::Pending));
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = encoding.Workload.Poll();
    }
    REQUIRE(status == Status::Ready);
    rhi::OrderedGraph graph;
    REQUIRE(encoding.Workload.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    CHECK(report.ComputePasses > 300);
    REQUIRE(rhi::SetFrameTarget(device, surface, {8, 8, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (status == Status::NotReady && time::NowTicks() - start < TIMEOUT)
    {
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(status == Status::Ready);

    rhi::BufferHandle resultBuffer;
    REQUIRE(encoding.Workload.GetBuffer(3, resultBuffer) == Status::Ready);
    Array<uint8> actual;
    REQUIRE(actual.TryResize(encoding.PacketBytes) == true);
    usize readOffset = 0;
    while (readOffset < actual.GetSize())
    {
        const usize readBytes = math::Min(rhi::StorageLimits{}.TransferBytes, actual.GetSize() - readOffset);
        rhi::ReadbackTicket ticket;
        status = rhi::RequestBufferReadback(device, resultBuffer, readOffset, readBytes, ticket);
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        {
            REQUIRE(rhi::PollLifetime(device) == Status::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, actual.GetData() + readOffset, readBytes) == Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
        readOffset += readBytes;
    }
    uint32 anchorWord = 0;
    REQUIRE(TryReadLittleEndian<uint32>({actual.GetData() + 3 * sizeof(uint32), sizeof(uint32)}, anchorWord) == true);
    const usize offset = anchorWord * sizeof(math::Vector4);
    REQUIRE(offset + anchors * 33 * sizeof(math::Vector4) == actual.GetSize());
    constexpr float32 ABSOLUTE_TOLERANCE = 0.0005F;
    constexpr float32 RELATIVE_TOLERANCE = 0.0005F;
    for (usize i = 0; i < anchors; ++i)
    {
        math::Vector4 positionModel;
        std::memcpy(&positionModel,
                    fixture.GetData() + 8 + inputBytes + i * sizeof(math::Vector4),
                    sizeof(positionModel));
        REQUIRE(encoding.AnchorPositionsModel[i].X == positionModel.X);
        REQUIRE(encoding.AnchorPositionsModel[i].Y == positionModel.Y);
        REQUIRE(encoding.AnchorPositionsModel[i].Z == positionModel.Z);
        for (usize channel = 0; channel < 128; ++channel)
        {
            float32 expected = 0;
            float32 value = 0;
            std::memcpy(&expected,
                        fixture.GetData() + 8 + inputBytes + positionBytes + (i * 128 + channel) * sizeof(float32),
                        sizeof(float32));
            std::memcpy(&value,
                        actual.GetData() + offset + i * 33 * sizeof(math::Vector4) + sizeof(math::Vector4) +
                            channel * sizeof(float32),
                        sizeof(float32));
            INFO("anchor " << i << " channel " << channel << " actual " << value << " expected " << expected);
            REQUIRE(math::IsFinite(value) == true);
            CHECK(math::Abs(value - expected) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * math::Abs(expected));
        }
    }
}
