#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/gpu_preview.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <ludus/graphics/renderer/world_grid.hpp>

namespace ludus::graphics::renderer
{
struct RenderGraphSceneDescription;
namespace internal
{
/// Private access to renderer logical identities.
struct Access;
} // namespace internal
/// Renderer-owned mesh incarnation; copies do not transfer ownership.
struct Mesh final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private renderer access to assign and validate this incarnation.
    friend struct internal::Access;
};
/// Immutable copied scene incarnation; Release detaches public ownership.
struct Snapshot final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private renderer access to assign and validate this incarnation.
    friend struct internal::Access;
};
/// Immutable prepared view retaining its snapshot and static instance buffer.
struct PreparedView final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private renderer access to assign and validate this incarnation.
    friend struct internal::Access;
};
/// Immutable copied per-draw uniform blocks and binding snapshot of a bound flat program.
/// Copies do not transfer ownership; Release detaches it and snapshots retain the exact version.
struct UniformSet final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private renderer access to assign and validate this incarnation.
    friend struct internal::Access;
};
/// Forward declaration of the shared logical/physical mapping contract in views.hpp.
struct ViewMapping;
/// Immutable sampled target and presentation geometry retained through GPU completion.
struct Presentation final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants private access to the renderer incarnation.
    friend struct internal::Access;
};
/// Explicit pairing of projection matrices, clear values and comparisons.
enum class DepthConvention : ludus::foundation::uint8
{
    /// R1 compatibility: near zero, far one, clear one, strict Less.
    Conventional,
    /// Near one, far zero, clear zero, strict Greater.
    ReverseZ,
};
/// Opaque scene geometry precedes painter-ordered overlays.
enum class Layer : ludus::foundation::uint8
{
    /// Depth-tested, depth-writing, alpha-one geometry.
    Opaque,
    /// Depth-disabled, premultiplied-alpha geometry in snapshot input order.
    Overlay,
};
/// Flat material in linear RGB; default magenta identifies missing material authoring.
struct FlatMaterial final
{
    /// Straight RGBA in [0,1]; opacity must be one for Opaque. Renderer premultiplies Overlay RGB.
    ludus::foundation::math::Vector4 Color{1, 0, 1, 1};
};
/// Borrowed static position/optional-UV triangle-list geometry, copied by CreateMesh.
/// Authored views must be empty or contain exactly VertexCount elements; creation validates
/// their extents before reading and copies them without retaining the caller's storage.
struct MeshDescription final
{
    /// Finite mesh-local XYZ positions, tightly packed Vector3 records.
    const ludus::foundation::math::Vector3* Positions = nullptr;
    /// Number of positions, positive and representable by uint32.
    ludus::foundation::usize VertexCount = 0;
    /// Mesh-local uint32 indices; every value must be below VertexCount.
    const ludus::foundation::uint32* Indices = nullptr;
    /// Positive multiple of three, representable by uint32.
    ludus::foundation::usize IndexCount = 0;
    /// Optional finite top-left UVs, exactly VertexCount records; null supplies zero UV.
    /// Copied into the shared position/UV mesh stream during creation.
    const ludus::foundation::math::Vector2* Texcoords = nullptr;
    /// Optional finite nonzero mesh-local normals, normalized while copying. Lit snapshots require them.
    const ludus::foundation::math::Vector3* Normals = nullptr;
    /// Optional finite bake-chart UVs, exactly VertexCount records; null uses Texcoords.
    const ludus::foundation::math::Vector2* BakeTexcoords = nullptr;
    /// Optional authored tangent XYZ and handedness W (+1/-1), one per vertex.
    /// Requires normals; finite nonzero XYZ is normalized/orthogonalized on copy.
    /// W=0 explicitly marks an unavailable vertex tangent and selects UV fallback.
    /// An empty view keeps the UV-derived frame. Mirrored object transforms adjust handedness.
    // Explicit default preserves partial aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    ludus::foundation::BorrowedView<const ludus::foundation::math::Vector4> Tangents{};
    /// Optional linear RGBA multiplier [0,1], one per vertex; an empty view supplies white.
    /// Used by authored material programs, including alpha acceptance.
    // Explicit default preserves partial aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    ludus::foundation::BorrowedView<const ludus::foundation::math::Vector4> Colors{};
    /// Optional second material UV set, one per vertex; an empty view copies Texcoords.
    /// Independent of BakeTexcoords; authored material programs select UV0 or UV1.
    // Explicit default preserves partial aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    ludus::foundation::BorrowedView<const ludus::foundation::math::Vector2> MaterialTexcoords1{};
};
/// Ordinary scene values; renderer has no ECS, gameplay camera or UI dependency.
struct SceneItem final
{
    /// Ready owned mesh at snapshot creation; accepted snapshots retain its incarnation.
    Mesh Geometry{};
    /// Optional coarse mesh incarnation, retained with Geometry. Opaque camera views only.
    /// Both meshes must use compatible material UVs/normals; shadow views retain full detail.
    Mesh LodGeometry{};
    /// Select coarse detail below this projected bounding-box diameter in pixels, positive when authored.
    ludus::foundation::float32 LodPixels = 0;
    /// Caller-owned previous coarse selection for hysteresis; copied with this snapshot.
    bool PreviousLod = false;
    /// Local-to-world for Opaque; local-to-canonical-clip for Overlay (view matrix ignored).
    ludus::foundation::math::Affine3 Transform{};
    /// Copied flat color; default is the error material.
    FlatMaterial Material{};
    /// Nonzero unique application source identity within this snapshot, for diagnostics.
    ludus::foundation::uint64 SourceId = 0;
    /// Pass and ordering semantics.
    Layer Pass = Layer::Opaque;
    /// Optional ready material incarnation replacing FlatMaterial entirely. Null keeps
    /// the L0 flat/error material. Snapshot retains its exact texture/program version.
    MaterialVersion Surface{};
    /// Optional ready per-draw uniform set of a bound flat program; requires a null Surface.
    /// Null binds the program's zero-filled defaults. Snapshot retains the exact version.
    UniformSet Uniforms{};
    /// Exact bake scene revision; zero declares no valid baked chart.
    ludus::foundation::uint64 BakeSceneRevision = 0;
    /// Exact bake dependency revision; mismatch selects environment and dynamic direct lighting.
    ludus::foundation::uint64 BakeDependencyRevision = 0;
};
/// Finite canonical clip transform: XY [-1,1], Z [0,1], Y up.
struct ViewDescription final
{
    /// Finite world-to-clip matrix matching the explicit Depth convention.
    ludus::foundation::math::Matrix4 WorldToClip = ludus::foundation::math::Matrix4::Identity();
    /// Finite nonnegative world-unit margin for conservative opaque AABB culling.
    ludus::foundation::float32 CullMargin = 0;
    /// Keep source order and one draw per object, with visibility disabled, for image comparison.
    bool Reference = false;
    /// L5 diagnostic: evaluate all admitted lights while retaining ordinary culling/batching.
    bool DirectLighting = false;
    /// Opt-in screen-size LOD; Reference bypasses LOD, ShadowCasters always uses full geometry.
    bool EnableLod = false;
    /// Physical viewport dimensions used only for LOD; both positive when EnableLod is true.
    ludus::foundation::uint32 LodWidth = 0;
    /// Physical viewport height for LOD, positive when EnableLod is true.
    ludus::foundation::uint32 LodHeight = 0;
    /// Fractional transition band [0,0.5]; previous selection comes from SceneItem::PreviousLod.
    ludus::foundation::float32 LodHysteresis = 0.1F;
    /// Schedule a bounded R2 upload version instead of immediate immutable creation.
    /// PrepareView then returns Pending; poll GetStatus between frames before drawing.
    /// Caller snapshots/arrays may be released immediately; accepted GPU versions never change.
    bool ScheduledUpload = false;
    /// Defaults preserve existing L0 projection behavior; reverse-Z requires InitializeViews.
    DepthConvention Depth = DepthConvention::Conventional;
    /// Select five-plane culling for an infinite reverse-Z perspective projection.
    bool InfiniteFar = false;
    /// Finite world-space camera position in meters, [-1000000,1000000]; required for lit shading.
    ludus::foundation::math::Vector3 CameraPosition{0, 0, 1};
    /// Prepare an opaque/masked caster view; skips overlays and disables camera visibility shortcuts.
    /// ShadowAtlas preparation sets this; ordinary callers should keep false.
    bool ShadowCasters = false;
    /// Use constant CameraForward for orthographic specular evaluation; perspective uses CameraPosition.
    bool OrthographicCamera = false;
    /// Nonzero finite world direction from camera into scene, normalized during preparation.
    ludus::foundation::math::Vector3 CameraForward{0, 0, -1};
    /// Canonical overlay crop transform, applied independently of the world camera.
    ludus::foundation::math::Matrix4 OverlayToClip = ludus::foundation::math::Matrix4::Identity();
    /// World-unit camera distances {near, far} that MaterialDebugView::LinearDepth maps to black
    /// and white. Perspective views measure radial distance from CameraPosition; orthographic
    /// views measure distance along CameraForward. Finite, 0 <= near < far <= 1e7, or {0, 0}
    /// (the default) to leave depth unmapped (black). Ignored by ordinary lit programs.
    ludus::foundation::math::Vector2 DebugDepthRange{0, 0};
    /// Optional depth-tested world coordinate guide. It does not alter scene bounds, item counts or shadows.
    /// Ignored by reference/path tracing captures; raster drawing retains guide buffers with this view.
    WorldGridDescription WorldGrid{};
};
/// Packet counts and a diagnostic prefix, copied to caller storage; IDs follow draw/instance order.
struct ViewReport final
{
    /// Number of submitted snapshot objects.
    ludus::foundation::uint32 Submitted = 0;
    /// Retained visible objects, including overlays.
    ludus::foundation::uint32 Visible = 0;
    /// Objects conservatively rejected by the CPU clip/frustum tests.
    ludus::foundation::uint32 Culled = 0;
    /// Indexed draws after adjacent compatible instancing.
    ludus::foundation::uint32 Draws = 0;
    /// Visible lit instances using environment because an authored bake key/transform mismatched.
    ludus::foundation::uint32 BakeFallbacks = 0;
    /// Visible lit instances using unshadowed direct light after conservative caster invalidation.
    ludus::foundation::uint32 ShadowFallbacks = 0;
    /// Number of conservative 16-object groups rejected before individual AABB tests.
    ludus::foundation::uint32 CulledGroups = 0;
    /// Number of individual AABB tests; fully inside/rejected groups avoid them.
    ludus::foundation::uint32 BoundsTests = 0;
    /// Number of visible objects selecting coarse detail.
    ludus::foundation::uint32 LodObjects = 0;
    /// First Reported coarse selections; feed back by source identity for hysteresis.
    bool SelectedLods[256]{};
    /// First Reported source identities in actual per-instance order.
    ludus::foundation::uint64 SourceIds[256]{};
    /// Number of meaningful entries in the inline arrays, min(Visible, 256).
    ludus::foundation::uint32 Reported = 0;
    /// Optional borrowed full-order output, written only after successful preparation.
    /// Null omits this output; otherwise DetailCapacity must accommodate every visible object.
    ludus::foundation::uint64* SourceIdOutput = nullptr;
    /// Optional borrowed full-order LOD output with the same capacity contract.
    bool* SelectedLodOutput = nullptr;
    /// Caller-provided element capacity for either nonnull output array. Failure preserves both arrays.
    ludus::foundation::usize DetailCapacity = 0;
};
/// Initialization-time renderer capacities. All counts are positive and below UINT32_MAX.
/// Metadata and per-view/snapshot workspaces are reserved once; no runtime pool growth.
/// Detached incarnations consume slots until their renderer references retire.
/// RHI byte/resource/packet budgets are independent and may exhaust earlier.
struct Limits final
{
    /// Maximum resident/retained meshes (two RHI buffers each).
    ludus::foundation::uint32 Meshes = 6;
    /// Maximum resident/retained snapshots.
    ludus::foundation::uint32 Snapshots = 4;
    /// Maximum prepared views, including shadow light views.
    ludus::foundation::uint32 Views = 8;
    /// Maximum objects and draw packets per snapshot/view; must fit RHI DrawsPerFrame.
    ludus::foundation::uint32 Objects = 256;
    /// Maximum retained target presentations, each consuming RHI buffers and binding sets.
    ludus::foundation::uint32 Presentations = 4;
    /// Maximum overlapping prewarmed shader/layout incarnations for reload.
    ludus::foundation::uint32 MaterialPrograms = 2;
    /// Maximum live/retained immutable parameter and texture versions.
    ludus::foundation::uint32 Materials = 8;
    /// Maximum resident/retained immutable shadow atlases for overlap and replacement.
    ludus::foundation::uint32 ShadowAtlases = 2;
    /// Maximum live/retained per-draw uniform sets of a bound flat program, each one RHI
    /// buffer and binding snapshot. Unbound programs reserve the records but never use them.
    ludus::foundation::uint32 UniformSets = 8;
    /// Positive byte ceiling for State, registry records and all reserved CPU workspaces.
    /// Excludes allocator overhead, transient mesh upload conversion, RT1's separately budgeted
    /// CPU data, RHI resources and driver allocations. Exhaustion returns CapacityExceeded.
    ludus::foundation::usize HostBytes = ludus::foundation::usize{64} * 1024 * 1024;
};
/// Portable flat/unlit/lit renderer, one owner thread per device.
/// Setup copies data and may allocate/create GPU resources. Repeated Submit creates
/// no renderer resources or heap allocations; RHI retains accepted work through completion.
/// Setup/release and destruction require a closed RHI frame; DrawView/DrawPresentation
/// require an acquired open frame. Reset before destroying the device.
/// See the portable scene submission section in `docs/architecture/renderer-systems.md` for the canonical guide.
class Renderer final
{
public:
    /// Construct an uninitialized renderer; no allocation or global registration.
    Renderer() noexcept;
    /// Release renderer ownership; submitted GPU uses retain their own RHI leases.
    ~Renderer();
    /// Renderer identity and owner thread cannot be copied.
    Renderer(const Renderer&) = delete;
    /// Renderer identity and owner thread cannot be assigned.
    Renderer& operator=(const Renderer&) = delete;
    /// Initialize on a ready portable-raster device using the shipped renderer_flat.slang program
    /// or an authored program with the same six-location interface.
    /// Ready/Pending publishes renderer ownership; failures leave it uninitialized.
    /// Shader inputs must match six shipped locations. Bindings, when present, must be
    /// constant scalar read-only uniform buffers (a bound flat program): one layout merges
    /// both stages, a binding number shared by both stages needs an equal MinSize, and
    /// every flat pipeline uses it. Items then select a UniformSet or zero-filled defaults.
    /// @param device Borrowed ready device, which must outlive this renderer.
    /// @param vertex Cooked vertex description from the installed shader helper.
    /// @param fragment Cooked fragment description from the same flat program.
    /// @param limits Copied capacities and host budget; invalid/overflowing requests preserve uninitialized state.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::RasterShaderDescription& vertex,
                                               const rhi::RasterShaderDescription& fragment,
                                               const Limits& limits = {}) noexcept;
    /// Advance asynchronous shader/layout/pipeline setup without blocking. Ready permits scene setup.
    /// Pending/failure retains ownership until Reset; no automatic backend switch.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Opt in to retained CPU geometry and the selected RT0/RT1 traversal before
    /// any meshes or material versions are live. Native mode requires ready optional RHI ray support and
    /// the installed fixed query shader; CpuOracle ignores the shader explicitly.
    /// Failure preserves disabled state. Owner-thread/between-frames only.
    [[nodiscard]] rhi::RasterStatus EnableReferenceRendering(const rhi::ShaderDescription&,
                                                             const ReferenceRenderingOptions&) noexcept;
    /// Cancel reference jobs, detach their accepted native work and drop retained
    /// CPU mesh/material copies. Raster versions remain usable. Re-enabling requires
    /// reauthoring meshes/materials after all existing incarnations are released.
    /// Owner-thread/between-frames only; no GPU wait or device capability switch.
    /// InvalidState while GPU preview owns retained input setup; Reset releases
    /// both services. Never disables preview input retention implicitly.
    [[nodiscard]] rhi::RasterStatus DisableReferenceRendering() noexcept;
    /// Freeze all opaque/masked snapshot objects and exact surface/texture versions,
    /// including offscreen objects and full-detail geometry. Overlays are excluded.
    /// Unlit/error surfaces or missing CPU texture shadows return Unsupported;
    /// uninitialized mips return InvalidDescription, pending uploads NotReady.
    /// Missing geometry, capacity and allocation failures preserve null output.
    /// Ready/Pending publishes one of at most four renderer-owned capture jobs.
    /// Native build readiness and progressive samples advance explicitly afterward.
    [[nodiscard]] rhi::RasterStatus
    BeginReferenceCapture(Snapshot, const ReferenceCaptureDescription&, ReferenceCapture&) noexcept;
    /// Advance at most one tile stage without blocking. Pending means native work
    /// or further stages/samples remain; Ready means the requested target is complete.
    /// Failed contributions leave that tile's old sums/counts intact and identify
    /// the offending pixel/depth. Owner-thread/between-frames only; no raster frame
    /// submission, denoising, silent fallback or synchronous GPU readback occurs.
    [[nodiscard]] rhi::RasterStatus AdvanceReferenceCapture(ReferenceCapture) noexcept;
    /// Update the frozen integral transactionally. Snapshot, camera, image extent,
    /// lights, mip, seed or integrator changes reset accumulation; a TargetSamples
    /// change alone continues it. Failure preserves the previous image/job. The
    /// identical request retries a failed uncommitted tile with the same addresses.
    /// Ready/Pending publishes copied input; caller arrays need not remain alive.
    [[nodiscard]] rhi::RasterStatus
    UpdateReferenceCapture(ReferenceCapture, Snapshot, const ReferenceCaptureDescription&) noexcept;
    /// Query progressive completion without advancing CPU path stages or polling native
    /// setup/work (AdvanceReferenceCapture performs that bounded polling). Pending/Failed
    /// remain owned; Update can retry a failed tile or Release cancels it. Invalid
    /// identity returns InvalidHandle. Owner-thread only.
    [[nodiscard]] rhi::RasterStatus GetStatus(ReferenceCapture) const noexcept;
    /// Copy bounded progress and profile evidence. Failure preserves output; valid
    /// failed captures still report their diagnostic counters. Owner-thread only.
    [[nodiscard]] rhi::RasterStatus GetReferenceCaptureInfo(ReferenceCapture, ReferenceCaptureInfo&) const noexcept;
    /// Enable hardware-accelerated preview before creating meshes/materials. Shader
    /// artifacts must be the installed native-query preview kernels. Requires enabled
    /// native rays, Compute/StorageVertex and a Ready renderer. Unsupported devices
    /// reject; no software traversal or CPU rendering fallback is selected.
    /// Copies options; positive budgets and finite exposure are required. Owner-thread,
    /// between frames. Failure preserves the previous disabled state. Pending owns
    /// setup and requires PollGpuPreview; Reset releases it. CPU input retention
    /// must be disabled before enable; it is enabled internally without CPU tracing.
    [[nodiscard]] rhi::RasterStatus EnableGpuPreview(const rhi::RasterShaderDescription& compute,
                                                     const rhi::RasterShaderDescription& vertex,
                                                     const rhi::RasterShaderDescription& fragment,
                                                     const GpuPreviewOptions&) noexcept;
    /// Enable the graph triangle kernel with flat opaque snapshots, without lit-material programs.
    /// Same GPU setup, budgets and lifetime as EnableGpuPreview; the compute stage must implement
    /// the authored triangle-ray ABI. BeginGpuPreview then copies flat tint/geometry only, with no
    /// lighting or CPU tracing. Ready/Pending owns setup; failure preserves the disabled state.
    [[nodiscard]] rhi::RasterStatus EnableTriangleRayPipeline(const rhi::RasterShaderDescription& compute,
                                                              const rhi::RasterShaderDescription& vertex,
                                                              const rhi::RasterShaderDescription& fragment,
                                                              const GpuPreviewOptions& options) noexcept;
    /// Poll asynchronous GPU preview setup and retire cancelled completed jobs on
    /// the owner thread between frames.
    /// No waiting; failure remains observable until Reset.
    [[nodiscard]] rhi::RasterStatus PollGpuPreview() noexcept;
    /// Copy an admitted static snapshot/camera/material integral into owned GPU
    /// buffers. Four live jobs; old/new GPU bytes share the enable-time budget.
    /// RT1 supported RGB reflection/opacity/emission only, fixed retained mip;
    /// overlay items are excluded. No
    /// baked/atlas lighting, transmission or deformation. Target <=4096, depth <=8,
    /// pixel count <=2^24; negotiated storage limits can be tighter.
    /// RGB emissive triangles, up to 128 analytic lights and constant/HDR environment.
    /// Ready/Pending publish ownership into a null output; rejection preserves it. All caller
    /// owners may detach after success. Changing camera/content/exposure creates a
    /// new job and restarts accumulation. Setup can allocate/upload; never shades.
    /// @param graphSceneOrNull Optional graph scene, consumed synchronously instead of Snapshot.
    /// Requires EnableTriangleRayPipeline. Material defaults/errors follow its declared policy.
    [[nodiscard]] rhi::RasterStatus
    BeginGpuPreview(Snapshot,
                    const ReferenceCaptureDescription&,
                    GpuPreview&,
                    const RenderGraphSceneDescription* graphSceneOrNull = nullptr) noexcept;
    /// Dispatch one full-image GPU sample, shade/accumulate/tone-map on GPU, and
    /// rasterize GPU-produced colors into an available output texture. Ordered GDI
    /// dependencies include compute-to-vertex and output export. Never reads rays,
    /// radiance or pixels to host. Owner-thread, closed frame; completion starts null.
    /// NotReady means an earlier producer is pending or both output images are leased.
    /// Success publishes producing completion; acquired images are never overwritten.
    [[nodiscard]] rhi::RasterStatus AdvanceGpuPreview(GpuPreview, rhi::SurfaceHandle, rhi::SubmissionToken&) noexcept;
    /// Prepare a one-sample GPU job's compute and image commands for an external graph-owned target.
    /// Owner thread, between frames. The target must be a ready offscreen RGBA8 attachment with the
    /// job's exact extent. Copies/retains resource uses in a new compiled OrderedGraph; never shades.
    /// The caller encodes it into a frame, tracks the actual EndFrame completion, and rebuilds it
    /// only after that producer finishes. The GPU job/target outlive graph encoding; RHI retains
    /// submitted resources through completion. Job progress/preview frame leases are unaffected.
    /// @param job Ready GPU job; do not also AdvanceGpuPreview this job.
    /// @param target Borrowed attachment, cleared/stored in color; private depth is discarded.
    /// @param outGraph Null graph receives the commands. Failure preserves it and releases temporary leases.
    [[nodiscard]] rhi::RasterStatus
    PrepareGpuPreviewGraph(GpuPreview job, rhi::TextureHandle target, rhi::OrderedGraph& outGraph) noexcept;
    /// Poll GPU completion and copy progress; no waiting or GPU-buffer readback.
    /// Ready/Pending copy progress; rejection preserves output. Owner-thread,
    /// between frames. Pending means creation or producer work is outstanding.
    [[nodiscard]] rhi::RasterStatus GetGpuPreviewInfo(GpuPreview, GpuPreviewInfo&) noexcept;
    /// Acquire latest GPU-complete output. Ready fills a null frame lease and
    /// copied image; Pending/NotReady/rejection preserves both outputs. The same
    /// output cannot be acquired twice. Retain through external sampling completion.
    [[nodiscard]] rhi::RasterStatus AcquireGpuPreviewFrame(GpuPreview, GpuPreviewFrame&, GpuPreviewImage&) noexcept;
    /// Return an acquired output lease and clear identity. @pre Every external GPU
    /// use completed and its RHI interoperability lease was released. Never releases
    /// on behalf of Qt; stale/foreign frames preserve input. Owner-thread, between frames.
    [[nodiscard]] rhi::RasterStatus Release(GpuPreviewFrame&) noexcept;
    /// Cancel future preview work and clear owner. Acquired frames keep GPU scene
    /// storage alive until returned. Pending producers keep their GPU budget until
    /// PollGpuPreview or the next Begin retires them after completion.
    /// Stale/foreign jobs preserve input. Owner-thread, between frames.
    [[nodiscard]] rhi::RasterStatus Release(GpuPreview&) noexcept;
    /// Copy exactly Width*Height raw unexposed means/AOVs and committed counts.
    /// Incomplete images expose zero/unequal counts; native pending work never
    /// enters the copied data. Failure preserves output. No allocation or GPU wait.
    [[nodiscard]] rhi::RasterStatus
    CopyReferencePixels(ReferenceCapture, ReferencePixel* output, ludus::foundation::usize count) const noexcept;
    /// Encode the versioned LUHDR01 little-endian FP32 RGB/count capture and frozen
    /// profile metadata into caller storage. Required bytes and PFM conversion are
    /// documented in the renderer guide. Capacity/other failure preserves output
    /// and written; partial images retain exact per-pixel counts. No file I/O/wait.
    [[nodiscard]] rhi::RasterStatus EncodeReferenceHdr(ReferenceCapture,
                                                       ludus::foundation::uint8* output,
                                                       ludus::foundation::usize capacity,
                                                       ludus::foundation::usize& written) const noexcept;
    /// Cancel future samples and detach capture ownership. Accepted native batches
    /// retire through completion; invalid identity preserves handle, success clears
    /// it. Snapshot/material/mesh references release exactly once. Between frames.
    [[nodiscard]] rhi::RasterStatus Release(ReferenceCapture&) noexcept;
    /// Enable L1 surface/offscreen reverse-Z and target composition after Initialize is Ready.
    /// Uses the shipped renderer_composite.slang stages; Ready/Pending publishes setup ownership.
    /// Failure preserves the usable L0 profile; failed published setup requires Reset.
    [[nodiscard]] rhi::RasterStatus InitializeViews(const rhi::RasterShaderDescription& vertex,
                                                    const rhi::RasterShaderDescription& fragment) noexcept;
    /// Advance L1 resource/pipeline creation on later owner turns; no GPU wait or backend switch.
    [[nodiscard]] rhi::RasterStatus PollViews() noexcept;
    /// Validate the installed unlit material ABI and prewarm every legal surface/offscreen
    /// conventional/reverse-Z/painter pipeline through R2's bounded cache, between frames.
    /// Two logical program versions; all six pipelines must be Ready before material creation.
    /// Null output required; Ready/Pending grants ownership. Failures preserve output.
    /// No driver pipeline work occurs in DrawView/Submit; no persistent native cache is implied.
    [[nodiscard]] rhi::RasterStatus PrewarmMaterialProgram(const rhi::RasterShaderDescription& vertex,
                                                           const rhi::RasterShaderDescription& fragment,
                                                           MaterialProgram& output) noexcept;
    /// Advance candidate prewarming between frames; Ready is immutable and allocation-free.
    [[nodiscard]] rhi::RasterStatus GetStatus(MaterialProgram handle) noexcept;
    /// Detach program ownership; earlier material/snapshot/GPU uses retain its resources.
    [[nodiscard]] rhi::RasterStatus Release(MaterialProgram& handle) noexcept;
    /// Copy parameters, sampler and exact sampled view; eight logical material versions.
    /// Requires a Ready owned program and Ready RGBA8 texture (or null white fallback).
    /// Ready/Pending grants ownership; invalid input/capacity preserves null output.
    [[nodiscard]] rhi::RasterStatus
    CreateMaterial(MaterialProgram program, const MaterialDescription& description, MaterialVersion& output) noexcept;
    /// Complete candidate binding creation between frames, without blocking.
    [[nodiscard]] rhi::RasterStatus GetStatus(MaterialVersion handle) noexcept;
    /// Atomically publish a Ready candidate and detach current (which may be null).
    /// Both are owned by this renderer and must be different. Failure preserves both;
    /// earlier snapshots/views retain current's old contents. Invalid reload never swaps.
    [[nodiscard]] rhi::RasterStatus PublishMaterial(MaterialVersion& candidate, MaterialVersion& current) noexcept;
    /// Detach material ownership; earlier snapshot/view/GPU uses retain the exact version.
    [[nodiscard]] rhi::RasterStatus Release(MaterialVersion& handle) noexcept;
    /// Enable linear lighting output and opaque shadow rendering after InitializeViews is Ready.
    /// Uses renderer_output.slang and renderer_shadow.slang; Ready/Pending owns setup resources.
    /// HdrColor enables prewarmed RGBA16F flat pipelines; RGBA8 remains the authored fallback.
    /// Failures preserve earlier profiles; a failed published setup requires Reset.
    [[nodiscard]] rhi::RasterStatus InitializeLighting(const rhi::RasterShaderDescription& outputVertex,
                                                       const rhi::RasterShaderDescription& outputFragment,
                                                       const rhi::RasterShaderDescription& shadowFragment) noexcept;
    /// Advance lighting setup without blocking, between frames.
    [[nodiscard]] rhi::RasterStatus PollLighting() noexcept;
    /// Validate renderer_lit.slang or renderer_clustered.slang and prewarm linear RGBA8/RGBA16F plus masked shadows.
    /// Clustered programs require eight bindings and a 16096-byte fragment uniform range;
    /// pass LitMaterialDescription::Clusters when creating their materials. Unsupported capabilities reject.
    /// Shadow fragment uses the same typed uniform schema and a subset of the lit bindings.
    /// Sixteen vertex locations required; HDR variants are omitted when HdrColor is false.
    /// Ready/Pending grants program ownership; failure preserves null output.
    [[nodiscard]] rhi::RasterStatus PrewarmLitProgram(const rhi::RasterShaderDescription& vertex,
                                                      const rhi::RasterShaderDescription& fragment,
                                                      const rhi::RasterShaderDescription& shadowFragment,
                                                      MaterialProgram& output) noexcept;
    /// Copy an immutable lit material and normalized bounded light list, between frames.
    /// Report and null output are preserved on failure. Shadows must be rendered and Ready;
    /// bake/shadow views are retained even after their owners are released.
    [[nodiscard]] rhi::RasterStatus CreateLitMaterial(MaterialProgram program,
                                                      const LitMaterialDescription& description,
                                                      MaterialVersion& output,
                                                      LightSelection& report) noexcept;
    /// Copy light regions and prepare retained conventional-depth caster views/atlas, between frames.
    /// Invalid gutters/overlap, unsupported masked-unlit casters or incomplete/noncanonical cube maps preserve null
    /// output. Ready/Pending grants ownership; drawing is required before material sampling is admitted.
    [[nodiscard]] rhi::RasterStatus
    PrepareShadowAtlas(Snapshot handle, const ShadowAtlasDescription& description, ShadowAtlas& output) noexcept;
    /// Poll retained atlas/instance setup. Ready does not mean it has been rendered yet.
    [[nodiscard]] rhi::RasterStatus GetStatus(ShadowAtlas handle) const noexcept;
    /// Render all prepared regions in an acquired frame; no allocation or shader creation.
    /// @pre Call before indexed draws in this frame; reservedDraws budgets the
    /// subsequent scene/composition draws. Aggregate packet overflow preserves
    /// the unrendered candidate before GPU work; other GPU failures may be partial.
    /// Configure enough Views for every region and retained prior incarnation.
    /// Only one accepted draw per immutable atlas. A partial failure permanently rejects reuse;
    /// recreate a candidate. Caller must end/submit the frame before material creation.
    [[nodiscard]] rhi::RasterStatus DrawShadowAtlas(ShadowAtlas handle,
                                                    ludus::foundation::uint32 reservedDraws = 0) noexcept;
    /// Detach atlas ownership; materials retain the exact rendered texture and caster version.
    [[nodiscard]] rhi::RasterStatus Release(ShadowAtlas& handle) noexcept;
    /// Prepare retained linear scene/display source and immutable exposure/output parameters.
    /// Accepts RGBA8 or supported RGBA16F attachments. Encode=false selects a linear display
    /// target for UI blending; Encode=true permits a final sRGB-encoded RGBA8Unorm
    /// texture for external presentation. Consumers must not encode that again.
    [[nodiscard]] rhi::RasterStatus PrepareLightingPresentation(rhi::TextureHandle source,
                                                                const ViewMapping& mapping,
                                                                const LightingOutput& lighting,
                                                                Presentation& output) noexcept;
    /// Compose a Ready lighting presentation in an acquired frame; select surface or linear RGBA8.
    /// Actual surface encoding selects shader or hardware conversion, exactly once. No blending.
    /// Use Encode=false for a linear UI intermediate. Encode=true encodes once for
    /// RGBA8Unorm offscreen output, or selects shader/hardware surface encoding.
    [[nodiscard]] rhi::RasterStatus DrawLightingPresentation(Presentation handle,
                                                             const rhi::RasterPassDescription& description =
                                                             {
                                                                 .ColorLoad = rhi::RasterLoad::Load,
                                                             }) noexcept;
    /// Release all public identities/resources; accepted CPU/GPU uses retire through RHI.
    /// @pre Owner thread, no open frame; invalidates every previously issued logical handle.
    void Reset() noexcept;
    /// Return compatibility defaults; use GetConfiguredLimits for an initialized renderer.
    [[nodiscard]] static Limits GetLimits() noexcept;
    /// Copied configured capacities, or defaults while uninitialized. Owner-thread only.
    [[nodiscard]] Limits GetConfiguredLimits() const noexcept;
    /// Reserved renderer metadata/workspace bytes, zero while uninitialized.
    /// Exclusions match Limits::HostBytes; this is requested storage, not physical memory usage.
    [[nodiscard]] ludus::foundation::usize GetHostBytes() const noexcept;
    /// Bytes a bound flat program's uniform set copies: each binding's declared stage MinSize,
    /// concatenated in ascending binding order. Zero for an unbound program or before Initialize.
    [[nodiscard]] ludus::foundation::usize GetUniformBytes() const noexcept;
    /// Copy per-draw uniform blocks into an immutable buffer and binding snapshot, between frames.
    /// @param bytes Exactly GetUniformBytes() target-packed bytes; need only survive the call.
    /// @param output Null set receiving ownership on Ready/Pending.
    /// @return NotReady before setup is Ready, InvalidDescription for an unbound program or a
    /// wrong size, CapacityExceeded when Limits::UniformSets are live; failures preserve output.
    [[nodiscard]] rhi::RasterStatus
    CreateUniformSet(ludus::foundation::BorrowedView<const ludus::foundation::uint8> bytes,
                     UniformSet& output) noexcept;
    /// Advance uniform buffer/binding creation without blocking; stale or foreign sets reject.
    [[nodiscard]] rhi::RasterStatus GetStatus(UniformSet handle) noexcept;
    /// Detach set ownership and clear handle; existing snapshots/views retain the exact version.
    [[nodiscard]] rhi::RasterStatus Release(UniformSet& handle) noexcept;
    /// Copy geometry and derive exact mesh-local bounds. Null output required.
    /// Ready/Pending publishes mesh ownership; rejected input preserves output.
    [[nodiscard]] rhi::RasterStatus CreateMesh(const MeshDescription& description, Mesh& output) noexcept;
    /// Poll owned mesh creation; stale/foreign or detached public identities reject.
    [[nodiscard]] rhi::RasterStatus GetStatus(Mesh handle) const noexcept;
    /// Detach mesh ownership and clear handle. Existing snapshots/views retain the old geometry.
    [[nodiscard]] rhi::RasterStatus Release(Mesh& handle) noexcept;
    /// Copy all items and retain mesh incarnations; accepts zero count with null items.
    /// Invalid IDs, duplicate source IDs, nonfinite transforms/colors, capacity or unready meshes
    /// reject atomically, preserving output and all existing scenes. Null output required.
    [[nodiscard]] rhi::RasterStatus
    CreateSnapshot(const SceneItem* items, ludus::foundation::usize count, Snapshot& output) noexcept;
    /// Atomically replace an owned snapshot with a complete copied dynamic scene.
    /// Earlier views retain exact snapshot/mesh versions. Admission failure preserves
    /// current; requires one free snapshot slot, between frames.
    [[nodiscard]] rhi::RasterStatus
    ReplaceSnapshot(const SceneItem* items, ludus::foundation::usize count, Snapshot& current) noexcept;
    /// Detach snapshot ownership and clear handle; existing prepared views remain valid.
    [[nodiscard]] rhi::RasterStatus Release(Snapshot& handle) noexcept;
    /// Cull, safely order and instance a copied view during setup, retaining the snapshot.
    /// Opaque mesh sorting is confined to mutually disjoint projected-bound runs; overlapping
    /// objects and overlays preserve source order. Reference disables sorting/culling/instancing.
    /// Ready/Pending publishes view ownership and report; failures preserve both outputs.
    /// @param handle Owned snapshot incarnation.
    /// @param description Copied view transform and reference policy.
    /// @param output Null prepared view receiving ownership; Pending requires GetStatus before Submit.
    /// @param report Copied visibility/draw/source diagnostics, unchanged on rejection.
    [[nodiscard]] rhi::RasterStatus
    PrepareView(Snapshot handle, const ViewDescription& description, PreparedView& output, ViewReport& report) noexcept;
    /// Poll the prepared immutable instance buffer without blocking.
    [[nodiscard]] rhi::RasterStatus GetStatus(PreparedView handle) const noexcept;
    /// Record/submit conventional-depth packets with R1 clear/depth behavior.
    /// Reverse-Z views return InvalidDescription; use DrawView in an acquired shared frame.
    /// @param handle Ready view. It remains reusable after successful submission.
    /// @param surface Borrowed surface owned by the initialized device.
    /// @param completion Null completion receiving accepted GPU work; may be published on partial failure.
    /// NotReady preserves a finished batch for retry. Other preflight failures drop that batch.
    /// Returned failure is independent of a possible non-null completion (R2 partial-work contract).
    [[nodiscard]] rhi::RasterStatus
    Submit(PreparedView handle, rhi::SurfaceHandle surface, rhi::SubmissionToken& completion) noexcept;
    /// Draw a ready view in an acquired frame using explicit attachment/load/store/rectangles.
    /// L1 must be Ready. Null pass.Color selects the acquired surface; offscreen targets must
    /// be linear RGBA8 attachments. View depth chooses Less/Greater and the matching clear.
    /// Clear is whole-attachment regardless of viewport/scissor; use offscreen composition
    /// for independently cleared neighboring views. Accepted packets retain resources until
    /// the caller ends the frame with R2 EndFrame. Later rejection does not undo earlier work.
    [[nodiscard]] rhi::RasterStatus DrawView(PreparedView handle,
                                             const rhi::RasterPassDescription& description) noexcept;
    /// Copy presentation geometry/mapping and retain the sampled linear RGBA8 attachment.
    /// Four presentation slots, one vertex buffer and binding snapshot each; shared RHI
    /// budgets may exhaust earlier. Ready/Pending publishes output; failure preserves it.
    /// Source must already be Ready, but may have undefined contents until a later draw.
    /// Its pixel aspect must equal the mapping's logical aspect; render resolution may differ.
    /// @param source Owned RHI attachment; may be destroyed after accepted preparation.
    /// @param mapping Resolved output mapping; recreated explicitly after drawable resize.
    /// @param output Null presentation receiving ownership.
    [[nodiscard]] rhi::RasterStatus
    PreparePresentation(rhi::TextureHandle source, const ViewMapping& mapping, Presentation& output) noexcept;
    /// Advance asynchronous presentation geometry/view/binding creation between frames;
    /// may create its binding snapshot once dependencies become Ready. Failure still requires Release.
    [[nodiscard]] rhi::RasterStatus GetStatus(Presentation handle) noexcept;
    /// Composite into the acquired surface, preserving its defined color outside the mapped
    /// viewport/scissor. Undefined sampled color rejects; resize mismatch returns NotReady
    /// before changing the current pass. Mapping uses the actual acquired extent.
    /// Caller ends the shared frame with R2 EndFrame; no renderer allocations during drawing.
    [[nodiscard]] rhi::RasterStatus DrawPresentation(Presentation handle) noexcept;
    /// Detach presentation CPU ownership; sampled texture/geometry retire after accepted GPU uses.
    [[nodiscard]] rhi::RasterStatus Release(Presentation& handle) noexcept;
    /// Drop a prepared view and any retry batch; clear handle. GPU retirement remains in RHI.
    [[nodiscard]] rhi::RasterStatus Release(PreparedView& handle) noexcept;

private:
    [[nodiscard]] rhi::RasterStatus PrewarmProgram(const rhi::RasterShaderDescription& vertex,
                                                   const rhi::RasterShaderDescription& fragment,
                                                   const rhi::RasterShaderDescription* shadow,
                                                   MaterialProgram& output) noexcept;
    [[nodiscard]] rhi::RasterStatus CreateMaterialData(MaterialProgram program,
                                                       const MaterialDescription& description,
                                                       const ludus::foundation::uint8* bytes,
                                                       ludus::foundation::usize size,
                                                       const LitMaterialDescription* lighting,
                                                       MaterialVersion& output,
                                                       const LightClusters* clusters = nullptr) noexcept;
    [[nodiscard]] rhi::RasterStatus PreparePresentationData(rhi::TextureHandle source,
                                                            const ViewMapping& mapping,
                                                            const LightingOutput* lighting,
                                                            Presentation& output) noexcept;
    struct State;
    ludus::foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
