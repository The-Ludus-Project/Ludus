#pragma once

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer
{
/// Fixed ordered-graph inputs: eight transport/image buffers, two textures and one instance stream.
inline constexpr foundation::uint32 VISIBILITY_GRAPH_FIXED_RESOURCES = 11;
/// Request at least this StorageLimits::ResourcesPerGraph before device creation to admit the
/// full visibility profile: fixed inputs, both structured buffers and vertex/index buffers for every mesh.
/// Smaller quotas remain legal, but preparation can report CapacityExceeded for larger scenes.
inline constexpr foundation::uint32 VISIBILITY_GRAPH_RESOURCE_CAPACITY =
    VISIBILITY_GRAPH_FIXED_RESOURCES +
    static_cast<foundation::uint32>(content::render_graph_format::MAX_STORAGE_BUFFERS) +
    2 * static_cast<foundation::uint32>(content::render_graph_format::MAX_MESHES);

/// Numeric offline plan; all records are copied at initialization. Borrowed views
/// must be valid for the call only. This is not a runtime graph/source parser.
struct RenderGraphPlan final
{
    content::render_graph_format::Header Description{};                               ///< Version and table extents.
    foundation::BorrowedView<const content::render_graph_format::Resource> Resources; ///< Logical attachments.
    foundation::BorrowedView<const content::render_graph_format::Camera> Cameras; ///< Active camera target contracts.
    foundation::BorrowedView<const content::render_graph_format::Pass> Passes;    ///< Authored serial execution order.
    /// Per-technique uniform bindings; empty when no technique reads uniforms.
    foundation::BorrowedView<const content::render_graph_format::UniformBinding> UniformBindings;
    /// Occupied block size of each binding on each carried target.
    foundation::BorrowedView<const content::render_graph_format::UniformBlock> UniformBlocks;
    /// Target byte offset of each schema field of each binding.
    foundation::BorrowedView<const content::render_graph_format::UniformField> UniformFields;
    /// Optional authored names, one per Passes entry, copied during Initialize. Nonempty UTF-8,
    /// shorter than rhi::GPU_SCOPE_NAME_BYTES. Empty table uses a generic label with a stable ordinal identity.
    foundation::BorrowedView<const foundation::BorrowedView<const char>> PassNames{};
    /// Native ray storage descriptors, copied during Initialize. Empty for legacy/raster plans.
    // Keep partial aggregate callers warning-clean when this optional field is omitted.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const content::render_graph_format::StorageBuffer> StorageBuffers{};
};
/// Resolve one fixed or view-relative resource without GPU work. Relative dimensions
/// use ceil(reference * numerator / denominator), with a minimum of one pixel.
/// Pixel buffers multiply the resolved pixel count by BytesPerPixel, returning capacity in Width.
/// @param resource Authored fixed dimensions or a validated view-relative ratio; borrowed for the call.
/// @param referenceWidth Drawable pixels, before render scale; zero suspends relative resolution.
/// @param referenceHeight Drawable pixels, with the same contract as referenceWidth.
/// @param outResource Resolved copy, unchanged on failure; its policy remains intact for later resizes.
/// @return Ready on success; NotReady for a zero relative reference; InvalidDescription
/// for malformed policies; CapacityExceeded above MAX_TARGET_EXTENT or MAX_RAY_BUFFER_BYTES.
/// Fixed resources ignore the reference.
[[nodiscard]] rhi::RasterStatus
ResolveRenderGraphResource(const content::render_graph_format::Resource& resource,
                           foundation::uint32 referenceWidth,
                           foundation::uint32 referenceHeight,
                           content::render_graph_format::Resource& outResource) noexcept;
/// Cooked static raster stage pairs and linked triangle ray compute/image stages.
/// No compiler or shader-source discovery. Ray plans also carry ColorVertex/ColorFragment
/// for shared static mesh ownership; that pair never rasterizes the ray scene.
/// Artifact/reflection storage remains immutable and alive until Initialize/Poll
/// reaches Ready; generated package headers provide static storage.
/// Each selected vertex uses the six Renderer flat locations. Raster stage bindings,
/// when present, are read-only uniform buffers that exactly match the plan's uniform
/// binding records. Ray compute can additionally bind the plan's StorageBuffers
/// at user slots 6/7. A depth-only stage pair shares its family's exact geometry
/// stage; the masked depth fragment is a separately authored coverage function.
struct RenderGraphPrograms final
{
    rhi::RasterShaderDescription ColorVertex{};         ///< Authored ColorAndDepth geometry entry.
    rhi::RasterShaderDescription ColorFragment{};       ///< Authored color entry.
    rhi::RasterShaderDescription DepthVertex{};         ///< Authored DepthOnly geometry entry.
    rhi::RasterShaderDescription DepthFragment{};       ///< Authored void depth entry.
    rhi::RasterShaderDescription CompositeVertex{};     ///< Installed renderer_composite recipe vertex.
    rhi::RasterShaderDescription CompositeFragment{};   ///< Installed renderer_composite recipe fragment.
    rhi::RasterShaderDescription MaskedColorVertex{};   ///< Authored MaskedColorAndDepth geometry entry.
    rhi::RasterShaderDescription MaskedColorFragment{}; ///< Authored masked color entry with its coverage function.
    rhi::RasterShaderDescription MaskedDepthVertex{};   ///< Authored MaskedDepthOnly geometry entry.
    rhi::RasterShaderDescription MaskedDepthFragment{}; ///< Separately authored void masked coverage entry.
    rhi::RasterShaderDescription RayCompute{};          ///< Linked authored triangle ray functions and BVH traversal.
    rhi::RasterShaderDescription RayImageVertex{};      ///< Renderer-owned GPU-produced pixel presentation.
    rhi::RasterShaderDescription RayImageFragment{};    ///< Renderer-owned color copy; no host pixel transfer.
    /// Independent raster stage pairs in numeric technique order after the legacy roles.
    /// Index (Program - LEGACY_TECHNIQUE_COUNT) * TECHNIQUE_STAGE_COUNT selects a
    /// vertex description; the following entry is its fragment description. All
    /// artifact/input/binding storage is borrowed through initialization/preparation,
    /// with the same lifetime contract as ColorVertex and ColorFragment.
    foundation::StaticArray<rhi::RasterShaderDescription,
                            content::render_graph_format::ADDITIONAL_RASTER_TECHNIQUE_COUNT *
                                content::render_graph_format::TECHNIQUE_STAGE_COUNT>
        AdditionalRasterStages{};
    /// Linked ray kernel reads the paired raster visibility/depth inputs declared by the plan.
    bool HasVisibilityBuffer = false;
    bool HasPersistentRayBuffers = false; ///< Extended ordered ray ABI with two persistent uint4 storage slots.
    bool HasSceneShaderInterface = false; ///< Opt-in material/light/query callback ABI, encoded in cooked stage roles.
};
/// Attach the frozen triangle-ray compute/image interface metadata to a cooked stage.
/// Does not compile, allocate or change artifact bytes. Used by generated headers and
/// runtime decoders; the RHI independently checks actual native shader reflection.
/// @param inoutDescription Receives binding metadata, and workgroup size for Compute.
/// @param stage Compute for the linked ray kernel, Vertex/Fragment for the image recipe.
/// @param hasVisibilityBuffer Adds paired unfiltered visibility/depth compute texture inputs.
/// @param hasPersistentBuffers Use the extended two-buffer compute ABI; image stages are unchanged.
void SetRenderGraphRayInterface(rhi::RasterShaderDescription& inoutDescription,
                                rhi::ShaderStage stage,
                                bool hasPersistentBuffers = false,
                                bool hasVisibilityBuffer = false) noexcept;
/// Declared geometry interface of an extracted draw item.
enum class RenderGraphLayout : foundation::uint8
{
    Static,      ///< Typed MeshDescription and immutable static local-to-world transform.
    Unsupported, ///< The position path is unknown; only an explicit bounds proxy may be drawn.
};
/// Required material/coverage semantics; never disappear in a query filter.
enum class RenderGraphMaterial : foundation::uint8
{
    Opaque,      ///< Static opaque coverage; covered by DefaultStaticOpaque or triangle ray techniques.
    Masked,      ///< Authored coverage; covered only by explicitly authored DefaultStaticMasked techniques.
    Transparent, ///< Does not implicitly participate in pre-Z.
    Unsupported, ///< Unmatched material or deformation contract.
};
/// Mesh table entry copied into both authored technique contexts during setup.
struct RenderGraphMesh final
{
    MeshDescription Geometry{};                           ///< Borrowed static geometry; consumed before Update returns.
    RenderGraphLayout Layout = RenderGraphLayout::Static; ///< Declared interface, validated independently of intention.
    foundation::math::Aabb3 BoundsMesh{}; ///< Explicit finite bounds for an unknown-position diagnostic proxy.
    bool HasDiagnosticBounds = false;     ///< Whether BoundsMesh is authoritative and may be rendered as a proxy.
    /// Triangle-to-material-slot map; empty means slot zero. Must have IndexCount/3 entries.
    foundation::BorrowedView<const foundation::uint32> MaterialSlots{};
    /// Slot-to-global-material mapping; unassigned slots use DEFAULT_SCENE_MATERIAL.
    foundation::BorrowedView<const foundation::uint32> MaterialBindings{};
};
/// One intended draw item/submesh/material slot extracted by the World adapter.
struct RenderGraphItem final
{
    foundation::uint64 SourceId = 0;         ///< Nonzero unique diagnostic identity.
    foundation::uint32 MeshIndex = 0;        ///< Index into the borrowed mesh table.
    foundation::math::Affine3 MeshToWorld{}; ///< Static geometry transform; deformation remains unsupported.
    FlatMaterial Material{};                 ///< Runtime color values; never shader permutation inputs.
    RenderGraphMaterial Coverage = RenderGraphMaterial::Opaque; ///< Required coverage/deformation contract.
    bool IsIntentionallyExcluded = false; ///< Authored exclusion, distinct from renderer visibility culling.
    /// Per-draw logical uniform values, laid out by the plan's uniform schemas (the generated
    /// header names each field's offset). Exactly UniformValueCount finite values are required
    /// for every item that is not intentionally excluded when the plan binds uniforms, and must
    /// be empty otherwise. Copied and packed for the device's target before Update returns;
    /// runtime data, never a shader permutation input.
    // Explicit default preserves partial aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const foundation::float32> Uniforms{};
    /// Per-instance slot overrides; INHERIT_SCENE_MATERIAL retains the mesh binding.
    foundation::BorrowedView<const foundation::uint32> MaterialOverrides{};
    foundation::math::Vector4 SurfaceTint{1, 1, 1, 1}; ///< Instance multiplier for the scene material profile.
};
/// Borrowed complete ray-scene inputs consumed during BeginGpuPreview. Not retained.
struct RenderGraphSceneDescription final
{
    foundation::BorrowedView<const RenderGraphMesh> Meshes{}; ///< Geometry and slot bindings.
    foundation::BorrowedView<const RenderGraphItem> Items{};  ///< Instances and overrides.
    SceneShaderInputs Inputs{};                               ///< Materials, analytic sources and constant environment.
    bool HasMaterialInterface = false;                        ///< False preserves legacy flat-color submissions.
    bool AreErrorsAllowed = false;                            ///< Preview uses an explicit non-emissive error material.
    SceneShaderDiagnostics* DiagnosticsOrNull = nullptr; ///< Optional candidate diagnostic output, including rejection.
    /// Caller scene revision; also seeds graph sampling when a new extraction starts history.
    /// Zero is reserved for standalone diagnostic preparation. Different revisions select different streams.
    foundation::uint64 SourceGeneration = 0;
    /// Selected ::ludus::content::render_graph_format::SCENE_DATA_* groups; Lights retain emitter emission
    /// dependencies. Consumed before Update returns.
    foundation::uint32 SceneData = ::ludus::content::render_graph_format::SCENE_DATA_ALL;
};
/// Scene compatibility for a rendering role, independent of runtime culling.
enum class RenderGraphCoverage : foundation::uint8
{
    Covered,               ///< Compatible active route; not a promise of visibility.
    Unsupported,           ///< Layout/material/coverage mismatch; diagnostic rendering does not repair it.
    Conflicting,           ///< Repeated incompatible role or duplicate draw identity.
    IntentionallyExcluded, ///< Explicit authored omission.
};
/// Reason retained even when an error-color/proxy route draws this item.
enum class RenderGraphReason : foundation::uint8
{
    None,      ///< Compatible route or intentional exclusion.
    Layout,    ///< Unknown mesh position/interface.
    Material,  ///< No route with a matching material family, or transparency/deformation.
    Duplicate, ///< Duplicate source identity or repeated role without author permission.
};
/// Per-item/per-camera/role coverage record; owned by the graph until its next Update or Reset.
struct RenderGraphCoverageRecord final
{
    foundation::uint64 SourceId = 0; ///< Extraction identity.
    foundation::uint32 Camera = 0;   ///< Generation-local camera index.
    content::render_graph_format::Aspect Role = content::render_graph_format::Aspect::Color; ///< Required output role.
    RenderGraphCoverage Status = RenderGraphCoverage::Unsupported;                           ///< Compatibility result.
    RenderGraphReason Reason = RenderGraphReason::None; ///< Preserved mismatch explanation.
    bool HasDiagnosticRoute = false; ///< A color marker/proxy may render; never establishes depth correctness.
};
/// Editor and shipping admission are explicit policies.
enum class RenderGraphCoveragePolicy : foundation::uint8
{
    RejectIncomplete, ///< Preserve the last drawable scene on unsupported/conflicting coverage.
    DiagnosticColor,  ///< Admit an editor diagnostic color/proxy route while retaining unsupported reports.
};
/// Summary of the latest candidate, separately from the last rendered image.
struct RenderGraphReport final
{
    SceneShaderDiagnostics SceneDiagnostics{}; ///< Material defaults/errors and discarded collapsed faces.
    foundation::uint64 SceneRevision = 0;      ///< Caller extraction revision, not a GPU completion number.
    foundation::uint32 Covered = 0;            ///< Compatible item/role/view tuples.
    foundation::uint32 Unsupported = 0;        ///< Missing required compatibility.
    foundation::uint32 Conflicting = 0;        ///< Ambiguous/duplicate role tuples.
    foundation::uint32 Excluded = 0;           ///< Explicit omissions.
    bool IsAccepted = false;                   ///< Candidate was accepted for asynchronous preparation.
    bool IsReady = false;                      ///< Accepted scene is drawable; producer completion is separate.
};
namespace internal
{
/// Private access from RenderGraph to its attachment pool.
struct RenderGraphPoolAccess;
} // namespace internal
/// Largest idle-entry capacity of one attachment pool: every backing of the largest plan.
/// Idle entries keep their RHI texture records and logical bytes (StorageLimits::ResourcesPerKind
/// and ResourceBytes), so choose a smaller capacity or Trim when those budgets are tight.
inline constexpr foundation::uint32 RENDER_GRAPH_POOL_CAPACITY =
    static_cast<foundation::uint32>(content::render_graph_format::MAX_RESOURCES);
/// Copied counters of one attachment pool; counts are allocations, never bytes. Measure bytes
/// with rhi::GetAllocationTelemetry, which owns the RHI size accounting.
struct RenderGraphPoolReport final
{
    foundation::uint32 IdleAllocations = 0;    ///< Released transient attachments awaiting an exact match.
    foundation::uint32 LeasedAllocations = 0;  ///< Pool-provided transient attachments currently owned by graphs.
    foundation::uint64 CreatedAllocations = 0; ///< Acquisitions without a reusable match; each created an attachment.
    foundation::uint64 ReusedAllocations = 0;  ///< Acquisitions served by an idle exact match with proven completion.
    /// Acquisitions that found idle exact matches but created an attachment, because none had proven completion.
    foundation::uint64 DeferredAcquisitions = 0;
    foundation::uint64 DestroyedAllocations = 0; ///< Idle entries destroyed by capacity, Trim, Reset or failed proof.
};
/// RG4 exact-descriptor pool for graph-owned transient attachments: allocations backing logical
/// resources that are neither exported nor the presentation surface. A RenderGraph initialized
/// with a pool takes each transient backing from it and returns it on Reset. An idle allocation is
/// reused only when its complete RHI texture description (format, extent, samples, mips, layers,
/// dimension, attachment usage and private depth/stencil aspects) is equal and the RHI submission
/// containing its last frame use has completed; a newer submission's completion covers earlier
/// ones. Exports and surfaces never enter the pool, so no consumer of an export can observe reuse.
/// This is allocation reuse across graph instances (reload, re-initialization), not physical heap
/// aliasing: each live transient still owns its whole allocation. Releasing into a full pool
/// evicts the least recently released idle entry. Not thread-safe: use the RHI
/// owner thread between frames. The device outlives the pool; the pool outlives every graph
/// initialized with it and must be Reset before the device is destroyed.
class RenderGraphAttachmentPool final
{
public:
    RenderGraphAttachmentPool() noexcept;                                            ///< Construct without a device.
    ~RenderGraphAttachmentPool() noexcept;                                           ///< Reset; see Reset.
    RenderGraphAttachmentPool(const RenderGraphAttachmentPool&) = delete;            ///< Ownership is noncopyable.
    RenderGraphAttachmentPool& operator=(const RenderGraphAttachmentPool&) = delete; ///< Ownership is nonassignable.
    /// Bind the pool to a ready device without allocating GPU resources.
    /// @param device Ready owned device; outlives the pool and every graph initialized with it.
    /// @param idleCapacity Most idle allocations retained, 1..RENDER_GRAPH_POOL_CAPACITY.
    /// @return InvalidState when already initialized, InvalidDescription for a capacity outside
    /// its range, OutOfMemory when bookkeeping cannot be allocated, otherwise the device
    /// admission status; failure leaves the pool empty.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               foundation::uint32 idleCapacity = RENDER_GRAPH_POOL_CAPACITY) noexcept;
    /// Destroy every idle allocation; RHI retires their pending GPU uses. Leased ones are unaffected.
    void Trim() noexcept;
    /// Copy the counters; an uninitialized pool reports zeros.
    [[nodiscard]] RenderGraphPoolReport GetReport() const noexcept;
    /// Destroy idle allocations and unbind the device.
    /// @pre No graph initialized with this pool still exists; leases are asserted to be zero.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
    /// Grants RenderGraph acquisition and release of pooled attachments.
    friend struct internal::RenderGraphPoolAccess;
};
/// Copied attachment ownership of one graph; counts are allocations, never bytes.
struct RenderGraphAllocationReport final
{
    foundation::uint32 ExportAllocations = 0;    ///< Allocations backing an export; created and destroyed by the graph.
    foundation::uint32 TransientAllocations = 0; ///< Allocations backing only unexported, non-surface resources.
    foundation::uint32 CreatedAllocations = 0;   ///< Allocations this graph created, exports included.
    foundation::uint32 ReusedAllocations = 0;    ///< Transient allocations reused from the pool.
    bool IsPooled = false;                       ///< Initialized with a pool; false is the serial, non-pooled control.
};
/// Renderer integration of one immutable cooked graph configuration.
/// All calls use the RHI owner thread; Encode requires an acquired frame,
/// metadata queries may inspect that encoding, and other operations are between
/// frames. Device/surface must
/// outlive this object. Setup/Update may allocate or create resources. Render
/// performs no shader/pipeline creation and retains accepted resources through
/// actual RHI completion. A failed/pending scene replacement preserves the last
/// drawable scene. Resize/import changes require a separately initialized graph;
/// no stale target is silently resized or reinterpreted. Ray passes retain bounded progressive
/// history for the current drawable extraction; general raster history profiles are unsupported.
class RenderGraph final
{
public:
    RenderGraph() noexcept;                              ///< Construct an empty graph without GPU work.
    ~RenderGraph() noexcept;                             ///< Reset; see Reset's device/export lifetime preconditions.
    RenderGraph(const RenderGraph&) = delete;            ///< Ownership is noncopyable.
    RenderGraph& operator=(const RenderGraph&) = delete; ///< Ownership is nonassignable.
    /// Copy/validate plan and cooked interfaces, prewarm all required contexts and allocate attachments.
    /// Ready/Pending owns setup. Failure of empty initialization leaves it empty;
    /// reinitialization rejects and preserves existing state. No open frame is permitted.
    /// @param device Ready owned device without an open frame; outlives this graph.
    /// @param plan Numeric plan; its records are copied before return.
    /// @param programs Cooked stages; artifact storage stays alive until Initialize/Poll reaches Ready.
    /// @param referenceWidth Drawable view width for relative resources; zero rejects unresolved relative sizes.
    /// @param referenceHeight Drawable view height, independent of any other graph instance.
    /// @param poolOrNull Optional RG4 attachment pool on the same device; transient backings are
    /// taken from it and returned on Reset. Null selects the serial control: every attachment is
    /// created here and destroyed on Reset. Both paths encode identical commands. An uninitialized
    /// pool, or one bound to a destroyed device, rejects with InvalidDescription. The pool must
    /// be bound to this device and outlive this graph.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const RenderGraphPlan& plan,
                                               const RenderGraphPrograms& programs,
                                               RenderGraphAttachmentPool* poolOrNull = nullptr,
                                               foundation::uint32 referenceWidth = 0,
                                               foundation::uint32 referenceHeight = 0) noexcept;
    /// Request new per-view drawable dimensions between frames, without shader/pipeline preparation.
    /// Fixed targets ignore the reference. Zero dimensions return NotReady and preserve any pending request.
    /// A new request supersedes an unactivated resize. Ready means unchanged; Pending requires Update
    /// with the current extraction (even at the same scene revision), then Poll. Attachments and scene
    /// publish together only when Ready. Failure preserves the drawable scene and output.
    /// Export/readback consumers must finish before this call and before Update/Poll may publish.
    [[nodiscard]] rhi::RasterStatus Resize(foundation::uint32 referenceWidth,
                                           foundation::uint32 referenceHeight) noexcept;
    /// Whether a resource replacement needs a fresh extraction or is preparing it.
    [[nodiscard]] bool HasPendingResize() const noexcept;
    /// Copy a drawing resource's resolved dimensions and policy. False preserves outResource
    /// when uninitialized or the index is out of range; never reports a pending replacement.
    [[nodiscard]] bool GetResourceDescription(foundation::uint32 resource,
                                              content::render_graph_format::Resource& outResource) const noexcept;
    /// Copy attachment ownership counters; an empty graph reports zeros.
    [[nodiscard]] RenderGraphAllocationReport GetAllocationReport() const noexcept;
    /// Poll prewarming and pending replacement; publish only a complete Ready candidate.
    /// Returns a candidate failure once while preserving the prior drawable scene.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Copy current World extraction and all generation-local cameras. One full coverage scan
    /// precedes visibility culling. Pending/failed preparation preserves prior drawing.
    /// Rejection updates diagnostic report but preserves the current and pending scene.
    /// Source storage need only survive this call. Revision must be nonzero.
    /// @param meshes Borrowed static geometry table, copied during a full extraction.
    /// @param items Borrowed instances, transforms, material bindings and per-draw values.
    /// @param cameras Complete generation-local camera table; consumed before return.
    /// @param sceneRevision Nonzero identity for the requested drawable scene/view publication.
    /// @param policy Whether unsupported coverage rejects or receives diagnostic color.
    /// @param outReport Latest candidate diagnostics, independent of the retained drawable output.
    /// Preflight rejection preserves it; coverage/preparation results replace it.
    /// @param sceneInputsOrNull Optional borrowed material/light/environment inputs for a full extraction.
    /// @param contentRevision Optional nonzero identity for all non-camera inputs. Reusing it promises
    /// unchanged geometry, transforms, materials, lights, uniforms and coverage policy; only cameras
    /// and sceneRevision may change. This reuses scene/acceleration buffers and resets camera history.
    /// Mesh/item/shader inputs are ignored while reusing that identity; their original storage
    /// need not survive the original Update. Zero always prepares a complete extraction.
    /// Resize and new generations also prepare in full. At ray-history identity exhaustion,
    /// supplied geometry permits a full restart; camera-only inputs return IdentityExhausted
    /// and preserve the drawable view. A full Update with contentRevision zero can restart it.
    [[nodiscard]] rhi::RasterStatus Update(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                           foundation::BorrowedView<const RenderGraphItem> items,
                                           foundation::BorrowedView<const ViewDescription> cameras,
                                           foundation::uint64 sceneRevision,
                                           RenderGraphCoveragePolicy policy,
                                           RenderGraphReport& outReport,
                                           const SceneShaderInputs* sceneInputsOrNull = nullptr,
                                           foundation::uint64 contentRevision = 0) noexcept;
    /// Borrow latest candidate coverage records until the next Update/Reset; no allocation.
    [[nodiscard]] foundation::BorrowedView<const RenderGraphCoverageRecord> GetCoverage() const noexcept;
    /// Copy the latest candidate report, independent of the drawable extraction.
    [[nodiscard]] RenderGraphReport GetReport() const noexcept;
    /// Return the revision of the last Ready extraction, or zero when none exists.
    [[nodiscard]] foundation::uint64 GetDrawableSceneRevision() const noexcept;
    /// True when the current drawable ray scene has not reached its submission sample budget.
    /// Persistent ray sequences always return true so temporal work continues each frame.
    /// False for an empty/raster-only graph or after a legacy ray history target is reached.
    /// Accepted submissions count even while their producer is pending; finish producers and
    /// export consumers before rendering again. This owner-thread query never waits or submits.
    /// A published scene/camera/size replacement starts new progress; pending/failed candidates
    /// preserve the drawable scene's progress. Shader failures do not establish valid samples.
    [[nodiscard]] bool HasPendingSamples() const noexcept;
    /// Return the valid completed producer's scene revision, or zero while pending/invalid.
    /// This identifies exported pixels independently of candidate/drawable reports;
    /// platform presentation and external consumer completion remain caller-owned.
    [[nodiscard]] foundation::uint64 GetOutputSceneRevision() const noexcept;
    /// Preflight caller-reported physical surface pixels, render the last Ready extraction in authored order,
    /// and publish actual producer completion. Null completion required. Before encoding,
    /// NotReady/resize/device rejection preserves the drawable scene and existing exports.
    /// @param surface Borrowed owned-device submission surface.
    /// @param outCompletion Empty token; receives accepted submission identity even on partial failure.
    /// @param surfaceWidth Current drawable width, supplied by the surface owner.
    /// @param surfaceHeight Current drawable height, supplied by the surface owner.
    /// Acquisition rechecks dimensions; a resize racing acquisition may clear/present
    /// the surface when ending that frame, but never publishes a new valid export.
    /// Partial accepted
    /// work may return a token even on failure; it never establishes image correctness.
    [[nodiscard]] rhi::RasterStatus Render(rhi::SurfaceHandle surface,
                                           foundation::uint32 surfaceWidth,
                                           foundation::uint32 surfaceHeight,
                                           rhi::SubmissionToken& outCompletion) noexcept;
    /// Prepare per-frame ray dispatch graphs between frames, after the preceding producer completes.
    /// Render calls this automatically. Call before acquiring a frame when using Encode with ray
    /// passes; preparation allocates graph records but never shades or submits GPU commands.
    /// Pending/failed replacement scenes do not change the current drawable extraction.
    /// @return Ready when encoding is prepared, NotReady for outstanding producer/setup work;
    /// allocation/device failures preserve drawable scene and existing output.
    [[nodiscard]] rhi::RasterStatus PrepareEncoding() noexcept;
    /// Encode into this device's already acquired frame without acquiring/presenting a surface.
    /// Setup is complete and no earlier producer/encoding may be pending. Ray passes require
    /// PrepareEncoding between frames before this call. After any attempted
    /// encoding, call ConfirmSubmission with the actual caller-owned EndFrame result, even
    /// after a partial failure. Update/Poll/Render reject until that submission is confirmed.
    /// No shader/pipeline creation. Camera surface extents use actual acquired pixels.
    [[nodiscard]] rhi::RasterStatus Encode(rhi::SurfaceHandle surface) noexcept;
    /// Whether an Encode attempt still needs the caller's actual submission result.
    /// May be queried in the owned frame; this is not a GPU completion query.
    [[nodiscard]] bool HasUnconfirmedEncoding() const noexcept;
    /// Track the actual submission containing this Encode; call between frames.
    /// @pre completion is the token returned by EndFrame for that encoding, not another frame.
    /// A null token reports failed/untracked submission and invalidates outputs. A foreign token rejects
    /// without clearing the pending encoding. Pending retains actual work, then GetExport polls
    /// completion. Surface presentation and consumer lifetime remain owned by the caller.
    [[nodiscard]] rhi::RasterStatus ConfirmSubmission(rhi::SubmissionToken completion) noexcept;
    /// Borrow a Ready exported allocation/aspect for immutable consumer preparation, without
    /// claiming initialized/completed pixels. Prepare sampled views/bindings before Encode;
    /// consumers draw only after the graph writes in the same frame or its producer completes.
    /// The graph owns the allocation; all uses finish before Reset. Outputs preserved on failure.
    [[nodiscard]] rhi::RasterStatus GetExportAllocation(foundation::uint32 resource,
                                                        rhi::TextureHandle& outTexture,
                                                        rhi::TextureAspect& outAspect) const noexcept;
    /// Borrow an exported allocation/aspect after producer completion. The graph owns it;
    /// callers must finish all consumers before the next Render or Reset. Outputs preserved on failure.
    [[nodiscard]] rhi::RasterStatus GetExport(foundation::uint32 resource,
                                              rhi::TextureHandle& outTexture,
                                              rhi::TextureAspect& outAspect) const noexcept;
    /// Borrow the allocation and aspect backing a logical texture that is not the presentation
    /// surface, after producer completion, for read-only diagnostic consumers such as a texture
    /// viewer. Persistent Buffer resources return Unsupported. Unlike GetExport this admits transient attachments. It
    /// reports what the backing allocation holds after the last completed frame: a backing shared by several logical
    /// resources (a depth resource always shares its colour resource's allocation) shows the last
    /// write, and a subresource discarded by its final pass is undefined (check it with
    /// rhi::GetTextureState before reading). Transient allocations may be pooled and reused by
    /// another graph after Reset, so callers must finish every use, including readbacks, before
    /// the next Render or Reset and must never write to the allocation.
    /// @param resource Logical resource index in the plan.
    /// @param outTexture Receives the borrowed backing allocation; unchanged on failure.
    /// @param outAspect Receives Color for colour resources and Depth for depth resources.
    /// @return Ready after producer completion; NotReady while no completed output exists;
    /// InvalidDescription for an out-of-range resource; Unsupported for a resource backed by
    /// the presentation surface. Outputs are preserved on failure.
    [[nodiscard]] rhi::RasterStatus GetResourceAllocation(foundation::uint32 resource,
                                                          rhi::TextureHandle& outTexture,
                                                          rhi::TextureAspect& outAspect) const noexcept;
    /// Detach graph resources. Device stays alive, no open frame or foreign export consumer.
    /// GPU uses retire in RHI; does not wait or destroy another graph generation.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
