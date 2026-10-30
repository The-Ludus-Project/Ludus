#pragma once

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::resources
{
struct SceneDefinition;
}
namespace ludus::graphics::renderer
{
/// Current World material terms for one placed object, copied at Initialize.
/// Texture indices refer to the retained SceneDefinition CPU texture array.
/// This override uses MaterialDescription's white untextured base; an empty
/// override list retains SceneDefinition's legacy black untextured contract.
struct StaticLightingSurfaceDescription final
{
    foundation::uint32 Texture =
        ~foundation::uint32{0};                 ///< CPU scene texture index or all ones for the untextured base.
    foundation::math::Vector4 Tint{1, 1, 1, 1}; ///< Linear RGB/alpha multiplier in [0,1].
    foundation::math::Vector4 UvTransform{1,
                                          1,
                                          0,
                                          0};    ///< Base texture UV scale XY and offset ZW, finite and bounded to 1e6.
    foundation::float32 Metallic = 0;            ///< Metallic fraction in [0,1].
    foundation::float32 Roughness = .5F;         ///< Perceptual roughness in [.045,1].
    foundation::math::Vector3 Emission{};        ///< Resolved linear RGB nits, each channel in [0,10000].
    bool IsEmissionTwoSided = true;              ///< Geometric emission sidedness.
    bool ShouldRepeatTexture = false;            ///< Repeat UV addressing; false clamps.
    MaterialAlpha Alpha = MaterialAlpha::Opaque; ///< Opaque or Masked transport; Painter returns Unsupported.
    foundation::float32 AlphaCutoff = .5F;       ///< Masked alpha threshold in [0,1].
    /// Typed source material parameters; default retains the legacy reflection model.
    content::scene_format::MaterialScattering Scattering{};
};
/// Explicit static diffuse transport quality and exact authoring provenance.
struct StaticLightingBakeDescription final
{
    foundation::uint32 SamplesPerTriangle = 64;      ///< Cosine-hemisphere samples per triangle, 1..65536.
    foundation::uint32 MaxDepth = 4;                 ///< Scattering-event limit, 2..64; introduces truncation bias.
    foundation::float32 SecondaryDistance = 1000000; ///< Ray range in metres, (0,2e6].
    foundation::uint64 Seed = 0;                     ///< Deterministic addressed Philox seed.
    foundation::uint64 SceneRevision = 0;            ///< Nonzero version; changes require a fresh job.
    foundation::uint64 DependencyRevision = 0;       ///< Nonzero exact cooked dependency version.
    /// Optional synchronous borrowed current-World overrides, exactly one per
    /// SceneDefinition::Object. Resolved emission power must already include
    /// the current placement's world area and side count. Initialize copies all
    /// terms, so the override array need not outlive that call.
    foundation::BorrowedView<const StaticLightingSurfaceDescription> Surfaces;
    foundation::usize CpuByteBudget =
        foundation::usize{256} * 1024 * 1024; ///< Upper bound on owned job arrays, including BVH scratch.
};
/// Borrowed completed chart payload; caller uploads it on the RHI owner thread.
/// No GPU readback, texture owner or display image is produced by the bake.
struct StaticLightingChart final
{
    foundation::uint32 Width = 0;  ///< Power-of-two chart width in texels.
    foundation::uint32 Height = 0; ///< Power-of-two chart height in texels.
    foundation::BorrowedView<const foundation::uint8>
        Pixels; ///< RGBA8 linear irradiance with replicated per-triangle gutters; borrow ends at job destruction.
    foundation::math::Vector3 Scale{1, 1, 1}; ///< Multiply normalized texture RGB by this irradiance scale.
    foundation::math::Vector4 UvTransform{1,
                                          1,
                                          0,
                                          0};  ///< Transform from helper chart UVs to sampled atlas coordinates.
    foundation::uint64 SourceId = 0;           ///< Exact placed instance identity.
    foundation::uint64 SceneRevision = 0;      ///< Must match the drawn extraction's revision.
    foundation::uint64 DependencyRevision = 0; ///< Must match the retained cooked dependency revision.
    foundation::math::Affine3 Transform{};     ///< Exact mesh-to-world used by the bake.
    /// Zero-area triangles of this object that the bake skipped; their chart tiles hold zero irradiance.
    foundation::uint32 SkippedTriangleCount = 0;
};
/// @brief Input problems the bake tolerated instead of failing, available once preparation completes.
///
/// A zero-area (collapsed) triangle covers no surface: it emits nothing, is never
/// hit and has no normal to gather irradiance about. The bake keeps its chart tile,
/// so chart layout and texture coordinates are unchanged, fills that tile with zero
/// irradiance and leaves the triangle out of ray traversal and emitter selection.
/// Non-finite geometry is not tolerated and still fails preparation.
struct StaticLightingBakeDiagnostics final
{
    foundation::uint32 SkippedTriangleCount = 0; ///< Zero-area triangles skipped across all objects.
    foundation::uint32 AffectedObjectCount = 0;  ///< Placed objects with at least one skipped triangle.
};
/// @brief Upper bound on StaticLightingScheduleDescription::RangeCount.
inline constexpr foundation::uint32 MAX_STATIC_LIGHTING_RANGES = 1024;
/// @brief Coarse range decomposition for StaticLightingBake::Submit.
///
/// The bake is split into one preparation job followed by RangeCount jobs over
/// contiguous, disjoint triangle ranges. Each range evaluates every sample of its
/// triangles in the same order as Step, so the output does not depend on the range
/// count, worker count or completion order.
struct StaticLightingScheduleDescription final
{
    /// @brief Triangle ranges, 1..MAX_STATIC_LIGHTING_RANGES; ranges beyond the triangle count are empty.
    ///
    /// Use several ranges per worker so uneven path cost balances, while keeping
    /// each range long enough that dispatch is small relative to its path work.
    foundation::uint32 RangeCount = 64;
};
/// @brief Phase durations of a scheduled bake, read from FoundationTime's monotonic clock.
///
/// All values are nanoseconds and describe the latest Submit after its lifetime
/// fence. The critical path of the job graph is PrepareNanoseconds +
/// DispatchNanoseconds + LongestRangeNanoseconds, followed by MergeNanoseconds on
/// the owner. Skipped (cancelled or blocked) ranges contribute no time.
struct StaticLightingBakeProfile final
{
    foundation::uint64 PrepareNanoseconds = 0;      ///< Preparation job: validation, surfaces and BVH build.
    foundation::uint64 DispatchNanoseconds = 0;     ///< End of preparation to the first range callback start.
    foundation::uint64 LongestRangeNanoseconds = 0; ///< Slowest executed range callback.
    foundation::uint64 RangeNanoseconds = 0;        ///< Sum of executed range callbacks (total path work).
    foundation::uint64 ExecutionNanoseconds = 0;    ///< Submit entry to the end of the last executed callback.
    foundation::uint64 MergeNanoseconds = 0;        ///< Owner-side status merge and chart encoding.
    foundation::uint32 RangeCount = 0;              ///< Submitted ranges, including empty ones.
    foundation::uint32 ExecutedRangeCount = 0;      ///< Range callbacks that ran; cancellation skips the rest.
};
/// @brief Path-sample progress of a bake; both counts are zero for an unbound bake.
struct StaticLightingBakeProgress final
{
    foundation::uint64 CompletedSamples = 0; ///< Evaluated cosine path samples.
    foundation::uint64 TotalSamples = 0;     ///< Triangles times SamplesPerTriangle.
};
/// CPU-only incremental static diffuse bake. Charts include all
/// visible static area/environment direct diffuse and later indirect transport;
/// analytic direct diffuse/specular stays raster. Set BakedLighting's
/// AreTriangleChartsEnabled and HasStaticAreaDirect, and DirectLightId=0.
/// Area specular is outside this profile. Per-triangle centroid irradiance is
/// coarse spatial reconstruction; sample count controls transport variance.
/// No Renderer, GPU, Qt or native ray-query state is touched. Zero-area triangles
/// are skipped and reported through GetDiagnostics rather than failing the bake.
///
/// Two execution modes share the same transport and produce bit-identical charts:
/// - Serial: Initialize, then Step until Ready, all on one thread.
/// - Scheduled: Submit to a caller-owned foundation::threading::JobSystem, then
///   Poll (nonblocking) or Finish (blocking) on the owner thread that serializes
///   that system. Preparation and coarse triangle ranges run as jobs reading one
///   immutable prepared scene; each range writes only its own triangles. After the
///   JobSystem::Wait lifetime fence, the owner merges range results in stable
///   range order and encodes the charts.
///
/// Source scene storage stays immutable and alive until destruction because
/// retained CPU texture bytes are borrowed. Discard obsolete revision results
/// before publication; transfer a completed bake to the composition owner.
class StaticLightingBake final
{
public:
    /// Construct an unbound job without allocation.
    LUDUS_INLINE StaticLightingBake() noexcept = default;
    /// Release only CPU storage; must not race any other call.
    /// An in-flight scheduled bake is cancelled and drained through its JobSystem
    /// first; this blocks until running ranges reach their next sample boundary.
    /// @warning Destroying an in-flight bake from a job callback is a fatal contract violation.
    ~StaticLightingBake() noexcept;
    /// Worker job identity cannot be copied.
    StaticLightingBake(const StaticLightingBake&) = delete;
    /// Worker job identity cannot be assigned.
    StaticLightingBake& operator=(const StaticLightingBake&) = delete;
    /// Build an owned world-space BVH and triangle chart accumulators on a worker.
    /// @pre scene satisfies SceneDefinition::Decode bounds and stays immutable during the job.
    /// Legacy empty bake coordinates are accepted; its upload adapter must assign
    /// the shared triangle chart helper coordinates to deindexed mesh vertices.
    /// Returns Pending on accepted preparation; failure preserves an unbound job.
    [[nodiscard]] rhi::RasterStatus Initialize(const resources::SceneDefinition& scene,
                                               const StaticLightingBakeDescription& description) noexcept;
    /// Evaluate at most maxPathSamples complete cosine paths; positive count required.
    /// Pending keeps uncommitted charts private; Ready means all charts are encoded.
    /// Each path has bounded depth; initialization/final encoding belong on a worker.
    /// Invalid contributions permanently fail this candidate; never publish it.
    [[nodiscard]] rhi::RasterStatus Step(foundation::uint32 maxPathSamples) noexcept;
    /// Borrow one placed object's completed linear chart; no allocation or GPU call.
    /// NotReady/invalid index preserves outChart. DirectLightId for uploaded
    /// BakedLighting must be zero: charts contain static area/environment direct diffuse plus indirect irradiance.
    [[nodiscard]] rhi::RasterStatus GetChart(foundation::usize objectIndex,
                                             StaticLightingChart& outChart) const noexcept;

