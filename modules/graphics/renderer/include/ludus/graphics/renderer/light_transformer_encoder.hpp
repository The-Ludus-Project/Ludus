#pragma once

#include <ludus/content/render_graph_format/detail/light_transformer_encoder_layout.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/light_transformer.hpp>
#include <ludus/graphics/rhi/compute_workload.hpp>

namespace ludus::graphics::renderer
{
/// Failed stage of a model scene candidate; the last drawable generation remains intact.
enum class LightTransformerPreparationIssue : foundation::uint8
{
    None,              ///< No model preparation failure.
    MaterialInterface, ///< Supply a complete material-aware extraction.
    AnalyticLights, ///< Disable analytic lights or use a renderer that supports them; this model encodes mesh emission.
    Environment,    ///< Disable environment illumination or use another renderer.
    Materials,      ///< Assign typed diffuse materials, including every previously unassigned surface.
    Sampling,       ///< Repair malformed/fully masked geometry or increase the surface sampling budget.
    FixedScale,     ///< Partition the World or use a model trained at the required extent; do not fit each scene
                    ///< independently.
    ModelAsset,     ///< Re-export a complete compatible finite checkpoint.
    EmbeddingCapacity, ///< Increase the graph embedding Buffer capacity; its authored limit is preserved.
    DeviceCapacity,    ///< Raise the model/device budget or reduce points; enabled device limits are immutable until
                       ///< restart.
};
/// Concrete model candidate inputs for a repair diagnostic, independent of the presented image.
struct LightTransformerPreparationReport final
{
    LightTransformerPreparationIssue Issue = LightTransformerPreparationIssue::None; ///< First rejected stage.
    foundation::uint32 AnalyticLights = 0;          ///< Submitted analytic light count.
    foundation::uint32 UnsupportedMaterials = 0;    ///< Non-diffuse global material records.
    foundation::uint32 UnassignedSamples = 0;       ///< Sampled surfaces with no authored diffuse material binding.
    foundation::uint32 RequestedPoints = 0;         ///< Surface sampling count before twice-FPS reduction.
    foundation::math::Vector3 ExtentWorld{};        ///< Admitted scene bounds, in World units.
    foundation::float32 WorldUnitsPerModelUnit = 0; ///< Fixed training extent in World units.
    foundation::usize MaximumBufferBytes = 0;       ///< Authored model GPU buffer budget.
    foundation::usize MaximumSurfacePreparationBytes = 0; ///< Independent logical surface scene/scratch budget.
    foundation::uint64 MaximumSamplingAttempts = 0;       ///< Work limit for masked surface proposals.
    foundation::usize EmbeddingBufferBytes = 0; ///< Authored embedding capacity when EmbeddingCapacity rejects.
    foundation::usize RequiredPacketBytes = 0;  ///< Required defined model packet bytes when EmbeddingCapacity rejects.
};
/// Borrowed trained-model setup for the executable Light Transformer graph template.
/// Initialize consumes/copies assets. Shader artifacts survive through graph Poll.
/// No model files, Python runtime or download is required at rendering time.
struct LightTransformerModelDescription final
{
    foundation::BorrowedView<const foundation::uint8> EncoderAsset; ///< Offline `--encoder-output` asset.
    foundation::BorrowedView<const foundation::uint8> DecoderAsset; ///< Offline `--decoder-output` asset.
    rhi::RasterShaderDescription EncoderShader{}; ///< Synchronous renderer_light_transformer_encoder recipe.
    foundation::float32 WorldUnitsPerModelUnit =
        0; ///< Required positive fixed training scale; never per-scene fitting.
    foundation::uint32 ScenePoints = LUDUS_LIGHT_ENCODER_MAX_POINTS; ///< Surface samples, 128..20000.
    foundation::uint64 Seed = 1; ///< Stable surface sampling stream; unchanged by camera movement.
    foundation::usize MaximumBufferBytes =
        0; ///< Required logical model GPU buffer budget; separate RHI device limits still apply.
    foundation::usize MaximumSurfacePreparationBytes = 0; ///< Required independent CPU surface scene/sampling budget.
    foundation::uint64 MaximumSamplingAttempts =
        foundation::uint64{LUDUS_LIGHT_ENCODER_MAX_POINTS} *
        LUDUS_LIGHT_ENCODER_DEFAULT_ATTEMPTS_PER_POINT; ///< Surface proposal work limit, at least ScenePoints.
};
/// Material-aware encoder input, in the trained model's coordinate system.
/// All W components are zero. NormalModel is normalized during preparation;
/// Albedo is linear reflectance in [0,1], Emission is nonnegative linear RGB.
struct LightTransformerScenePoint final
{
    foundation::math::Vector4 PositionModel{}; ///< Finite XYZ; the point cloud spans at most 655 model units per axis.
    foundation::math::Vector4 NormalModel{};   ///< Nonzero XYZ shading normal in model space.
    foundation::math::Vector4 Albedo{};        ///< Linear RGB reflectance.
    foundation::math::Vector4 Emission{}; ///< Authored linear RGB emitted radiance, without reflected illumination.
};
/// Prepared view-independent encoder and its CPU geometry identities.
/// Workload buffer 3 starts with the packed tree/anchor packet consumed by the
/// Light Transformer graph template. Learned feature evaluation runs on the GPU.
/// The owner and shader artifacts follow ComputeWorkload's owner-thread/lifetime contract.
struct LightTransformerEncoding final
{
    rhi::ComputeWorkload Workload; ///< Execute its ordered graph once after Poll reaches Ready.
    foundation::Array<foundation::math::Vector3> AnchorPositionsModel; ///< Twice-FPS positions, in packet order.
    foundation::usize PacketBytes = 0; ///< Defined packet prefix bytes; scratch after this prefix is private.
};
/// Prepare the released Point Transformer V3 scene encoder using ordinary compute.
/// CPU work prepares geometry only: deterministic FPS (first point zero), exact
/// kNN, sparse convolution neighborhoods, pooling and four serialized point orders.
/// Dropout/order shuffling is disabled; neural arithmetic is binary32, including
/// serialized attention, rather than the authors' half-precision FlashAttention.
/// @param encoderAsset Complete offline `--encoder-output` asset, copied/validated synchronously.
/// @param points 128..20000 finite model-space surface points, copied. Point order determines FPS ties.
/// @param worldToModel XYZ world translation and W positive uniform scale: (positionWorld-XYZ)*W.
/// @param device Ready compute owner-thread device between frames; Metal and Vulkan use the same kernels.
/// @param shader Cooked renderer_light_transformer_encoder recipe; retain artifact through Poll.
/// @param maximumBufferBytes Explicit total logical GPU buffer budget; reduce points or raise it on CapacityExceeded.
/// @param outEncoding Empty owner, replaced only on Ready/Pending. Failure preserves it.
/// @param decoderWeights Optional canonical decoder parameters, copied into the packet for the graph template.
/// Empty keeps the standalone encoder packet.
/// @param destination Optional borrowed graph-owned storage; kept alive through workload Reset.
/// @param destinationBytes Usable destination capacity, zero when no destination is supplied.
/// @return Ready/Pending; malformed assets/features return InvalidDescription; bounded memory/dispatch
/// exhaustion returns CapacityExceeded; allocation/backend failures remain explicit.
/// This is an inference adapter, without an interactive performance or GI-quality guarantee.
[[nodiscard]] rhi::RasterStatus
PrepareLightTransformerEncoder(foundation::BorrowedView<const foundation::uint8> encoderAsset,
                               foundation::BorrowedView<const LightTransformerScenePoint> points,
                               foundation::math::Vector4 worldToModel,
                               rhi::DeviceHandle device,
                               const rhi::RasterShaderDescription& shader,
                               foundation::usize maximumBufferBytes,
                               LightTransformerEncoding& outEncoding,
                               foundation::BorrowedView<const foundation::float32> decoderWeights = {},
                               rhi::BufferHandle destination = {},
                               foundation::usize destinationBytes = 0) noexcept;
/// Prepare queries against an encoder's GPU packet without reading latents back.
/// @param weights Canonical decoder parameters; copied and required finite.
/// @param encoding Successfully prepared encoder, kept alive until the query workload resets.
/// Execute its encoder graph before executing this query graph on the same device.
/// @param queries Model-space queries, copied/normalized as in PrepareLightTransformerDecoder.
/// @param device Same ready owner-thread device as encoding, between frames.
/// @param shader Cooked renderer_light_transformer_encoded_queries recipe, retained through Poll.
/// @param maximumBufferBytes Total logical storage budget, including the borrowed packet prefix.
/// @param outWorkload Empty owner, acquired only on Ready/Pending. Failure preserves it.
/// @return Ready/Pending, NotReady while encoding is preparing, or explicit validation,
/// capacity/allocation/backend failure. Workload buffer 2 contains query-major float4 RGB/W=0.
[[nodiscard]] rhi::RasterStatus
PrepareLightTransformerEncodedQueries(foundation::BorrowedView<const foundation::float32> weights,
                                      const LightTransformerEncoding& encoding,
                                      foundation::BorrowedView<const LightTransformerQuery> queries,
                                      rhi::DeviceHandle device,
                                      const rhi::RasterShaderDescription& shader,
                                      foundation::usize maximumBufferBytes,
                                      rhi::ComputeWorkload& outWorkload) noexcept;
/// Prepare batched queries against a graph-borrowed encoder packet, without CPU latents.
/// @param weights Canonical finite decoder parameters, copied.
/// @param packet Ready same-device buffer produced by a completed encoder or Light Transformer graph frame.
/// Caller keeps its owner alive until the query workload resets; arbitrary/uninitialized data is unsupported.
/// @param packetBytes Complete accessible packet prefix, a positive multiple of sixteen.
/// @param queries Model-space queries, copied and normalized before return.
/// @param device Ready owner-thread device, between frames.
/// @param shader Cooked renderer_light_transformer_encoded_queries artifact, retained through Poll.
/// @param maximumBufferBytes Logical storage budget, including borrowed packet bytes.
/// @param outWorkload Empty owner, acquired only on Ready/Pending. Failure preserves it.
/// @return Explicit validation/capacity/allocation/backend status; buffer 2 contains raw query-major float4 RGB/W=0.
[[nodiscard]] rhi::RasterStatus
PrepareLightTransformerPacketQueries(foundation::BorrowedView<const foundation::float32> weights,
                                     rhi::BufferHandle packet,
                                     foundation::usize packetBytes,
                                     foundation::BorrowedView<const LightTransformerQuery> queries,
                                     rhi::DeviceHandle device,
                                     const rhi::RasterShaderDescription& shader,
                                     foundation::usize maximumBufferBytes,
                                     rhi::ComputeWorkload& outWorkload) noexcept;
} // namespace ludus::graphics::renderer
