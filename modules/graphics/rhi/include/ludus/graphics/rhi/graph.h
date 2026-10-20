#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/lifetime.h>

namespace ludus::graphics::rhi
{
namespace internal
{
/// Private access to ordered graph identities.
struct GraphAccess;
} // namespace internal
/// Full-attachment initialization at a raster pass boundary.
enum class RasterLoad : ludus::foundation::uint8
{
    /// Initialize the complete attachment with the specified clear value.
    Clear,
    /// Preserve defined contents; undefined contents are an error.
    Load,
    /// Previous contents are undefined; partial draws do not initialize the image.
    Discard,
};
/// Attachment validity after a raster pass.
enum class RasterStore : ludus::foundation::uint8
{
    /// Preserve initialized contents for later passes or executions.
    Store,
    /// Contents become undefined even when the pass cleared them.
    Discard,
};
/// Semantic whole-color-image use, translated privately to backend synchronization.
enum class RasterTextureUse : ludus::foundation::uint8
{
    /// Newly allocated storage has no prior use or defined contents.
    Undefined,
    /// Read in the vertex shader.
    SampledVertex,
    /// Read in the fragment shader.
    SampledFragment,
    /// Read in either raster shader stage.
    SampledBoth,
    /// Read/write as a color attachment.
    ColorAttachment,
};
/// Top-left physical-pixel rectangle with half-open edges; zero area clips every draw.
struct RasterRectangle final
{
    /// Left edge in physical pixels.
    ludus::foundation::uint32 X = 0;
    /// Top edge in physical pixels.
    ludus::foundation::uint32 Y = 0;
    /// Horizontal pixel count.
    ludus::foundation::uint32 Width = 0;
    /// Vertical pixel count.
    ludus::foundation::uint32 Height = 0;
};
/// One selected color attachment, with independent initialization and optional MSAA resolve.
struct RasterColorAttachment final
{
    /// Ready attachment view of exactly one mip/layer; retained through execution/completion.
    TextureViewHandle View{};
    /// Optional single-sample attachment view of matching extent/format. Source must be four-sample.
    /// Resolve initializes the destination even when source Store is Discard, provided source is defined.
    TextureViewHandle Resolve{};
    /// Full selected-subresource initialization, independent of viewport/scissor.
    RasterLoad Load = RasterLoad::Clear;
    /// Validity of the selected source subresource after the pass.
    RasterStore Store = RasterStore::Store;
    /// Linear clear RGBA, finite in [0,1].
    ludus::foundation::float32 Clear[4]{0, 0, 0, 1};
};
/// One raster pass: explicit color views or the legacy color/private-depth pair.
/// With empty Colors, null Color selects the single-sample acquired surface.
/// Explicit offscreen views select one mip/layer per attachment and optional color resolves.
struct RasterPassDescription final
{
    /// Owned attachment texture, or null for the acquired surface.
    TextureHandle Color{};
    /// Full-color initialization contract.
    RasterLoad ColorLoad = RasterLoad::Clear;
    /// Whether initialized color survives the pass.
    RasterStore ColorStore = RasterStore::Store;
    /// Linear clear RGBA values, each finite and in [0,1].
    ludus::foundation::float32 Clear[4]{0, 0, 0, 1};
    /// Full-depth initialization using ClearDepth; independent of viewport/scissor.
    RasterLoad DepthLoad = RasterLoad::Clear;
    /// Whether initialized depth survives for a later pass on this attachment.
    RasterStore DepthStore = RasterStore::Discard;
    /// Reject depth-writing pipelines; Load is required for read-only depth.
    bool DepthReadOnly = false;
    /// Finite canonical clear depth in [0,1]; zero pairs with reverse-Z comparisons.
    ludus::foundation::float32 ClearDepth = 1;
    /// Explicit viewport when UseViewport is true; must fit the actual attachment.
    RasterRectangle Viewport{};
    /// Explicit scissor when UseScissor is true; intersected with attachment bounds.
    RasterRectangle Scissor{};
    /// False selects the complete actual attachment, including after resize.
    bool UseViewport = false;
    /// False clips only against attachment bounds. An enabled empty scissor is a no-op.
    bool UseScissor = false;
    /// Borrowed offscreen color entries, consumed by BeginRasterPass or copied by AddGraphPass.
    /// Non-null when ColorCount is positive; zero count uses Color.
    /// Nonempty requires null Color. Depth/stencil belongs to the first view's selected
    /// mip/layer. All sources have matching extents/sample counts; allocations must
    /// be distinct (including resolves) under conservative whole-allocation hazards.
    const RasterColorAttachment* Colors = nullptr;
    /// Number of borrowed Colors entries; zero selects the legacy Color attachment.
    ludus::foundation::usize ColorCount = 0;
    /// Private stencil initialization; ignored for a depth-only allocation.
    RasterLoad StencilLoad = RasterLoad::Clear;
    /// Stencil validity after the pass; independent of color and depth.
    RasterStore StencilStore = RasterStore::Discard;
    /// Read-only stencil requires Load and rejects nonzero pipeline stencil writes.
    bool StencilReadOnly = false;
    /// Eight-bit clear stencil value.
    ludus::foundation::uint32 ClearStencil = 0;
};
/// Snapshot from the single RHI registry; describes accepted same-queue work,
/// including pending GPU uses, rather than presentation or CPU frame numbers.
struct RasterTextureState final
{
    /// Non-wrapping mutation revision; compare when importing into a graph.
    ludus::foundation::uint64 Revision = 0;
    /// Most recent accepted use; null means no submitted frame use.
    SubmissionToken Dependency{};
    /// Last semantic color use, including sampled bindings of direct draws.
    RasterTextureUse Use = RasterTextureUse::Undefined;
    /// Clear/upload followed by Store establishes full valid color contents.
    bool ColorDefined = false;
    /// Clear followed by Store establishes full valid private depth contents.
    bool DepthDefined = false;
    /// Every private stencil subresource is initialized and stored.
    bool StencilDefined = false;
};
/// Query an owned texture on the owner thread; failure preserves output.
[[nodiscard]] RasterStatus GetTextureState(DeviceHandle, TextureHandle, RasterTextureState&) noexcept;
/// Switch passes inside an open frame. Offscreen-only frames require owned texture attachments.
/// Validates load/feedback/state before
/// native calls; ends the previous pass and starts this one. Direct draws must
/// match its attachment format. Rejected calls preserve the current pass.
[[nodiscard]] RasterStatus BeginRasterPass(DeviceHandle, const RasterPassDescription&) noexcept;
/// Opaque graph identity, retaining copied packets and resource leases. Owner-thread
/// only; copies do not transfer ownership. Execute consumes it once encoding begins.
struct OrderedGraph final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private compiler access to the graph incarnation.
    friend struct internal::GraphAccess;
};
/// Resource version scoped to one graph incarnation; zero-initialized is invalid.
struct GraphVersion final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Graph = 0;
    ludus::foundation::uint32 Resource = 0;
    ludus::foundation::uint32 Version = 0;
    /// Grants the private compiler access to resource/version provenance.
    friend struct internal::GraphAccess;
};
/// Actual use of a declared buffer range or complete color texture.
enum class GraphAccessMode : ludus::foundation::uint8
{
    /// Vertex stream read; range must cover the packet's slice.
    Vertex,
    /// Index read; range must cover every indexed byte.
    Index,
    /// Reflected uniform range read.
    Uniform,
    /// Reflected texture binding read, including inactive extra bindings.
    Sampled,
    /// Attachment write to a new version; Clear or Discard.
    ColorWrite,
    /// Attachment Load of the preceding version and write of a new version.
    ColorReadWrite,
    /// Read-only compute storage range.
    StorageRead,
    /// Read/write compute storage range, writing the next in-place version.
    StorageReadWrite,
    /// Indexed indirect argument range.
    Indirect,
};
/// One declared use; whole textures and conservative whole-buffer hazards.
struct GraphUse final
{
    /// Read version, or newly reserved output version for attachment/storage writes.
    GraphVersion Resource{};
    /// Required resource role and access.
    GraphAccessMode Access = GraphAccessMode::Sampled;
    /// Buffer start in bytes; zero for textures.
    ludus::foundation::usize Offset = 0;
    /// Nonzero buffer length; zero for textures.
    ludus::foundation::usize Size = 0;
    /// Required stages for uniform/sample/storage uses; storage requires Compute.
    /// Vertex/index/indirect and attachment uses ignore it.
    RasterVisibility Visibility = RasterVisibility::Both;
};
/// Copied authored pass; no callbacks, lambdas or borrowed data survive AddGraphPass.
struct GraphPassDescription final
{
    /// Stable diagnostic name, 1..63 bytes; copied, not retained.
    const char* Name = nullptr;
    /// Optional source path, at most 127 bytes; copied for reports.
    const char* Source = nullptr;
    /// One-based source line, or zero when unavailable.
    ludus::foundation::uint32 Line = 0;
    /// Actual attachment/load/store state. Surface passes are present roots.
    RasterPassDescription Attachment;
    /// Complete use declarations; at most sixteen, unique physical resources.
    const GraphUse* Uses = nullptr;
    /// Number of Uses; pointer may be null only when zero.
    ludus::foundation::usize UseCount = 0;
    /// Complete immutable draws, copied/retained and preflighted before execution.
    const RasterDraw* Draws = nullptr;
    /// Number of Draws; graph total at most 256.
    ludus::foundation::usize DrawCount = 0;
    /// Observable work which must survive culling.
    bool SideEffect = false;
    /// Optional single compute dispatch. Non-null forbids raster attachments/draws;
    /// copied and retained before return. Null selects the existing raster pass.
    const ComputeDispatch* Dispatch = nullptr;
};
/// Resource roots observable after this execution.
enum class GraphRoot : ludus::foundation::uint8
{
    /// An external consumer will use this version.
    Export,
    /// Future-frame history write, even if unused by this frame.
    History,
    /// Readback consumer; scheduling/copy-out remains the R2 service's job.
    Readback,
};
/// Semantic dependency reason, independent of backend synchronization enums.
enum class GraphHazard : ludus::foundation::uint8
{
    /// Earlier writer must make data visible to this reader.
    ReadAfterWrite,
    /// Earlier reader must finish before this in-place overwrite.
    WriteAfterRead,
    /// Ordered in-place writes, even when contents are discarded.
    WriteAfterWrite,
};
/// Sentinel for an external pass or an unavailable pass/resource index; never an authored index.
inline constexpr ludus::foundation::uint32 GRAPH_EXTERNAL_INDEX = ~ludus::foundation::uint32{0};
/// Full diagnostic for one authored pass. Copied values own their bounded text.
struct GraphPassDiagnostic final
{
    /// Copied authored pass name, zero-terminated.
    char Name[64]{};
    /// Copied source path, zero-terminated; empty when not supplied.
    char Source[128]{};
    /// Authored source line, zero when not supplied.
    ludus::foundation::uint32 Line = 0;
    /// True when root analysis removed this pass.
    bool Culled = false;
    /// First pass in its raster group; GRAPH_EXTERNAL_INDEX for culled/compute passes.
    ludus::foundation::uint32 RasterGroup = GRAPH_EXTERNAL_INDEX;
};
/// Full diagnostic for one logical graph resource.
struct GraphResourceDiagnostic final
{
    /// First live authored use, or GRAPH_EXTERNAL_INDEX when unused.
    ludus::foundation::uint32 FirstUse = GRAPH_EXTERNAL_INDEX;
    /// Last live authored use, or GRAPH_EXTERNAL_INDEX when unused.
    ludus::foundation::uint32 LastUse = GRAPH_EXTERNAL_INDEX;
};
/// One bounded report entry; pass indices are zero-based authored positions.
struct GraphDependency final
{
    /// Producer/reader pass, or GRAPH_EXTERNAL_INDEX for an external import.
    ludus::foundation::uint32 Before = GRAPH_EXTERNAL_INDEX;
    /// Dependent pass, or GRAPH_EXTERNAL_INDEX for the final external use.
    ludus::foundation::uint32 After = GRAPH_EXTERNAL_INDEX;
    /// Zero-based logical resource index.
    ludus::foundation::uint32 Resource = 0;
    /// RAW, WAR or WAW reason.
    GraphHazard Hazard = GraphHazard::ReadAfterWrite;
    /// Earlier semantic access.
    GraphAccessMode Source = GraphAccessMode::Sampled;
    /// Later semantic access.
    GraphAccessMode Destination = GraphAccessMode::Sampled;
};
/// Owner-thread investigation controls; neither option changes authored draw order.
struct GraphCompileOptions final
{
    /// Keep every legal authored pass when false; initialization is still validated.
    bool CullPasses = true;
    /// Opt into conservative consecutive raster-pass merging. Equal attachment,
    /// read scopes and rectangles, Load/Store preservation and no side effects are
    /// required. Compute and incompatible passes always retain their boundaries.
    bool MergeRasterPasses = false;
};
/// Deterministic fixed-size summary copied without allocation. Totals cover the full plan.
/// Diagnostic arrays contain only prefixes, bounded by ReportedPasses/Resources/Dependencies.
/// Use the indexed diagnostic queries for larger plans; never index arrays by total counts.
struct GraphReport final
{
    /// Total authored passes admitted under the configured R6 limit.
    ludus::foundation::uint32 PassCount = 0;
    /// Total logical resources in import/transient creation order, including unused imports.
    ludus::foundation::uint32 ResourceCount = 0;
    /// Passes retained by roots, or all passes in reference mode.
    ludus::foundation::uint32 LivePasses = 0;
    /// Planned raster encoder/pass starts, excluding the facade's acquisition clear.
    ludus::foundation::uint32 RasterPasses = 0;
    /// Live compute dispatches; each retains a separate compute scope.
    ludus::foundation::uint32 ComputePasses = 0;
    /// Raster boundaries removed by the explicit merge option.
    ludus::foundation::uint32 MergedPasses = 0;
    /// Live direct and indirect draw packets; zero-instance indirect work still counts.
    ludus::foundation::uint32 DrawCount = 0;
    /// Owned graph control/payload bytes, including allocated slack; excludes shared table pages and compiler scratch.
    ludus::foundation::usize PlanStorageBytes = 0;
    /// Sum of imported buffer sizes, including unused imports; logical bytes, not driver allocation.
    ludus::foundation::uint64 BufferBytes = 0;
    /// Imported initialized mip texel bytes and private 32-bit depth bytes for attachments.
    /// Includes unused imports; excludes driver padding, staging, surface and pool slack.
    ludus::foundation::uint64 TextureBytes = 0;
    /// Total semantic dependencies, including entries beyond the copied prefix.
    ludus::foundation::uint32 DependencyCount = 0;
    /// Failing pass, or GRAPH_EXTERNAL_INDEX when not applicable.
    ludus::foundation::uint32 ErrorPass = GRAPH_EXTERNAL_INDEX;
    /// Failing resource, or GRAPH_EXTERNAL_INDEX when not applicable.
    ludus::foundation::uint32 ErrorResource = GRAPH_EXTERNAL_INDEX;
    /// Conservative whole-buffer tracking is always used in this initial profile.
    bool WholeBufferHazards = true;
    /// Number of initialized entries in Names/Sources/Lines/Culled/RasterGroup, at most 32.
    ludus::foundation::uint32 ReportedPasses = 0;
    /// Number of initialized FirstUse/LastUse entries, at most 16.
    ludus::foundation::uint32 ReportedResources = 0;
    /// Number of initialized copied Dependencies entries, at most 512.
    ludus::foundation::uint32 ReportedDependencies = 0;
    /// True when any diagnostic prefix omits full-plan entries; indexed queries remain complete.
    bool DiagnosticsTruncated = false;
    /// Authored pass diagnostic names.
    char Names[32][64]{};
    /// Copied setup source locations.
    char Sources[32][128]{};
    /// Copied setup line numbers.
    ludus::foundation::uint32 Lines[32]{};
    /// True for passes removed by root analysis.
    bool Culled[32]{};
    /// Authored index of the first pass in this raster group; GRAPH_EXTERNAL_INDEX for culled/compute passes.
    ludus::foundation::uint32 RasterGroup[32]{};
    /// First live use for each resource, or GRAPH_EXTERNAL_INDEX if unused.
    ludus::foundation::uint32 FirstUse[16]{};
    /// Last live use for each resource, or GRAPH_EXTERNAL_INDEX if unused.
    ludus::foundation::uint32 LastUse[16]{};
    /// Semantic dependencies in stable pass/resource/hazard order.
    GraphDependency Dependencies[512]{};
};
/// Copy an authored pass diagnostic after a compile attempt; owner-thread only, no allocation.
/// NotReady before a compile attempt; InvalidDescription for an out-of-range authored index.
/// Admission failure and all rejected queries preserve output.
[[nodiscard]] RasterStatus
GetGraphPassDiagnostic(DeviceHandle, OrderedGraph, ludus::foundation::uint32, GraphPassDiagnostic&) noexcept;
/// Copy a resource lifetime diagnostic after a compile attempt; failures preserve output.
/// Resource indices refer to import order; owner-thread only, no allocation.
[[nodiscard]] RasterStatus
GetGraphResourceDiagnostic(DeviceHandle, OrderedGraph, ludus::foundation::uint32, GraphResourceDiagnostic&) noexcept;
/// Copy one full-plan dependency after a compile attempt; failures preserve output.
/// Index follows deterministic dependency order; owner-thread only, no allocation.
[[nodiscard]] RasterStatus
GetGraphDependency(DeviceHandle, OrderedGraph, ludus::foundation::uint32, GraphDependency&) noexcept;
/// Reserve a graph under StorageLimits::Graphs; null output required, failure preserves it.
[[nodiscard]] RasterStatus CreateOrderedGraph(DeviceHandle, OrderedGraph&) noexcept;
/// Import current buffer contents and freeze their accepted revision, retaining the
/// physical record. Output version starts at zero. No duplicate physical imports;
/// resource count is bounded by StorageLimits::ResourcesPerGraph. Compute writes invalidate earlier frozen plans.
[[nodiscard]] RasterStatus ImportGraphBuffer(DeviceHandle, OrderedGraph, BufferHandle, GraphVersion&) noexcept;
/// Import a persistent texture using an exact registry snapshot. Stale snapshots
/// reject; final use must be a sampled stage or ColorAttachment. Same-queue pending
/// dependencies need no host wait. Pool textures cannot be imported by another graph.
[[nodiscard]] RasterStatus ImportGraphTexture(DeviceHandle,
                                              OrderedGraph,
                                              TextureHandle,
                                              const RasterTextureState&,
                                              RasterTextureUse finalUse,
                                              GraphVersion&) noexcept;
