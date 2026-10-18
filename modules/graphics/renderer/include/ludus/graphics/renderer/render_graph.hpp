#pragma once

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer
{
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
};
/// Cooked static-instanced stage pairs. No compiler or shader-source discovery.
/// Artifact/reflection storage remains immutable and alive until Initialize/Poll
/// reaches Ready; generated package headers provide static storage.
/// Each selected vertex uses the six Renderer flat locations. Stage bindings, when
/// present, are read-only uniform buffers that exactly match the plan's uniform
/// binding records. A depth-only stage pair shares its family's exact geometry
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
};
/// Declared geometry interface of an extracted draw item.
enum class RenderGraphLayout : foundation::uint8
{
    Static,      ///< Typed MeshDescription and immutable static local-to-world transform.
    Unsupported, ///< The position path is unknown; only an explicit bounds proxy may be drawn.
};
/// Required material/coverage semantics; never disappear in a query filter.
enum class RenderGraphMaterial : foundation::uint8
{
    Opaque,      ///< Static opaque coverage; covered only by DefaultStaticOpaque techniques.
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
    foundation::uint64 SceneRevision = 0; ///< Caller extraction revision, not a GPU completion number.
    foundation::uint32 Covered = 0;       ///< Compatible item/role/view tuples.
    foundation::uint32 Unsupported = 0;   ///< Missing required compatibility.
    foundation::uint32 Conflicting = 0;   ///< Ambiguous/duplicate role tuples.
    foundation::uint32 Excluded = 0;      ///< Explicit omissions.
    bool IsAccepted = false;              ///< Candidate was accepted for asynchronous preparation.
    bool IsReady = false;                 ///< Accepted scene is drawable; producer completion is separate.
};
/// Renderer integration of one immutable cooked graph configuration.
/// All calls use the RHI owner thread; Encode requires an acquired frame,
/// metadata queries may inspect that encoding, and other operations are between
/// frames. Device/surface must
/// outlive this object. Setup/Update may allocate or create resources. Render
/// performs no shader/pipeline creation and retains accepted resources through
/// actual RHI completion. A failed/pending scene replacement preserves the last
/// drawable scene. Resize/import changes require a separately initialized graph;
/// no stale target is silently resized or reinterpreted. No history profile yet.
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
    [[nodiscard]] rhi::RasterStatus
    Initialize(rhi::DeviceHandle device, const RenderGraphPlan& plan, const RenderGraphPrograms& programs) noexcept;
    /// Poll prewarming and pending replacement; publish only a complete Ready candidate.
    /// Returns a candidate failure once while preserving the prior drawable scene.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Copy current World extraction and all generation-local cameras. One full coverage scan
    /// precedes visibility culling. Pending/failed preparation preserves prior drawing.
    /// Rejection updates diagnostic report but preserves the current and pending scene.
    /// Source storage need only survive this call. Revision must be nonzero.
    [[nodiscard]] rhi::RasterStatus Update(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                           foundation::BorrowedView<const RenderGraphItem> items,
                                           foundation::BorrowedView<const ViewDescription> cameras,
                                           foundation::uint64 sceneRevision,
                                           RenderGraphCoveragePolicy policy,
                                           RenderGraphReport& outReport) noexcept;
    /// Borrow latest candidate coverage records until the next Update/Reset; no allocation.
    [[nodiscard]] foundation::BorrowedView<const RenderGraphCoverageRecord> GetCoverage() const noexcept;
    /// Copy the latest candidate report, independent of the drawable extraction.
    [[nodiscard]] RenderGraphReport GetReport() const noexcept;
    /// Return the revision of the last Ready extraction, or zero when none exists.
    [[nodiscard]] foundation::uint64 GetDrawableSceneRevision() const noexcept;
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
    /// Encode into this device's already acquired frame without acquiring/presenting a surface.
    /// Setup is complete and no earlier producer/encoding may be pending. After any attempted
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
    /// Detach graph resources. Device stays alive, no open frame or foreign export consumer.
    /// GPU uses retire in RHI; does not wait or destroy another graph generation.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
