#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/capture.h>
#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

/// @file
/// Offscreen capture of one render graph frame at a target sample count, saved as linear radiance.
/// See docs/architecture/render-graph-frame-capture.md.
namespace ludus::graphics::renderer
{
/// Smallest admitted capture target, in samples per pixel.
inline constexpr foundation::uint32 MIN_RENDER_GRAPH_CAPTURE_SAMPLES = 1;
/// Default capture target, in samples per pixel; enough to judge an estimator, not to remove its noise.
inline constexpr foundation::uint32 DEFAULT_RENDER_GRAPH_CAPTURE_SAMPLES = 64;
/// Largest shadow-query budget a capture profile links, per pixel and sample; mirrors
/// MAX_CAPTURE_VISIBILITY_QUERIES in scripts/python/ludus_tools/render_graph_ray.py.
inline constexpr foundation::uint32 MAX_RENDER_GRAPH_CAPTURE_VISIBILITY_QUERIES = 256;
/// Estimated GPU nanoseconds of one query per pixel: the shadow-query cost measured on an Apple M5 in the
/// capture budget decision of docs/architecture/render-graph-frame-capture.md.
inline constexpr foundation::uint32 RENDER_GRAPH_CAPTURE_QUERY_NANOSECONDS = 8;
/// Shadow queries charged per light: a two-technique MIS estimator tests each light twice.
inline constexpr foundation::uint32 RENDER_GRAPH_CAPTURE_QUERIES_PER_LIGHT = 2;
/// Estimated GPU time each band of a capture targets: a quarter second, eight times below the two-second
/// Windows TDR default, which also leaves a margin for GPUs slower than the measured one.
inline constexpr foundation::uint64 RENDER_GRAPH_CAPTURE_TILE_NANOSECONDS = 250'000'000;

/// Choose the bands per sample (RenderGraph::SetRayTileCount) that keep each band of a capture under
/// RENDER_GRAPH_CAPTURE_TILE_NANOSECONDS. Each pixel is charged one camera query plus
/// RENDER_GRAPH_CAPTURE_QUERIES_PER_LIGHT shadow queries per light, at most
/// MAX_RENDER_GRAPH_CAPTURE_VISIBILITY_QUERIES, since a kernel cannot exceed its linked budget, each at
/// RENDER_GRAPH_CAPTURE_QUERY_NANOSECONDS. The model is linear in lights, as the polygonal-light estimators
/// are, so it overestimates estimators that sample a fixed number of lights.
/// @param workload Pixels and lights of one sample, from RenderGraph::GetRayWorkload.
/// @return From one to MAX_RENDER_GRAPH_RAY_TILES; MAX_RENDER_GRAPH_RAY_TILES when the estimate needs more.
[[nodiscard]] foundation::uint32 ComputeRenderGraphCaptureTileCount(const RenderGraphRayWorkload& workload) noexcept;

/// Opaque identity bytes recorded in a capture file, normally a SHA-256 digest.
using RenderGraphCaptureIdentity =
    foundation::StaticArray<foundation::uint8, content::render_graph_format::CAPTURE_IDENTITY_BYTES>;

/// Choices for one RenderGraphCapture.
struct RenderGraphCaptureSettings final
{
    /// Logical plan resource to capture; ABSENT selects the plan's only exported colour resource.
    foundation::uint32 Output = content::render_graph_format::ABSENT;
    /// Samples per pixel at completion, from MIN_RENDER_GRAPH_CAPTURE_SAMPLES to MAX_RENDER_GRAPH_CAPTURE_SAMPLES.
    foundation::uint32 TargetSamples = DEFAULT_RENDER_GRAPH_CAPTURE_SAMPLES;
    /// Samples between progress read-backs, after the first sample and then every interval; zero reads back only
    /// at the end. Each progress read-back pauses rendering until it completes, then GetPixels shows it.
    foundation::uint32 PreviewInterval = 0;
    /// Bands per sample, from one to MAX_RENDER_GRAPH_RAY_TILES, each rendered by its own submission; zero
    /// chooses ComputeRenderGraphCaptureTileCount of the scene's workload. More than one band needs
    /// RenderGraphPrograms::HasRayTiles.
    foundation::uint32 TileCount = 0;
    /// Identity of the runtime package, recorded in the file; zero when unknown.
    RenderGraphCaptureIdentity PackageIdentity{};
    /// Identity of the caller's scene inputs, recorded in the file; zero when unknown.
    RenderGraphCaptureIdentity SceneIdentity{};
};

/// Progress of one RenderGraphCapture.
enum class RenderGraphCapturePhase : foundation::uint8
{
    Idle,           ///< Nothing started, or released.
    Initializing,   ///< The private graph is preparing its pipelines.
    AwaitingScene,  ///< Ready for SubmitScene.
    PreparingScene, ///< The submitted scene is preparing.
    Rendering,      ///< Frames are accumulating toward the target.
    ReadingBack,    ///< Copying the Accumulate mean and counts to the CPU, for progress or at the end.
    Complete,       ///< Every frame of the target was submitted and read back without a latched failure.
    Stopped,        ///< Stop ended the capture early; the committed samples were read back.
    Failed,         ///< The capture failed; GetProblem and GetStatus say why. Read-back pixels stay readable.
};

/// Why a capture rejected its source or failed.
enum class RenderGraphCaptureProblem : foundation::uint8
{
    None,            ///< No problem.
    NoOutput,        ///< The output is not an offscreen colour resource, or the plan has no single exported one.
    RasterRoute,     ///< No ray pass with an Accumulate writes the output; raster outputs are display-encoded.
    NoAccumulation,  ///< The ray pass that writes the output outputs individual samples; add an Accumulate.
    InvalidSettings, ///< The sample target, camera or scene arguments are out of range.
    GraphFailure,    ///< The private graph rejected initialization, the scene or a frame; see GetStatus.
    ReadbackFailure, ///< Copying the accumulation to the CPU failed; see GetStatus.
    LatchedPixels,   ///< Some pixels latched a shader failure (the graph's failure colour).
    /// The sample needs more than one band, but the package's ray kernels predate tiled dispatch (package
    /// version 28); recook the graph.
    UntiledPackage,
};

/// Progress and provenance of one capture.
struct RenderGraphCaptureInfo final
{
    RenderGraphCapturePhase Phase = RenderGraphCapturePhase::Idle;              ///< Current phase.
    foundation::uint32 Width = 0;                                               ///< Captured image width in pixels.
    foundation::uint32 Height = 0;                                              ///< Captured image height in pixels.
    foundation::uint32 Output = content::render_graph_format::ABSENT;           ///< Captured logical resource.
    foundation::uint32 AccumulationPass = content::render_graph_format::ABSENT; ///< Plan pass whose mean is read.
    foundation::uint32 AuthoredLimit = 0; ///< Accumulate limit authored on that pass.
    foundation::uint32 TargetSamples = 0; ///< Requested samples per pixel.
    /// Samples submitted for every pixel so far: frames, or complete rounds of bands.
    foundation::uint32 SubmittedSamples = 0;
    /// Bands per sample, each submitted alone; zero until the scene is ready.
    foundation::uint32 TileCount = 0;
    foundation::uint32 FailedPixels = 0;    ///< Pixels with a latched shader failure, known after read-back.
    foundation::uint32 MinimumSamples = 0;  ///< Smallest committed count across pixels, known after read-back.
    foundation::uint32 ReadbackSamples = 0; ///< Submitted samples covered by the pixels last read back.
    foundation::uint64 SceneRevision = 0;   ///< Revision submitted with the scene; it also seeds sampling.
};

/// One captured pixel: the Accumulate's linear mean radiance and its committed sample count.
struct RenderGraphCapturePixel final
{
    foundation::math::Vector3 Mean{}; ///< Mean linear radiance; zero when no sample committed.
    foundation::uint32 Samples = 0;   ///< Committed samples; below the target after a latched failure or Stop.
};

/// Renders a frozen scene through a private RenderGraph until a target sample count and reads back the
/// linear mean of the Accumulate that feeds the chosen output.
///
/// The capture owns its RenderGraph, so an interactive graph on the same device keeps its drawable output,
/// accumulation and pending samples. It sets RenderGraph::SetAccumulationLimitOverride to the target, so the
/// target may exceed the authored Accumulate limit. Only ray outputs with an Accumulate are captured: raster
/// outputs are display-encoded, not linear radiance. Owner thread only, between frames; never waits for the
/// GPU. The device and surface outlive the capture or Release runs first. The surface's frames are submitted
/// like RenderGraph::Render; use an offscreen surface where presentation is unwanted.
class RenderGraphCapture final
{
public:
    RenderGraphCapture() noexcept;                                     ///< Construct an idle capture.
    ~RenderGraphCapture() noexcept;                                    ///< Release; see Release.
    RenderGraphCapture(const RenderGraphCapture&) = delete;            ///< Ownership is noncopyable.
    RenderGraphCapture& operator=(const RenderGraphCapture&) = delete; ///< Ownership is nonassignable.

