#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/detail/light_transformer_layout.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/compute_workload.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// Trained latent width of the authors' released 128-dimensional model.
inline constexpr foundation::usize LIGHT_TRANSFORMER_DIMENSION = LUDUS_LIGHT_TRANSFORMER_DIMENSION;
/// Exact packed decoder parameter count; BatchNorm is folded by the offline adapter.
inline constexpr foundation::usize LIGHT_TRANSFORMER_DECODER_WEIGHTS = LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT;
/// Query-major float4 model predictions reside in this workload buffer (RGB, reserved zero).
inline constexpr foundation::usize LIGHT_TRANSFORMER_RESULT_BUFFER = 3;
/// An encoded anchor in model coordinates. These are learned encoder outputs,
/// never raw surface features or arbitrary zero-filled substitutes.
struct LightTransformerAnchor final
{
    foundation::math::Vector4 PositionModel{}; ///< XYZ in the encoder's coordinate system; W must be zero.
    foundation::StaticArray<foundation::float32, LIGHT_TRANSFORMER_DIMENSION> Embedding{}; ///< Encoder latent.
};
/// Decoder inputs in the same coordinate system as the encoded anchors.
struct LightTransformerQuery final
{
    foundation::math::Vector4 PositionModel{}; ///< XYZ model position, W zero.
    foundation::math::Vector4 NormalModel{};   ///< XYZ nonzero shading normal, normalized during preparation; W zero.
    foundation::math::Vector4 Albedo{};        ///< Linear RGB reflectance in [0,1], W zero.
};
/// Read the offline adapter's little-endian, versioned decoder parameter asset.
/// This does not deserialize Python, execute a checkpoint or own a model runtime.
/// @param bytes Exact complete asset, consumed before return; trailing bytes reject.
/// @param outWeights Replaced only on Ready with LIGHT_TRANSFORMER_DECODER_WEIGHTS finite scalars.
/// @return Ready, InvalidDescription (repair/re-export the asset), or OutOfMemory.
[[nodiscard]] rhi::RasterStatus
ReadLightTransformerDecoderWeights(foundation::BorrowedView<const foundation::uint8> bytes,
                                   foundation::Array<foundation::float32>& outWeights) noexcept;
/// Prepare the released six-block trained query decoder as portable compute.
/// It performs exact point-tree queries, two query MLP layers, vector attention,
/// residual/feed-forward blocks and Softplus on the GPU, without native ray or tensor hardware.
/// This API consumes existing encoder latents; it does not encode raw surface points.
/// @param device Ready owner-thread compute device, between frames.
/// @param weights Offline-adapted parameters in the canonical packed order; copied.
/// @param anchors At least 32 finite encoded anchors in matched model coordinates; copied.
/// @param queries Finite model queries; nonzero normals are normalized; copied.
/// @param shader Cooked renderer_light_transformer_decoder recipe. Artifact survives through Poll.
/// @param maximumBufferBytes Explicit budget for parameters, weights, anchors, queries, tree and results.
/// @param outWorkload Empty owner, acquired only on Ready/Pending; failure preserves it.
/// @return Ready/Pending, InvalidDescription, CapacityExceeded, OutOfMemory or backend preparation failure.
/// Output is raw network RGB; Softplus is nonnegative for finite evaluation. Nonfinite
/// inference values remain visible, without clamping or a fallback. Reconstruct the
/// trained target quantity separately: diffuse irradiance uses albedo/PI plus direct emission.
/// No inference-quality, interactive-performance or reference-integrator guarantee is implied.
[[nodiscard]] rhi::RasterStatus
PrepareLightTransformerDecoder(foundation::BorrowedView<const foundation::float32> weights,
                               foundation::BorrowedView<const LightTransformerAnchor> anchors,
                               foundation::BorrowedView<const LightTransformerQuery> queries,
                               rhi::DeviceHandle device,
                               const rhi::RasterShaderDescription& shader,
                               foundation::usize maximumBufferBytes,
                               rhi::ComputeWorkload& outWorkload) noexcept;
} // namespace ludus::graphics::renderer
