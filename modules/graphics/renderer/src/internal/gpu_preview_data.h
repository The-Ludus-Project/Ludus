#pragma once

#include "../../shaders/renderer_gpu_preview_layout.h"
#include "gpu_denoiser.h"
#include "gpu_preview_camera.h"
#include "reference_data.h"
#include "render_graph_storage.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/gpu_preview.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
inline constexpr usize GPU_PREVIEW_JOB_COUNT = 16; // Four passes, three retained scenes and retiring work.
inline constexpr usize GPU_GRAPH_BUFFER_COUNT = 2;
/// Every compute binding of one job: the frozen ABI, visibility textures, structured storage, the
/// ray path capture buffer of a diagnostic capture-variant kernel, the LTC table and, last, the failure summary.
inline constexpr usize GPU_PREVIEW_COMPUTE_BINDING_CAPACITY =
    LUDUS_GRAPH_VISIBILITY_BINDING_COUNT + content::render_graph_format::MAX_STORAGE_BUFFERS + 1 +
    LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT + LUDUS_GRAPH_FAILURE_BINDING_COUNT;
/// The presentation quad's four float2 corners and six uint16 indices.
inline constexpr usize GPU_PREVIEW_QUAD_BYTES = 44;
/// Components of GpuPreviewUniform::Tile: origin x, y, then exclusive end x, y.
inline constexpr usize GPU_PREVIEW_TILE_COMPONENT_COUNT = 4;
struct GpuPreviewUniform final
{
    math::Matrix4 ClipToWorld;
    math::Vector4 CameraPositionWorld;
    uint32 Dimensions[4]{};
    uint32 Offsets[4]{};
    uint32 Counts[4]{};
    uint32 Sampling[4]{};
    math::Vector4 Environment;
    math::Vector4 Options;
    math::Vector4 BoundsMinimumWorld;
    math::Vector4 BoundsMaximumWorld;
    uint32 SurfaceLayout[4]{};
    /// Dispatched pixel region: origin x, y, then exclusive end x, y, at most Dimensions x, y. The whole image
    /// unless a tiled capture selects one row band (GpuPreviewTiles).
    uint32 Tile[GPU_PREVIEW_TILE_COMPONENT_COUNT]{};
};
static_assert(sizeof(GpuPreviewUniform) == LUDUS_GPU_PREVIEW_PARAMETERS_BYTES);
/// Components of GpuPreviewUniform::Tile.
inline constexpr usize GPU_PREVIEW_TILE_ORIGIN_X = 0;
inline constexpr usize GPU_PREVIEW_TILE_ORIGIN_Y = 1;
inline constexpr usize GPU_PREVIEW_TILE_END_X = 2;
inline constexpr usize GPU_PREVIEW_TILE_END_Y = 3;
/// Select row band `tile` of `count` equal bands of a width x height image in a uniform's Tile. Band edges
/// are floor(tile * height / count), so the bands partition the rows; a band may be empty when count exceeds
/// height, and its dispatch then returns without work.
LUDUS_INLINE void SetGpuPreviewTile(GpuPreviewUniform& inoutUniform, uint32 tile, uint32 count) noexcept
{
    const uint64 height = inoutUniform.Dimensions[1];
    inoutUniform.Tile[GPU_PREVIEW_TILE_ORIGIN_X] = 0;
    inoutUniform.Tile[GPU_PREVIEW_TILE_ORIGIN_Y] = static_cast<uint32>(height * tile / count);
    inoutUniform.Tile[GPU_PREVIEW_TILE_END_X] = inoutUniform.Dimensions[0];
    inoutUniform.Tile[GPU_PREVIEW_TILE_END_Y] = static_cast<uint32>(height * (tile + 1) / count);
}
/// The band a tiled ray sample (RenderGraph::SetRayTileCount) dispatches next: a copy of the job's uniform whose
/// Tile selects band Tile of Count, and compute bindings that read that copy. Each band is dispatched in its own
/// submission, so a long sample stays under the GPU watchdog. Only the next band is held: the RHI bounds live
/// buffers and binding descriptors per kind, so one copy per band would not scale to MAX_RENDER_GRAPH_RAY_TILES.
/// The previous band's copy retires with its producer, which completes before the next frame is encoded. Built
/// from the committed camera's uniform; a camera commit or a dropped job releases it. Owner thread only.
struct GpuPreviewTiles final
{
    uint32 Count = 0;
    uint32 Tile = 0;
    rhi::BufferHandle Parameters{};
    rhi::BindingSetHandle Bindings{};
    bool HasBindings = false;
};
struct GpuFrameRecord final
{
    uint64 Generation = 1;
    bool Owned = false;
    rhi::TextureHandle Texture{};
    rhi::SubmissionToken Producer{};
    uint32 Samples = 0;
};
/// A job's slot in GpuPreviewState::Scenes when it holds no uploaded scene.
inline constexpr usize NO_GPU_PREVIEW_SCENE = ~usize{0};
/// One uploaded packed scene: triangles, BVH, emitters, surfaces, lights, environment, texels and
/// native geometry. The ray jobs of one graph update that read the same scene data reference one
/// record, so the scene is stored and charged to the preview and RHI budgets once. The last job
/// dropped releases it, after its own producers retired.
struct GpuPreviewSceneRecord final
{
    usize References = 0;
    rhi::BufferHandle Data{};
    rhi::BufferHandle Texels{};
    rhi::RayGeometryHandle NativeGeometry{};
    bool HasNativeGeometry = false;
    usize DataBytes = 0;
    usize TexelBytes = 0;
    /// Data, texels and native geometry bytes, charged to GpuPreviewState::GpuBytes once.
    usize GpuBytes = 0;
    /// Scene data groups the record was packed from; a sharing job must request the same groups.
    uint32 SceneData = 0;
    /// Scene content consistency check for sharing jobs, alongside the exact sizes.
    uint64 FingerprintLow = 0;
    uint64 FingerprintHigh = 0;
};
struct GpuPreviewRecord final
{
    /// Referenced GpuPreviewState::Scenes slot; Data, Texels and NativeGeometry below copy its handles.
    usize Scene = NO_GPU_PREVIEW_SCENE;
    bool HasNativeRayInvocation = false;
    uint64 Generation = 1;
    bool Used = false;
    bool Owned = false;
    rhi::RayGeometryHandle NativeGeometry{};
    rhi::RaySceneHandle NativeScene{};
    bool HasNativeGeometry = false;
    bool IsNativeSceneCreated = false;
    RenderGraphStorage Storage;
    rhi::BufferHandle Data{};
    rhi::BufferHandle Texels{};
    rhi::BufferHandle Parameters{};
    rhi::BufferHandle Accumulation{};
    /// Pixels that latched each failure reason in the current camera history (LUDUS_GRAPH_FAILURE_BINDING); null
    /// when the kernel declares no failure summary. A camera commit replaces it with a zeroed buffer.
    rhi::BufferHandle Failures{};
    rhi::BufferHandle Colors{};
    rhi::BufferHandle ImageParameters{};
    rhi::TextureHandle VisibilityTexture{};
    rhi::TextureViewHandle VisibilityView{};
    rhi::TextureViewHandle VisibilityDepthView{};
    rhi::BindingSetHandle ComputeBindings{};
    rhi::BindingSetHandle ImageBindings{};
    foundation::StaticArray<GpuPreviewGraphBuffer, GPU_GRAPH_BUFFER_COUNT> GraphBuffers{};
    foundation::StaticArray<bool, GPU_GRAPH_BUFFER_COUNT> HasOwnedGraphBuffers{};
    GpuPreviewGraphBuffer RayPathCapture{}; // Caller-owned; bound only by capture-variant kernels.
    GpuFrameRecord Frames[2];
    GpuPreviewInfo Info;
    GpuPreviewUniform Uniform{};
    GpuPreviewCameraUpdate CameraUpdate;
    /// Row bands of a tiled capture; null for whole-image dispatches.
    foundation::core::UniquePtr<GpuPreviewTiles> TilesOrNull;
    GpuDenoiser Denoiser;
    uint32 DenoisingQuality = 0;
    bool HasPendingCamera = false;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 PendingFrame = 0;
    uint32 LatestFrame = 0;
    bool HasImage = false;
    usize DataBytes = 0;
    usize TexelBytes = 0;
    bool ComputeBindingsCreated = false;
    bool ImageBindingsCreated = false;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
    /// This job's own bytes; its referenced scene record charges the shared scene separately.
    usize GpuBytes = 0;
};
struct GpuPreviewState final
{
    GpuPreviewOptions Options;
    StaticArray<content::render_graph_format::StorageBuffer, content::render_graph_format::MAX_STORAGE_BUFFERS>
        StorageDescriptions{};
    // RasterBinding initializes Visibility to Both for every nested element.
    StaticArray<rhi::RasterBinding, GPU_PREVIEW_COMPUTE_BINDING_CAPACITY>
        ComputeBindings{}; // NOLINT(bugprone-invalid-enum-default-initialization)
    usize ComputeBindingCount = 0;
    uint32 WorkgroupWidth = 1;
    uint32 WorkgroupHeight = 1;
    bool HasRayPathCapture = false;
    /// The kernel reads the renderer's LTC table; LtcTable holds the uploaded copy.
    bool HasLtcTable = false;
    /// The kernel counts latched pixels by failure reason in each job's Failures buffer.
    bool HasFailureSummary = false;
    bool IsGraphRayScene = false;
    bool HasNativeRayQueries = true;
    bool HasVisibilityBuffer = false;
    bool HasPersistentBuffers = false;
    uint32 Phase = 0;
    bool QuadCreated = false;
    bool IndicesCreated = false;
    bool IsLtcTableCreated = false;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
    rhi::RasterShaderHandle ComputeShader{};
    rhi::RasterShaderHandle NativeRayShader{};
    rhi::ComputePipelineHandle NativeRayPipeline{};
    bool HasNativeRayShader = false;
    bool HasCreatedNativeRayPipeline = false;
    rhi::RasterShaderHandle VertexShader{};
    rhi::RasterShaderHandle FragmentShader{};
    rhi::BindingLayoutHandle ComputeLayout{};
    rhi::BindingLayoutHandle ImageLayout{};
    rhi::ComputePipelineHandle ComputePipeline{};
    rhi::RasterPipelineHandle ImagePipeline{};
    rhi::BufferHandle Quad{};
    rhi::BufferHandle Indices{};
    rhi::BufferHandle LtcTable{};
    GpuPreviewRecord Jobs[GPU_PREVIEW_JOB_COUNT];
    /// At most one record per job, so a free record exists whenever a job slot does.
    StaticArray<GpuPreviewSceneRecord, GPU_PREVIEW_JOB_COUNT> Scenes{};
    usize GpuBytes = 0;
};
} // namespace ludus::graphics::renderer::internal
