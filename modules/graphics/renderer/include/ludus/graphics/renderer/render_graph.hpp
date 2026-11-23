#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/light_transformer_encoder.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/renderer/world_feature.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/pipeline_preparation.h>

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

/// CPU preparation ceiling for graph ray scenes, including the frozen scene, BVH,
/// fingerprint and packing/native-geometry scratch. Four GiB on 64-bit hosts; two
/// GiB on 32-bit hosts. This is an admission ceiling, not an initial allocation.
/// A device's storage-buffer range and physical allocation failures remain independent.
inline constexpr foundation::usize RENDER_GRAPH_RAY_CPU_BYTES =
    foundation::usize{2} * 1024 * 1024 * 1024 * (sizeof(foundation::usize) > sizeof(foundation::uint32) ? 2 : 1);
/// Logical GPU ceiling across current/replacement graph ray scenes, per-pass images,
/// native acceleration structures and pending producer/consumer holds. Six GiB on
/// 64-bit hosts; two GiB on 32-bit hosts. Device creators must separately budget RHI
/// resources, including raster geometry, textures and graph targets sharing the device.
inline constexpr foundation::usize RENDER_GRAPH_RAY_GPU_BYTES =
    foundation::usize{2} * 1024 * 1024 * 1024 * (sizeof(foundation::usize) > sizeof(foundation::uint32) ? 3 : 1);

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
    // Preserve warning-clean aggregate callers that omit this optional table.
    // NOLINTNEXTLINE(readability-redundant-member-init)
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
/// Cooked pipeline recipe identity of one technique on one shader target. Key holds the SHA-256 digest that the
/// graph build spells in hexadecimal as `inventory.recipes[].key` in `build-report.json` and `recipes[].key` in the
/// package, so device preparation can be joined with the offline cost inventory. Diagnostic identity only: it
/// never selects a technique, stage or pipeline.
struct RenderGraphRecipeKey final
{
    content::render_graph_format::Technique Technique{}; ///< Numeric technique role that uses the recipe.
    content::render_graph_format::ShaderTarget Target{}; ///< Shader profile the recipe was cooked for.
    /// Recipe digest bytes, most significant hexadecimal pair first.
    foundation::StaticArray<foundation::uint8, content::render_graph_format::IDENTITY_BYTES> Key{};
};
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
/// @note The struct is tens of KiB. Browser hosts run on emscripten's default 64 KiB stack,
/// so construct it in owned storage, for example `new (std::nothrow) RenderGraphPrograms(MakePrograms(...))`,
/// rather than as a stack temporary.
struct RenderGraphPrograms final
{
    /// Discarded startup invocations before a frame capture collects its requested count; zero for IID graphs.
    /// Indexed by Pass::RayProgram and cooked from each compute program's UInt32 capture_warmup_setting.
    /// Interactive limits are unchanged.
    foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_RAY_PROGRAMS> CaptureWarmupSamples{};
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
    /// Native Metal ray stages; borrowed through graph preparation like RayCompute. Empty when unused.
    rhi::RasterShaderDescription NativeRayStages{};
    /// Ordered compute callbacks that read the shared structured Buffer. Empty when unused.
    rhi::RasterShaderDescription RecordReadCompute{};
    /// Bits indexed by Pass::RayProgram that select RecordReadCompute for compute invocations.
    foundation::uint32 RecordReaderMask = 0;
    rhi::RasterShaderDescription RayCompute{};       ///< Linked authored triangle ray functions and BVH traversal.
    rhi::RasterShaderDescription RayImageVertex{};   ///< Renderer-owned GPU-produced pixel presentation.
    rhi::RasterShaderDescription RayImageFragment{}; ///< Renderer-owned color copy; no host pixel transfer.
    /// Independent raster stage pairs in numeric technique order after the legacy roles.
    /// Index (Program - LEGACY_TECHNIQUE_COUNT) * TECHNIQUE_STAGE_COUNT selects a
    /// vertex description; the following entry is its fragment description. All
    /// artifact/input/binding storage is borrowed through initialization/preparation,
    /// with the same lifetime contract as ColorVertex and ColorFragment.
    foundation::StaticArray<rhi::RasterShaderDescription,
                            content::render_graph_format::ADDITIONAL_RASTER_TECHNIQUE_COUNT *
                                content::render_graph_format::TECHNIQUE_STAGE_COUNT>
        AdditionalRasterStages{};
    rhi::RasterShaderDescription
        ForwardVertex{}; ///< Authored material geometry, supplied together with all forward stages.
    rhi::RasterShaderDescription
        ForwardFragment{}; ///< Authored material ABI; HasClusteredForward selects its complete light-list layout.
    rhi::RasterShaderDescription ForwardShadow{};    ///< Matching material shadow ABI used by program qualification.
    rhi::RasterShaderDescription LightingVertex{};   ///< Renderer output preparation support.
    rhi::RasterShaderDescription LightingFragment{}; ///< Renderer output preparation support.
    /// Void opaque prepass fragment, required with HasClusteredForward; uses ForwardVertex unchanged.
    rhi::RasterShaderDescription ForwardDepth{};
    /// Optional forward fragment for draws with ready and validated surface charts. Empty selects realtime shading.
    rhi::RasterShaderDescription ForwardSurfaceChartFragment{};
    rhi::RasterShaderDescription LightingShadow{}; ///< Flat caster support for lighting initialization.
    /// Linked ray kernel reads the paired raster visibility/depth inputs declared by the plan.
    bool HasVisibilityBuffer = false;
    /// Use the complete clustered material ABI and conservative spatial light lists.
    bool HasClusteredForward = false;
    /// Opaque material fragment writing normal/roughness, base/metallic and emission/coverage in RGBA16F. It applies
    /// alpha-masked coverage and draws masked materials when DeferredOpaqueGBufferFragment is present, every material
    /// otherwise.
    rhi::RasterShaderDescription DeferredGBufferFragment{};
    /// The same G-buffer fragment compiled without the coverage test, drawn for opaque materials before masked ones so
    /// tile-based GPUs keep hidden-surface removal. Present with HasClusteredDeferred in packages from
    /// format::OPAQUE_GBUFFER_PACKAGE_VERSION; empty in earlier packages, which draw every material through
    /// DeferredGBufferFragment.
    rhi::RasterShaderDescription DeferredOpaqueGBufferFragment{};
    /// Optional G-buffer fragment for GI Probe Receivers of a deferred pass with a connected Baked Lighting input.
    /// It applies alpha-masked coverage and adds the baked probe volume's diffuse bounce to the emission target.
    /// Present with HasClusteredDeferred in packages from format::DEFERRED_PROBE_GBUFFER_PACKAGE_VERSION that connect
    /// Baked Lighting; empty otherwise, which draws every receiver with realtime lighting.
    rhi::RasterShaderDescription DeferredProbeGBufferFragment{};
    /// Optional G-buffer fragment for static surfaces with a ready chart in a deferred pass with a connected Baked
    /// Lighting input. It applies alpha-masked coverage, adds the chart's baked diffuse to the emission target and
    /// marks the pixel so the resolve omits the baked lights' live diffuse while keeping their specular. Present with
    /// HasClusteredDeferred in packages from format::DEFERRED_SURFACE_CHART_GBUFFER_PACKAGE_VERSION that connect
    /// Baked Lighting; empty otherwise, which draws every static surface with realtime lighting.
    rhi::RasterShaderDescription DeferredSurfaceChartGBufferFragment{};
    /// Renderer fullscreen geometry for deferred lighting and optional prepass depth transfer.
    rhi::RasterShaderDescription DeferredLightingVertex{};
    /// Clustered lighting resolve; reads the G-buffer and writes linear color plus scene depth.
    rhi::RasterShaderDescription DeferredLightingFragment{};
    /// Optional prepass depth transfer into the G-buffer's private depth owner.
    rhi::RasterShaderDescription DeferredDepthCopyFragment{};
    /// Enable the opaque three-target deferred profile; requires the complete clustered material ABI.
    bool HasClusteredDeferred = false;
    bool HasPersistentRayBuffers = false; ///< Extended ordered ray ABI with two persistent storage slots.
    bool HasSceneShaderInterface = false; ///< Opt-in material/light/query callback ABI, encoded in cooked stage roles.
    /// RayCompute is the diagnostic ray path capture variant (`LUDUS_RAY_PATH_CAPTURE`) of the same
    /// source snapshot: it also binds one read-write capture buffer at slot 10. RenderGraph appends
    /// that binding when RayCompute.Bindings omit it. Such a graph requires RequestRayPathCapture
    /// before its first Update and accepts exactly one extraction. Runtime packages never carry this variant.
    bool HasRayPathCapture = false;
    /// RayCompute reads the renderer-owned LTC table (`scene_interface.polygonal_light_sampling`) and ends its
    /// bindings with the read-only table buffer at slot 13 (LUDUS_GRAPH_LTC_TABLE_BINDING). The
    /// renderer uploads and binds that buffer; RayCompute.Bindings must name the slot exactly when
    /// this is true. See docs/development/ltc-table.md.
    bool HasLtcTable = false;
    /// RayCompute calls EvaluateEnvironment, so reflection kept the Atmosphere sky table at slot 15
    /// (LUDUS_GRAPH_ATMOSPHERE_SKY_BINDING), its last binding after the failure summary. The renderer resolves the
    /// sky for each prepared scene and binds it; RayCompute.Bindings must name the slot exactly when this is true.
    /// Requires HasSceneShaderInterface. See GetWorldFeatureSupport.
    bool HasAtmosphereSky = false;
    /// RayCompute calls ApplyAerialPerspective, so reflection kept the aerial perspective table at slot 11
    /// (LUDUS_GRAPH_AERIAL_PERSPECTIVE_BINDING). The renderer resolves and binds it for each prepared scene;
    /// RayCompute.Bindings must name the slot exactly when this is true. Requires HasSceneShaderInterface.
    bool HasAerialPerspective = false;
    /// RayCompute offsets its dispatch by the ray uniform's tile region (runtime package version 28 and later,
    /// and every generated header), so RenderGraph::SetRayTileCount may split a sample into row bands. Older
    /// kernels cover the whole image on every dispatch and therefore cannot be tiled.
    bool HasRayTiles = false;
    bool HasLightTransformer =
        false; ///< Requires a trained model description; prepares a view-independent GPU encoder.
    /// Recipe keys of the routed techniques, at most one per technique and target; copied during Initialize.
    /// Generated headers and runtime generations may supply these keys. Techniques without a supplied key
    /// still appear in RenderGraph::GetPipelineReport with HasRecipeKey false.
    // Keep partial aggregate callers warning-clean when this optional field is omitted.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const RenderGraphRecipeKey> RecipeKeys{};
    /// Deferred resolve understands the near-region index in the fourth light word. Older packages leave this false.
    bool HasDirectionalNearCascades = false;
};
/// How much of a World feature a graph with these programs draws. The clustered deferred resolve composes every
/// feature (Supported). Otherwise a ray kernel draws a feature as the documented approximation (Approximated) exactly
/// when it calls the feature's scene function, which HasAtmosphereSky and HasAerialPerspective record from the compiled
/// kernel's reflection; every other graph draws the World without it (NotRendered).
/// @param programs Programs as RenderGraph::Initialize admits them.
/// @param feature A WorldFeature value; values outside the enum return NotRendered.
[[nodiscard]] WorldFeatureSupport GetWorldFeatureSupport(const RenderGraphPrograms& programs,
                                                         WorldFeature feature) noexcept;
