#pragma once

#include <ludus/foundation/base/compiler.h>

#include <ludus/content/render_graph_format/scene_data.h>
#include <ludus/foundation/base/types.h>

/// @file
/// Versioned numeric render-plan records shared by offline tools and Renderer.
/// Generated C++ plans use these records directly. A file codec must write each
/// field in little-endian order, never copy host struct bytes. No GPU ownership.
namespace ludus::content::render_graph_format
{
/// Static triangle execution profile: opaque/masked raster with per-draw uniforms,
/// and authored primary triangle ray functions with flat opaque World inputs.
inline constexpr foundation::uint32 VERSION = 7;
/// Maximum authored passes in one admitted configuration.
inline constexpr foundation::usize MAX_PASSES = 16;
/// Maximum logical resources; depth may alias a color allocation's private aspect.
inline constexpr foundation::usize MAX_RESOURCES = 32;
/// Maximum camera inputs per plan; indices are stable within its generation.
inline constexpr foundation::usize MAX_CAMERAS = 8;
/// Largest supported scene extraction in this initial profile.
inline constexpr foundation::usize MAX_ITEMS = 128;
/// Largest supported mesh table in one scene replacement.
inline constexpr foundation::usize MAX_MESHES = 32;
/// Legacy program roles, including the one ray role; their numeric IDs remain stable.
inline constexpr foundation::usize LEGACY_TECHNIQUE_COUNT = 5;
/// Maximum authored techniques, matching the build-manifest configuration bound.
inline constexpr foundation::usize MAX_AUTHORED_TECHNIQUES = 8;
/// Number of additional shader pairs per raster coverage family.
inline constexpr foundation::usize ADDITIONAL_VARIANTS_PER_FAMILY = MAX_AUTHORED_TECHNIQUES - 1;
/// Number of raster stage pairs; ray traversal uses separate compute/image roles.
inline constexpr foundation::usize RASTER_TECHNIQUE_COUNT = 4;
/// Additional raster roles encode family as (role - LEGACY_TECHNIQUE_COUNT) modulo RASTER_TECHNIQUE_COUNT.
inline constexpr foundation::usize ADDITIONAL_RASTER_TECHNIQUE_COUNT =
    RASTER_TECHNIQUE_COUNT * ADDITIONAL_VARIANTS_PER_FAMILY;
/// Bounded shader-pair slots, independent of how many graph passes reuse a pair.
inline constexpr foundation::usize TECHNIQUE_COUNT = LEGACY_TECHNIQUE_COUNT + ADDITIONAL_RASTER_TECHNIQUE_COUNT;
/// Maximum consecutive ray invocations in one configuration.
inline constexpr foundation::usize MAX_RAY_PASSES = 4;
/// Read/write uint4 storage slots per ray invocation; shared across passes and frames.
inline constexpr foundation::usize MAX_RAY_BUFFERS = 2;
/// Byte width of one persistent buffer word, matching RWStructuredBuffer<uint4>.
inline constexpr foundation::uint32 RAY_BUFFER_WORD_BYTES = 16;
/// Maximum logical bytes per RenderSize pixel in shared uint4 storage.
inline constexpr foundation::uint32 MAX_RAY_BUFFER_PIXEL_BYTES = 4096;
/// Maximum bytes in one persistent buffer, fixed or resolved per pixel.
inline constexpr foundation::uint32 MAX_RAY_BUFFER_BYTES = 256 * 1024 * 1024;
/// Number of shader stages (vertex and fragment) of one technique.
inline constexpr foundation::usize TECHNIQUE_STAGE_COUNT = 2;
/// Number of artifact targets a generation may carry.
inline constexpr foundation::usize SHADER_TARGET_COUNT = 4;
/// Largest logical float32 uniform value table supplied by one draw item.
inline constexpr foundation::usize MAX_UNIFORM_VALUES = 64;
/// Largest number of uniform bindings across every technique of one plan.
inline constexpr foundation::usize MAX_UNIFORM_BINDINGS = 16;
/// Largest uniform binding slot plus one; matches the RG1 build profile (slots 0-7).
inline constexpr foundation::usize UNIFORM_SLOT_COUNT = 8;
/// Largest number of per-target block records: one per binding and carried target.
inline constexpr foundation::usize MAX_UNIFORM_BLOCKS = MAX_UNIFORM_BINDINGS * SHADER_TARGET_COUNT;
/// Largest number of per-target field records across every binding.
inline constexpr foundation::usize MAX_UNIFORM_FIELDS = 256;
/// Largest occupied byte size of one uniform block on any target.
inline constexpr foundation::usize MAX_UNIFORM_BLOCK_BYTES = 1024;
/// Largest number of distinct per-draw uniform value sets one technique prepares per extraction.
inline constexpr foundation::usize MAX_UNIFORM_SETS = 16;
/// Number of explicitly serialized uint32 fields in a current pass record.
inline constexpr foundation::usize PASS_FIELD_COUNT = 13;
/// Resource or input index absent from the declared plan.
inline constexpr foundation::uint32 ABSENT = ~foundation::uint32{0};
/// Compiled shader-pair role, independent of graph instance and viewport size.
/// Values below LEGACY_TECHNIQUE_COUNT name the historical pairs. Additional
/// raster pairs occupy [LEGACY_TECHNIQUE_COUNT, TECHNIQUE_COUNT); GetTechniqueFamily
/// returns their coverage family. Numeric plans select these bounded values explicitly.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class Technique : foundation::uint32
{
    ColorAndDepth,       ///< Authored opaque color plus depth stage pair.
    DepthOnly,           ///< Authored opaque void fragment plus identical static geometry path.
    MaskedColorAndDepth, ///< Authored masked color stage pair with its own coverage function.
    MaskedDepthOnly,     ///< Separately authored masked coverage function plus identical masked geometry path.
    RayTracing,          ///< Authored triangle ray generation/miss/closest-hit functions, lowered to GPU BVH traversal.
};
/// Return the coverage family of a bounded compiled shader-pair role; no allocation.
/// @pre The numeric role is less than TECHNIQUE_COUNT.
[[nodiscard]] LUDUS_INLINE constexpr Technique GetTechniqueFamily(Technique technique) noexcept
{
    const foundation::usize index = static_cast<foundation::usize>(technique);
    return index < LEGACY_TECHNIQUE_COUNT
               ? technique
               : static_cast<Technique>((index - LEGACY_TECHNIQUE_COUNT) % RASTER_TECHNIQUE_COUNT);
}
/// Logical image aspect; physical depth is owned by its Backing allocation.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class Aspect : foundation::uint32
{
    Color,  ///< Linear RGBA8 color.
    Depth,  ///< Depth32Float private depth aspect.
    Buffer, ///< Zero-initialized persistent uint4 storage; Width is bytes and Height is one.
};
/// Initialization of the complete attachment, before viewport/scissor clipping.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class Load : foundation::uint32
{
    Clear,   ///< Define the full attachment.
    Load,    ///< Preserve initialized earlier contents.
    Discard, ///< Earlier contents are undefined.
};
/// Contents retained after execution.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class Store : foundation::uint32
{
    Store,   ///< Preserve defined output.
    Discard, ///< Output becomes undefined.
};
/// Artifact representation; also selects the target-specific uniform layout records.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class ShaderTarget : foundation::uint32
{
    Metal,  ///< Metal Shading Language source text.
    Spirv,  ///< SPIR-V words; the byte length is a multiple of four.
    Wgsl,   ///< WGSL source text.
    GlslEs, ///< GLSL ES 3.00 source text.
};
/// Shader stages that read one uniform binding.
// NOLINTNEXTLINE(performance-enum-size): versioned numeric fields have explicit 32-bit widths.
enum class Visibility : foundation::uint32
{
    Vertex = 1,   ///< Read by the technique's vertex stage only.
    Fragment = 2, ///< Read by the technique's fragment stage only.
    Both = 3,     ///< Read by both stages with one identical schema and layout.
};
/// Largest resolved image dimension and scale component admitted by graph authoring.
inline constexpr foundation::uint32 MAX_TARGET_EXTENT = 16384;
/// Logical image requirement. Fixed extents are pixels; relative extents carry
/// nominal offline dimensions plus a rational scale of each view's drawable size.
/// No runtime native handle. Width/Height are resolved per instance before allocation.
struct Resource final
{
    foundation::uint32 Width;               ///< Positive pixel width, or Buffer byte capacity (multiple of 16).
    foundation::uint32 Height;              ///< Positive physical height.
    Aspect Kind;                            ///< Color, depth or persistent raw storage.
    foundation::uint32 Backing;             ///< Index of the color/private-depth allocation owner; may name itself.
    foundation::uint32 IsSurface;           ///< Zero or one; owner is the acquired presentation surface.
    foundation::uint32 IsExported;          ///< Zero or one; output may be leased after producer completion.
    foundation::uint32 ViewNumerator = 0;   ///< Zero selects fixed Width/Height; otherwise scale numerator.
    foundation::uint32 ViewDenominator = 1; ///< Positive scale denominator; both components at most MAX_TARGET_EXTENT.
    foundation::uint32 BytesPerPixel = 0;   ///< Buffer byte stride per RenderSize pixel; zero selects fixed capacity.
};
/// Maximum named structured buffers in one native triangle-ray invocation.
inline constexpr foundation::usize MAX_STORAGE_BUFFERS = 2;
/// First user storage slot; lower slots belong to the renderer's ray interface.
inline constexpr foundation::uint32 FIRST_STORAGE_SLOT = 6;
/// Maximum reflected element stride, in bytes.
inline constexpr foundation::uint32 MAX_STORAGE_STRIDE = 4096;
/// Maximum fixed or per-pixel element count admitted by authoring.
inline constexpr foundation::uint32 MAX_STORAGE_ELEMENTS = 16 * 1024 * 1024;
/// Number of serialized uint32 fields in a structured storage record.
inline constexpr foundation::usize STORAGE_BUFFER_FIELD_COUNT = 4;
/// Graph-owned, zero-initialized structured storage for one ray job. Contents persist
/// across dispatches and reset with scene/camera/extent/generation replacement.
/// Buffers are private to that job; no host upload, export or implicit ping-pong.
struct StorageBuffer final
{
    foundation::uint32 Slot; ///< Unique shader slot in [FIRST_STORAGE_SLOT, FIRST_STORAGE_SLOT + MAX_STORAGE_BUFFERS).
    foundation::uint32 Stride;     ///< Reflected element bytes, positive multiple of four, at most MAX_STORAGE_STRIDE.
    foundation::uint32 Count;      ///< Fixed element count; zero means one element per resolved color-target pixel.
    foundation::uint32 IsReadOnly; ///< One for StructuredBuffer, zero for RWStructuredBuffer.
};
/// Required destination for one active camera, verified independently of pass coverage.
struct Camera final
{
    foundation::uint32 Target; ///< Logical surface color or exported color/depth resource.
};
/// One retained scene raster or triangle ray operation; indices are checked before GPU setup.
struct Pass final
{
    Technique Program;         ///< Explicit authored shader choice.
    foundation::uint32 Camera; ///< Index into the current camera extraction.
    foundation::uint32 Color;  ///< Logical color resource, or ABSENT for a depth-only technique.
    foundation::uint32 Depth;  ///< Logical depth resource; ABSENT for ray passes, which produce only color.
    Load ColorLoad;            ///< For depth-only techniques, declares compatibility color initialization.
    Store ColorStore;          ///< For depth-only techniques, controls compatibility allocation color retention.
    Load DepthLoad;            ///< Depth initialization.
    Store DepthStore;          ///< Depth validity after this operation.
    foundation::uint32 AllowsRepeatedCoverage; ///< Zero or one; explicitly permits repeated rendering in a role/view.
    /// Selected SCENE_DATA_* groups. Geometry is mandatory; default preserves full-scene callers.
    foundation::uint32 SceneData = SCENE_DATA_ALL;
    foundation::uint32 RayProgram = 0;   ///< Callback-set selector in the cooked ray kernel; zero for raster.
    foundation::uint32 Buffer0 = ABSENT; ///< First persistent storage resource, or ABSENT for a private dummy.
    foundation::uint32 Buffer1 = ABSENT; ///< Second persistent storage resource, or ABSENT for a private dummy.
};
/// One read-only uniform binding of a technique, filled per draw from logical values.
/// Its values occupy [ValueOffset, ValueOffset + ValueCount) of each item's float32
/// table; bindings of different techniques that use one schema share that span.
struct UniformBinding final
{
    Technique Program;              ///< Technique whose stages read the binding.
    foundation::uint32 Slot;        ///< Group/set-zero binding number, below UNIFORM_SLOT_COUNT.
    Visibility Stages;              ///< Stages that declare the binding.
    foundation::uint32 ValueOffset; ///< First logical float32 value of its schema.
    foundation::uint32 ValueCount;  ///< Positive logical float32 value count of its schema.
};
/// Occupied byte size of one binding on one carried target, from target reflection.
struct UniformBlock final
{
    foundation::uint32 Binding; ///< Index into the plan's uniform binding table.
    ShaderTarget Target;        ///< Target whose emitted layout this describes.
    foundation::uint32 Size;    ///< Positive occupied bytes, at most MAX_UNIFORM_BLOCK_BYTES.
};
/// Target byte offset of one schema field; fields of a block appear in schema order.
struct UniformField final
{
    foundation::uint32 Binding;     ///< Index into the plan's uniform binding table.
    ShaderTarget Target;            ///< Target whose emitted layout this describes.
    foundation::uint32 ValueOffset; ///< First logical value, relative to the binding's ValueOffset.
    foundation::uint32 Components;  ///< Float32 component count, 1 to 4.
    foundation::uint32 ByteOffset;  ///< Four-byte aligned offset inside the target block.
};
/// Header for a generated configuration; limits include unexecuted root effects.
struct Header final
{
    foundation::uint32 Version;             ///< VERSION.
    foundation::uint32 ResourceCount;       ///< Bounded resource table size.
    foundation::uint32 PassCount;           ///< Bounded scene operation count.
    foundation::uint32 CameraCount;         ///< Required current camera input count.
    foundation::uint32 UniformValueCount;   ///< Logical float32 values each drawn item supplies; zero when unbound.
    foundation::uint32 UniformBindingCount; ///< Bounded uniform binding table size.
    foundation::uint32 UniformBlockCount;   ///< Bounded per-target block table size.
    foundation::uint32 UniformFieldCount;   ///< Bounded per-target field table size.
};
static_assert(sizeof(StorageBuffer) == sizeof(foundation::uint32) * STORAGE_BUFFER_FIELD_COUNT);
/// Number of explicitly serialized resource fields in the current runtime layout.
inline constexpr foundation::usize RESOURCE_FIELD_COUNT = 9;
static_assert(sizeof(Resource) == sizeof(foundation::uint32) * RESOURCE_FIELD_COUNT);
static_assert(sizeof(Pass) == sizeof(foundation::uint32) * PASS_FIELD_COUNT);
static_assert(sizeof(Camera) == sizeof(foundation::uint32));
static_assert(sizeof(UniformBinding) == sizeof(foundation::uint32) * 5);
static_assert(sizeof(UniformBlock) == sizeof(foundation::uint32) * 3);
static_assert(sizeof(UniformField) == sizeof(foundation::uint32) * 5);
static_assert(sizeof(Header) == sizeof(foundation::uint32) * 8);
} // namespace ludus::content::render_graph_format
