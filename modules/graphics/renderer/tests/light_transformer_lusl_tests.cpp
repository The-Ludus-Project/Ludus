#include "baseline_renderer_light_transformer_encoded_queries.h"
#include "baseline_renderer_light_transformer_encoder.h"
#include "recipe_ab_timing.h"
#include "renderer_light_transformer_encoded_queries.h"
#include "renderer_light_transformer_encoded_queries_slots.h"
#include "renderer_light_transformer_encoder.h"
#include "renderer_light_transformer_encoder_slots.h"

#include "../src/internal/light_transformer_encoded_queries.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/detail/point_search_layout.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_transformer.hpp>
#include <ludus/graphics/renderer/light_transformer_encoder.hpp>
#include <ludus/graphics/rhi/profile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>

// LUSL L5d wave 2: the Light Transformer encoder and encoded-query recipes beside their Slang originals
// (shaders/renderer_light_transformer_encoder.slang, shaders/renderer_light_transformer_encoded_queries.slang),
// which stay as differential oracles. Each case runs both programs on one device and requires identical bytes.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
namespace shaders = ludus::shaders;
namespace encoder_slots = ludus::shaders::renderer_light_transformer_encoder::slots;
namespace query_slots = ludus::shaders::renderer_light_transformer_encoded_queries::slots;
using Status = rhi::RasterStatus;
constexpr usize BUDGET = usize{512} * 1024 * 1024;
constexpr uint64 TIMEOUT = 120 * time::NANOSECONDS_PER_SECOND;
constexpr uint32 SURFACE_EXTENT = 8;
constexpr usize ENCODER_TENSOR_BUFFER = 3;
constexpr usize QUERY_RESULT_BUFFER = 2;
constexpr usize QUERY_COUNT = 64;
constexpr usize BENCHMARK_QUERY_COUNT = 4096;
constexpr usize WORD_BYTES = sizeof(uint32);
constexpr usize PACKET_WORD_BYTES = sizeof(math::Vector4);
constexpr usize PACKET_WORD_CHANNELS = PACKET_WORD_BYTES / WORD_BYTES;
constexpr usize ANCHOR_WORDS = LUDUS_LIGHT_ENCODER_ANCHOR_WORDS;
// Header word fields: {magic, anchor count, point-tree root, first anchor word}.
constexpr usize ROOT_FIELD = 2;
constexpr usize FIRST_ANCHOR_FIELD = 3;
// A point-tree node's second word is {left, right, split axis, reserved}; an axis above 2 fails every search.
constexpr usize NODE_AXIS_FIELD = 2;
constexpr uint32 INVALID_AXIS = 3;
constexpr uint32 QUIET_NAN_BITS = 0x7fc00000;
// The Slang original's manual layout: its results take slot 3, after the unused feature binding of the
// unpacked decoder it shares (renderer_light_transformer_decoder.slang).
constexpr renderer::internal::LightTransformerPacketQuerySlots SLANG_QUERY_SLOTS
{
    .Packet = 0,
    .Weights = 1,
    .Results = 3,
};
constexpr float32 QUERY_OFFSET_MODEL = 0.25F;
constexpr float32 QUERY_ALBEDO = 0.5F;

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
// A compute-capable device, optionally with GPU frame timing, or SKIP without a Metal adapter.
void StartComputeDevice(bool shouldTimeFrames, rhi::DeviceHandle& outDevice, rhi::SurfaceHandle& outSurface)
{
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.EnableGpuTiming = shouldTimeFrames;
    const rhi::DeviceStatus started = rhi::CreateDevice({},
                                                        {
                                                            .Width = SURFACE_EXTENT,
                                                            .Height = SURFACE_EXTENT,
                                                        },
                                                        description,
                                                        outDevice,
                                                        outSurface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(outDevice, outSurface, {SURFACE_EXTENT, SURFACE_EXTENT, 0, 0, 0, 1}) ==
            rhi::DeviceStatus::Ready);
}
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
// An input file: the path in environment variable Variable when it is set, else Fallback.
struct InputPath final
{
    const char* Variable = "";
    const char* Fallback = "";
};
const char* GetPath(const InputPath& path) noexcept
{
    const char* pathOrNull = std::getenv(path.Variable);
    return pathOrNull == nullptr ? path.Fallback : pathOrNull;
}
// The encoder fixture's model-space scene points, after its {point count, anchor count} header.
Array<renderer::LightTransformerScenePoint> ReadScenePoints()
{
    const Array<uint8> fixture = ReadFile(GetPath(
    {
        .Variable = "LUDUS_LIGHT_TRANSFORMER_ENCODER_FIXTURE",
        .Fallback = LUDUS_LIGHT_TRANSFORMER_ENCODER_FIXTURE,
    }));
    constexpr usize HEADER_BYTES = 2 * WORD_BYTES;
    REQUIRE(fixture.GetSize() >= HEADER_BYTES);
    uint32 count = 0;
    REQUIRE(TryReadLittleEndian<uint32>({fixture.GetData(), WORD_BYTES}, count) == true);
    REQUIRE(count >= LUDUS_LIGHT_ENCODER_MIN_POINTS);
    REQUIRE(count <= LUDUS_LIGHT_ENCODER_MAX_POINTS);
    const usize pointBytes = usize{count} * sizeof(renderer::LightTransformerScenePoint);
    REQUIRE(fixture.GetSize() >= HEADER_BYTES + pointBytes);
    Array<renderer::LightTransformerScenePoint> points;
    REQUIRE(points.TryResize(count) == true);
    std::memcpy(points.GetData(), fixture.GetData() + HEADER_BYTES, pointBytes);
    return points;
}
void WaitForPreparation(rhi::DeviceHandle device, rhi::ComputeWorkload& inoutWorkload, Status status)
{
    REQUIRE((status == Status::Ready || status == Status::Pending));
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = inoutWorkload.Poll();
    }
    REQUIRE(status == Status::Ready);
}
// Execute every kernel of a ready workload once and wait for the GPU to finish.
void Execute(rhi::DeviceHandle device, rhi::SurfaceHandle surface, const rhi::ComputeWorkload& workload)
{
    rhi::OrderedGraph graph;
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    rhi::SubmissionToken completion;
    const uint64 start = time::NowTicks();
    Status status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (status == Status::NotReady && time::NowTicks() - start < TIMEOUT)
    {
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(status == Status::Ready);
    while (rhi::GetStatus(device, completion) == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
    }
    REQUIRE(rhi::GetStatus(device, completion) == Status::Ready);
}
// Read `size` bytes of `buffer` back in transfer-sized pieces.
void ReadBuffer(rhi::DeviceHandle device, rhi::BufferHandle buffer, usize size, Array<uint8>& outBytes)
{
    REQUIRE(outBytes.TryResize(size) == true);
    const uint64 start = time::NowTicks();
    usize offset = 0;
    while (offset < size)
    {
        const usize bytes = math::Min(rhi::StorageLimits{}.TransferBytes, size - offset);
        rhi::ReadbackTicket ticket;
        Status status = rhi::RequestBufferReadback(device, buffer, offset, bytes, ticket);
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        {
            REQUIRE(rhi::PollLifetime(device) == Status::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outBytes.GetData() + offset, bytes) == Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
        offset += bytes;
    }
}
usize CountDifferingWords(const Array<uint8>& first, const Array<uint8>& second) noexcept
{
    usize differing = 0;
    for (usize i = 0; i + WORD_BYTES <= first.GetSize(); i += WORD_BYTES)
    {
        differing += std::memcmp(first.GetData() + i, second.GetData() + i, WORD_BYTES) != 0 ? 1 : 0;
    }
    return differing;
}
uint32 ReadWord(const Array<uint8>& bytes, usize word)
{
    uint32 value = 0;
    REQUIRE(TryReadLittleEndian<uint32>({bytes.GetData() + word * WORD_BYTES, WORD_BYTES}, value) == true);
    return value;
}
void WriteWord(uint32 value, usize word, Array<uint8>& inoutBytes)
{
    REQUIRE(TryWriteLittleEndian<uint32>(value, {inoutBytes.GetData() + word * WORD_BYTES, WORD_BYTES}) == true);
}

struct EncoderInputs final
{
    Array<uint8> Asset;
    Array<float32> DecoderWeights;
    Array<renderer::LightTransformerScenePoint> Points;
    uint64 EstimatedBytes = 0;
};
void ReadEncoderInputs(rhi::DeviceHandle device, EncoderInputs& outInputs)
{
    outInputs.Asset = ReadFile(GetPath(
        { .Variable = "LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET", .Fallback = LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET }));
    const Array<uint8> decoder = ReadFile(GetPath(
    {
        .Variable = "LUDUS_LIGHT_TRANSFORMER_GRAPH_DECODER_ASSET",
        .Fallback = LUDUS_LIGHT_TRANSFORMER_DECODER_ASSET,
    }));
    REQUIRE(renderer::ReadLightTransformerDecoderWeights({decoder.GetData(), decoder.GetSize()},
                                                         outInputs.DecoderWeights) == Status::Ready);
    outInputs.Points = ReadScenePoints();
    // A budget of the parameter file alone reports the full workload's logical bytes.
    renderer::LightTransformerEncoding rejected;
    REQUIRE(renderer::PrepareLightTransformerEncoder({outInputs.Asset.GetData(), outInputs.Asset.GetSize()},
                                                     {outInputs.Points.GetData(), outInputs.Points.GetSize()},
                                                     {0, 0, 0, 1},
                                                     device,
                                                     shaders::renderer_light_transformer_encoder::Compute(),
                                                     outInputs.Asset.GetSize(),
                                                     rejected,
                                                     {},
                                                     {},
                                                     0,
                                                     &outInputs.EstimatedBytes) == Status::CapacityExceeded);
    REQUIRE(outInputs.EstimatedBytes > outInputs.Asset.GetSize());
}
// Prepare the encoder with `shader` and wait until it is ready to execute.
void PrepareEncoder(rhi::DeviceHandle device,
                    const EncoderInputs& inputs,
                    const rhi::RasterShaderDescription& shader,
                    renderer::LightTransformerEncoding& outEncoding)
{
    const Status status = renderer::PrepareLightTransformerEncoder({inputs.Asset.GetData(), inputs.Asset.GetSize()},
                                                                   {inputs.Points.GetData(), inputs.Points.GetSize()},
                                                                   {0, 0, 0, 1},
                                                                   device,
                                                                   shader,
                                                                   static_cast<usize>(inputs.EstimatedBytes),
                                                                   outEncoding);
    WaitForPreparation(device, outEncoding.Workload, status);
}
// Encode the scene with `shader` and read the packet back.
void EncodeScene(rhi::DeviceHandle device,
                 rhi::SurfaceHandle surface,
                 const EncoderInputs& inputs,
                 const rhi::RasterShaderDescription& shader,
                 renderer::LightTransformerEncoding& outEncoding,
                 Array<uint8>& outPacket)
{
    PrepareEncoder(device, inputs, shader, outEncoding);
    Execute(device, surface, outEncoding.Workload);
    rhi::BufferHandle packet;
    REQUIRE(outEncoding.Workload.GetBuffer(ENCODER_TENSOR_BUFFER, packet) == Status::Ready);
    ReadBuffer(device, packet, outEncoding.PacketBytes, outPacket);
}
// One query a quarter model unit beside each of the first `count` anchors, cycling through them.
Array<renderer::LightTransformerQuery> MakeQueries(const renderer::LightTransformerEncoding& encoding, usize count)
{
    REQUIRE(encoding.AnchorPositionsModel.IsEmpty() == false);
    Array<renderer::LightTransformerQuery> queries;
    REQUIRE(queries.TryResize(count) == true);
    for (usize i = 0; i < count; ++i)
    {
        const math::Vector3 anchor = encoding.AnchorPositionsModel[i % encoding.AnchorPositionsModel.GetSize()];
        queries[i] = {{anchor.X + QUERY_OFFSET_MODEL, anchor.Y, anchor.Z, 0},
                      {0, 0, 1, 0},
                      {QUERY_ALBEDO, QUERY_ALBEDO, QUERY_ALBEDO, 0}};
    }
    return queries;
}
// Prepare `queries` against `packet` with `shader` bound at `slots`.
void PrepareQueries(rhi::DeviceHandle device,
                    const EncoderInputs& inputs,
                    rhi::BufferHandle packet,
                    usize packetBytes,
                    const Array<renderer::LightTransformerQuery>& queries,
                    const rhi::RasterShaderDescription& shader,
                    const renderer::internal::LightTransformerPacketQuerySlots& slots,
                    rhi::ComputeWorkload& outWorkload)
{
    const Status status = renderer::internal::PrepareLightTransformerPacketQueries(
        {inputs.DecoderWeights.GetData(), inputs.DecoderWeights.GetSize()},
        packet,
        packetBytes,
        {queries.GetData(), queries.GetSize()},
        device,
        shader,
        slots,
        BUDGET,
        outWorkload);
    WaitForPreparation(device, outWorkload, status);
}
// Decode `queries` against `packet` and read the query-major float4 results back.
void DecodePacket(rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  const EncoderInputs& inputs,
                  rhi::BufferHandle packet,
                  usize packetBytes,
                  const Array<renderer::LightTransformerQuery>& queries,
                  const rhi::RasterShaderDescription& shader,
                  const renderer::internal::LightTransformerPacketQuerySlots& slots,
                  Array<uint8>& outResults)
{
    rhi::ComputeWorkload workload;
    PrepareQueries(device, inputs, packet, packetBytes, queries, shader, slots, workload);
    Execute(device, surface, workload);
    rhi::BufferHandle results;
    REQUIRE(workload.GetBuffer(QUERY_RESULT_BUFFER, results) == Status::Ready);
    ReadBuffer(device, results, queries.GetSize() * PACKET_WORD_BYTES, outResults);
}
// A device buffer holding `bytes`: the tensor arena of an encoder workload whose one dispatch record has a zero
// point count, so its kernel returns at once and the uploaded bytes stay as they are.
void UploadPacket(rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  const Array<uint8>& bytes,
                  rhi::ComputeWorkload& outCarrier,
                  rhi::BufferHandle& outPacket)
{
    const StaticArray<uint32, LUDUS_LIGHT_ENCODER_DISPATCH_WORDS> dispatch{};
    const StaticArray<float32, PACKET_WORD_CHANNELS> weights{};
    const StaticArray<uint32, PACKET_WORD_CHANNELS> topology{};
    const StaticArray<rhi::ComputeWorkloadBuffer, ENCODER_TENSOR_BUFFER + 1> buffers{
        {{sizeof(dispatch), {reinterpret_cast<const uint8*>(dispatch.GetData()), sizeof(dispatch)}, true},
         {sizeof(weights), {reinterpret_cast<const uint8*>(weights.GetData()), sizeof(weights)}, true},
         {sizeof(topology), {reinterpret_cast<const uint8*>(topology.GetData()), sizeof(topology)}, true},
         {bytes.GetSize(), {bytes.GetData(), bytes.GetSize()}, false}}};
    const StaticArray<rhi::ComputeWorkloadBinding, ENCODER_TENSOR_BUFFER + 1> bindings{
        {{encoder_slots::DISPATCH, 0, 0, sizeof(dispatch), false},
         {encoder_slots::WEIGHTS, 1, 0, sizeof(weights), false},
         {encoder_slots::TOPOLOGY, 2, 0, sizeof(topology), false},
         {encoder_slots::TENSORS, ENCODER_TENSOR_BUFFER, 0, bytes.GetSize(), true}}};
    rhi::ComputeWorkloadKernel kernel;
    kernel.NameOrNull = "Light Transformer packet upload";
    kernel.Shader = shaders::renderer_light_transformer_encoder::Compute();
    kernel.Bindings = {bindings.GetData(), bindings.GetSize()};
    const Status status = outCarrier.Initialize(device, {{buffers.GetData(), buffers.GetSize()}, {&kernel, 1}, BUDGET});
    WaitForPreparation(device, outCarrier, status);
    Execute(device, surface, outCarrier);
    REQUIRE(outCarrier.GetBuffer(ENCODER_TENSOR_BUFFER, outPacket) == Status::Ready);
    Array<uint8> uploaded;
    ReadBuffer(device, outPacket, bytes.GetSize(), uploaded);
    REQUIRE(CountDifferingWords(uploaded, bytes) == 0);
}
// Decode `queries` against `packetBytes` with both programs, require identical results, and count the queries
// whose result is the decoder's quiet-NaN failure record.
usize CountFailedQueriesOfBothPrograms(rhi::DeviceHandle device,
                                       rhi::SurfaceHandle surface,
                                       const EncoderInputs& inputs,
                                       const Array<uint8>& packetBytes,
                                       const Array<renderer::LightTransformerQuery>& queries)
{
    rhi::ComputeWorkload carrier;
    rhi::BufferHandle packet;
    UploadPacket(device, surface, packetBytes, carrier, packet);
    Array<uint8> slang;
    Array<uint8> lusl;
    DecodePacket(device,
                 surface,
                 inputs,
                 packet,
                 packetBytes.GetSize(),
                 queries,
                 shaders::baseline_renderer_light_transformer_encoded_queries::Compute(),
                 SLANG_QUERY_SLOTS,
                 slang);
    DecodePacket(device,
                 surface,
                 inputs,
                 packet,
                 packetBytes.GetSize(),
                 queries,
                 shaders::renderer_light_transformer_encoded_queries::Compute(),
                 { .Packet = query_slots::PACKET, .Weights = query_slots::WEIGHTS, .Results = query_slots::RESULTS },
                 lusl);
    CHECK(CountDifferingWords(lusl, slang) == 0);
    usize failed = 0;
    for (usize i = 0; i < queries.GetSize(); ++i)
    {
        const bool isFailure = ReadWord(lusl, i * PACKET_WORD_CHANNELS) == QUIET_NAN_BITS &&
                               ReadWord(lusl, i * PACKET_WORD_CHANNELS + 1) == 0 &&
                               ReadWord(lusl, i * PACKET_WORD_CHANNELS + 2) == 0 &&
                               ReadWord(lusl, i * PACKET_WORD_CHANNELS + PACKET_WORD_CHANNELS - 1) == 0;
        failed += isFailure == true ? 1 : 0;
    }
    return failed;
}
} // namespace

TEST_CASE("The LUSL Light Transformer encoder and encoded-query recipes return their Slang originals' bytes",
          "[renderer][gpu][light-transformer][lusl]")
{
    // The encoder binds the LUSL recipe's slots, which are the Slang original's manual layout.
    static_assert(encoder_slots::DISPATCH == 0 && encoder_slots::WEIGHTS == 1 && encoder_slots::TOPOLOGY == 2 &&
                  encoder_slots::TENSORS == ENCODER_TENSOR_BUFFER);
    // The encoded-query recipe declares only its three buffers, so its results move from the original's slot 3.
    static_assert(query_slots::PACKET == SLANG_QUERY_SLOTS.Packet &&
                  query_slots::WEIGHTS == SLANG_QUERY_SLOTS.Weights && query_slots::RESULTS == 2);
    const RhiSession session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(false, device, surface);
    EncoderInputs inputs;
    ReadEncoderInputs(device, inputs);

    renderer::LightTransformerEncoding slangEncoding;
    renderer::LightTransformerEncoding luslEncoding;
    Array<uint8> slangPacket;
    Array<uint8> luslPacket;
    EncodeScene(device,
                surface,
                inputs,
                shaders::baseline_renderer_light_transformer_encoder::Compute(),
                slangEncoding,
                slangPacket);
    EncodeScene(device,
                surface,
                inputs,
                shaders::renderer_light_transformer_encoder::Compute(),
                luslEncoding,
                luslPacket);
    REQUIRE(luslPacket.GetSize() == slangPacket.GetSize());
    CHECK(CountDifferingWords(luslPacket, slangPacket) == 0);
    // Equality is not vacuous: the packet carries the encoder's magic and finite, nonzero latents for every anchor.
    REQUIRE(ReadWord(luslPacket, 0) == LUDUS_LIGHT_ENCODER_PACKET_MAGIC);
    const usize anchors = luslEncoding.AnchorPositionsModel.GetSize();
    const usize firstAnchorWord = ReadWord(luslPacket, FIRST_ANCHOR_FIELD);
    REQUIRE((firstAnchorWord + anchors * ANCHOR_WORDS) * PACKET_WORD_BYTES == luslPacket.GetSize());
    usize nonzeroAnchors = 0;
    usize finiteLatents = 0;
    for (usize anchor = 0; anchor < anchors; ++anchor)
    {
        bool hasNonzeroLatent = false;
        for (usize channel = 0; channel < renderer::LIGHT_TRANSFORMER_DIMENSION; ++channel)
        {
            float32 latent = 0;
            std::memcpy(&latent,
                        luslPacket.GetData() +
                            ((firstAnchorWord + anchor * ANCHOR_WORDS + 1) * PACKET_WORD_CHANNELS + channel) *
                                WORD_BYTES,
                        sizeof(latent));
            finiteLatents += math::IsFinite(latent) == true ? 1 : 0;
            hasNonzeroLatent = hasNonzeroLatent == true || latent != 0;
        }
        nonzeroAnchors += hasNonzeroLatent == true ? 1 : 0;
    }
    CHECK(finiteLatents == anchors * renderer::LIGHT_TRANSFORMER_DIMENSION);
    CHECK(nonzeroAnchors == anchors);

    // Both query programs decode the LUSL encoder's GPU packet.
    rhi::BufferHandle packet;
    REQUIRE(luslEncoding.Workload.GetBuffer(ENCODER_TENSOR_BUFFER, packet) == Status::Ready);
    const Array<renderer::LightTransformerQuery> queries = MakeQueries(luslEncoding, QUERY_COUNT);
    Array<uint8> slangResults;
    Array<uint8> luslResults;
    DecodePacket(device,
                 surface,
                 inputs,
                 packet,
                 luslEncoding.PacketBytes,
                 queries,
                 shaders::baseline_renderer_light_transformer_encoded_queries::Compute(),
                 SLANG_QUERY_SLOTS,
                 slangResults);
    rhi::ComputeWorkload publicWorkload;
    // The public entry point binds the LUSL recipe's generated slots.
    WaitForPreparation(device,
                       publicWorkload,
                       renderer::PrepareLightTransformerEncodedQueries(
                           {inputs.DecoderWeights.GetData(), inputs.DecoderWeights.GetSize()},
                           luslEncoding,
                           {queries.GetData(), queries.GetSize()},
                           device,
                           shaders::renderer_light_transformer_encoded_queries::Compute(),
                           BUDGET,
                           publicWorkload));
    Execute(device, surface, publicWorkload);
    rhi::BufferHandle results;
    REQUIRE(publicWorkload.GetBuffer(QUERY_RESULT_BUFFER, results) == Status::Ready);
    ReadBuffer(device, results, queries.GetSize() * PACKET_WORD_BYTES, luslResults);
    CHECK(CountDifferingWords(luslResults, slangResults) == 0);
    // Equality is not vacuous: every query decodes a finite, nonzero prediction with W zero.
    usize decoded = 0;
    for (usize i = 0; i < queries.GetSize(); ++i)
    {
        math::Vector4 prediction;
        std::memcpy(&prediction, luslResults.GetData() + i * PACKET_WORD_BYTES, sizeof(prediction));
        decoded += math::IsFinite(prediction) == true && prediction.W == 0 &&
                           (prediction.X != 0 || prediction.Y != 0 || prediction.Z != 0)
                       ? 1
                       : 0;
    }
    CHECK(decoded == queries.GetSize());

    // Failure paths. A packet the encoder did not write fails every query before the first barrier; a tree whose
    // root splits on no axis fails every neighbour search, which the LUSL decoder reports after decoding anchor 0.
    Array<uint8> foreignPacket;
    REQUIRE(foreignPacket.TryResize(luslPacket.GetSize()) == true);
    std::memcpy(foreignPacket.GetData(), luslPacket.GetData(), luslPacket.GetSize());
    WriteWord(ReadWord(foreignPacket, 0) ^ 1, 0, foreignPacket);
    CHECK(CountFailedQueriesOfBothPrograms(device, surface, inputs, foreignPacket, queries) == queries.GetSize());
    Array<uint8> brokenTree;
    REQUIRE(brokenTree.TryResize(luslPacket.GetSize()) == true);
    std::memcpy(brokenTree.GetData(), luslPacket.GetData(), luslPacket.GetSize());
    const usize root = ReadWord(brokenTree, ROOT_FIELD);
    const usize rootChildrenWord = LUDUS_LIGHT_ENCODER_PACKET_HEADER_WORDS + root * LUDUS_POINT_SEARCH_NODE_WORDS + 1;
    WriteWord(INVALID_AXIS, rootChildrenWord * PACKET_WORD_CHANNELS + NODE_AXIS_FIELD, brokenTree);
    CHECK(CountFailedQueriesOfBothPrograms(device, surface, inputs, brokenTree, queries) == queries.GetSize());
    // The control: the unmodified packet uploaded the same way decodes every query.
    CHECK(CountFailedQueriesOfBothPrograms(device, surface, inputs, luslPacket, queries) == 0);
}

TEST_CASE("The LUSL Light Transformer encoder and encoded-query recipes time like their Slang originals",
          "[.benchmark][renderer][gpu][light-transformer][lusl]")
{
    // Hidden: the Linux Vulkan validation step requires a default run without skipped cases. Run it with
    // LUDUS_RENDERER_BENCHMARK=1 ludus_light_transformer_render_tests "[benchmark][light-transformer]".
    // In-process A/B GPU timing (LUDUS_RENDERER_BENCHMARK=1): the whole encoder workload, its hottest program, and
    // a batch of encoded queries, each Slang original against its LUSL recipe on one device.
    if (renderer::testing::IsRecipeBenchmark() == false)
    {
        SKIP("Set LUDUS_RENDERER_BENCHMARK=1 to time the Light Transformer recipes");
    }
    const RhiSession session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(true, device, surface);
    EncoderInputs inputs;
    ReadEncoderInputs(device, inputs);
    StaticArray<renderer::LightTransformerEncoding, renderer::testing::RECIPE_AB_VARIANTS> encodings;
    PrepareEncoder(device,
                   inputs,
                   shaders::baseline_renderer_light_transformer_encoder::Compute(),
                   encodings[renderer::testing::RECIPE_AB_SLANG]);
    PrepareEncoder(device,
                   inputs,
                   shaders::renderer_light_transformer_encoder::Compute(),
                   encodings[renderer::testing::RECIPE_AB_LUSL]);
    renderer::testing::RunRecipeAbTiming(device,
                                         "light_transformer_encoder",
                                         static_cast<uint32>(inputs.Points.GetSize()),
                                         1,
                                         [&](usize variant) { Execute(device, surface, encodings[variant].Workload); });

    rhi::BufferHandle packet;
    REQUIRE(encodings[renderer::testing::RECIPE_AB_LUSL].Workload.GetBuffer(ENCODER_TENSOR_BUFFER, packet) ==
            Status::Ready);
    const Array<renderer::LightTransformerQuery> queries =
        MakeQueries(encodings[renderer::testing::RECIPE_AB_LUSL], BENCHMARK_QUERY_COUNT);
    StaticArray<rhi::ComputeWorkload, renderer::testing::RECIPE_AB_VARIANTS> decoders;
    PrepareQueries(device,
                   inputs,
                   packet,
                   encodings[renderer::testing::RECIPE_AB_LUSL].PacketBytes,
                   queries,
                   shaders::baseline_renderer_light_transformer_encoded_queries::Compute(),
                   SLANG_QUERY_SLOTS,
                   decoders[renderer::testing::RECIPE_AB_SLANG]);
    PrepareQueries(device,
                   inputs,
                   packet,
                   encodings[renderer::testing::RECIPE_AB_LUSL].PacketBytes,
                   queries,
                   shaders::renderer_light_transformer_encoded_queries::Compute(),
                   { .Packet = query_slots::PACKET, .Weights = query_slots::WEIGHTS, .Results = query_slots::RESULTS },
                   decoders[renderer::testing::RECIPE_AB_LUSL]);
    renderer::testing::RunRecipeAbTiming(device,
                                         "light_transformer_encoded_queries",
                                         static_cast<uint32>(queries.GetSize()),
                                         1,
                                         [&](usize variant) { Execute(device, surface, decoders[variant]); });
}