/// Why a graph ray invocation failed: the scene interface's reasons, recorded by the shader function
/// FailSceneQuery (scene_shader_layout.h, LUDUS_SCENE_FAILURE_*). Authored shaders may report their own reasons from
/// RENDER_GRAPH_FAILURE_AUTHORED_FIRST. See docs/development/render-graph-authoring.md, "Explain failed pixels".
enum class RenderGraphFailureReason : foundation::uint8
{
    Unspecified,                ///< A shader set queries.HasFailed without a reason.
    InvalidRadiance,            ///< A sample returned non-finite or negative radiance.
    InterfaceVersion,           ///< The package was compiled for another scene interface or kernel version.
    SurfaceBudget,              ///< More surface queries than scene_interface.surface_queries.
    VisibilityBudget,           ///< More visibility queries than scene_interface.visibility_queries.
    LineBudget,                 ///< More line queries than scene_interface.line_queries.
    InvalidRay,                 ///< A ray with a non-unit direction or an invalid distance range.
    Traversal,                  ///< The traversal could not finish, such as a full BVH stack.
    RandomExhausted,            ///< The invocation's random stream ran out of dimensions.
    InvalidArgument,            ///< A scene function received an out-of-range or degenerate input.
    LightSample,                ///< No light could be selected or sampled.
    StorageCapacity,            ///< A graph storage buffer has fewer entries than the pixels using it.
    StoredRecord,               ///< Per-pixel data an earlier pass did not write for this pixel or frame.
    SceneData,                  ///< An invalid triangle, surface or material from a scene lookup.
    InvalidWeight,              ///< A non-finite or negative sampling weight, probability or PDF.
    BsdfOnlyLights,             ///< Analytic lights require a light-sampling transport profile.
    PrimaryMaterial,            ///< A material requires random dimensions outside the primary-sample profile.
    PhotonCamera,               ///< Guided photon backprojection requires a perspective camera.
    PhotonCapacity,             ///< Guided photon storage or light admission exceeds its declared template limits.
    PhotonDispatch,             ///< Photon maps and spatial reuse require a whole-image dispatch.
    GeometrySamplingLights,     ///< A geometry-sampling template received analytic or environment lighting.
    GeometryStatisticsCapacity, ///< A geometry-sampling statistics buffer cannot hold the scene hierarchy.
    WaveDirection,              ///< The wave aperture plane has no propagation direction.
    WaveResolution,             ///< The aperture quadrature cannot resolve the Fresnel phase gradient.
    WaveCapacity,               ///< A scalar optical connection exceeds the region or triangle workspace.
    WaveConnection,             ///< The optical wavelength or finite connection is outside the paraxial profile.
    WavePropagationCapacity,    ///< An optical connection exceeds the admitted number of successive regions.
    FixedDimensionMaterial,     ///< An assigned surface lacks continuous reflection with a fixed sample dimension.
    LuslBounds,                 ///< A development LUSL callback indexed outside its declared bounds.
};
/// First failure reason an authored shader may report with FailSceneQuery.
inline constexpr foundation::uint32 RENDER_GRAPH_FAILURE_AUTHORED_FIRST = 32;
/// Failure reasons a graph ray kernel counts: the scene interface's and the authored ones.
inline constexpr foundation::uint32 MAX_RENDER_GRAPH_FAILURE_REASONS = 64;
/// Describe a failure reason in one sentence for an author, with what to change. Authored reasons get a generic
/// sentence naming the reason number.
/// @param reason A RenderGraphFailureReason value or an authored reason below MAX_RENDER_GRAPH_FAILURE_REASONS.
/// @return Static UTF-8 text; an empty string for a reason outside the range.
[[nodiscard]] const char* DescribeRenderGraphFailureReason(foundation::uint32 reason) noexcept;
/// Pixels of one ray pass that latched each failure reason since its camera or scene last changed.
struct RenderGraphRayFailures final
{
    /// Pixels by reason; index with a RenderGraphFailureReason value or an authored reason.
    foundation::StaticArray<foundation::uint32, MAX_RENDER_GRAPH_FAILURE_REASONS> Pixels{};
    /// Sum of Pixels.
    foundation::uint64 TotalPixels = 0;
    /// The reason with the most pixels; meaningful when TotalPixels is nonzero.
    foundation::uint32 MainReason = 0;
    /// Highest authored line reported by a checked LUSL invocation in this history, or zero if none.
    foundation::uint32 LuslBoundsLine = 0;
};
/// Ray work of one sample of a RenderGraph's current scene, from which a frame capture sizes its tiles.
struct RenderGraphRayWorkload final
{
    /// Invocation count per sample, summed over image pixels and buffer-only work items.
    foundation::uint64 Pixels = 0;
    /// Emissive triangles plus analytic lights of the largest ray pass scene; an environment is not counted.
    foundation::uint32 Lights = 0;
};
/// Attach the frozen triangle-ray compute/image interface metadata to a cooked stage.
/// Does not compile, allocate or change artifact bytes. Used by generated headers and
/// runtime decoders; the RHI independently checks actual native shader reflection.
/// @param inoutDescription Receives binding metadata and the legacy 64x1x1 Compute default.
/// Cooked headers and version 19+ packages replace that default with reflected dimensions.
/// @param stage Compute for the linked ray kernel, Vertex/Fragment for the image recipe.
/// @param hasVisibilityBuffer Adds paired unfiltered visibility/depth compute texture inputs.
/// @param hasPersistentBuffers Use the extended two-buffer compute ABI; image stages are unchanged.
/// @param hasNativeRayQueries Include the acceleration-structure binding. False uses only ordinary
/// compute buffers/textures; the artifact must contain software traversal and no native ray instructions.
void SetRenderGraphRayInterface(rhi::RasterShaderDescription& inoutDescription,
                                rhi::ShaderStage stage,
                                bool hasPersistentBuffers = false,
                                bool hasVisibilityBuffer = false,
                                bool hasNativeRayQueries = true) noexcept;
