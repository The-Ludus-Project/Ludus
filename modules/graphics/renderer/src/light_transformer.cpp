#include <ludus/graphics/renderer/light_transformer.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/profile.h>

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189,
// for the local latent query decoder. The adapter independently implements the
// released parameter/feature contract; no upstream implementation or weights are bundled.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
constexpr usize HEADER_WORDS = LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS;
constexpr usize HEADER_BYTES = HEADER_WORDS * sizeof(uint32);
constexpr usize BUFFER_COUNT = LIGHT_TRANSFORMER_RESULT_BUFFER + 1;
constexpr usize PARAMETER_COUNT = 4;
constexpr usize ANCHOR_WORDS = 1 + LIGHT_TRANSFORMER_DIMENSION / LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
constexpr usize QUERY_WORDS = LUDUS_LIGHT_TRANSFORMER_QUERY_WORDS;
constexpr uint32 FLOAT_EXPONENT_MASK = 0x7f800000;
// Decode through unsigned character object representations, without alignment,
// native byte-order or float-pointer aliasing assumptions.
float32 DecodeFloat(uint32 bits) noexcept
{
    float32 value = 0;
    const uint8* source = reinterpret_cast<const uint8*>(&bits);
    uint8* destination = reinterpret_cast<uint8*>(&value);
    for (usize i = 0; i < sizeof(value); ++i)
    {
        destination[i] = source[i];
    }
    return value;
}
template <typename Element>
BorrowedView<const uint8> GetBytes(BorrowedView<const Element> values) noexcept
{
    return {reinterpret_cast<const uint8*>(values.GetDataOrNull()), values.GetSize() * sizeof(Element)};
}
bool IsValidPosition(math::Vector4 positionModel) noexcept
{
    return math::IsFinite(positionModel) == true && positionModel.W == 0 &&
           math::Abs(positionModel.X) <= geometry::MAX_POINT_SEARCH_COORDINATE &&
           math::Abs(positionModel.Y) <= geometry::MAX_POINT_SEARCH_COORDINATE &&
           math::Abs(positionModel.Z) <= geometry::MAX_POINT_SEARCH_COORDINATE;
}
static_assert(sizeof(LightTransformerAnchor) == ANCHOR_WORDS * sizeof(math::Vector4));
static_assert(sizeof(LightTransformerQuery) == QUERY_WORDS * sizeof(math::Vector4));
} // namespace
Status ReadLightTransformerDecoderWeights(BorrowedView<const uint8> bytes, Array<float32>& outWeights) noexcept
{
    if (bytes.IsValid() == false ||
        bytes.GetSize() != HEADER_BYTES + LIGHT_TRANSFORMER_DECODER_WEIGHTS * sizeof(float32))
    {
        return Status::InvalidDescription;
    }
    StaticArray<uint32, HEADER_WORDS> header{};
    for (usize i = 0; i < HEADER_WORDS; ++i)
    {
        if (TryReadLittleEndian<uint32>({bytes.GetDataOrNull() + i * sizeof(uint32), sizeof(uint32)}, header[i]) ==
            false)
        {
            return Status::InvalidDescription;
        }
    }
    constexpr usize COUNT_FIELD = 2;
    constexpr usize FIRST_RESERVED_FIELD = 3;
    if (header[0] != LUDUS_LIGHT_TRANSFORMER_FILE_MAGIC || header[1] != LUDUS_LIGHT_TRANSFORMER_FILE_VERSION ||
        header[COUNT_FIELD] != LIGHT_TRANSFORMER_DECODER_WEIGHTS)
    {
        return Status::InvalidDescription;
    }
    for (usize i = FIRST_RESERVED_FIELD; i < HEADER_WORDS; ++i)
    {
        if (header[i] != 0)
        {
            return Status::InvalidDescription;
        }
    }
    Array<float32> candidate;
    if (candidate.TryResize(LIGHT_TRANSFORMER_DECODER_WEIGHTS) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < candidate.GetSize(); ++i)
    {
        uint32 bits = 0;
        if (TryReadLittleEndian<uint32>({bytes.GetDataOrNull() + HEADER_BYTES + i * sizeof(uint32), sizeof(uint32)},
                                        bits) == false ||
            (bits & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK)
        {
            return Status::InvalidDescription;
        }
        candidate[i] = DecodeFloat(bits);
    }
    outWeights = Move(candidate);
    return Status::Ready;
}
Status PrepareLightTransformerDecoder(BorrowedView<const float32> weights,
                                      BorrowedView<const LightTransformerAnchor> anchors,
                                      BorrowedView<const LightTransformerQuery> queries,
                                      rhi::DeviceHandle device,
                                      const rhi::RasterShaderDescription& shader,
                                      usize maximumBufferBytes,
                                      rhi::ComputeWorkload& outWorkload) noexcept
{
    if (weights.IsValid() == false || weights.GetSize() != LIGHT_TRANSFORMER_DECODER_WEIGHTS ||
        anchors.IsValid() == false || anchors.GetSize() < LUDUS_LIGHT_TRANSFORMER_NEIGHBORS ||
        anchors.GetSize() > (~uint32{0}) / ANCHOR_WORDS || queries.IsValid() == false || queries.IsEmpty() == true ||
        queries.GetSize() > (~uint32{0}) / QUERY_WORDS)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < weights.GetSize(); ++i)
    {
        if (math::IsFinite(weights[i]) == false)
        {
            return Status::InvalidDescription;
        }
    }
    usize anchorBytes = 0;
    usize queryBytes = 0;
    usize resultBytes = 0;
    usize nodeBytes = 0;
    usize featureBytes = 0;
    usize packetBytes = 0;
    usize totalBytes = weights.GetSize() * sizeof(float32);
    if (TryMultiply(anchors.GetSize(), sizeof(LightTransformerAnchor), anchorBytes) == false ||
        TryMultiply(queries.GetSize(), sizeof(LightTransformerQuery), queryBytes) == false ||
        TryMultiply(queries.GetSize(), sizeof(math::Vector4), resultBytes) == false ||
        TryMultiply(anchors.GetSize(), sizeof(geometry::PointSearchNode), nodeBytes) == false ||
        TryAdd(anchorBytes, queryBytes, featureBytes) == false ||
        TryAdd(nodeBytes, PARAMETER_COUNT * sizeof(uint32), packetBytes) == false ||
        TryAdd(totalBytes, featureBytes, totalBytes) == false || TryAdd(totalBytes, resultBytes, totalBytes) == false ||
        TryAdd(totalBytes, packetBytes, totalBytes) == false || totalBytes > maximumBufferBytes ||
        featureBytes / sizeof(math::Vector4) > ~uint32{0})
    {
        return Status::CapacityExceeded;
    }
    rhi::ComputeCapabilities capabilities;
    const Status supported = rhi::GetComputeCapabilities(device, capabilities);
    if (supported != Status::Ready)
    {
        return supported;
    }
    if (capabilities.MaxDispatch[0] == 0 || capabilities.MaxDispatch[1] == 0 ||
        capabilities.MaxWorkgroupSize[0] < LIGHT_TRANSFORMER_DIMENSION ||
        capabilities.MaxWorkgroupInvocations < LIGHT_TRANSFORMER_DIMENSION ||
        capabilities.MaxStorageBindings < BUFFER_COUNT || featureBytes > capabilities.MaxStorageRange ||
        packetBytes > capabilities.MaxStorageRange || resultBytes > capabilities.MaxStorageRange ||
        weights.GetSize() * sizeof(float32) > capabilities.MaxStorageRange)
    {
        return Status::CapacityExceeded;
    }
    const uint32 groupsX = math::Min(static_cast<uint32>(queries.GetSize()), capabilities.MaxDispatch[0]);
    const uint32 groupsY = static_cast<uint32>((queries.GetSize() - 1) / groupsX + 1);
    if (groupsY > capabilities.MaxDispatch[1])
    {
        return Status::CapacityExceeded;
    }
    Array<math::Vector3> positionsModel;
    Array<math::Vector4> features;
    Array<uint8> nodePacket;
    if (positionsModel.TryResize(anchors.GetSize()) == false ||
        features.TryResize(featureBytes / sizeof(math::Vector4)) == false || nodePacket.TryResize(packetBytes) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < anchors.GetSize(); ++i)
    {
        if (IsValidPosition(anchors[i].PositionModel) == false)
        {
            return Status::InvalidDescription;
        }
        for (const float32 feature : anchors[i].Embedding)
        {
            if (math::IsFinite(feature) == false)
            {
                return Status::InvalidDescription;
            }
        }
        const math::Vector4 positionModel = anchors[i].PositionModel;
        positionsModel[i] = {positionModel.X, positionModel.Y, positionModel.Z};
        features[i * ANCHOR_WORDS] = positionModel;
        for (usize word = 0; word < LIGHT_TRANSFORMER_DIMENSION / LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS; ++word)
        {
            const usize channel = word * LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
            const math::Vector4 embeddingWord
            {
                .X = anchors[i].Embedding[channel],
                .Y = anchors[i].Embedding[channel + 1],
                .Z = anchors[i].Embedding[channel + 2],
                .W = anchors[i].Embedding[channel + LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS - 1],
            };
            features[i * ANCHOR_WORDS + 1 + word] = embeddingWord;
        }
    }
    for (usize i = 0; i < queries.GetSize(); ++i)
    {
        LightTransformerQuery query = queries[i];
        const math::Vector3 normalModel{query.NormalModel.X, query.NormalModel.Y, query.NormalModel.Z};
        math::Vector3 unitNormalModel;
        if (IsValidPosition(query.PositionModel) == false || math::IsFinite(query.NormalModel) == false ||
            query.NormalModel.W != 0 || math::TryNormalize(normalModel, unitNormalModel) != math::MathStatus::Success ||
            math::IsFinite(query.Albedo) == false || query.Albedo.W != 0 || query.Albedo.X < 0 || query.Albedo.X > 1 ||
            query.Albedo.Y < 0 || query.Albedo.Y > 1 || query.Albedo.Z < 0 || query.Albedo.Z > 1)
        {
            return Status::InvalidDescription;
        }
        query.NormalModel = {unitNormalModel.X, unitNormalModel.Y, unitNormalModel.Z, 0};
        const usize firstWord = anchors.GetSize() * ANCHOR_WORDS + i * QUERY_WORDS;
        features[firstWord] = query.PositionModel;
        features[firstWord + 1] = query.NormalModel;
        features[firstWord + 2] = query.Albedo;
    }
    geometry::PointSearchTree tree;
    const geometry::GeometryStatus built = tree.Build({positionsModel.GetData(), positionsModel.GetSize()});
    if (built != geometry::GeometryStatus::Success)
    {
        return built == geometry::GeometryStatus::OutOfMemory ? Status::OutOfMemory : Status::InvalidDescription;
    }
    const StaticArray<uint32, PARAMETER_COUNT> parameters{
        {static_cast<uint32>(queries.GetSize()), static_cast<uint32>(anchors.GetSize()), tree.GetRoot(), groupsX}};
    const BorrowedView<const uint8> parameterBytes =
        GetBytes(BorrowedView<const uint32>{parameters.GetData(), parameters.GetSize()});
    const BorrowedView<const uint8> treeBytes = GetBytes(tree.GetNodes());
    for (usize i = 0; i < parameterBytes.GetSize(); ++i)
    {
        nodePacket[i] = parameterBytes[i];
    }
    for (usize i = 0; i < treeBytes.GetSize(); ++i)
    {
        nodePacket[parameterBytes.GetSize() + i] = treeBytes[i];
    }
    const StaticArray<rhi::ComputeWorkloadBuffer, BUFFER_COUNT> buffers{{
        {packetBytes, {nodePacket.GetData(), nodePacket.GetSize()}, true},
        {weights.GetSize() * sizeof(float32), GetBytes(weights), true},
        {featureBytes, GetBytes(BorrowedView<const math::Vector4>{features.GetData(), features.GetSize()}), true},
        {resultBytes, {}, false},
    }};
    StaticArray<rhi::ComputeWorkloadBinding, BUFFER_COUNT> bindings{};
    for (usize i = 0; i < BUFFER_COUNT; ++i)
    {
        bindings[i] = {static_cast<uint32>(i),
                       static_cast<uint32>(i),
                       0,
                       buffers[i].Size,
                       i == LIGHT_TRANSFORMER_RESULT_BUFFER};
    }
    rhi::ComputeWorkloadKernel kernel;
    kernel.NameOrNull = "Light Transformer query decoder";
    kernel.Shader = shader;
    kernel.Bindings = {bindings.GetData(), bindings.GetSize()};
    kernel.GroupsX = groupsX;
    kernel.GroupsY = groupsY;
    return outWorkload.Initialize(device, {{buffers.GetData(), buffers.GetSize()}, {&kernel, 1}, maximumBufferBytes});
}
} // namespace ludus::graphics::renderer