/// Acquire a matching complete attachment object under StorageLimits::TransientTextures.
/// Reuse requires no other leases and actual GPU completion. Contents begin undefined.
/// Returned texture is borrowed from the graph/pool: Destroy rejects it; create views
/// and binding sets normally and destroy those owners to permit later pooling. A stale
/// borrowed texture cannot be used once its graph is discarded/consumed. No heap aliasing.
/// Allocation occurs during setup, before compilation or frame execution; Pending owns
/// both outputs and can be polled using GetStatus on the texture before setup continues.
[[nodiscard]] RasterStatus
CreateGraphTexture(DeviceHandle, OrderedGraph, const TextureDescription&, GraphVersion&, TextureHandle&) noexcept;
/// Reserve the next attachment or storage-buffer version without new physical storage.
/// May reserve before its producer is added; compilation verifies authored ordering.
[[nodiscard]] RasterStatus NextGraphVersion(DeviceHandle, OrderedGraph, GraphVersion, GraphVersion&) noexcept;
/// Copy one ordered pass and retain its validated packets. Failure changes no graph
/// state. Uses/declarations are verified by CompileOrderedGraph before any GPU work.
[[nodiscard]] RasterStatus AddGraphPass(DeviceHandle, OrderedGraph, const GraphPassDescription&) noexcept;
/// Mark a version as externally observable. Transient pool resources cannot escape;
/// History/Export require persistent textures or buffers. Compilation rejects undefined roots.
[[nodiscard]] RasterStatus AddGraphRoot(DeviceHandle, OrderedGraph, GraphVersion, GraphRoot) noexcept;
/// Validate versions, actual packet uses, initialization, capacities and roots, then
/// report dependencies/lifetimes. Reference mode keeps all otherwise legal passes.
/// Failure preserves setup for repair/discard and writes its diagnostic report. Success
/// freezes setup; no execution occurs. Whole resources remain physically distinct.
[[nodiscard]] RasterStatus
CompileOrderedGraph(DeviceHandle, OrderedGraph, GraphReport&, bool reference = false) noexcept;
/// Compile with explicit culling/merging controls, retaining the same validation,
/// failure, freeze and ownership contract as the compatibility overload above.
/// Reports logical passes/dependencies separately from planned raster groups.
[[nodiscard]] RasterStatus
CompileOrderedGraph(DeviceHandle, OrderedGraph, GraphReport&, const GraphCompileOptions&) noexcept;
/// Preflight frozen imports/packets and backend pass objects, acquire a frame, encode
/// authored live order and submit once. Preflight/target skip preserves graph/token for
/// retry; once encoding begins graph is consumed even on failure. Partial backend work
/// faults the session. Success publishes GPU completion and commits the existing ledger.
[[nodiscard]] RasterStatus ExecuteOrderedGraph(DeviceHandle, SurfaceHandle, OrderedGraph&, SubmissionToken&) noexcept;
/// Encode a compiled graph into the device's acquired frame without submitting it.
/// The graph must have been built between frames; imports and packets are revalidated.
/// Consumes the graph once encoding starts, retaining all GPU uses in the current
/// frame ledger. The caller must EndFrame even after a partial failure and retain
/// outputs through its actual submission completion. Owner thread only; no waiting.
/// @param device Owned device with an acquired frame on surface.
/// @param surface Surface of that frame.
/// @param inoutGraph Compiled graph; cleared after accepted encoding, preserved on preflight rejection.
[[nodiscard]] RasterStatus
EncodeOrderedGraph(DeviceHandle device, SurfaceHandle surface, OrderedGraph& inoutGraph) noexcept;
/// Drop all graph packet/resource leases without GPU work; clears handle on success.
/// Invalid handle or open frame preserves it. Pool reuse still waits for completion.
[[nodiscard]] RasterStatus DiscardOrderedGraph(DeviceHandle, OrderedGraph&) noexcept;
} // namespace ludus::graphics::rhi