/// Attach the immutable forward recipe ABI to one cooked support stage. Artifact bytes
/// remain untouched; shader reflection and Renderer prewarming independently validate it.
/// @param inoutDescription Receives exact inputs, bindings and uniform member metadata.
/// @param role A forward material/output, ForwardDepth or deferred support role; other roles leave the description
/// unchanged.
/// @param isClustered Select the clustered material layout; old forward packages retain their ABI.
void SetRenderGraphForwardInterface(rhi::RasterShaderDescription& inoutDescription,
                                    content::render_graph_format::StageRole role,
                                    bool isClustered = false) noexcept;
/// Triangle, instance and primitive value of a ray path event without a committed hit.
inline constexpr foundation::uint32 RAY_PATH_INVALID_INDEX = 0xffffffffU;
/// Default number of progressive samples a ray path capture records.
inline constexpr foundation::uint32 DEFAULT_RAY_PATH_SAMPLES = 16;
/// Largest Accumulate sample limit a frame capture may set with RenderGraph::SetAccumulationLimitOverride.
/// Interactive graphs keep RenderGraphFormat::MAX_ACCUMULATION_SAMPLES.
inline constexpr foundation::uint32 MAX_RENDER_GRAPH_CAPTURE_SAMPLES = 65536;
/// Most row bands RenderGraph::SetRayTileCount may split one ray sample into. Each band is a separate
/// submission, so a long frame-capture sample stays under the GPU watchdog; see
/// docs/architecture/render-graph-frame-capture.md.
inline constexpr foundation::uint32 MAX_RENDER_GRAPH_RAY_TILES = 128;
/// float4 values per pixel in a ray pass's accumulation buffer: the running sum with the committed
/// count's uint32 bits in W, then the compensation with the history tag in W (odd after a latched failure).
inline constexpr foundation::uint32 RENDER_GRAPH_ACCUMULATION_WORDS = 2;
/// Largest number of progressive samples one ray path capture records.
inline constexpr foundation::uint32 MAX_RAY_PATH_SAMPLES = 256;
/// Default event record capacity of one ray path capture.
inline constexpr foundation::uint32 DEFAULT_RAY_PATH_RECORDS = 1024;
/// Largest event record capacity of one ray path capture. The whole capture buffer must
/// also fit the device's rhi::LifetimeCapabilities::TransferBytes, so it is read back at once.
inline constexpr foundation::uint32 MAX_RAY_PATH_RECORDS = 2048;
/// Selection of one ray path capture: one launch pixel of the ray passes' color extent and
/// the progressive samples [0, SampleCount). This first slice captures a single pixel.
struct RayPathCaptureDescription final
{
    foundation::uint32 PixelX = 0; ///< Launch pixel column; a pixel outside a pass's extent records nothing.
    foundation::uint32 PixelY = 0; ///< Launch pixel row, top row zero.
    /// Progressive samples recorded, 1..MAX_RAY_PATH_SAMPLES. A pass whose authored Accumulate
    /// limit is lower traces, and so records, only that many samples.
    foundation::uint32 SampleCount = DEFAULT_RAY_PATH_SAMPLES;
    /// Event records kept, 1..MAX_RAY_PATH_RECORDS. Later events beyond it are counted as dropped.
    foundation::uint32 RecordCapacity = DEFAULT_RAY_PATH_RECORDS;
};
/// Which kernel operation produced a ray path event.
enum class RayPathEventKind : foundation::uint8
{
    Closest,        ///< A closest-hit query: camera ray, bounce or authored TraceClosest.
    Visibility,     ///< An any-hit visibility (shadow) query; Hit means occluded.
    RasterFirstHit, ///< A visibility-buffer first hit replacing the camera query; no traversal.
};
/// Result of one ray path event.
enum class RayPathOutcome : foundation::uint8
{
    Miss,    ///< No accepted triangle in the submitted interval.
    Hit,     ///< A committed triangle; its identity fields are valid.
    Failure, ///< The failed-triangle sentinel, an invalid submitted ray or a failed raster reconstruction.
};
/// One decoded Trace() call, or raster first hit, of a captured invocation. Values are exactly
/// what the kernel submitted and committed; driver BVH traversal is not observable.
struct RayPathEvent final
{
    foundation::uint32 PixelX = 0;   ///< Launch pixel column.
    foundation::uint32 PixelY = 0;   ///< Launch pixel row.
    foundation::uint32 Sample = 0;   ///< Progressive sample index within the capture's history epoch.
    foundation::uint32 Pass = 0;     ///< Index into the plan's Passes of the ray pass that traced it.
    foundation::uint32 Sequence = 0; ///< Zero-based order within the invocation of that pixel, sample and pass.
    RayPathEventKind Kind = RayPathEventKind::Closest;     ///< Query kind.
    RayPathOutcome Outcome = RayPathOutcome::Miss;         ///< Query result.
    foundation::math::Vector3 OriginWorld{};               ///< Submitted origin, world meters.
    foundation::math::Vector3 DirectionWorld{};            ///< Submitted direction as passed to the query.
    foundation::float32 MinDistanceWorld = 0;              ///< Submitted interval start, world meters.
    foundation::float32 MaxDistanceWorld = 0;              ///< Submitted interval end, world meters.
    foundation::float32 HitDistanceWorld = 0;              ///< Committed hit distance; MaxDistanceWorld for a miss.
    foundation::uint32 Triangle = RAY_PATH_INVALID_INDEX;  ///< Packed scene triangle of a hit.
    foundation::uint32 Instance = RAY_PATH_INVALID_INDEX;  ///< Extraction instance index of a hit.
    foundation::uint32 Primitive = RAY_PATH_INVALID_INDEX; ///< Mesh-local primitive of a hit.
    foundation::uint64 SourceId = 0;                       ///< RenderGraphItem::SourceId of a hit, otherwise zero.
    foundation::uint32 RejectedCandidates = 0;             ///< Candidates refused by the alpha test before the commit.
};
/// End state of one captured invocation (pixel, sample and pass).
struct RayPathSummary final
{
    foundation::uint32 PixelX = 0;        ///< Launch pixel column.
    foundation::uint32 PixelY = 0;        ///< Launch pixel row.
    foundation::uint32 Sample = 0;        ///< Progressive sample index.
    foundation::uint32 Pass = 0;          ///< Index into the plan's Passes.
    foundation::uint32 EventCount = 0;    ///< Events the invocation produced, including dropped ones.
    foundation::math::Vector3 Radiance{}; ///< Sample radiance passed to accumulation, before display conversion.
    bool HasFailed = false;               ///< A failure event or an invalid (non-finite/negative) sample.
    /// Recorded sequences are not contiguous from zero to EventCount: capacity dropped part of the
    /// path. A truncated path must never be presented as complete.
    bool IsTruncated = false;
};
/// Counters of the latest decoded ray path capture.
struct RayPathCaptureReport final
{
    foundation::uint32 RecordCapacity = 0;     ///< Requested event record capacity.
    foundation::uint32 StoredRecords = 0;      ///< Decoded events, at most RecordCapacity.
    foundation::uint32 DroppedRecords = 0;     ///< Events lost because capacity was exhausted.
    foundation::uint32 WrittenSummaries = 0;   ///< Captured invocations so far.
    foundation::uint32 TruncatedSummaries = 0; ///< Captured invocations with dropped events.
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
    foundation::BorrowedView<const foundation::uint32> MaterialSlots;
    /// Slot-to-global-material mapping; unassigned slots use DEFAULT_SCENE_MATERIAL.
    foundation::BorrowedView<const foundation::uint32> MaterialBindings;
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
    foundation::BorrowedView<const foundation::uint32> MaterialOverrides;
    foundation::math::Vector4 SurfaceTint{1, 1, 1, 1}; ///< Instance multiplier for the scene material profile.
};
/// Borrowed complete rendering inputs. Preparation consumes them before returning;
/// scene/material GPU resources belong to Renderer and retain exact incarnations.
struct RenderGraphSceneDescription final
{
    foundation::BorrowedView<const RenderGraphMesh> Meshes; ///< Geometry and slot bindings.
    foundation::BorrowedView<const RenderGraphItem> Items;  ///< Instances and overrides.
    SceneShaderInputs Inputs{};                             ///< Materials, analytic sources and constant environment.
    bool HasMaterialInterface = false;                      ///< False preserves legacy flat-color submissions.
    bool AreErrorsAllowed = false;                          ///< Preview uses an explicit non-emissive error material.
    SceneShaderDiagnostics* DiagnosticsOrNull = nullptr; ///< Optional candidate diagnostic output, including rejection.
    /// Caller scene revision; also seeds graph sampling when a new extraction starts history.
    /// Zero is reserved for standalone diagnostic preparation. Different revisions select different streams.
    foundation::uint64 SourceGeneration = 0;
    /// Selected ::ludus::content::render_graph_format::SCENE_DATA_* groups; Lights retain emitter emission
    /// dependencies. Consumed before Update returns.
    foundation::uint32 SceneData = ::ludus::content::render_graph_format::SCENE_DATA_ALL;
    /// Atmosphere sky table read by EvaluateEnvironment in kernels with RenderGraphPrograms::HasAtmosphereSky, header
    /// included, in the LUDUS_GRAPH_ATMOSPHERE_SKY_BINDING layout of renderer_gpu_preview_layout.h. RenderGraph::Update
    /// resolves it from the camera's Atmosphere; preparation copies it before returning. Empty binds no table.
    // Keep partial aggregate callers warning-clean when these optional fields are omitted.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const foundation::math::Vector4> AtmosphereSkyTable{};
    /// Aerial perspective table read by ApplyAerialPerspective in kernels with
    /// RenderGraphPrograms::HasAerialPerspective, in the LUDUS_GRAPH_AERIAL_PERSPECTIVE_BINDING layout. Copied during
    /// preparation; empty binds no table.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const foundation::math::Vector4> AerialPerspectiveTable{};
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
/// Scene-level cause of the latest Update rejection, so callers can name the input and the repair.
/// Only causes this graph can attribute are reported; other Unsupported results report None.
enum class RenderGraphSceneRejection : foundation::uint8
{
    None,               ///< The latest Update was not rejected for a scene-level cause.
    CameraDepth,        ///< A camera has an infinite far plane or an unknown depth convention.
    Coverage,           ///< Conflicting coverage, or incomplete coverage under RejectIncomplete.
    MaskedDepthPrepass, ///< Alpha-masked materials would be sealed by the opaque depth-only prepass.
};
/// Step at which a graph operation last failed, so a host can say where a generation was rejected and what to repair.
/// RenderGraph records the steps of Poll and Update (GetLastFailureStage); RenderGraphActivation adds Setup and Resize
/// for the Initialize and Resize calls it makes on a candidate. A stage locates the check that failed; the returned
/// rhi::RasterStatus remains the outcome, and RenderGraphSceneRejection still names the attributable scene causes.
enum class RenderGraphFailureStage : foundation::uint8
{
    None,                ///< No failure: the latest call was accepted, pending or not ready, or none was made.
    Setup,               ///< Initialize rejected the plan, its programs or resource descriptions, or ran out of memory.
    Resize,              ///< Resize rejected the reference extent or could not recreate the graph's attachments.
    PipelinePreparation, ///< Poll: preparing the graph's pipelines, programs or resources failed.
    ScenePreparation,    ///< Poll or Update: preparing an accepted camera or scene on the GPU failed.
    ExtractionInputs,    ///< Update: mesh, item or camera counts, the scene revisions or the coverage policy.
    CameraValues,        ///< Update: a camera's exposure, exposure mode or lighting view.
    ItemValues,          ///< Update: an item's source identity, mesh index or per-draw uniform values.
    SceneResources,      ///< Update: materials and textures, or the buffers and uniform sets created for the scene.
    Meshes,              ///< Update: a mesh's bounds or its geometry upload.
    WorldTables,         ///< Update: resolving the Atmosphere sky and haze tables that ray passes read.
    LightTransformer,    ///< Update: preparing the light transformer model for the scene.
    RayScene,            ///< Update: building a ray pass's packed scene and acceleration structure.
};
/// Editor and shipping admission are explicit policies.
enum class RenderGraphCoveragePolicy : foundation::uint8
{
    RejectIncomplete, ///< Preserve the last drawable scene on unsupported/conflicting coverage.
    DiagnosticColor,  ///< Admit an editor diagnostic color/proxy route while retaining unsupported reports.
};
/// Caller-owned revision promises for independent non-camera scene domains. All five
/// must be nonzero to enable camera-only reuse, or all zero for full validation.
/// Reusing every identity promises byte/semantic equality of all non-camera inputs,
/// including coverage and uniform values. A changed identity stages a replacement;
/// exact immutable mesh/texture content can still reuse the graph-local cache.
struct RenderGraphSceneRevisions final
{
    foundation::uint64 Geometry = 0;    ///< Mesh attributes, topology and slot mapping.
    foundation::uint64 Instances = 0;   ///< Transforms, exclusions, coverage and per-draw values.
    foundation::uint64 Materials = 0;   ///< Material values, assignments, samplers and texture content.
    foundation::uint64 Lights = 0;      ///< Complete analytic light set and its authored values.
    foundation::uint64 Environment = 0; ///< Constant/HDR environment values and image content.
    /// Compare domain identities; this does not compare source bytes or validate caller promises.
    [[nodiscard]] LUDUS_INLINE bool operator==(const RenderGraphSceneRevisions&) const noexcept = default;
};
/// Maximum whole-scene directional maps admitted for a single generation (Sun and Moon).
inline constexpr foundation::usize MAX_DIRECTIONAL_SHADOW_LIGHTS = 2;
/// Coverage and recorded caster work for a directional map in the drawable generation.
struct DirectionalCascadeReport final
{
    /// World metres per interior texel along the light projection X and Y axes. Zero when absent.
    foundation::math::Vector2 MetresPerTexel{};
    /// Conventional light transform. Clip XY in [-1,1] and Z in [0,1] bound the fitted caster column.
    foundation::math::Matrix4 WorldToClip{};
    /// Gutter-excluded atlas rectangle in pixels. Zero width means the map is absent.
    rhi::RasterRectangle Interior{};
    foundation::float32 Bias = 0;     ///< Receiver tolerance in normalized light depth.
    foundation::uint32 Draws = 0;     ///< Prepared caster packets.
    foundation::uint64 Triangles = 0; ///< Instanced caster triangles submitted when filling the map.
};
/// Coverage of a drawable directional source. A missing near region selects WholeScene at every depth.
struct DirectionalLightShadowReport final
{
    foundation::uint64 SourceId = 0;       ///< Stable World light identity, or zero in an unused report entry.
    DirectionalCascadeReport WholeScene{}; ///< Whole-scene map available throughout the view.
    DirectionalCascadeReport Near{};       ///< Fine projection used up to DIRECTIONAL_NEAR_END_METRES of forward depth.
};
/// Finest sky visibility coverage submitted for the drawable generation.
enum class SkyVisibilityLevel : foundation::uint8
{
    None,       ///< No directions have been submitted.
    WholeWorld, ///< Whole-World depth coverage is available.
    Local,      ///< Bounded local maps supplement whole-World coverage.
};
/// Submitted sky-depth coverage of the currently drawable deferred scene generation.
struct SkyVisibilityReport final
{
    /// Finest submitted level. Local coverage varies with the receiver and direction.
    SkyVisibilityLevel FinestSubmittedLevel = SkyVisibilityLevel::None;
    foundation::uint64 Revision = 0; ///< Scene revision that prepared these maps, preserved across camera-only updates.
    foundation::uint32 WholeWorldMaps = 0; ///< Submitted whole-World direction maps.
    foundation::uint32 LocalMaps = 0;      ///< Submitted maps in the bounded local neighbourhood.
    foundation::math::Vector3
        AnchorWorld{}; ///< Centre of the local neighbourhood in world metres, valid with local coverage.
    foundation::float32 LocalMetresPerTexel = 0; ///< Local XY footprint, zero without allocated local maps.
    foundation::float32 CoarseMetresPerTexel =
        0;                          ///< Horizontal footprint of the first whole-World direction, in metres.
    bool HasBudgetFallback = false; ///< Local refinement was requested but could not be admitted in the shadow atlas.
};
/// Device/context-local forward resource counters and the drawable scene's chart draw count.
/// Camera-only updates preserve resource acquisition counters. Reuse compares complete copied content.
struct RenderGraphSceneResourceReport final
{
    /// Draw items using a specialized surface-chart shader (the forward chart fragment or the deferred chart G-buffer)
    /// in the currently drawable scene.
    foundation::usize SurfaceChartDraws = 0;
    /// Draw items sampling the baked probe volume through the deferred probe G-buffer in the currently drawable scene.
    foundation::usize ProbeReceiverDraws = 0;
    foundation::uint64 CreatedMeshes = 0;     ///< Immutable mesh uploads admitted by the cache.
    foundation::uint64 ReusedMeshes = 0;      ///< Exact cached mesh acquisitions.
    foundation::uint64 CreatedTextures = 0;   ///< Immutable texture uploads admitted by the cache.
    foundation::uint64 ReusedTextures = 0;    ///< Exact cached texture acquisitions.
    foundation::uint32 MeshLightPolygons = 0; ///< Latest prepared deferred scene's emitter polygon records.
    foundation::uint64 MeshLightBytes = 0; ///< Immutable mesh-light upload bytes, including an empty-set dummy record.
    /// Latest prepared deferred scene's mesh-light shadow casters, each shadowing a cluster of emitter records from
    /// one point with cube faces rendered once into the shadow atlas.
    foundation::uint32 MeshLightShadowCasters = 0;
    /// Latest prepared deferred scene's emitter records left unshadowed because the caster limit,
    /// MAX_MESH_LIGHT_SHADOW_CASTERS, was reached; the brightest records are shadowed first.
    foundation::uint32 UnshadowedMeshLights = 0;
    /// Latest prepared deferred scene's shadow faces, each rendered once after the scene is published.
    foundation::uint32 MeshLightShadowFaces = 0;
    foundation::uint32 OverflowClusters = 0;   ///< Latest material's cells using the full light-list fallback.
    foundation::uint32 ClusterAssignments = 0; ///< Latest material's stored cell/light intersections.
    foundation::uint32 DroppedLights =
        0; ///< Latest forward material's dropped light count; clustered shading retains all admitted lights.
    /// Source ID of the brightest directional light given a shadow map in the latest prepared deferred scene, or zero
    /// if none. Ties go to the lower source ID.
    foundation::uint64 DirectionalShadowLightSourceId = 0;
    /// Directional lights given a shadow map in the latest prepared deferred scene, brightest first, at most two:
    /// enough for an Atmosphere's Sun and Moon.
    foundation::uint32 DirectionalShadowLights = 0;
    /// Number of additional stabilized near regions admitted for this candidate. Zero means whole-scene coverage.
    foundation::uint32 DirectionalNearRegions = 0;
    /// Eligible directional lights (raster shadows requested, positive intensity, nonblack color) of the latest
    /// prepared deferred scene left without a map because two brighter ones hold them. They illuminate unshadowed;
    /// clear their raster shadow request to make that explicit.
    foundation::uint32 UnshadowedDirectionalLights = 0;
    /// Whole-World sky visibility maps of the latest prepared deferred scene: SKY_VISIBILITY_DIRECTION_COUNT
    /// (deferred_layout.h) orthographic depth maps from fixed upper-hemisphere directions, rendered once into the
    /// shadow atlas, or zero for a scene without geometry. The resolve integrates environment and Atmosphere sky
    /// light over those directions only where they see past the scene (render-graph-authoring.md, "Sky visibility").
    /// GetSkyVisibilityReport reports both submitted levels of the currently drawable scene.
    foundation::uint32 SkyVisibilityMaps = 0;
    /// Directional map footprints and work in the drawable scene, in admission order.
    foundation::StaticArray<DirectionalLightShadowReport, MAX_DIRECTIONAL_SHADOW_LIGHTS> DrawableDirectionalShadows{};
    /// Combined unpadded RGBA8 and private depth bytes of the drawable generation's shadow atlas.
    foundation::uint64 DrawableShadowAttachmentBytes = 0;
    /// Texels occupied by allocated mesh-light face slots, including their gutters, in the drawable atlas.
    foundation::uint64 DrawableMeshLightFaceTexels = 0;
    /// Cumulative near projections prepared on a cache miss (including a replacement scene).
    foundation::uint64 PreparedDirectionalNearRegions = 0;
};
/// Summary of the latest candidate, separately from the last rendered image.
struct RenderGraphReport final
{
    RenderGraphSceneResourceReport
        Resources{}; ///< Forward cache activity; candidate diagnostics remain separate from the displayed scene.
    SceneShaderDiagnostics SceneDiagnostics{}; ///< Material defaults/errors and discarded collapsed faces.
    foundation::uint64 SceneRevision = 0;      ///< Caller extraction revision, not a GPU completion number.
    foundation::uint32 Covered = 0;            ///< Compatible item/role/view tuples.
    foundation::uint32 Unsupported = 0;        ///< Missing required compatibility.
    foundation::uint32 Conflicting = 0;        ///< Ambiguous/duplicate role tuples.
    foundation::uint32 Excluded = 0;           ///< Explicit omissions.
    bool IsAccepted = false;                   ///< Candidate was accepted for asynchronous preparation.
    bool IsReady = false;                      ///< Accepted scene is drawable; producer completion is separate.
    /// World features the accepted extraction uses and this graph does not render (GetWorldFeatureSupport returns
    /// NotRendered). A camera that enables an Atmosphere uses AtmosphereSky; one whose Atmosphere also lights the scene
    /// uses AerialPerspective. The graph still draws the rest of the World, so callers can tell the user what is
    /// missing (DescribeWorldFeature). Zero when nothing is missing and before an extraction is accepted.
    WorldFeatureMask UnrenderedWorldFeatures = 0;
    LightTransformerPreparationReport Model{}; ///< Latest model candidate diagnostics; independent of drawable output.
    /// Mesh-light shadow faces of the drawable scene not yet drawn into its atlas; each Render draws some within its
    /// per-frame budgets, and receivers of an undrawn face read as unoccluded. Zero without a drawable scene or
    /// casters; a candidate's faces count only once it is published.
    foundation::uint32 PendingMeshLightShadowFaces = 0;
};
namespace internal
{
class RenderGraphRayPass;
/// Private access from RenderGraph to its attachment pool.
struct RenderGraphPoolAccess;
/// Private access from graph programs to their shared program dependencies.
struct RenderGraphProgramCacheAccess;
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
/// Most distinct renderer-owned program dependency sets one RenderGraphProgramCache holds at once.
/// Every graph using the built-in composite recipe needs one set; a second set exists only while
/// graphs initialized from different composite artifacts are alive together.
inline constexpr foundation::uint32 RENDER_GRAPH_PROGRAM_CACHE_CAPACITY = 4;
/// Copied counters of one program cache. Counts are dependency sets and leases, never bytes or
/// native pipelines: rhi::PipelinePreparationReport counts the pipelines prepared from them.
struct RenderGraphProgramCacheReport final
{
    foundation::uint32 LiveEntries = 0;    ///< Dependency sets currently held by at least one lease.
    foundation::uint32 Leases = 0;         ///< Program leases currently held across those sets.
    foundation::uint64 CreatedEntries = 0; ///< Acquisitions that created a set: its shaders, layout, sampler and quad.
    foundation::uint64 SharedLeases = 0;   ///< Acquisitions that joined a live set with an equal canonical description.
    foundation::uint64 ReleasedEntries = 0; ///< Sets destroyed by their final lease; RHI retires their in-flight uses.
};
/// Device-session owner of the immutable dependencies of renderer-owned graph programs, shared by
/// every RenderGraph initialized with it. The first pilot is the built-in bilinear upscale pass
/// (BilinearUpscale and the MetalFX surface copy): its vertex and fragment stages, binding layout,
/// linear sampler and full-target quad. Graphs acquire a set by the complete canonical description of
/// the composite stages (stage, every target artifact and entry, reflected inputs, bindings, uniform
/// members and GLSL ES names), never by pointer or hash, so graphs and generations with equal stages
/// share the same RHI incarnations. Each graph then acquires its raster pipeline through
/// rhi::RequestPipeline, which coalesces equal shader, layout and fixed-state requests into one native
/// pipeline; distinct targets (surface or RGBA8) keep distinct pipelines. Input bindings, targets and
/// extents remain local to each graph instance. A set is destroyed by its final lease release, and RHI
/// retires uses still referenced by accepted work. No idle set is retained.
/// Not thread-safe: use the RHI owner thread between frames. The device outlives the cache; the cache
/// outlives every graph initialized with it and must be Reset before the device is destroyed.
/// See docs/architecture/renderer-systems.md, "First migration and acceptance".
class RenderGraphProgramCache final
{
public:
    RenderGraphProgramCache() noexcept;                                          ///< Construct without a device.
    ~RenderGraphProgramCache() noexcept;                                         ///< Reset; see Reset.
    RenderGraphProgramCache(const RenderGraphProgramCache&) = delete;            ///< Ownership is noncopyable.
    RenderGraphProgramCache& operator=(const RenderGraphProgramCache&) = delete; ///< Ownership is nonassignable.
    /// Bind the cache to a ready device without allocating GPU resources.
    /// @param device Ready owned device; outlives the cache and every graph initialized with it.
    /// @return InvalidState when already initialized, OutOfMemory when bookkeeping cannot be
    /// allocated, otherwise the device admission status; failure leaves the cache empty.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device) noexcept;
    /// Copy the counters; an uninitialized cache reports zeros.
    [[nodiscard]] RenderGraphProgramCacheReport GetReport() const noexcept;
    /// Unbind the device.
    /// @pre No graph initialized with this cache still exists; leases are asserted to be zero.
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
    /// Grants renderer-owned graph programs acquisition and release of shared dependency sets.
    friend struct internal::RenderGraphProgramCacheAccess;
};
/// Copied attachment ownership of one graph; byte estimates are explicitly named.
struct RenderGraphAllocationReport final
{
    foundation::uint32 ExportAllocations = 0;    ///< Allocations backing an export; created and destroyed by the graph.
    foundation::uint32 TransientAllocations = 0; ///< Allocations backing only unexported, non-surface resources.
    foundation::uint32 CreatedAllocations = 0;   ///< Allocations this graph created, exports included.
    foundation::uint32 ReusedAllocations = 0;    ///< Transient allocations reused from the pool.
    bool IsPooled = false;                       ///< Initialized with a pool; false is the serial, non-pooled control.
    /// Live deferred G-buffer allocations across drawable and pending scenes; separate from plan allocations above.
    foundation::uint32 DeferredAllocations = 0;
    /// Logical color plus private depth bytes of those allocations; excludes native padding and per-view bindings.
    foundation::uint64 DeferredAttachmentBytes = 0;
    /// Live skin diffusion allocations: three RGBA16F targets with private depth per deferred view that draws skin
    /// (a Skin material with a nonzero scattering distance), allocated on its first such view. Separate from the
    /// G-buffer allocations above.
    foundation::uint32 DeferredSkinAllocations = 0;
    /// Logical color plus private depth bytes of the skin allocations, 36 per pixel of the view.
    foundation::uint64 DeferredSkinAttachmentBytes = 0;
};
/// How Render and Encode record the raster passes of the drawable scene.
/// See "Logical resource uses and submitted work" in docs/architecture/render-graph-authoring.md.
enum class RenderGraphExecution : foundation::uint8
{
    /// Each pass records through its Renderer path. The default, and the comparison control of the pilot.
    Direct,
    /// Pilot: lower the plan to one RHI ordered graph per frame, built between frames by PrepareEncoding and
    /// encoded by Render/Encode. Admits a plan of exactly one ColorAndDepth pass, outside the forward,
    /// deferred, visibility and light-transformer profiles, that clears the color and depth of one
    /// offscreen RGBA8 allocation. Its view must hold only flat opaque packets without uniform values.
    OrderedGraph,
};
/// Copied lowering of the latest PrepareEncoding in RenderGraphExecution::OrderedGraph, kept after encoding
/// consumes the graph. Index arrays follow the cooked plan; ABSENT marks no counterpart. Pass indices are
/// cooked serial operations, comparable with the offline plan dump's intervals.
struct RenderGraphExecutionReport final
{
    /// Mode of the latest lowering attempt; Direct when none was made.
    RenderGraphExecution Execution = RenderGraphExecution::Direct;
    /// Ready after a compiled graph; otherwise the rejection, raised before any command was recorded.
    rhi::RasterStatus Result = rhi::RasterStatus::NotReady;
    /// Cooked pass whose view or capacity rejected lowering, or ABSENT.
    foundation::uint32 FailedPass = content::render_graph_format::ABSENT;
    /// Draw packets the plan requires, checked against DrawLimit before the graph is created.
    foundation::uint32 DrawCount = 0;
    /// The device's StorageLimits::DrawsPerPlan.
    foundation::uint32 DrawLimit = 0;
    /// RHI resources the plan requires: attachment backings, then each pass's instance and mesh buffers.
    foundation::uint32 ResourceCount = 0;
    /// The device's StorageLimits::ResourcesPerGraph, which also bounds one pass's declared uses.
    foundation::uint32 ResourceLimit = 0;
    /// Planned RHI raster pass starts of the compiled graph.
    foundation::uint32 RasterPasses = 0;
    /// Semantic dependencies the RHI compiler reported, including the external import and export edges.
    foundation::uint32 DependencyCount = 0;
    /// RHI authored pass ordinal of each cooked pass.
    foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_PASSES> GraphPasses{};
    /// RHI resource index of each logical resource's physical backing. Logical color and depth sharing a
    /// backing map to one resource; the presentation surface and unused resources are ABSENT.
    foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_RESOURCES> GraphResources{};
    /// First cooked pass using each logical resource's backing, from the RHI lifetime of the physical
    /// resource. A backing reports the union of the logical intervals it holds.
    foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_RESOURCES> FirstPass{};
    /// Last cooked pass using each logical resource's backing. Exports and readbacks remain in use beyond it,
    /// until their consumers finish.
    foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_RESOURCES> LastPass{};
};
/// Device preparation of the native raster pipelines compiled from one used technique's cooked stages.
/// A recipe is prepared as several fixed-function native pipelines (surface and offscreen targets,
/// conventional and reverse depth, overlay), so NativePipelines can exceed one for one recipe key.
/// Renderer-owned composite, lighting, deferred-resolve, upscale and compute pipelines are not
/// attributed to a recipe; they appear only in RenderGraphPipelineReport::Device, except the bilinear
/// upscale pipelines, reported in RenderGraphPipelineReport::Upscale. Stage pipelines are created directly
/// for each graph, so another graph or generation with identical stages prepares its own native pipelines;
/// only RHI RequestPipeline callers share them (rhi::PipelinePreparationReport::SharedRequests).
struct RenderGraphPipelineRecipe final
{
    /// Recipe identity; Recipe.Target is the device's shader target and Recipe.Key is valid when HasRecipeKey.
    RenderGraphRecipeKey Recipe{};
    /// The programs carried a key for this technique and target.
    bool HasRecipeKey = false;
    /// Native pipelines created from the technique's stages, including ones still preparing.
    foundation::uint32 NativePipelines = 0;
    /// Of those, pipelines whose creation completed with Ready.
    foundation::uint32 ReadyPipelines = 0;
    /// Of those, pipelines used by at least one accepted draw. Prepared variants that this graph never draws
    /// (for example surface variants of a graph that renders offscreen) remain counted in NativePipelines.
    foundation::uint32 DrawnPipelines = 0;
    /// Draw requests that found this recipe still preparing: Render or Encode calls rejected before all of its
    /// pipelines were Ready, plus RHI draw-time uses of its pipelines while preparing (each pipeline once).
    /// Render never creates a pipeline, so a miss is a skipped frame, never a hidden creation stall.
    foundation::uint64 FirstUseMisses = 0;
    /// Sum of the native preparation seconds of the ready pipelines (rhi::PipelinePreparation::PreparationSeconds).
    foundation::float64 PrewarmSeconds = 0;
    /// Sum of the seconds those pipelines waited in the request queue before native creation launched.
    foundation::float64 QueueSeconds = 0;
};
/// Device preparation of the raster pipelines one graph acquires through rhi::RequestPipeline for a
/// renderer-owned pass. Preparation time is owner-thread wall time and is separate from frame time:
/// Render and Encode never prepare a pipeline.
struct RenderGraphManagedPipelines final
{
    /// Pipeline requests the graph owns for the pass, one per distinct target it draws (surface, RGBA8).
    foundation::uint32 Requests = 0;
    /// Of those, requests that joined a native pipeline another request (another graph or generation
    /// sharing the RenderGraphProgramCache) had already queued or created, so they launched no creation.
    foundation::uint32 SharedRequests = 0;
    /// Of those, requests whose shared native pipeline completed with Ready.
    foundation::uint32 ReadyRequests = 0;
    /// Native preparation seconds of the ready pipelines; a shared pipeline reports its single creation.
    foundation::float64 PrewarmSeconds = 0;
    /// Seconds those pipelines waited in the request queue before native creation launched.
    foundation::float64 QueueSeconds = 0;
};
/// Device pipeline preparation of one graph, keyed by recipe and the qualified device's backend and target.
struct RenderGraphPipelineReport final
{
    /// Bilinear upscale pipelines (BilinearUpscale passes and the MetalFX surface copy) of this graph.
    RenderGraphManagedPipelines Upscale{};
    /// Totals of the whole device session, including pipelines owned by other graphs and renderers.
    rhi::PipelinePreparationReport Device{};
    /// Shader target this graph's device consumes; RenderGraphPipelineRecipe rows use it.
    content::render_graph_format::ShaderTarget Target = content::render_graph_format::ShaderTarget::Spirv;
    /// One row per used technique in numeric technique order; RecipeCount rows are valid.
    foundation::StaticArray<RenderGraphPipelineRecipe, content::render_graph_format::TECHNIQUE_COUNT> Recipes{};
    /// Number of valid Recipes rows.
    foundation::uint32 RecipeCount = 0;
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
    /// @param modelOrNull Required exactly for HasLightTransformer programs. Assets are copied;
    /// encoder artifact storage survives Poll. The model uses diffuse mesh-emitter inputs
    /// and fixed training scale; pending/failed scene encoding keeps the drawable generation.
    /// @param programCacheOrNull Optional program cache on the same device, shared with other graphs and
    /// generations so equal renderer-owned programs (the bilinear upscale pass) share their shaders,
    /// layout and native pipeline. Null gives the graph a private cache: it prepares its own dependencies
    /// and shares a pipeline only with requests for those same incarnations. An uninitialized cache, or one
    /// bound to another device, rejects with InvalidDescription. The cache must outlive this graph.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const RenderGraphPlan& plan,
                                               const RenderGraphPrograms& programs,
                                               RenderGraphAttachmentPool* poolOrNull = nullptr,
                                               foundation::uint32 referenceWidth = 0,
                                               foundation::uint32 referenceHeight = 0,
                                               const LightTransformerModelDescription* modelOrNull = nullptr,
                                               RenderGraphProgramCache* programCacheOrNull = nullptr) noexcept;
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
    /// Borrow a stable persistent Buffer resource for an external prepared compute
    /// producer/consumer, using the same RHI device, graph hazards and submission ledger.
    /// The graph owns the allocation: never Destroy the returned handle. Reacquire
    /// after a pixel-buffer resize or graph replacement; retained bindings retire normally.
    /// Call between frames. Pending scene/resize work or an unfinished frame producer
    /// returns NotReady so callers cannot mutate the last drawable view's storage.
    /// A subsequent authored scene-compute pass consumes GPU results through its wired
    /// GraphBuffer slot. No readback, upload or ownership transfer occurs here.
    /// Model embedding buffers expose their actual prepared packet range, not the placeholder
    /// allocation in the source graph. Their contents are valid after the first completed Render.
    /// @param resource Numeric Buffer resource index in this graph's cooked plan.
    /// @param outBuffer Borrowed allocation, written only on Ready.
    /// @param outBytes Usable byte capacity, written only on Ready.
    /// @return Ready; InvalidDescription for an invalid index; Unsupported for a texture;
    /// NotReady before preparation/while a replacement or GPU producer is outstanding.
    [[nodiscard]] rhi::RasterStatus GetPersistentBuffer(foundation::uint32 resource,
                                                        rhi::BufferHandle& outBuffer,
                                                        foundation::usize& outBytes) const noexcept;
    /// Copy attachment ownership counters; an empty graph reports zeros.
    [[nodiscard]] RenderGraphAllocationReport GetAllocationReport() const noexcept;
    /// Report device pipeline preparation for each used technique and the device totals.
    /// Diagnostic and owner-thread only; performs no allocation, GPU work or completion polling, and changes
    /// no graph state. Timing is wall time on the owner thread and never enters any identity.
    /// @param outReport Written on Ready; preserved on failure.
    /// @return Ready after Initialize was accepted; NotReady for an empty graph; device admission failures.
    [[nodiscard]] rhi::RasterStatus GetPipelineReport(RenderGraphPipelineReport& outReport) const noexcept;
    /// Poll prewarming and pending replacement; publish only a complete Ready candidate.
    /// Returns a candidate failure once while preserving the prior drawable scene.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Copy current World extraction and all generation-local cameras. One full coverage scan
    /// precedes visibility culling. Pending/failed preparation preserves prior drawing.
    /// Rejection updates diagnostic report but preserves the current and pending scene.
    /// Source storage need only survive this call. Revision must be nonzero.
    /// @param meshes Borrowed static geometry table, copied during a full extraction.
    /// @param items Borrowed instances, transforms, material bindings and per-draw values.
    /// @param cameras Complete generation-local camera table; consumed before return. Each camera uses
    /// a finite projection in either depth convention. A reverse-Z camera keeps reverse-Z depth in a
    /// graph that CanUseReverseDepth; any other graph receives the exactly equivalent conventional
    /// camera (clip z rewritten as w - z), so its depth targets and captures are conventional.
    /// An infinite far plane returns Unsupported.
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
    /// @param revisions Optional domain revision promises. All-zero preserves contentRevision behavior.
    /// A fully nonzero record enables the same camera-only reuse when every domain and coverage policy match.
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
                                           foundation::uint64 contentRevision = 0,
                                           const RenderGraphSceneRevisions& revisions = {}) noexcept;
    /// Whether the initialized graph keeps reverse-Z cameras in reverse-Z depth. Every pass must be a
    /// raster scene pass outside the visibility codec, or an image operation: ray tracing passes and
    /// visibility-buffer consumers rebuild positions with conventional depth, so a graph containing
    /// one converts reverse-Z cameras instead. Reverse-Z float32 depth keeps rebuilt positions
    /// within micrometres at tens of metres, where conventional depth with a centimetre near plane
    /// loses centimetres. False before Initialize. Owner-thread query; no allocation.
    [[nodiscard]] bool CanUseReverseDepth() const noexcept;
    /// Borrow latest candidate coverage records until the next Update/Reset; no allocation.
    [[nodiscard]] foundation::BorrowedView<const RenderGraphCoverageRecord> GetCoverage() const noexcept;
    /// Copy the latest candidate report, independent of the drawable extraction.
    [[nodiscard]] RenderGraphReport GetReport() const noexcept;
    /// Cause of the latest Update's Unsupported result; None after any other result, before the
    /// first Update and after Reset. Diagnostic only: it never changes the drawable or pending scene.
    [[nodiscard]] RenderGraphSceneRejection GetLastRejection() const noexcept;
    /// Step of the latest Poll or Update that failed; None after an accepted, Pending or NotReady result, before the
    /// first call and after Reset. Diagnostic only: it never changes the drawable or pending scene. Owner-thread query.
    [[nodiscard]] RenderGraphFailureStage GetLastFailureStage() const noexcept;
    /// How much of a World feature this graph draws, derived from its programs (see GetWorldFeatureSupport).
    /// NotRendered before Initialize succeeds.
    [[nodiscard]] WorldFeatureSupport GetWorldFeatureSupport(WorldFeature feature) const noexcept;
    /// Inspect the current drawable sky atlas on the graph owner thread. An empty report precedes publication.
    /// Counts advance on submission. Pending and failed replacement scenes preserve the current report.
    [[nodiscard]] SkyVisibilityReport GetSkyVisibilityReport() const noexcept;
    /// Return the revision of the last Ready extraction, or zero when none exists.
    [[nodiscard]] foundation::uint64 GetDrawableSceneRevision() const noexcept;
    /// True when the last ray writer of any color target outputs individual samples or has an
    /// unfinished authored Accumulate sample limit. False for an empty/raster-only graph or
    /// when every final ray writer has reached its positive limit. Reservoir reuse does not average color.
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
    /// Select the pixel and samples recorded by a graph initialized with RenderGraphPrograms::HasRayPathCapture.
    /// Call between frames after Initialize reaches Ready and before the first Update; each graph
    /// instance captures once. Allocates the capture buffer within the device's transfer budget and binds
    /// it into every ray pass prepared by later Updates. Run the capture in its own graph instance, so the
    /// live graph's drawable scene and output never change; render it from the frozen camera until the
    /// requested samples complete, then call PollRayPathCapture. See "Ray path inspection" in
    /// docs/architecture/render-graph-authoring.md.
    /// @param description Pixel, sample count and record capacity; copied before return.
    /// @return Ready or Pending once the buffer is accepted; Unsupported when RayCompute is not the capture
    /// variant or two ray passes share one cooked callback set; InvalidState when uninitialized, not Ready,
    /// in a frame, already requested or after a scene was accepted; InvalidDescription for a sample count
    /// or record capacity outside its range; CapacityExceeded when the buffer exceeds TransferBytes.
    /// Failure leaves the graph unchanged.
    [[nodiscard]] rhi::RasterStatus RequestRayPathCapture(const RayPathCaptureDescription& description) noexcept;
    /// Read back and decode the capture without waiting. The first call after the last producer completes
    /// starts a bounded readback and returns Pending; a later call returns Ready with the decoded events,
    /// sorted by pixel, sample, pass and sequence, and the per-invocation summaries. Calling again starts a
    /// new readback of the same buffer, so callers may poll while more samples render. Owner thread, between
    /// frames. Never changes drawable output.
    /// @param outReport Receives the decoded counters on Ready; unchanged otherwise.
    /// @return Ready after decoding; Pending while the producer or readback is outstanding; InvalidState
    /// without a request or during an open/unconfirmed frame; a device or readback failure otherwise,
    /// which keeps the previously decoded capture.
    [[nodiscard]] rhi::RasterStatus PollRayPathCapture(RayPathCaptureReport& outReport) noexcept;
    /// Replace every positive authored Accumulate sample limit with samples, for a graph instance that
    /// captures a frame rather than drawing the viewport. Passes that output individual samples keep doing so.
    /// Call between frames after Initialize reaches Ready and before the first Update; the override applies to
    /// every later scene of this instance. See docs/architecture/render-graph-frame-capture.md.
    /// @param samples New limit, from one to MAX_RENDER_GRAPH_CAPTURE_SAMPLES.
    /// @return Ready when set; InvalidDescription for samples out of range; InvalidState when uninitialized,
    /// not Ready, in a frame or after a scene was accepted. Failure leaves the graph unchanged.
    [[nodiscard]] rhi::RasterStatus SetAccumulationLimitOverride(foundation::uint32 samples) noexcept;
    /// Borrow the accumulation buffer of a ray pass after its producer completed, for a read-only consumer
    /// such as a frame capture's readback. The buffer holds RENDER_GRAPH_ACCUMULATION_WORDS float4 values per
    /// pixel of the pass's colour resource, in row-major order. The graph owns it; finish every use, including
    /// readbacks, before the next Render, Update or Reset, and never write to it.
    /// Ordinary accumulated callbacks store RGB sums; ResolveGraphRadiance callbacks
    /// store their latest resolved estimate times the invocation count. Divide either
    /// by the bit-preserved uint32 count in the first word's W to recover linear RGB.
    /// @param pass Plan pass index of a ray pass.
    /// @param outBuffer Receives the borrowed buffer; unchanged on failure.
    /// @return Ready after the current scene and camera produce a completed frame; NotReady while they have none.
    /// InvalidDescription for a pass that is not a ray pass, or Unsupported for an independent buffer or
    /// storage texture dispatch. Outputs are preserved on failure.
    [[nodiscard]] rhi::RasterStatus GetAccumulation(foundation::uint32 pass,
                                                    rhi::BufferHandle& outBuffer) const noexcept;
    /// Split every ray sample into tileCount row bands, rendering one band per Render, for a graph instance
    /// that captures a frame. Each band dispatches every ray pass over the same rows, with the pixel indices,
    /// sample streams and accumulation of an untiled sample, so only the submission boundaries change.
    /// HasPendingSamples and GetSubmittedRaySamples count a sample once its last band was submitted. Call
    /// between frames at a sample boundary; the count applies to later scenes of this instance too.
    /// @param tileCount Bands per sample, from one (whole-image dispatches) to MAX_RENDER_GRAPH_RAY_TILES.
    /// @return Ready when set; InvalidDescription for a count out of range; Unsupported for more than one band
    /// when RenderGraphPrograms::HasRayTiles is false or the plan contains an independent buffer or storage texture
    /// dispatch or a sampled scene color input; InvalidState when uninitialized, not Ready, in a frame or between
    /// the bands of a sample.
    /// Failure leaves the graph unchanged.
    [[nodiscard]] rhi::RasterStatus SetRayTileCount(foundation::uint32 tileCount) noexcept;
    /// Select how later frames record the drawable scene. Direct is always admitted. OrderedGraph checks the
    /// cooked plan against the pilot profile here; its scene-dependent admission (packets, draw and resource
    /// limits) runs in PrepareEncoding, between frames, and a rejection leaves the last output and exports
    /// untouched with the cause in GetExecutionReport. A change discards any prepared, unencoded graph.
    /// @param execution Recording path for later Render/Encode calls.
    /// @return Ready when set; Unsupported for a plan outside RenderGraphExecution::OrderedGraph's profile;
    /// InvalidState when uninitialized, not Ready, in a frame or awaiting ConfirmSubmission. Failure leaves the
    /// graph unchanged.
    [[nodiscard]] rhi::RasterStatus SetExecution(RenderGraphExecution execution) noexcept;
    /// Copy the lowering of the latest ordered-graph PrepareEncoding; a default report before any attempt.
    [[nodiscard]] RenderGraphExecutionReport GetExecutionReport() const noexcept;
    /// Ray samples submitted for every pixel of the drawable scene: whole frames, or complete rounds of bands
    /// after SetRayTileCount. Zero without a scene; a replaced scene or camera starts again from zero.
    [[nodiscard]] foundation::uint32 GetSubmittedRaySamples() const noexcept;
    /// Describe the ray work of one sample of the drawable scene, for sizing tiles.
    /// @param outWorkload Receives the pixels and lights; unchanged on failure.
    /// @return Ready; NotReady without a drawable scene or before its ray passes are prepared.
    [[nodiscard]] rhi::RasterStatus GetRayWorkload(RenderGraphRayWorkload& outWorkload) const noexcept;
    /// Read back, without waiting, how many pixels of a ray pass latched each failure reason (the graph's failure
    /// colour) since its camera or scene last changed. The first call requests a small read-back; a later call
    /// returns it, and the call after that requests a fresh one. Owner thread, between frames; never changes output.
    /// @param pass Plan pass index of a ray pass.
    /// @param outFailures Receives the counts on Ready; unchanged otherwise.
    /// @return Ready with counts; Pending while the read-back or the producer is outstanding; NotReady without a
    /// drawable scene; Unsupported when the pass's kernel declares no failure summary (it was cooked before failure
    /// reasons; rebuild the graph); InvalidDescription for a pass that is not a ray pass; a read-back failure
    /// otherwise.
    [[nodiscard]] rhi::RasterStatus PollRayFailures(foundation::uint32 pass,
                                                    RenderGraphRayFailures& outFailures) noexcept;
    /// Borrow the events of the last Ready PollRayPathCapture until the next poll, request or Reset.
    [[nodiscard]] foundation::BorrowedView<const RayPathEvent> GetRayPathEvents() const noexcept;
    /// Borrow the summaries of the last Ready PollRayPathCapture, sorted by pixel, sample and pass,
    /// until the next poll, request or Reset.
    [[nodiscard]] foundation::BorrowedView<const RayPathSummary> GetRayPathSummaries() const noexcept;
    /// Detach graph resources. Device stays alive, no open frame or foreign export consumer.
    /// GPU uses retire in RHI; does not wait or destroy another graph generation.
    void Reset() noexcept;

private:
    /// Allow the internal parity fixture to borrow completed pass history without a public readback API.
    friend class internal::RenderGraphRayPass;
    struct State;
    /// Body of Poll; it marks State's current step so Poll can record the failing one.
    [[nodiscard]] rhi::RasterStatus pollPreparation() noexcept;
    /// Body of Update with the same contract; it marks State's current step so Update can record the failing one.
    [[nodiscard]] rhi::RasterStatus updateScene(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                                foundation::BorrowedView<const RenderGraphItem> items,
                                                foundation::BorrowedView<const ViewDescription> cameras,
                                                foundation::uint64 sceneRevision,
                                                RenderGraphCoveragePolicy policy,
                                                RenderGraphReport& outReport,
                                                const SceneShaderInputs* sceneInputsOrNull,
                                                foundation::uint64 contentRevision,
                                                const RenderGraphSceneRevisions& revisions) noexcept;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
