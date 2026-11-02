#include <ludus/graphics/renderer/light_transformer_encoder.hpp>

#include "internal/light_transformer_topology.h"

#include <ludus/content/render_graph_format/detail/light_transformer_encoder_layout.h>
#include <ludus/content/render_graph_format/detail/light_transformer_layout.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/graphics/rhi/profile.h>

#include <cstring>

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189,
// and Xiaoyang Wu et al., "Point Transformer V3: Simpler, Faster, Stronger",
// CVPR 2024, https://arxiv.org/abs/2312.10035. Independently schedule their released
// encoder with fixed point orders and binary32 attention. No training runtime,
// upstream source or trained assets ship in Ludus. Geometry-only preparation
// belongs to light_transformer_topology.cpp; all learned operations run on GPU.
namespace ludus::graphics::renderer
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
constexpr usize WORD_BYTES = sizeof(uint32);
constexpr uint32 WORD_CHANNELS = LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
constexpr usize LAYER_WORDS = LUDUS_LIGHT_ENCODER_LAYER_WORDS;
constexpr usize DISPATCH_WORDS = LUDUS_LIGHT_ENCODER_DISPATCH_WORDS;
constexpr usize HEADER_WORDS = LUDUS_LIGHT_ENCODER_HEADER_WORDS;
constexpr usize BUFFER_COUNT = 4;
constexpr usize ARENA_BUFFER = 3;
constexpr usize WEIGHT_CHUNK_BYTES = 64 * 1024 * 1024;
constexpr uint32 AXES = 3;
constexpr uint32 CPE_KERNEL_SIZE = 3;
constexpr uint32 STEM_KERNEL_SIZE = 5;
constexpr uint32 FEED_FORWARD_RATIO = 4;
constexpr uint32 NO_TENSOR = ~uint32{0};
constexpr uint32 FLOAT_EXPONENT_MASK = 0x7f800000;
constexpr uint32 DIMENSION = LUDUS_LIGHT_TRANSFORMER_DIMENSION;
constexpr uint32 FIRST_CHANNELS = 32;
constexpr uint32 SECOND_CHANNELS = FIRST_CHANNELS * 2;
constexpr usize DESTINATION_FIELD = LUDUS_LIGHT_ENCODER_DISPATCH_DESTINATION;
constexpr usize SOURCE_FIELD = LUDUS_LIGHT_ENCODER_DISPATCH_SOURCE;
constexpr usize OTHER_FIELD = LUDUS_LIGHT_ENCODER_DISPATCH_OTHER;
constexpr usize GROUPS_X_FIELD = LUDUS_LIGHT_ENCODER_DISPATCH_GROUPS_X;
constexpr usize PACKET_HEADER_SCALARS = LUDUS_LIGHT_ENCODER_PACKET_HEADER_WORDS * WORD_CHANNELS;
using Instruction = StaticArray<uint32, DISPATCH_WORDS>;
using LayerRecord = StaticArray<uint32, LAYER_WORDS>;
constexpr StaticArray<uint32, LUDUS_LIGHT_ENCODER_LEVELS> ENCODER_CHANNELS{
    {DIMENSION, DIMENSION, DIMENSION, DIMENSION * 2, DIMENSION * 2}};
constexpr StaticArray<uint32, LUDUS_LIGHT_ENCODER_LEVELS> ENCODER_DEPTHS{
    {2, 2, 2, LUDUS_LIGHT_ENCODER_MIDDLE_STAGE_DEPTH, 2}};
