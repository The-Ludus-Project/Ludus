#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/plan.h>

/// @file
/// Versioned runtime generation (`.lurgrt`) layout for RG3 atomic activation.
/// The offline tool writes every field in little-endian order; decoders read it
/// field by field and never copy host struct bytes. The file carries the same
/// numeric plan records as a compiled RG2 header, plus cooked stage artifacts and
/// the CPU interface identity. It contains no source text that needs parsing.
///
/// Layout: a header of HEADER_FIELD_COUNT uint32 values (magic, version,
/// resource/pass/camera counts, uniform value/binding/block/field counts and the
/// stage count) followed by the interface and generation identities;
/// ResourceCount resource, CameraCount camera, PassCount pass, UniformBindingCount
/// binding, UniformBlockCount block and UniformFieldCount field records; then
/// StageCount stage records. A stage record holds STAGE_FIELD_COUNT uint32 values
/// (role, target, input count, entry bytes, artifact bytes, block name count), its
/// input pairs, its GLSL ES uniform block names (slot, byte count, zero-padded
/// name), the entry name and the artifact bytes, with every byte string zero-padded
/// to a four-byte boundary. Version 3 appends PassCount names, each a uint32 byte
/// count and that many UTF-8 bytes zero-padded to four bytes. Names are nonempty,
/// contain no zero, and have at most MAX_PASS_NAME_BYTES bytes. No trailing bytes
/// are permitted. Version 2 has no name table. Version 4 adds ViewNumerator and
/// ViewDenominator after each resource's six legacy fields; versions 2/3 are fixed.
/// Version 8 inserts a storage count and that many four-word StorageBuffer records
/// after stage records and before pass names. Version 6 has native-query ray stages.
/// Version 5 appends one SceneData mask to each pass; older versions imply all scene data.
/// Version 10 appends visibility color/depth resource indices to every pass.
/// Version 7 admits additional raster shader-pair IDs and stage roles; record widths are unchanged.
/// Version 9 appends RayProgram, Buffer0 and Buffer1 to each pass and admits Buffer resources.
/// Older records imply callback zero with no persistent buffers.
/// Version 11 appends BytesPerPixel to each resource for RenderSize-derived storage.
/// Version 12 admits six forward support roles. Only those stage records insert a
/// texture-name count after block names, then (texture slot, sampler slot, byte count,
/// padded name) records. Non-GLSL targets carry zero texture names; older roles keep
/// their existing stage record layout. Sampler slot ABSENT means no paired sampler.
/// Version 13 appends AccumulationSamples to every pass. Older ray packages retain
/// their implicit averaging; new packages require an authored Accumulate node.
/// Version 14 appends float32 ToneMappingExposure to every pass. Older scene-ray packages
/// retain Reinhard/sRGB conversion at unit exposure; new packages require an authored ToneMap.
/// Version 15 appends SourceColor and admits the renderer-owned BilinearUpscale technique.
/// Version 16 appends DenoisingQuality for OIDN RT HDR filtering before display conversion.
/// Version 17 admits MetalFX Spatial roles without changing the version 16 record layout.
/// Version 18 appends Traversal to every pass; earlier ray records require native queries.
/// Version 19 admits authored custom compute traversal; the version 18 record layout is unchanged.
/// Version 20 admits native ray-stage artifacts and explicit hardware invocation.
/// Version 21 inserts RAY_WORKGROUP_DIMENSIONS uint32 values after the six fixed fields
/// of each RayCompute/SceneRayCompute/PersistentRayCompute stage record. X/Y must be positive and Z
/// must be one; targets of the same role must agree. Earlier compute stages imply 64x1x1.
/// Version 22 admits RayTraversal::ComputeBvh in each pass's traversal word.
/// Version 24 adds four clustered deferred support roles without changing record widths.
/// Version 23 admits ClusteredForwardFragment in place of ForwardFragment; other forward
/// stages then use the clustered material ABI. ForwardDepth carries its matching opaque
/// depth-only fragment. It preserves the version 22 record widths.
namespace ludus::content::render_graph_format
{
/// "LURG" read as a little-endian uint32.
inline constexpr foundation::uint32 PACKAGE_MAGIC = 0x4752554CU;
/// "LURP" read as a little-endian uint32: an editor-only diagnostic ray path capture generation.
/// Its layout is a runtime package's, but its ray kernel is the `LUDUS_RAY_PATH_CAPTURE` variant
/// compiled from the same source snapshot. Runtime decoding rejects this magic, so a shipping host
/// never loads it; see docs/architecture/render-graph-authoring.md, "Ray path inspection".
inline constexpr foundation::uint32 RAY_PATH_CAPTURE_PACKAGE_MAGIC = 0x5052554CU;
/// First generation admitting MetalFX Spatial roles; version 16's pass layout is unchanged.
inline constexpr foundation::uint32 METALFX_SPATIAL_PACKAGE_VERSION = 17;
/// First generation carrying software/native ray traversal requirements per pass.
inline constexpr foundation::uint32 SOFTWARE_RAY_PACKAGE_VERSION = 18;
/// First generation admitting authored custom compute traversal without native ray resources.
inline constexpr foundation::uint32 CUSTOM_RAY_PACKAGE_VERSION = 19;
/// First generation carrying native ray-stage artifacts and explicit hardware invocation.
inline constexpr foundation::uint32 NATIVE_RAY_PIPELINE_PACKAGE_VERSION = 20;
/// First generation carrying reflected X/Y/Z dimensions on ray compute stages.
inline constexpr foundation::uint32 RAY_WORKGROUP_PACKAGE_VERSION = 21;
/// First generation admitting the ComputeBvh pass traversal; version 21's layout is unchanged.
inline constexpr foundation::uint32 COMPUTE_BVH_PACKAGE_VERSION = 22;
/// First generation admitting the clustered forward fragment role; record widths are unchanged.
inline constexpr foundation::uint32 CLUSTERED_FORWARD_PACKAGE_VERSION = 23;
/// First generation carrying the four clustered deferred support stages; record widths are unchanged.
inline constexpr foundation::uint32 CLUSTERED_DEFERRED_PACKAGE_VERSION = 24;
/// Current runtime generation layout version.
inline constexpr foundation::uint32 PACKAGE_VERSION = CLUSTERED_DEFERRED_PACKAGE_VERSION;
/// First package carrying explicit OIDN denoising quality per ray pass.
inline constexpr foundation::uint32 DENOISING_PACKAGE_VERSION = 16;
/// First package carrying the renderer-owned bilinear image operation.
inline constexpr foundation::uint32 BILINEAR_PACKAGE_VERSION = 15;
/// First generation carrying explicit ToneMap exposure multipliers.
inline constexpr foundation::uint32 TONE_MAPPING_PACKAGE_VERSION = 14;
/// First generation requiring explicit Accumulate operations in the source graph.
inline constexpr foundation::uint32 ACCUMULATION_PACKAGE_VERSION = 13;
/// First generation carrying the six forward support roles and texture names.
inline constexpr foundation::uint32 FORWARD_PACKAGE_VERSION = 12;
/// Last generation before forward material stages; adds pixel-derived storage.
inline constexpr foundation::uint32 PIXEL_BUFFER_PACKAGE_VERSION = 11;
/// First generation carrying visibility color/depth resource indices in pass records.
inline constexpr foundation::uint32 VISIBILITY_PACKAGE_VERSION = 10;
/// First generation with ordered callback selectors and persistent buffer references.
inline constexpr foundation::uint32 ORDERED_PACKAGE_VERSION = 9;
/// Structured-storage records with the previous ten-word pass layout.
inline constexpr foundation::uint32 STORAGE_PACKAGE_VERSION = 8;
/// First generation supporting independent raster shader pairs, without storage records.
inline constexpr foundation::uint32 INDEPENDENT_PROGRAM_PACKAGE_VERSION = 7;
/// First generation using native hardware ray traversal.
inline constexpr foundation::uint32 NATIVE_RAY_PACKAGE_VERSION = 6;
/// Last package version carrying explicit scene inputs with portable software ray traversal.
inline constexpr foundation::uint32 SCENE_PACKAGE_VERSION = 5;
/// Version with view-relative resources and implicit full-scene pass inputs.
inline constexpr foundation::uint32 VIEW_PACKAGE_VERSION = 4;
/// Version with pass names and fixed resource extents.
inline constexpr foundation::uint32 NAMED_PACKAGE_VERSION = 3;
/// Previous numeric-only runtime generation, accepted with fallback debug names.
inline constexpr foundation::uint32 LEGACY_PACKAGE_VERSION = 2;
/// Maximum encoded UTF-8 bytes of an authored pass name (excludes terminating zero).
inline constexpr foundation::usize MAX_PASS_NAME_BYTES = 127;
/// SHA-256 digest width of interface and generation identities.
inline constexpr foundation::usize IDENTITY_BYTES = 32;
/// Largest accepted runtime generation.
inline constexpr foundation::usize MAX_PACKAGE_BYTES = foundation::usize{16} * 1024 * 1024;
/// Largest single stage artifact, matching the RG1 per-artifact bound.
inline constexpr foundation::usize MAX_ARTIFACT_BYTES = foundation::usize{2} * 1024 * 1024;
/// Largest stage entry-point or GLSL ES uniform block name.
inline constexpr foundation::usize MAX_ENTRY_BYTES = 63;
/// Legacy and scene callback compute roles plus shared image stages.
inline constexpr foundation::usize LEGACY_RAY_STAGE_ROLE_COUNT = 4;
/// Compute/image roles including persistent compute and native ray-stage roles.
inline constexpr foundation::usize RAY_STAGE_ROLE_COUNT = LEGACY_RAY_STAGE_ROLE_COUNT + 2;
/// Legacy stage-role prefix, including ray roles whose IDs remain stable.
inline constexpr foundation::usize LEGACY_STAGE_ROLE_COUNT =
    RASTER_TECHNIQUE_COUNT * TECHNIQUE_STAGE_COUNT + LEGACY_RAY_STAGE_ROLE_COUNT;
/// Additional vertex/fragment roles; each pair follows its compiled technique ID.
inline constexpr foundation::usize ADDITIONAL_RASTER_STAGE_COUNT =
    ADDITIONAL_RASTER_TECHNIQUE_COUNT * TECHNIQUE_STAGE_COUNT;
/// First appended forward material support role; earlier role IDs stay stable.
inline constexpr foundation::usize FORWARD_STAGE_ROLE_FIRST =
    LEGACY_STAGE_ROLE_COUNT + ADDITIONAL_RASTER_STAGE_COUNT + 1;
/// Number of forward material and lighting support stages.
inline constexpr foundation::usize FORWARD_STAGE_ROLE_COUNT = 6;
/// Authored forward vertex interface size.
inline constexpr foundation::usize FORWARD_INPUT_COUNT = 16;
/// Appended native Metal stage role; earlier role identifiers stay stable.
inline constexpr foundation::usize NATIVE_RAY_STAGE_ROLE = FORWARD_STAGE_ROLE_FIRST + FORWARD_STAGE_ROLE_COUNT;
/// Fragment-role marker selecting the complete clustered material ABI for all forward stages.
inline constexpr foundation::usize CLUSTERED_FORWARD_FRAGMENT_ROLE = NATIVE_RAY_STAGE_ROLE + 1;
/// Void clustered forward prepass fragment sharing the authored material vertex entry.
inline constexpr foundation::usize FORWARD_DEPTH_STAGE_ROLE = CLUSTERED_FORWARD_FRAGMENT_ROLE + 1;
/// First appended deferred support role; earlier stage IDs are unchanged.
inline constexpr foundation::usize DEFERRED_STAGE_ROLE_FIRST = FORWARD_DEPTH_STAGE_ROLE + 1;
/// Geometry fragment, lighting vertex/fragment and depth-copy fragment roles.
inline constexpr foundation::usize DEFERRED_STAGE_ROLE_COUNT = 4;
/// Complete role table including clustered deferred support.
inline constexpr foundation::usize STAGE_ROLE_COUNT = DEFERRED_STAGE_ROLE_FIRST + DEFERRED_STAGE_ROLE_COUNT;
/// Return a raster pair's vertex (stage 0) or fragment (stage 1) role.
/// @pre technique is a bounded raster role and stage is below TECHNIQUE_STAGE_COUNT.
[[nodiscard]] LUDUS_INLINE constexpr foundation::usize GetRasterStageRole(Technique technique,
                                                                          foundation::usize stage) noexcept
{
    const foundation::usize index = static_cast<foundation::usize>(technique);
    return index < LEGACY_TECHNIQUE_COUNT
               ? index * TECHNIQUE_STAGE_COUNT + stage
               : LEGACY_STAGE_ROLE_COUNT + (index - LEGACY_TECHNIQUE_COUNT) * TECHNIQUE_STAGE_COUNT + stage;
}
/// Largest stage table: one artifact per role and target.
inline constexpr foundation::usize MAX_STAGES = STAGE_ROLE_COUNT * SHADER_TARGET_COUNT;
/// Renderer static vertex/instance interface: position plus four transform rows and color.
inline constexpr foundation::usize STATIC_INPUT_COUNT = 6;
/// Fixed uint32 fields before the identities.
inline constexpr foundation::usize HEADER_FIELD_COUNT = 10;
/// Serialized header width.
inline constexpr foundation::usize HEADER_BYTES = HEADER_FIELD_COUNT * sizeof(foundation::uint32) + 2 * IDENTITY_BYTES;
/// Serialized resource record width, derived from the explicit field schema.
inline constexpr foundation::usize RESOURCE_RECORD_BYTES = RESOURCE_FIELD_COUNT * sizeof(foundation::uint32);
/// Serialized resource width for relative-size package versions 4 through 10.
inline constexpr foundation::usize RELATIVE_RESOURCE_RECORD_BYTES = 8 * sizeof(foundation::uint32);
/// Serialized structured storage record width; fields are encoded independently of host padding.
inline constexpr foundation::usize STORAGE_BUFFER_RECORD_BYTES =
    STORAGE_BUFFER_FIELD_COUNT * sizeof(foundation::uint32);
/// Serialized camera record width; matches Camera's one field.
inline constexpr foundation::usize CAMERA_RECORD_BYTES = sizeof(foundation::uint32);
/// Serialized pass record width; version 5 appends SceneData to the nine legacy fields.
inline constexpr foundation::usize PASS_RECORD_BYTES = PASS_FIELD_COUNT * sizeof(foundation::uint32);
/// Serialized uniform binding record width; matches UniformBinding's five fields.
inline constexpr foundation::usize UNIFORM_BINDING_RECORD_BYTES = 5 * sizeof(foundation::uint32);
/// Serialized uniform block record width; matches UniformBlock's three fields.
inline constexpr foundation::usize UNIFORM_BLOCK_RECORD_BYTES = 3 * sizeof(foundation::uint32);
/// Serialized uniform field record width; matches UniformField's five fields.
inline constexpr foundation::usize UNIFORM_FIELD_RECORD_BYTES = 5 * sizeof(foundation::uint32);
/// Fixed uint32 fields at the start of each stage record.
inline constexpr foundation::usize STAGE_FIELD_COUNT = 6;
/// Reflected X, Y and Z words on a ray compute stage in version 19 and later.
inline constexpr foundation::usize RAY_WORKGROUP_DIMENSIONS = 3;
/// Serialized width of one (location, format) stage input pair.
inline constexpr foundation::usize INPUT_RECORD_BYTES = 2 * sizeof(foundation::uint32);
/// Explicit program role: raster vertex/fragment pairs, followed by three linked ray roles.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class StageRole : foundation::uint32
{
    ColorVertex,         ///< ColorAndDepth geometry entry.
    ColorFragment,       ///< ColorAndDepth color entry.
    DepthVertex,         ///< DepthOnly geometry entry.
    DepthFragment,       ///< DepthOnly void coverage entry.
    MaskedColorVertex,   ///< MaskedColorAndDepth geometry entry.
    MaskedColorFragment, ///< MaskedColorAndDepth color entry with authored coverage.
    MaskedDepthVertex,   ///< MaskedDepthOnly geometry entry.
    MaskedDepthFragment, ///< MaskedDepthOnly separately authored void coverage entry.
    RayCompute,          ///< Linked authored ray functions and renderer-owned GPU BVH traversal.
    RayImageVertex,      ///< Renderer-owned GPU color/index presentation vertex entry.
    RayImageFragment,    ///< Renderer-owned color presentation fragment entry.
    SceneRayCompute,     ///< Scene/material/light callback compute profile; mutually exclusive with RayCompute.
    PersistentRayCompute = LEGACY_STAGE_ROLE_COUNT +
        ADDITIONAL_RASTER_STAGE_COUNT, ///< Ordered callback sets with two persistent storage slots; exclusive with
                                       ///< other computes.
    ForwardVertex = FORWARD_STAGE_ROLE_FIRST, ///< Authored forward geometry entry.
    ForwardFragment,                          ///< Authored forward material and direct-light entry.
    ForwardShadow,                            ///< Matching material shadow coverage entry.
    LightingVertex,                           ///< Renderer lighting composition vertex.
    LightingFragment,                         ///< Renderer lighting composition fragment.
    LightingShadow,                           ///< Renderer flat shadow fragment.
    /// Clustered material fragment; replaces ForwardFragment without renumbering earlier roles.
    ClusteredForwardFragment = CLUSTERED_FORWARD_FRAGMENT_ROLE,
    /// Optional opaque depth-only fragment, executed with the exact forward vertex entry.
    ForwardDepth = FORWARD_DEPTH_STAGE_ROLE,
    DeferredGBufferFragment = DEFERRED_STAGE_ROLE_FIRST, ///< Authored opaque material to three G-buffer targets.
    DeferredLightingVertex,                              ///< Fullscreen clustered lighting geometry.
    DeferredLightingFragment,  ///< Authored lighting resolve with reconstructed world position and exported depth.
    DeferredDepthCopyFragment, ///< Copies optional prepass depth to the G-buffer owner.
    NativeRayStages = NATIVE_RAY_STAGE_ROLE, ///< Linked native ray stages sharing the persistent scene ABI.
};
/// Float vertex input width of one stage input record.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class InputFormat : foundation::uint32
{
    Float32, ///< One float.
    Float2,  ///< Two floats.
    Float3,  ///< Three floats.
    Float4,  ///< Four floats.
};
static_assert(HEADER_BYTES == 104);
static_assert(RESOURCE_RECORD_BYTES == sizeof(Resource));
static_assert(CAMERA_RECORD_BYTES == sizeof(Camera));
static_assert(PASS_RECORD_BYTES == sizeof(Pass));
static_assert(UNIFORM_BINDING_RECORD_BYTES == sizeof(UniformBinding));
static_assert(UNIFORM_BLOCK_RECORD_BYTES == sizeof(UniformBlock));
static_assert(UNIFORM_FIELD_RECORD_BYTES == sizeof(UniformField));
} // namespace ludus::content::render_graph_format
