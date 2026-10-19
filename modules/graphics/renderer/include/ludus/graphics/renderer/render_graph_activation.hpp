#pragma once

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

/// @file
/// RG3 runtime generations and atomic activation of cooked render graphs.
/// See docs/architecture/render-graph-authoring.md, "Editor interaction,
/// diagnostics and hot reload", for the staging/activation contract.
namespace ludus::graphics::renderer
{
/// Result of decoding a `.lurgrt` runtime generation.
enum class RenderGraphPackageStatus : foundation::uint8
{
    Ready,              ///< The package decoded and owns copies of every record and artifact.
    Malformed,          ///< Truncated, trailing, out-of-range or inconsistent records.
    UnsupportedVersion, ///< Unknown magic or layout version; never guessed.
    CapacityExceeded,   ///< Counts or byte sizes exceed the runtime format bounds.
    OutOfMemory,        ///< Owned storage could not be allocated.
    InvalidState,       ///< Decode was called on a package that already holds a generation.
};
/// Immutable decoded runtime generation. Decode copies the input bytes, so the
/// caller (for example an unloadable game module) may release its buffer as soon
/// as Decode returns. Borrowed plan/program views stay valid until Reset or
/// destruction. Not thread-safe; read-only queries may run on any thread once
/// no thread mutates the package. Allocates only inside Decode.
class RenderGraphPackage final
{
public:
    RenderGraphPackage() noexcept;                                     ///< Construct an empty package.
    ~RenderGraphPackage() noexcept;                                    ///< Release owned storage.
    RenderGraphPackage(const RenderGraphPackage&) = delete;            ///< Ownership is noncopyable.
    RenderGraphPackage& operator=(const RenderGraphPackage&) = delete; ///< Ownership is nonassignable.
    /// Decode and copy a little-endian runtime generation into owned storage.
    /// @param bytes Complete file image; need only be valid during the call.
    /// @return Ready on success. Any failure leaves the package empty. A package
    /// that already holds a generation returns InvalidState and is unchanged.
    [[nodiscard]] RenderGraphPackageStatus Decode(foundation::BorrowedView<const foundation::uint8> bytes) noexcept;
    /// Whether a decoded generation is held.
    [[nodiscard]] bool IsLoaded() const noexcept;
    /// Borrow the numeric plan; empty when no generation is held. Valid until Reset.
    [[nodiscard]] RenderGraphPlan GetPlan() const noexcept;
    /// Describe the cooked stages, borrowing owned artifacts and the caller's composite recipes.
    /// The composite descriptions are copied by value; their artifact storage must outlive use.
    /// A role the generation carries no stage for (for example DepthOnly in a color-only graph)
    /// stays a default description, exactly as the matching compiled header leaves it.
    /// @param compositeVertex Built-in composite vertex recipe, borrowed until GPU setup copies it.
    /// @param compositeFragment Built-in composite fragment recipe, with the same source lifetime.
    /// @param shouldPrepareAsynchronously Queue native driver preparation for these stages
    /// and the composite pair. Activation selects true; one-shot consumers may keep false.
    [[nodiscard]] RenderGraphPrograms GetPrograms(const rhi::RasterShaderDescription& compositeVertex,
                                                  const rhi::RasterShaderDescription& compositeFragment,
                                                  bool shouldPrepareAsynchronously = false) const noexcept;
    /// SHA-256 of the CPU-visible vertex/instance interface; empty when not loaded.
    [[nodiscard]] foundation::BorrowedView<const foundation::uint8> GetInterfaceIdentity() const noexcept;
    /// Immutable RG1 generation identity this runtime package was cooked from; empty when not loaded.
    [[nodiscard]] foundation::BorrowedView<const foundation::uint8> GetGenerationIdentity() const noexcept;
    /// Release the generation. Any RenderGraph initialized from it must have reached Ready first.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
/// Lifecycle of the most recently staged candidate generation.
enum class RenderGraphCandidateState : foundation::uint8
{
    None,            ///< No candidate is staged, or the last one was activated.
    Preparing,       ///< Pipelines or the candidate scene are pending; the active generation keeps drawing.
    NeedsUpdate,     ///< Pipelines are Ready; call Update with the current extraction before activation.
    Failed,          ///< Setup, coverage or device rejection; the active generation and its pixels remain.
    RestartRequired, ///< The CPU interface differs from the host's compiled interface; nothing was staged.
};
/// RG4 attachment allocation of every generation staged through one activation.
enum class RenderGraphAllocation : foundation::uint8
{
    /// Reuse transient attachments across generations through one owned RenderGraphAttachmentPool:
    /// an exact descriptor match whose last frame use has completed. Exports are never pooled.
    ExactPool,
    /// Diagnostic serial control: every generation creates and destroys all of its attachments,
    /// exactly as before RG4. Encodes the same commands as ExactPool.
    SerialControl,
};
/// Copied activation status, independent of the active graph's scene reports.
struct RenderGraphActivationReport final
{
    foundation::uint64 ActiveGeneration = 0;    ///< Activation serial of the drawing generation; 0 when none.
    foundation::uint64 CandidateGeneration = 0; ///< Serial of the last staged candidate; 0 when none was staged.
    RenderGraphCandidateState Candidate = RenderGraphCandidateState::None; ///< Candidate lifecycle.
    rhi::RasterStatus LastFailure = rhi::RasterStatus::Ready;              ///< Status that failed the last candidate.
    foundation::uint64 LatestSceneRevision = 0; ///< Revision of the last accepted Update call.
};
/// Stage a complete graph + shader + plan generation and publish it atomically
/// between frames. The active generation keeps rendering while a candidate
/// prepares; a failed or rejected candidate never replaces it, so its last
/// pixels and output revision remain. Activation happens only inside Poll, when
/// the candidate's pipelines are Ready and it has prepared the latest accepted
/// extraction. The replaced generation is Reset at that boundary: its GPU uses
/// retire through RHI completion, independently of any game-module quiescence.
/// Packages are owned here, so no plan retains pointers into caller memory.
/// All calls use the RHI owner thread between frames, except the active graph's
/// own Encode contract. The device and composite artifact storage outlive this object.
class RenderGraphActivation final
{
public:
    RenderGraphActivation() noexcept;                                        ///< Construct without GPU work.
    ~RenderGraphActivation() noexcept;                                       ///< Reset every generation.
    RenderGraphActivation(const RenderGraphActivation&) = delete;            ///< Ownership is noncopyable.
    RenderGraphActivation& operator=(const RenderGraphActivation&) = delete; ///< Ownership is nonassignable.
    /// Bind the device, the host's compiled CPU interface identity and composite recipes.
    /// @param device Owned device; must outlive this object and every staged generation.
    /// @param interfaceIdentity IDENTITY_BYTES digest from the host's generated header; copied.
    /// @param compositeVertex Installed renderer_composite vertex recipe; its artifact storage outlives this object.
    /// @param compositeFragment Installed renderer_composite fragment recipe; same lifetime as compositeVertex.
    /// @param allocation ExactPool (default) shares one attachment pool across generations;
    /// SerialControl is the non-pooled diagnostic control.
    /// @return InvalidState when already initialized; InvalidDescription for a wrong identity width
    /// or an unknown allocation; the pool's device admission status when it cannot be bound.
    [[nodiscard]] rhi::RasterStatus
    Initialize(rhi::DeviceHandle device,
               foundation::BorrowedView<const foundation::uint8> interfaceIdentity,
               const rhi::RasterShaderDescription& compositeVertex,
               const rhi::RasterShaderDescription& compositeFragment,
               RenderGraphAllocation allocation = RenderGraphAllocation::ExactPool) noexcept;
    /// Take ownership of a decoded package and begin preparing it as the candidate.
    /// Native shader/driver pipeline work is queued; owner-thread calls never wait
    /// for it. Queue admission/allocation may fail, preserving the active graph.
    /// An earlier unactivated candidate is discarded. A CPU interface mismatch
    /// returns Unsupported with RestartRequired; setup failure returns its status
    /// with Failed. Neither changes the active generation. Pending/Ready is accepted.
    [[nodiscard]] rhi::RasterStatus Stage(foundation::core::UniquePtr<RenderGraphPackage> package) noexcept;
    /// Advance the active graph and the candidate; activate a complete candidate.
    /// Returns the active graph's Poll status, or the candidate's while no generation is active.
    /// Export consumers of the active generation must have completed before a Poll that may activate.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Forward one extraction to the active graph and to a candidate whose pipelines are Ready.
    /// outReport describes the active graph (or the candidate when none is active). A candidate that
    /// rejects an extraction the active generation accepts becomes Failed. Inputs need only survive the call.
    [[nodiscard]] rhi::RasterStatus Update(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                           foundation::BorrowedView<const RenderGraphItem> items,
                                           foundation::BorrowedView<const ViewDescription> cameras,
                                           foundation::uint64 sceneRevision,
                                           RenderGraphCoveragePolicy policy,
                                           RenderGraphReport& outReport) noexcept;
    /// Borrow the drawing generation's graph for Render/Encode/exports; null when none is active.
    /// The pointer changes when ActiveGeneration changes; never retain it across Poll.
    [[nodiscard]] RenderGraph* GetActiveOrNull() noexcept;
    /// Copy the activation status.
    [[nodiscard]] RenderGraphActivationReport GetReport() const noexcept;
    /// Copy the owned pool's counters; SerialControl and an uninitialized activation report zeros.
    [[nodiscard]] RenderGraphPoolReport GetPoolReport() const noexcept;
    /// Destroy idle pooled attachments between frames, for example under memory pressure.
    /// Attachments of the active and candidate generations are unaffected.
    void TrimPool() noexcept;
    /// Release every generation; no open frame or outstanding export consumer is permitted.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