constexpr usize DECODER_LEVELS = LUDUS_LIGHT_ENCODER_LEVELS - 1;
constexpr StaticArray<uint32, DECODER_LEVELS> DECODER_CHANNELS{{DIMENSION, DIMENSION, DIMENSION * 2, DIMENSION * 2}};
struct Layer final
{
    LayerRecord Record{};
    uint32 Offset = 0;
    uint32 Buffer = 1;
};
struct Tensor final
{
    uint32 Count = 0;
    uint32 Channels = 0;
    uint32 FirstUse = 0;
    uint32 LastUse = 0;
    uint32 Offset = 0;
};
struct Model final
{
    Array<Layer> Layers;
    Array<Array<float32>> Chunks;
};
template <typename Element>
BorrowedView<const uint8> GetBytes(BorrowedView<const Element> values) noexcept
{
    return {reinterpret_cast<const uint8*>(values.GetDataOrNull()), values.GetSize() * sizeof(Element)};
}
uint32 ReadWord(BorrowedView<const uint8> bytes, usize word) noexcept
{
    uint32 value = 0;
    const bool isRead = TryReadLittleEndian<uint32>({bytes.GetDataOrNull() + word * WORD_BYTES, WORD_BYTES}, value);
    LUDUS_ASSERT(isRead == true);
    return value;
}
float32 DecodeFloat(uint32 bits) noexcept
{
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
uint32 EncodeFloat(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
Status ReadModel(BorrowedView<const uint8> asset, usize alignment, usize maximumBytes, Model& outModel) noexcept
{
    if (asset.IsValid() == false || asset.GetSize() < HEADER_WORDS * WORD_BYTES ||
        ReadWord(asset, 0) != LUDUS_LIGHT_ENCODER_MAGIC || ReadWord(asset, 1) != LUDUS_LIGHT_ENCODER_VERSION)
    {
        return Status::InvalidDescription;
    }
    const uint32 layers = ReadWord(asset, 2);
    constexpr usize PARAMETER_COUNT_FIELD = 3;
    const uint32 parameters = ReadWord(asset, PARAMETER_COUNT_FIELD);
    const usize metadataWords = HEADER_WORDS + usize{layers} * LAYER_WORDS;
    constexpr usize RESERVED_FIELD = 4;
    for (usize i = RESERVED_FIELD; i < HEADER_WORDS; ++i)
    {
        if (ReadWord(asset, i) != 0)
        {
            return Status::InvalidDescription;
        }
    }
    if (layers != LUDUS_LIGHT_ENCODER_LAYER_COUNT || parameters != LUDUS_LIGHT_ENCODER_WEIGHT_COUNT ||
        asset.GetSize() != (metadataWords + parameters) * WORD_BYTES)
    {
        return Status::InvalidDescription;
    }
    if (asset.GetSize() > maximumBytes)
    {
        return Status::CapacityExceeded;
    }
    if (outModel.Layers.TryResize(layers) == false || outModel.Chunks.TryResize(1) == false)
    {
        return Status::OutOfMemory;
    }
    usize cursor = 0;
    constexpr usize OFFSET_FIELD = LUDUS_LIGHT_ENCODER_LAYER_OFFSET;
    constexpr usize LENGTH_FIELD = LUDUS_LIGHT_ENCODER_LAYER_LENGTH;
    constexpr usize FIRST_RESERVED_LAYER_FIELD = LUDUS_LIGHT_ENCODER_LAYER_RESERVED_FIRST;
    const usize alignmentScalars = math::Max(alignment / sizeof(float32), usize{1});
    for (usize i = 0; i < layers; ++i)
    {
        Layer& layer = outModel.Layers[i];
        for (usize field = 0; field < LAYER_WORDS; ++field)
        {
            layer.Record[field] = ReadWord(asset, HEADER_WORDS + i * LAYER_WORDS + field);
        }
        for (usize field = FIRST_RESERVED_LAYER_FIELD; field < LAYER_WORDS; ++field)
        {
            if (layer.Record[field] != 0)
            {
                return Status::InvalidDescription;
            }
        }
        const uint32 kind = layer.Record[LUDUS_LIGHT_ENCODER_LAYER_KIND];
        const uint32 inputs = layer.Record[LUDUS_LIGHT_ENCODER_LAYER_INPUTS];
        const uint32 outputs = layer.Record[LUDUS_LIGHT_ENCODER_LAYER_OUTPUTS];
        const uint32 kernel = layer.Record[LUDUS_LIGHT_ENCODER_LAYER_KERNEL];
        if (inputs == 0 || inputs > DIMENSION * FEED_FORWARD_RATIO * 2 || outputs == 0 ||
            outputs > DIMENSION * FEED_FORWARD_RATIO * 2 || kernel == 0 || kernel > STEM_KERNEL_SIZE ||
            kind < LUDUS_LIGHT_ENCODER_AFFINE || kind > LUDUS_LIGHT_ENCODER_POOL)
        {
            return Status::InvalidDescription;
        }
        const usize expected = kind == LUDUS_LIGHT_ENCODER_NORMALIZE
                                   ? usize{outputs} * 2
                                   : usize{inputs} * outputs * kernel * kernel * kernel + outputs +
                                         (kind == LUDUS_LIGHT_ENCODER_POOL ? usize{outputs} * 2 : 0);
        if (layer.Record[OFFSET_FIELD] != cursor || layer.Record[LENGTH_FIELD] != expected ||
            expected > parameters - math::Min(cursor, usize{parameters}))
        {
            return Status::InvalidDescription;
        }
        Array<float32>* chunk = &outModel.Chunks.GetLast();
        usize offset = (chunk->GetSize() + alignmentScalars - 1) / alignmentScalars * alignmentScalars;
        if ((offset + expected) * sizeof(float32) > WEIGHT_CHUNK_BYTES)
        {
            if (outModel.Chunks.TryAdd(Array<float32>{}) == false)
            {
                return Status::OutOfMemory;
            }
            chunk = &outModel.Chunks.GetLast();
            offset = 0;
        }
        if (chunk->TryResize(offset + expected) == false)
        {
            return Status::OutOfMemory;
        }
        layer.Offset = static_cast<uint32>(offset);
        layer.Buffer = outModel.Chunks.GetSize() == 1 ? 1 : static_cast<uint32>(outModel.Chunks.GetSize() + 2);
        for (usize parameter = 0; parameter < expected; ++parameter)
        {
            const uint32 bits = ReadWord(asset, metadataWords + cursor + parameter);
            if ((bits & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK)
            {
                return Status::InvalidDescription;
            }
            (*chunk)[offset + parameter] = DecodeFloat(bits);
        }
        cursor += expected;
    }
    return cursor == parameters ? Status::Ready : Status::InvalidDescription;
}
struct Program final
{
    Model Parameters;
    Array<Instruction> Instructions;
    Array<uint32> WeightBuffers;
    Array<uint32> Topology;
    Array<Tensor> Tensors;
    usize NextLayer = 0;
    Status Result = Status::Ready;
    uint32 PacketScalars = 0;
    uint32 PacketPrefixScalars = 0;
    uint32 AnchorOffset = 0;
    uint32 PacketTopology = 0;
    uint32 PositionTopology = 0;
    uint32 DecoderTopology = 0;
    uint32 DecoderOffset = 0;

    uint32 AppendTopology(BorrowedView<const uint32> words) noexcept
    {
        const uint32 offset = static_cast<uint32>(Topology.GetSize());
        if (Topology.TryResize(Topology.GetSize() + words.GetSize()) == false)
        {
            Result = Status::OutOfMemory;
        }
        if (Result == Status::Ready)
        {
            for (usize i = 0; i < words.GetSize(); ++i)
            {
                Topology[offset + i] = words[i];
            }
        }
        return offset;
    }
    Layer TakeLayer(uint32 kind, uint32 inputs, uint32 outputs, uint32 kernel = 1) noexcept
    {
        if (Result != Status::Ready)
        {
            return {};
        }
        if (NextLayer >= Parameters.Layers.GetSize())
        {
            Result = Status::InvalidDescription;
            return {};
        }
        const Layer layer = Parameters.Layers[NextLayer++];
        if (layer.Record[LUDUS_LIGHT_ENCODER_LAYER_KIND] != kind ||
            layer.Record[LUDUS_LIGHT_ENCODER_LAYER_INPUTS] != inputs ||
            layer.Record[LUDUS_LIGHT_ENCODER_LAYER_OUTPUTS] != outputs ||
            layer.Record[LUDUS_LIGHT_ENCODER_LAYER_KERNEL] != kernel)
        {
            Result = Status::InvalidDescription;
        }
        return layer;
    }
    uint32 Add(uint32 kind,
               uint32 count,
               uint32 inputs,
               uint32 outputs,
               uint32 source,
               Layer layer = {},
               uint32 activation = 0,
               uint32 indices = 0,
               uint32 pairs = 0,
               uint32 other = NO_TENSOR,
               uint32 parameter = 0,
               uint32 auxiliaryWeights = 0) noexcept
    {
        if (Result != Status::Ready)
        {
            return 0;
        }
        if (Instructions.GetSize() >= LUDUS_LIGHT_ENCODER_MAX_DISPATCHES)
        {
            Result = Status::CapacityExceeded;
            return 0;
        }
        const uint32 destination = static_cast<uint32>(Tensors.GetSize());
        const uint32 instruction = static_cast<uint32>(Instructions.GetSize());
        const Instruction words{{kind,
                                 count,
                                 inputs,
                                 outputs,
                                 source,
                                 destination,
                                 layer.Offset,
                                 auxiliaryWeights,
                                 indices,
                                 pairs,
                                 other,
                                 activation,
                                 parameter,
                                 0,
                                 0,
                                 0}};
        if (Tensors.TryAdd({count, outputs, instruction, instruction, 0}) == false ||
            Instructions.TryAdd(words) == false || WeightBuffers.TryAdd(layer.Buffer) == false)
        {
            Result = Status::OutOfMemory;
        }
        return destination;
    }
    uint32 Affine(uint32 source, uint32 count, uint32 inputs, uint32 outputs, uint32 activation = 0) noexcept
    {
        return Add(LUDUS_LIGHT_ENCODER_AFFINE,
                   count,
                   inputs,
                   outputs,
                   source,
                   TakeLayer(LUDUS_LIGHT_ENCODER_AFFINE, inputs, outputs),
                   activation);
    }
    uint32 Normalize(uint32 source, uint32 count, uint32 channels) noexcept
    {
        return Add(LUDUS_LIGHT_ENCODER_NORMALIZE,
                   count,
                   channels,
                   channels,
                   source,
                   TakeLayer(LUDUS_LIGHT_ENCODER_NORMALIZE, channels, channels));
    }
    uint32 Block(uint32 source,
                 const internal::EncoderLevel& level,
                 uint32 channels,
                 uint32 sparseStarts,
                 uint32 sparsePairs,
                 uint32 patches) noexcept
    {
        const uint32 count = static_cast<uint32>(level.PositionsModel.GetSize());
        const Layer convolution = TakeLayer(LUDUS_LIGHT_ENCODER_CONVOLUTION, channels, channels, CPE_KERNEL_SIZE);
        uint32 current = Add(LUDUS_LIGHT_ENCODER_CONVOLUTION,
                             count,
                             channels,
                             channels,
                             source,
                             convolution,
                             0,
                             sparseStarts,
                             sparsePairs,
                             NO_TENSOR,
                             CPE_KERNEL_SIZE * CPE_KERNEL_SIZE * CPE_KERNEL_SIZE);
        current = Affine(current, count, channels, channels);
        current = Normalize(current, count, channels);
        current = Add(LUDUS_LIGHT_ENCODER_ADD, count, channels, channels, source, {}, 0, 0, 0, current);
        uint32 residual = current;
        current = Normalize(current, count, channels);
        current = Affine(current, count, channels, channels * AXES);
        const uint32 heads = channels == DIMENSION ? channels / FIRST_CHANNELS : channels / (FIRST_CHANNELS / 2);
        current =
            Add(LUDUS_LIGHT_ENCODER_ATTENTION, count, channels, channels, current, {}, 0, patches, 0, NO_TENSOR, heads);
        current = Affine(current, count, channels, channels);
        current = Add(LUDUS_LIGHT_ENCODER_ADD, count, channels, channels, residual, {}, 0, 0, 0, current);
        residual = current;
        current = Normalize(current, count, channels);
        current = Affine(current, count, channels, channels * FEED_FORWARD_RATIO, LUDUS_LIGHT_ENCODER_GELU);
        current = Affine(current, count, channels * FEED_FORWARD_RATIO, channels);
        return Add(LUDUS_LIGHT_ENCODER_ADD, count, channels, channels, residual, {}, 0, 0, 0, current);
    }
    uint32 AllocateArena() noexcept
    {
        for (usize i = 0; i < Instructions.GetSize(); ++i)
        {
            const Instruction& instruction = Instructions[i];
            if (instruction[LUDUS_LIGHT_ENCODER_DISPATCH_KIND] != LUDUS_LIGHT_ENCODER_INPUT)
            {
                Tensors[instruction[SOURCE_FIELD]].LastUse = static_cast<uint32>(i);
            }
            if (instruction[OTHER_FIELD] != NO_TENSOR)
            {
                Tensors[instruction[OTHER_FIELD]].LastUse = static_cast<uint32>(i);
            }
        }
        uint32 peak = PacketScalars;
        for (usize i = 0; i < Tensors.GetSize(); ++i)
        {
            Tensor& tensor = Tensors[i];
            uint32 offset = PacketScalars;
            const uint32 size = tensor.Count * tensor.Channels;
            usize earlier = 0;
            while (earlier < i)
            {
                const Tensor& previous = Tensors[earlier];
                const uint32 end = previous.Offset + previous.Count * previous.Channels;
                if (previous.LastUse >= tensor.FirstUse && offset < end && previous.Offset < offset + size)
                {
                    offset = end;
                    earlier = 0;
                }
                else
                {
                    ++earlier;
                }
            }
            tensor.Offset = offset;
            peak = math::Max(peak, offset + size);
        }
        for (Instruction& instruction : Instructions)
        {
            if (instruction[LUDUS_LIGHT_ENCODER_DISPATCH_KIND] != LUDUS_LIGHT_ENCODER_INPUT)
            {
                instruction[SOURCE_FIELD] = Tensors[instruction[SOURCE_FIELD]].Offset;
            }
            if (instruction[LUDUS_LIGHT_ENCODER_DISPATCH_KIND] != LUDUS_LIGHT_ENCODER_PACK)
            {
                instruction[DESTINATION_FIELD] = Tensors[instruction[DESTINATION_FIELD]].Offset;
            }
            if (instruction[OTHER_FIELD] != NO_TENSOR)
            {
                instruction[OTHER_FIELD] = Tensors[instruction[OTHER_FIELD]].Offset;
            }
        }
        return peak;
    }
};
Status PreparePacket(BorrowedView<const math::Vector3> anchorsModel,
                     math::Vector4 worldToModel,
                     Program& inoutProgram) noexcept
{
    geometry::PointSearchTree tree;
    if (tree.Build(anchorsModel) != geometry::GeometryStatus::Success)
    {
        return Status::OutOfMemory;
    }
    const BorrowedView<const uint8> nodes = GetBytes(tree.GetNodes());
    const uint32 prefix = static_cast<uint32>(PACKET_HEADER_SCALARS + nodes.GetSize() / sizeof(uint32));
    inoutProgram.PacketPrefixScalars = prefix;
    inoutProgram.AnchorOffset = prefix;
    inoutProgram.PacketScalars =
        prefix + static_cast<uint32>(anchorsModel.GetSize()) * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS * WORD_CHANNELS;
    const StaticArray<uint32, PACKET_HEADER_SCALARS> header{{LUDUS_LIGHT_ENCODER_PACKET_MAGIC,
                                                             static_cast<uint32>(anchorsModel.GetSize()),
                                                             tree.GetRoot(),
                                                             prefix / WORD_CHANNELS,
                                                             EncodeFloat(worldToModel.X),
                                                             EncodeFloat(worldToModel.Y),
                                                             EncodeFloat(worldToModel.Z),
                                                             EncodeFloat(worldToModel.W)}};
    inoutProgram.PacketTopology = inoutProgram.AppendTopology({header.GetData(), header.GetSize()});
    const usize begin = inoutProgram.Topology.GetSize();
    if (inoutProgram.Topology.TryResize(begin + nodes.GetSize() / WORD_BYTES) == false)
    {
        return Status::OutOfMemory;
    }
    std::memcpy(inoutProgram.Topology.GetData() + begin, nodes.GetDataOrNull(), nodes.GetSize());
    inoutProgram.PositionTopology = static_cast<uint32>(inoutProgram.Topology.GetSize());
    for (usize i = 0; i < anchorsModel.GetSize(); ++i)
    {
        const math::Vector3 positionModel = anchorsModel[i];
        const StaticArray<uint32, WORD_CHANNELS> words{
            {EncodeFloat(positionModel.X), EncodeFloat(positionModel.Y), EncodeFloat(positionModel.Z), 0}};
        static_cast<void>(inoutProgram.AppendTopology({words.GetData(), words.GetSize()}));
    }
    return inoutProgram.Result;
}
Status BuildProgram(BorrowedView<const LightTransformerScenePoint> points,
                    math::Vector4 worldToModel,
                    BorrowedView<const float32> decoderWeights,
                    Program& inoutProgram,
                    Array<math::Vector3>& outAnchorsModel) noexcept
{
    Array<math::Vector3> positionsModel;
    Array<uint32> features;
    if (positionsModel.TryResize(points.GetSize()) == false ||
        features.TryResize(points.GetSize() * LUDUS_LIGHT_ENCODER_SCENE_CHANNELS) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        const LightTransformerScenePoint& point = points[i];
        math::Vector3 normalModel{point.NormalModel.X, point.NormalModel.Y, point.NormalModel.Z};
        if (math::IsFinite(point.PositionModel) == false || point.PositionModel.W != 0 ||
            math::IsFinite(point.NormalModel) == false || point.NormalModel.W != 0 ||
            math::TryNormalize(normalModel, normalModel) != math::MathStatus::Success ||
            math::IsFinite(point.Albedo) == false || point.Albedo.W != 0 || point.Albedo.X < 0 || point.Albedo.X > 1 ||
            point.Albedo.Y < 0 || point.Albedo.Y > 1 || point.Albedo.Z < 0 || point.Albedo.Z > 1 ||
            math::IsFinite(point.Emission) == false || point.Emission.W != 0 || point.Emission.X < 0 ||
            point.Emission.Y < 0 || point.Emission.Z < 0)
        {
            return Status::InvalidDescription;
        }
        positionsModel[i] = {point.PositionModel.X, point.PositionModel.Y, point.PositionModel.Z};
        constexpr usize FEATURE_WORDS = LUDUS_LIGHT_ENCODER_SCENE_CHANNELS;
        const StaticArray<float32, FEATURE_WORDS> values{{point.PositionModel.X,
                                                          point.PositionModel.Y,
                                                          point.PositionModel.Z,
                                                          normalModel.X,
                                                          normalModel.Y,
                                                          normalModel.Z,
                                                          point.Albedo.X,
                                                          point.Albedo.Y,
                                                          point.Albedo.Z,
                                                          point.Emission.X,
                                                          point.Emission.Y,
                                                          point.Emission.Z}};
        for (usize channel = 0; channel < values.GetSize(); ++channel)
        {
            features[i * FEATURE_WORDS + channel] = EncodeFloat(values[channel]);
        }
    }
    const uint32 featureTopology = inoutProgram.AppendTopology({features.GetData(), features.GetSize()});
    uint32 count = static_cast<uint32>(points.GetSize());
    uint32 current =
        inoutProgram
            .Add(LUDUS_LIGHT_ENCODER_INPUT, count, 0, LUDUS_LIGHT_ENCODER_SCENE_CHANNELS, 0, {}, 0, featureTopology);
    current = inoutProgram.Affine(current,
                                  count,
                                  LUDUS_LIGHT_ENCODER_SCENE_CHANNELS,
                                  FIRST_CHANNELS,
                                  LUDUS_LIGHT_ENCODER_RELU);
    current = inoutProgram.Affine(current, count, FIRST_CHANNELS, FIRST_CHANNELS, LUDUS_LIGHT_ENCODER_RELU);
    for (uint32 stage = 0; stage < 2; ++stage)
    {
        Array<uint32> centers;
        Array<math::Vector3> selectedModel;
        Status status =
            internal::PrepareEncoderFps({positionsModel.GetData(), positionsModel.GetSize()}, centers, selectedModel);
        if (status != Status::Ready)
        {
            return status;
        }
        Array<uint32> neighbors;
        status = internal::PrepareEncoderNeighbors({positionsModel.GetData(), positionsModel.GetSize()},
                                                   {selectedModel.GetData(), selectedModel.GetSize()},
                                                   {centers.GetData(), centers.GetSize()},
                                                   neighbors);
        if (status != Status::Ready)
        {
            return status;
        }
        const uint32 inputs = stage == 0 ? FIRST_CHANNELS : SECOND_CHANNELS;
        const uint32 outputs = inputs * 2;
        const Layer first = inoutProgram.TakeLayer(LUDUS_LIGHT_ENCODER_AFFINE, inputs, outputs);
        const Layer second = inoutProgram.TakeLayer(LUDUS_LIGHT_ENCODER_AFFINE, outputs, outputs);
        if (first.Buffer != second.Buffer)
        {
            return Status::CapacityExceeded;
        }
        const uint32 topology = inoutProgram.AppendTopology({neighbors.GetData(), neighbors.GetSize()});
        count = static_cast<uint32>(selectedModel.GetSize());
        current = inoutProgram.Add(LUDUS_LIGHT_ENCODER_NEIGHBOR,
                                   count,
                                   inputs,
                                   outputs,
                                   current,
                                   first,
                                   0,
                                   topology,
                                   0,
                                   NO_TENSOR,
                                   0,
                                   second.Offset);
        positionsModel = Move(selectedModel);
    }
    StaticArray<internal::EncoderLevel, LUDUS_LIGHT_ENCODER_LEVELS> levels;
    Status status = internal::PrepareEncoderLevels({positionsModel.GetData(), positionsModel.GetSize()}, levels);
    if (status != Status::Ready)
    {
        return status;
    }
    status = PreparePacket({positionsModel.GetData(), positionsModel.GetSize()}, worldToModel, inoutProgram);
    if (status != Status::Ready)
    {
        return status;
    }
    StaticArray<uint32, LUDUS_LIGHT_ENCODER_LEVELS> sparseStarts{};
    StaticArray<uint32, LUDUS_LIGHT_ENCODER_LEVELS> sparsePairs{};
    StaticArray<StaticArray<uint32, LUDUS_LIGHT_ENCODER_ORDERS>, LUDUS_LIGHT_ENCODER_LEVELS> patchOffsets{};
    for (usize stage = 0; stage < levels.GetSize(); ++stage)
    {
        Array<uint32> starts;
        Array<uint32> pairs;
        status = internal::PrepareEncoderSparse(levels[stage], CPE_KERNEL_SIZE, starts, pairs);
        if (status != Status::Ready)
        {
            return status;
        }
        sparseStarts[stage] = inoutProgram.AppendTopology({starts.GetData(), starts.GetSize()});
        sparsePairs[stage] = inoutProgram.AppendTopology({pairs.GetData(), pairs.GetSize()});
        for (uint32 order = 0; order < LUDUS_LIGHT_ENCODER_ORDERS; ++order)
        {
            Array<uint32> patches;
            status = internal::PrepareEncoderPatches(levels[stage], order, patches);
            if (status != Status::Ready)
            {
                return status;
            }
            patchOffsets[stage][order] = inoutProgram.AppendTopology({patches.GetData(), patches.GetSize()});
        }
    }
    Array<uint32> stemStarts;
    Array<uint32> stemPairs;
    status = internal::PrepareEncoderSparse(levels[0], STEM_KERNEL_SIZE, stemStarts, stemPairs);
    if (status != Status::Ready)
    {
        return status;
    }
    const uint32 stemStart = inoutProgram.AppendTopology({stemStarts.GetData(), stemStarts.GetSize()});
    const uint32 stemPair = inoutProgram.AppendTopology({stemPairs.GetData(), stemPairs.GetSize()});
    current = inoutProgram.Add(
        LUDUS_LIGHT_ENCODER_CONVOLUTION,
        count,
        DIMENSION,
        DIMENSION,
        current,
        inoutProgram.TakeLayer(LUDUS_LIGHT_ENCODER_CONVOLUTION, DIMENSION, DIMENSION, STEM_KERNEL_SIZE),
        LUDUS_LIGHT_ENCODER_GELU,
        stemStart,
        stemPair,
        NO_TENSOR,
        STEM_KERNEL_SIZE * STEM_KERNEL_SIZE * STEM_KERNEL_SIZE);
    StaticArray<uint32, LUDUS_LIGHT_ENCODER_LEVELS> retained{};
    for (usize stage = 0; stage < levels.GetSize(); ++stage)
    {
        const internal::EncoderLevel& level = levels[stage];
        count = static_cast<uint32>(level.PositionsModel.GetSize());
        const uint32 channels = ENCODER_CHANNELS[stage];
        if (stage > 0)
        {
            const uint32 starts =
                inoutProgram.AppendTopology({level.ChildStarts.GetData(), level.ChildStarts.GetSize()});
            const uint32 children = inoutProgram.AppendTopology({level.Children.GetData(), level.Children.GetSize()});
            current = inoutProgram.Add(
                LUDUS_LIGHT_ENCODER_POOL,
                count,
                ENCODER_CHANNELS[stage - 1],
                channels,
                current,
                inoutProgram.TakeLayer(LUDUS_LIGHT_ENCODER_POOL, ENCODER_CHANNELS[stage - 1], channels),
                LUDUS_LIGHT_ENCODER_GELU,
                starts,
                children);
        }
        for (uint32 block = 0; block < ENCODER_DEPTHS[stage]; ++block)
        {
            current = inoutProgram.Block(current,
                                         level,
                                         channels,
                                         sparseStarts[stage],
                                         sparsePairs[stage],
                                         patchOffsets[stage][block % LUDUS_LIGHT_ENCODER_ORDERS]);
        }
        retained[stage] = current;
    }
    for (usize remaining = DECODER_LEVELS; remaining > 0; --remaining)
    {
        const usize stage = remaining - 1;
        const internal::EncoderLevel& level = levels[stage];
        const internal::EncoderLevel& child = levels[stage + 1];
        const uint32 inputs = stage + 1 == DECODER_LEVELS ? ENCODER_CHANNELS.GetLast() : DECODER_CHANNELS[stage + 1];
        const uint32 channels = DECODER_CHANNELS[stage];
        current = inoutProgram.Affine(current,
                                      static_cast<uint32>(child.PositionsModel.GetSize()),
                                      inputs,
                                      channels,
                                      LUDUS_LIGHT_ENCODER_GELU);
        count = static_cast<uint32>(level.PositionsModel.GetSize());
        const uint32 skip =
            inoutProgram.Affine(retained[stage], count, ENCODER_CHANNELS[stage], channels, LUDUS_LIGHT_ENCODER_GELU);
        const uint32 inverse =
            inoutProgram.AppendTopology({child.ParentToChild.GetData(), child.ParentToChild.GetSize()});
        current =
            inoutProgram.Add(LUDUS_LIGHT_ENCODER_GATHER, count, channels, channels, current, {}, 0, inverse, 0, skip);
        for (uint32 block = 0; block < 2; ++block)
        {
            current = inoutProgram.Block(current,
                                         level,
                                         channels,
                                         sparseStarts[stage],
                                         sparsePairs[stage],
                                         patchOffsets[stage][block]);
        }
    }
    if (inoutProgram.Result != Status::Ready)
    {
        return inoutProgram.Result;
    }
    if (inoutProgram.NextLayer != inoutProgram.Parameters.Layers.GetSize())
    {
        return Status::InvalidDescription;
    }
    if (decoderWeights.IsEmpty() == false)
    {
        inoutProgram.DecoderTopology = static_cast<uint32>(inoutProgram.Topology.GetSize());
        inoutProgram.DecoderOffset = inoutProgram.PacketScalars;
        constexpr uint32 PADDED_DECODER_SCALARS =
            (LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT + WORD_CHANNELS - 1) / WORD_CHANNELS * WORD_CHANNELS;
        inoutProgram.PacketScalars += PADDED_DECODER_SCALARS;
        for (usize i = 0; i < PADDED_DECODER_SCALARS; ++i)
        {
            const uint32 bits = i < decoderWeights.GetSize() ? EncodeFloat(decoderWeights[i]) : 0;
            if (inoutProgram.Topology.TryAdd(bits) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    const Instruction pack{{LUDUS_LIGHT_ENCODER_PACK,
                            count,
                            DIMENSION,
                            DIMENSION,
                            current,
                            inoutProgram.AnchorOffset,
                            0,
                            0,
                            inoutProgram.PacketTopology,
                            inoutProgram.PositionTopology,
                            NO_TENSOR,
                            0,
                            inoutProgram.PacketPrefixScalars,
                            0,
                            inoutProgram.DecoderTopology,
                            inoutProgram.DecoderOffset}};
    if (inoutProgram.Instructions.TryAdd(pack) == false || inoutProgram.WeightBuffers.TryAdd(1) == false)
    {
        return Status::OutOfMemory;
    }
    outAnchorsModel = Move(positionsModel);
    return Status::Ready;
}
} // namespace
Status PrepareLightTransformerEncoder(BorrowedView<const uint8> encoderAsset,
                                      BorrowedView<const LightTransformerScenePoint> points,
                                      math::Vector4 worldToModel,
                                      rhi::DeviceHandle device,
                                      const rhi::RasterShaderDescription& shader,
                                      usize maximumBufferBytes,
                                      LightTransformerEncoding& outEncoding,
                                      BorrowedView<const float32> decoderWeights,
                                      rhi::BufferHandle destination,
                                      usize destinationBytes) noexcept
{
    if (points.IsValid() == false || points.GetSize() < LUDUS_LIGHT_ENCODER_MIN_POINTS ||
        points.GetSize() > LUDUS_LIGHT_ENCODER_MAX_POINTS || math::IsFinite(worldToModel) == false ||
        worldToModel.W <= 0 || outEncoding.Workload.GetBufferBytes() != 0 ||
        (destination == rhi::BufferHandle{}) != (destinationBytes == 0))
    {
        return Status::InvalidDescription;
    }
    if (decoderWeights.IsValid() == false ||
        (decoderWeights.IsEmpty() == false && decoderWeights.GetSize() != LIGHT_TRANSFORMER_DECODER_WEIGHTS))
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < decoderWeights.GetSize(); ++i)
    {
        if (math::IsFinite(decoderWeights[i]) == false)
        {
            return Status::InvalidDescription;
        }
    }
    rhi::ComputeCapabilities capabilities;
    Status status = rhi::GetComputeCapabilities(device, capabilities);
    if (status != Status::Ready)
    {
        return status;
    }
    if (capabilities.MaxDispatch[0] == 0 || capabilities.MaxDispatch[1] == 0 ||
        capabilities.MaxStorageBindings < BUFFER_COUNT ||
        capabilities.MaxWorkgroupSize[0] < LUDUS_LIGHT_ENCODER_GROUP_WIDTH ||
        capabilities.MaxWorkgroupInvocations < LUDUS_LIGHT_ENCODER_GROUP_WIDTH)
    {
        return Status::CapacityExceeded;
    }
    Program program;
    status = ReadModel(encoderAsset, capabilities.StorageOffsetAlignment, maximumBufferBytes, program.Parameters);
    if (status != Status::Ready)
    {
        return status;
    }
    LightTransformerEncoding candidate;
    status = BuildProgram(points, worldToModel, decoderWeights, program, candidate.AnchorPositionsModel);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize arenaBytes = usize{program.AllocateArena()} * sizeof(float32);
    const usize dispatchBytes = DISPATCH_WORDS * sizeof(uint32);
    const usize dispatchAlignment = math::Max(capabilities.StorageOffsetAlignment, dispatchBytes);
    const usize dispatchStride = (dispatchBytes + dispatchAlignment - 1) / dispatchAlignment * dispatchAlignment;
    Array<uint8> dispatchData;
    Array<rhi::ComputeWorkloadBuffer> buffers;
    Array<rhi::ComputeWorkloadKernel> kernels;
    Array<StaticArray<rhi::ComputeWorkloadBinding, BUFFER_COUNT>> bindings;
    const usize kernelCount = program.Instructions.GetSize();
    if (dispatchData.TryResize(dispatchStride * kernelCount) == false ||
        buffers.TryResize(program.Parameters.Chunks.GetSize() + BUFFER_COUNT - 1) == false ||
        kernels.TryResize(kernelCount) == false || bindings.TryResize(kernelCount) == false)
    {
        return Status::OutOfMemory;
    }
    candidate.PacketBytes = usize{program.PacketScalars} * sizeof(float32);
    if (destination != rhi::BufferHandle{} && arenaBytes > destinationBytes)
    {
        return Status::CapacityExceeded;
    }
    buffers[0] = {dispatchData.GetSize(), {dispatchData.GetData(), dispatchData.GetSize()}, true};
    buffers[2] = {program.Topology.GetSize() * sizeof(uint32),
                  GetBytes(BorrowedView<const uint32>{program.Topology.GetData(), program.Topology.GetSize()}),
                  true};
    buffers[ARENA_BUFFER] = {destinationBytes == 0 ? arenaBytes : destinationBytes, {}, false, destination};
    for (usize i = 0; i < program.Parameters.Chunks.GetSize(); ++i)
    {
        const Array<float32>& chunk = program.Parameters.Chunks[i];
        const usize index = i == 0 ? 1 : i + BUFFER_COUNT - 1;
        buffers[index] = {chunk.GetSize() * sizeof(float32),
                          GetBytes(BorrowedView<const float32>{chunk.GetData(), chunk.GetSize()}),
                          true};
    }
    for (usize i = 0; i < kernelCount; ++i)
    {
        Instruction& instruction = program.Instructions[i];
        const uint32 groupsX = math::Min(instruction[LUDUS_LIGHT_ENCODER_DISPATCH_COUNT], capabilities.MaxDispatch[0]);
        instruction[GROUPS_X_FIELD] = groupsX;
        const uint32 groupsY = (instruction[LUDUS_LIGHT_ENCODER_DISPATCH_COUNT] - 1) / groupsX + 1;
        if (groupsY > capabilities.MaxDispatch[1])
        {
            return Status::CapacityExceeded;
        }
        std::memcpy(dispatchData.GetData() + i * dispatchStride, instruction.GetData(), dispatchBytes);
        const uint32 weightBuffer = program.WeightBuffers[i];
        bindings[i] = {{{0, 0, i * dispatchStride, dispatchBytes, false},
                        {1, weightBuffer, 0, buffers[weightBuffer].Size, false},
                        {2, 2, 0, buffers[2].Size, false},
                        {static_cast<uint32>(ARENA_BUFFER), ARENA_BUFFER, 0, buffers[ARENA_BUFFER].Size, true}}};
        kernels[i].NameOrNull = "Light Transformer encoder layer";
        if (i == 0)
        {
            kernels[i].Shader = shader;
        }
        else
        {
            kernels[i].UseProgramFrom = 0;
        }
        kernels[i].Bindings = {bindings[i].GetData(), bindings[i].GetSize()};
        kernels[i].GroupsX = groupsX;
        kernels[i].GroupsY = groupsY;
    }
    status = candidate.Workload.Initialize(
        device,
        {{buffers.GetData(), buffers.GetSize()}, {kernels.GetData(), kernels.GetSize()}, maximumBufferBytes});
    if (status == Status::Ready || status == Status::Pending)
    {
        outEncoding = Move(candidate);
    }
    return status;
}
} // namespace ludus::graphics::renderer
