#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/light_transformer_encoder.hpp>

namespace ludus::graphics::renderer::internal
{
/// Binding slots of the encoded-query kernel's packet, weight and result buffers.
struct LightTransformerPacketQuerySlots final
{
    foundation::uint32 Packet = 0;
    foundation::uint32 Weights = 0;
    foundation::uint32 Results = 0;
};

/// PrepareLightTransformerPacketQueries that binds the three buffers at `slots`. The public function passes the
/// LUSL recipe's generated slot table; the renderer GPU tests pass the manual layout of its Slang original
/// (renderer_light_transformer_encoded_queries.slang), whose results take slot 3, to compare the two programs on
/// one device (LUSL L5d wave 2). Every other contract is the public function's.
[[nodiscard]] rhi::RasterStatus
PrepareLightTransformerPacketQueries(foundation::BorrowedView<const foundation::float32> weights,
                                     rhi::BufferHandle packet,
                                     foundation::usize packetBytes,
                                     foundation::BorrowedView<const LightTransformerQuery> queries,
                                     rhi::DeviceHandle device,
                                     const rhi::RasterShaderDescription& shader,
                                     const LightTransformerPacketQuerySlots& slots,
                                     foundation::usize maximumBufferBytes,
                                     rhi::ComputeWorkload& outWorkload) noexcept;
} // namespace ludus::graphics::renderer::internal