    /// @brief Submits preparation and range evaluation of an unbound bake to a JobSystem.
    ///
    /// Copies the description, including every Surfaces override, before returning.
    /// Validation and preparation run in the first job, so their failures are
    /// reported by Poll or Finish. With zero workers the whole bake runs inside this
    /// call; Poll or Finish is still required to merge it. Step is unavailable for a
    /// scheduled bake.
    /// @param inoutSystem Initialized system with no graph in flight. It must outlive
    ///        this bake; do not reinitialize it while this bake is in flight.
    /// @param scene Source scene; immutable and alive until this bake is destroyed.
    /// @param description Transport quality and provenance, validated as for Initialize.
    /// @param schedule Range decomposition.
    /// @return Pending once submitted. InvalidState when already initialized or submitted,
    ///         or when the system is uninitialized, busy, or called from a job callback;
    ///         InvalidDescription for an invalid range count or Surfaces view; or
    ///         OutOfMemory. Failure leaves the bake unbound.
    [[nodiscard]] rhi::RasterStatus Submit(foundation::threading::JobSystem& inoutSystem,
                                           const resources::SceneDefinition& scene,
                                           const StaticLightingBakeDescription& description,
                                           const StaticLightingScheduleDescription& schedule = {}) noexcept;
    /// @brief Nonblocking completion check for a scheduled bake, on the owner thread.
    ///
    /// Returns Pending while any job is unresolved. Once every job is resolved, it
    /// releases the graph through JobSystem::Wait without helping, merges range
    /// results in stable order and encodes all charts on the calling thread.
    /// @return Pending; Ready when every chart is encoded; NotReady after cancellation;
    ///         the first preparation or range failure in range order; or InvalidState
    ///         for a bake that was not submitted, or a call from a job callback.
    ///         Later calls return the same final status.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// @brief Blocking form of Poll: waits for the submitted graph, helping execute ready jobs.
    /// @return As for Poll, never Pending.
    [[nodiscard]] rhi::RasterStatus Finish() noexcept;
    /// @brief Requests cooperative cancellation of a scheduled bake; no effect otherwise.
    ///
    /// Ranges that have not started are skipped, and running ranges stop at their
    /// next sample boundary. Poll or Finish then returns NotReady, unless every job
    /// had already completed. Call on the owner thread while the system is initialized.
    void Cancel() noexcept;
    /// @brief Reads evaluated and total path samples.
    ///
    /// Scheduled progress is a relaxed sum of per-range counters advanced once per
    /// completed triangle, so it is monotonic but may lag the work in flight.
    /// @param outProgress Receives the counts; zero for an unbound bake.
    void GetProgress(StaticLightingBakeProgress& outProgress) const noexcept;
    /// @brief Reads phase durations of a scheduled bake once Poll or Finish has merged it.
    /// @return Ready, including after cancellation or failure; Pending while in flight;
    ///         or InvalidState for a bake that was not submitted. Pending and
    ///         InvalidState preserve outProfile.
    [[nodiscard]] rhi::RasterStatus GetProfile(StaticLightingBakeProfile& outProfile) const noexcept;
    /// @brief Reads the input problems preparation tolerated, such as skipped zero-area triangles.
    ///
    /// Available as soon as preparation completes, while ranges are still running, so
    /// a caller can warn its user and offer Cancel before the bake finishes. Callable
    /// on the owner thread at any time; it never blocks or touches jobs.
    /// @param outDiagnostics Receives the counts on Ready; preserved otherwise.
    /// @return Ready once preparation has succeeded; Pending while a scheduled
    ///         preparation is unresolved; NotReady when preparation was cancelled or
    ///         failed; InvalidState for an unbound bake.
    [[nodiscard]] rhi::RasterStatus GetDiagnostics(StaticLightingBakeDiagnostics& outDiagnostics) const noexcept;
    /// @brief Reads how many zero-area triangles of one placed object the bake skipped.
    ///
    /// Available under the same conditions as GetDiagnostics, so a caller can name the
    /// affected objects while the bake runs. Callable on the owner thread; never blocks.
    /// @param objectIndex Index into SceneDefinition::Objects.
    /// @param outSkippedTriangleCount Receives the count on Ready; preserved otherwise.
    /// @return As for GetDiagnostics, or InvalidDescription for an out-of-range index.
    [[nodiscard]] rhi::RasterStatus GetSkippedTriangleCount(foundation::usize objectIndex,
                                                            foundation::uint32& outSkippedTriangleCount) const noexcept;

private:
    struct State;
    State* mStateOrNull = nullptr;
};
} // namespace ludus::graphics::renderer
