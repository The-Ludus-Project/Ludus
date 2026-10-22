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
namespace ludus::content::render_graph_format
{
/// "LURG" read as a little-endian uint32.
inline constexpr foundation::uint32 PACKAGE_MAGIC = 0x4752554CU;
/// Runtime generation layout version.
inline constexpr foundation::uint32 PACKAGE_VERSION = 10;
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
/// Compute/image roles including the separately appended persistent compute role.
inline constexpr foundation::usize RAY_STAGE_ROLE_COUNT = LEGACY_RAY_STAGE_ROLE_COUNT + 1;
/// Legacy stage-role prefix, including ray roles whose IDs remain stable.
inline constexpr foundation::usize LEGACY_STAGE_ROLE_COUNT =
    RASTER_TECHNIQUE_COUNT * TECHNIQUE_STAGE_COUNT + LEGACY_RAY_STAGE_ROLE_COUNT;
/// Additional vertex/fragment roles; each pair follows its compiled technique ID.
inline constexpr foundation::usize ADDITIONAL_RASTER_STAGE_COUNT =
    ADDITIONAL_RASTER_TECHNIQUE_COUNT * TECHNIQUE_STAGE_COUNT;
/// All admitted stage roles, including independent raster pairs.
inline constexpr foundation::usize STAGE_ROLE_COUNT = LEGACY_STAGE_ROLE_COUNT + ADDITIONAL_RASTER_STAGE_COUNT + 1;
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
inline constexpr foundation::usize MAX_STAGES =
    (MAX_AUTHORED_TECHNIQUES * TECHNIQUE_STAGE_COUNT + RAY_STAGE_ROLE_COUNT) * SHADER_TARGET_COUNT;
/// Renderer static vertex/instance interface: position plus four transform rows and color.
inline constexpr foundation::usize STATIC_INPUT_COUNT = 6;
/// Fixed uint32 fields before the identities.
inline constexpr foundation::usize HEADER_FIELD_COUNT = 10;
/// Serialized header width.
inline constexpr foundation::usize HEADER_BYTES = HEADER_FIELD_COUNT * sizeof(foundation::uint32) + 2 * IDENTITY_BYTES;
/// Serialized resource record width; matches Resource's eight fields.
inline constexpr foundation::usize RESOURCE_RECORD_BYTES = 8 * sizeof(foundation::uint32);
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
