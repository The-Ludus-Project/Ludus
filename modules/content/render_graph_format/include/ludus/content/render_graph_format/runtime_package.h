#pragma once

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
namespace ludus::content::render_graph_format
{
/// "LURG" read as a little-endian uint32.
inline constexpr foundation::uint32 PACKAGE_MAGIC = 0x4752554CU;
/// Runtime generation layout version.
inline constexpr foundation::uint32 PACKAGE_VERSION = 4;
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
inline constexpr foundation::usize RAY_STAGE_ROLE_COUNT = 4;
/// Raster stage pairs plus admitted ray-stage roles.
inline constexpr foundation::usize STAGE_ROLE_COUNT =
    RASTER_TECHNIQUE_COUNT * TECHNIQUE_STAGE_COUNT + RAY_STAGE_ROLE_COUNT;
/// Largest stage table: one artifact per role and target.
inline constexpr foundation::usize MAX_STAGES = STAGE_ROLE_COUNT * SHADER_TARGET_COUNT;
/// Renderer static vertex/instance interface: position plus four transform rows and color.
inline constexpr foundation::usize STATIC_INPUT_COUNT = 6;
/// Fixed uint32 fields before the identities.
inline constexpr foundation::usize HEADER_FIELD_COUNT = 10;
/// Serialized header width.
inline constexpr foundation::usize HEADER_BYTES = HEADER_FIELD_COUNT * sizeof(foundation::uint32) + 2 * IDENTITY_BYTES;
/// Serialized resource record width; matches Resource's eight fields.
inline constexpr foundation::usize RESOURCE_RECORD_BYTES = 8 * sizeof(foundation::uint32);
/// Serialized camera record width; matches Camera's one field.
inline constexpr foundation::usize CAMERA_RECORD_BYTES = sizeof(foundation::uint32);
/// Serialized pass record width; matches Pass's nine fields.
inline constexpr foundation::usize PASS_RECORD_BYTES = 9 * sizeof(foundation::uint32);
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
