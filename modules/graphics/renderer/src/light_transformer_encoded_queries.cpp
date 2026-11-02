#include <ludus/graphics/renderer/light_transformer_encoder.hpp>

#include <ludus/content/render_graph_format/detail/light_transformer_encoder_layout.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/graphics/rhi/profile.h>

#include <cstring>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
constexpr usize PARAMETER_SCALARS = LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
constexpr usize BUFFER_COUNT = 3;
constexpr usize RESULT_BUFFER = 2;
constexpr usize RESULT_SLOT = 3;
constexpr usize QUERY_CHANNELS = LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS;
template <typename Element>
BorrowedView<const uint8> GetBytes(BorrowedView<const Element> values) noexcept
{
    return {reinterpret_cast<const uint8*>(values.GetDataOrNull()), values.GetSize() * sizeof(Element)};
}
} // namespace
Status PrepareLightTransformerPacketQueries(BorrowedView<const float32> weights,
                                            rhi::BufferHandle packet,
                                            usize packetBytes,
                                            BorrowedView<const LightTransformerQuery> queries,
                                            rhi::DeviceHandle device,
                                            const rhi::RasterShaderDescription& shader,
                                            usize maximumBufferBytes,
                                            rhi::ComputeWorkload& outWorkload) noexcept
{
    if (weights.IsValid() == false || weights.GetSize() != LIGHT_TRANSFORMER_DECODER_WEIGHTS ||
        queries.IsValid() == false || queries.IsEmpty() == true || queries.GetSize() > ~uint32{0} ||
        packet == rhi::BufferHandle{} ||
        packetBytes < LUDUS_LIGHT_ENCODER_PACKET_HEADER_WORDS * sizeof(math::Vector4) ||
        packetBytes % sizeof(math::Vector4) != 0 || packetBytes / sizeof(math::Vector4) > ~uint32{0})
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
    usize queryScalars = 0;
    usize parameterScalars = 0;
    usize parameterBytes = 0;
    usize resultBytes = 0;
    usize totalBytes = packetBytes;
    if (TryMultiply(queries.GetSize(), QUERY_CHANNELS, queryScalars) == false ||
        TryAdd(queryScalars, weights.GetSize() + PARAMETER_SCALARS, parameterScalars) == false ||
        TryMultiply(parameterScalars, sizeof(float32), parameterBytes) == false ||
        TryMultiply(queries.GetSize(), sizeof(math::Vector4), resultBytes) == false ||
        TryAdd(totalBytes, parameterBytes, totalBytes) == false ||
        TryAdd(totalBytes, resultBytes, totalBytes) == false || totalBytes > maximumBufferBytes)
    {
        return Status::CapacityExceeded;
    }
    rhi::ComputeCapabilities capabilities;
    Status status = rhi::GetComputeCapabilities(device, capabilities);
    if (status != Status::Ready)
    {
        return status;
    }
    if (capabilities.MaxDispatch[0] == 0 || capabilities.MaxDispatch[1] == 0)
    {
        return Status::CapacityExceeded;
    }
    const uint32 groupsX = math::Min(static_cast<uint32>(queries.GetSize()), capabilities.MaxDispatch[0]);
    const uint32 groupsY = static_cast<uint32>((queries.GetSize() - 1) / groupsX + 1);
    if (groupsY > capabilities.MaxDispatch[1])
    {
        return Status::CapacityExceeded;
    }
    Array<float32> parameters;
    if (parameters.TryResize(parameterScalars) == false)
    {
        return Status::OutOfMemory;
    }
    const StaticArray<uint32, PARAMETER_SCALARS> header{
        {static_cast<uint32>(queries.GetSize()), static_cast<uint32>(packetBytes / sizeof(math::Vector4)), 0, groupsX}};
    std::memcpy(parameters.GetData(), header.GetData(), header.GetSize() * sizeof(uint32));
    for (usize i = 0; i < weights.GetSize(); ++i)
    {
        parameters[PARAMETER_SCALARS + i] = weights[i];
    }
    for (usize i = 0; i < queries.GetSize(); ++i)
    {
        const LightTransformerQuery& query = queries[i];
        math::Vector3 normalModel{query.NormalModel.X, query.NormalModel.Y, query.NormalModel.Z};
        if (math::IsFinite(query.PositionModel) == false || query.PositionModel.W != 0 ||
            math::Abs(query.PositionModel.X) > geometry::MAX_POINT_SEARCH_COORDINATE ||
            math::Abs(query.PositionModel.Y) > geometry::MAX_POINT_SEARCH_COORDINATE ||
            math::Abs(query.PositionModel.Z) > geometry::MAX_POINT_SEARCH_COORDINATE ||
            math::IsFinite(query.NormalModel) == false || query.NormalModel.W != 0 ||
            math::TryNormalize(normalModel, normalModel) != math::MathStatus::Success ||
            math::IsFinite(query.Albedo) == false || query.Albedo.W != 0 || query.Albedo.X < 0 || query.Albedo.X > 1 ||
            query.Albedo.Y < 0 || query.Albedo.Y > 1 || query.Albedo.Z < 0 || query.Albedo.Z > 1)
        {
            return Status::InvalidDescription;
        }
        const StaticArray<float32, QUERY_CHANNELS> values{{query.PositionModel.X,
                                                           query.PositionModel.Y,
                                                           query.PositionModel.Z,
                                                           normalModel.X,
                                                           normalModel.Y,
                                                           normalModel.Z,
                                                           query.Albedo.X,
                                                           query.Albedo.Y,
                                                           query.Albedo.Z}};
        const usize begin = PARAMETER_SCALARS + weights.GetSize() + i * QUERY_CHANNELS;
        for (usize channel = 0; channel < QUERY_CHANNELS; ++channel)
        {
            parameters[begin + channel] = values[channel];
        }
    }
    const StaticArray<rhi::ComputeWorkloadBuffer, BUFFER_COUNT> buffers{
        {{packetBytes, {}, true, packet},
         {parameterBytes, GetBytes(BorrowedView<const float32>{parameters.GetData(), parameters.GetSize()}), true},
         {resultBytes, {}, false}}};
    const StaticArray<rhi::ComputeWorkloadBinding, BUFFER_COUNT> bindings{
        {{0, 0, 0, packetBytes, false},
         {1, 1, 0, parameterBytes, false},
         {RESULT_SLOT, RESULT_BUFFER, 0, resultBytes, true}}};
    rhi::ComputeWorkloadKernel kernel;
    kernel.NameOrNull = "Light Transformer encoded queries";
    kernel.Shader = shader;
    kernel.Bindings = {bindings.GetData(), bindings.GetSize()};
    kernel.GroupsX = groupsX;
    kernel.GroupsY = groupsY;
    return outWorkload.Initialize(device, {{buffers.GetData(), buffers.GetSize()}, {&kernel, 1}, maximumBufferBytes});
}
Status PrepareLightTransformerEncodedQueries(BorrowedView<const float32> weights,
                                             const LightTransformerEncoding& encoding,
                                             BorrowedView<const LightTransformerQuery> queries,
                                             rhi::DeviceHandle device,
                                             const rhi::RasterShaderDescription& shader,
                                             usize maximumBufferBytes,
                                             rhi::ComputeWorkload& outWorkload) noexcept
{
    rhi::BufferHandle packet;
    const Status ready = encoding.Workload.GetBuffer(LIGHT_TRANSFORMER_RESULT_BUFFER, packet);
    if (ready != Status::Ready)
    {
        return ready;
    }
    return PrepareLightTransformerPacketQueries(weights,
                                                packet,
                                                encoding.PacketBytes,
                                                queries,
                                                device,
                                                shader,
                                                maximumBufferBytes,
                                                outWorkload);
}
} // namespace ludus::graphics::renderer