    /// Resolve the captured route and begin initializing a private graph. Any earlier capture is released.
    /// @param device Ready owned device without an open frame.
    /// @param surface Surface whose frames the capture submits.
    /// @param plan Numeric plan; copied by the graph.
    /// @param programs Cooked stages; their storage stays alive until the phase leaves Initializing.
    /// @param referenceWidth Drawable view width for view-relative resources.
    /// @param referenceHeight Drawable view height for view-relative resources.
    /// @param settings Output, target and identities; copied.
    /// @return Ready or Pending when initialization started; InvalidDescription with GetProblem set when the
    /// route or settings are rejected; the graph's failure otherwise. Failure leaves the capture Failed.
    [[nodiscard]] rhi::RasterStatus Start(rhi::DeviceHandle device,
                                          rhi::SurfaceHandle surface,
                                          const RenderGraphPlan& plan,
                                          const RenderGraphPrograms& programs,
                                          foundation::uint32 referenceWidth,
                                          foundation::uint32 referenceHeight,
                                          const RenderGraphCaptureSettings& settings) noexcept;
    /// Submit the frozen scene once the phase is AwaitingScene. Inputs are borrowed only for this call, as
    /// with RenderGraph::Update; the scene and camera then belong to the capture.
    /// @param meshes Static geometry table.
    /// @param items Instances, transforms and material bindings.
    /// @param cameras Complete camera table of the plan.
    /// @param sceneRevision Nonzero identity; equal inputs and revision reproduce the capture bit for bit.
    /// @param sceneInputsOrNull Optional material, light and environment inputs.
    /// @return Ready or Pending when accepted; InvalidState outside AwaitingScene; the graph's rejection
    /// otherwise, which fails the capture.
    [[nodiscard]] rhi::RasterStatus SubmitScene(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                                foundation::BorrowedView<const RenderGraphItem> items,
                                                foundation::BorrowedView<const ViewDescription> cameras,
                                                foundation::uint64 sceneRevision,
                                                const SceneShaderInputs* sceneInputsOrNull = nullptr) noexcept;
    /// Advance initialization, scene preparation, at most one frame submission, or the read-back.
    /// Never waits for the GPU. Returns the resulting phase.
    RenderGraphCapturePhase Service() noexcept;
    /// Submit no further frames and read back the samples already committed; the capture ends Stopped (or
    /// Failed). Has no effect after the capture ended.
    void Stop() noexcept;
    /// Cancel outstanding work and release the private graph. Read-back pixels are discarded. Idempotent.
    void Release() noexcept;

    /// Current phase.
    [[nodiscard]] RenderGraphCapturePhase GetPhase() const noexcept;
    /// Why the capture failed or rejected its source; None otherwise.
    [[nodiscard]] RenderGraphCaptureProblem GetProblem() const noexcept;
    /// The RHI status behind a GraphFailure or ReadbackFailure; Ready otherwise.
    [[nodiscard]] rhi::RasterStatus GetStatus() const noexcept;
    /// Copy progress and provenance.
    [[nodiscard]] RenderGraphCaptureInfo GetInfo() const noexcept;
    /// Borrow the read-back pixels in top-left row order until the next Start or Release. Empty until a
    /// read-back finished; a Complete, Stopped or LatchedPixels capture has one per pixel.
    [[nodiscard]] foundation::BorrowedView<const RenderGraphCapturePixel> GetPixels() const noexcept;
    /// Encode the read-back pixels and provenance as a `.lurgcap` file (RenderGraphFormat capture.h).
    /// @param outBytes Receives the file; unchanged on failure.
    /// @return Ready; NotReady before a read-back finished; OutOfMemory when the file cannot be allocated.
    [[nodiscard]] rhi::RasterStatus Encode(foundation::Array<foundation::uint8>& outBytes) const noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
