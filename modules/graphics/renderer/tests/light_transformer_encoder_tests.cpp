#include "renderer_light_transformer_encoder.h"

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
#include <iostream>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
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
    // This budget admits the complete parameter file but rejects the full workload.
    uint64 estimatedBytes = 0;
    REQUIRE(renderer::PrepareLightTransformerEncoder({asset.GetData(), asset.GetSize()},
                                                     {points.GetData(), points.GetSize()},
                                                     {0, 0, 0, 1},
                                                     device,
                                                     ludus::shaders::renderer_light_transformer_encoder::Compute(),
                                                     asset.GetSize(),
                                                     rejected,
                                                     {},
                                                     {},
                                                     0,
                                                     &estimatedBytes) == Status::CapacityExceeded);
    REQUIRE(estimatedBytes > asset.GetSize());
    Array<renderer::LightTransformerScenePoint> densePoints;
    densePoints.Resize(LUDUS_LIGHT_ENCODER_MAX_POINTS, points[0]);
    // Distinct positions inside a single voxel stress sparse occupancy while
    // allowing the preceding exact kNN searches to prune spatially.
    constexpr usize GRID_SIDE = 32;
    constexpr float32 SPACING = static_cast<float32>(LUDUS_LIGHT_ENCODER_GRID_SIZE) / GRID_SIDE;
    for (usize i = 0; i < densePoints.GetSize(); ++i)
    {
        densePoints[i].PositionModel = {1 + static_cast<float32>(i % GRID_SIDE) * SPACING,
                                        1 + static_cast<float32>((i / GRID_SIDE) % GRID_SIDE) * SPACING,
                                        1 + static_cast<float32>(i / (GRID_SIDE * GRID_SIDE)) * SPACING,
                                        0};
    }
    uint64 denseBytes = 0;
    const uint64 denseStart = time::NowTicks();
    REQUIRE(renderer::PrepareLightTransformerEncoder({asset.GetData(), asset.GetSize()},
                                                     {densePoints.GetData(), densePoints.GetSize()},
                                                     {0, 0, 0, 1},
                                                     device,
                                                     ludus::shaders::renderer_light_transformer_encoder::Compute(),
                                                     asset.GetSize(),
                                                     rejected,
                                                     {},
                                                     {},
                                                     0,
                                                     &denseBytes) == Status::CapacityExceeded);
    constexpr uint64 MAX_ANCHORS = LUDUS_LIGHT_ENCODER_MAX_POINTS / LUDUS_LIGHT_ENCODER_ANCHOR_DIVISOR;
    // Both the stem and the first encoder stage contain a full dense table.
    REQUIRE(denseBytes >= 2 * MAX_ANCHORS * MAX_ANCHORS * 2 * sizeof(uint32));
    std::cout << "dense workload bytes=" << denseBytes << " rejected preparation ns=" << time::NowTicks() - denseStart
              << '\n';
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
    const uint64 preparationStart = time::NowTicks();
    Status status =
        renderer::PrepareLightTransformerEncoder({asset.GetData(), asset.GetSize()},
                                                 {points.GetData(), points.GetSize()},
                                                 {0, 0, 0, 1},
                                                 device,
                                                 ludus::shaders::renderer_light_transformer_encoder::Compute(),
                                                 static_cast<usize>(estimatedBytes),
                                                 encoding);
    std::cout << "accepted encoder preparation ns=" << time::NowTicks() - preparationStart << '\n';
    REQUIRE((status == Status::Ready || status == Status::Pending));
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = encoding.Workload.Poll();
    }
    REQUIRE(status == Status::Ready);
    CHECK(encoding.Workload.GetBufferBytes() == estimatedBytes);
    rhi::OrderedGraph graph;
    REQUIRE(encoding.Workload.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    CHECK(report.ComputePasses > 300);
    REQUIRE(rhi::SetFrameTarget(device, surface, {8, 8, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    const uint64 dispatchStart = time::NowTicks();
    status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (status == Status::NotReady && time::NowTicks() - start < TIMEOUT)
    {
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(status == Status::Ready);

    while (rhi::GetStatus(device, completion) == Status::Pending && time::NowTicks() - dispatchStart < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
    }
    REQUIRE(rhi::GetStatus(device, completion) == Status::Ready);
    std::cout << "encoder submit-to-completion ns=" << time::NowTicks() - dispatchStart << '\n';
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
