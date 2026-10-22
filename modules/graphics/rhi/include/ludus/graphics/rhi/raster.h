#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/profile.h>
#include <ludus/graphics/rhi/render.h>

#include <span>
#include <string_view>

namespace ludus::graphics::rhi
{
namespace internal
{
/// Private access to portable resource identities; not an application API.
struct RasterAccess;
} // namespace internal
/// Immutable TLAS and copied opacity table, including offscreen instances.
/// Copies borrow identity; query batches and compute binding snapshots retain the
/// exact TLAS and its BLAS versions until their accepted GPU uses complete.
struct RaySceneHandle final
{
private:
    ludus::foundation::uint64 mOwner = 0;
    ludus::foundation::uint64 mGeneration = 0;
    ludus::foundation::uint32 mSlot = 0;
    /// Private registry access; applications cannot construct resource identities.
    friend struct internal::RasterAccess;
};
/// Opaque buffer identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct BufferHandle final
{
    /// Compare device, generation and slot identity; this does not query resource readiness.
    friend LUDUS_INLINE constexpr bool operator==(BufferHandle, BufferHandle) noexcept = default;

private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque texture allocation identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct TextureHandle final
{
    /// Compare device, generation and slot identity; this does not query resource readiness.
    friend LUDUS_INLINE constexpr bool operator==(TextureHandle, TextureHandle) noexcept = default;

private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque immutable sampled texture view identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct TextureViewHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque sampler identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct SamplerHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque reflected shader stage identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct RasterShaderHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque binding layout identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct BindingLayoutHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque immutable binding snapshot identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct BindingSetHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Opaque immutable graphics pipeline identity, scoped to a device, slot and non-wrapping generation.
/// Copies do not transfer ownership. Destroy invalidates resolution immediately;
/// binding/pipeline and submitted uses keep the underlying record alive.
struct RasterPipelineHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the private registry access to assign and validate this incarnation.
    friend struct internal::RasterAccess;
};
/// Explicit portable-resource result. Only Ready/Pending publish ownership.
enum class RasterStatus : ludus::foundation::uint8
{
    /// Operation completed, or an asynchronous resource is usable.
    Ready,
    /// Output owns a resource awaiting backend validation; poll GetStatus.
    Pending,
    /// Device startup or a dependency is pending; retry after it becomes ready.
    NotReady,
    /// Backend validation or creation failed; a published resource must be destroyed.
    Failed,
    /// Null, stale, foreign-owner or wrong-incarnation handle.
    InvalidHandle,
    /// Operation conflicts with an open frame or an already-owned output.
    InvalidState,
    /// Malformed description, range, packing or layout mismatch.
    InvalidDescription,
    /// Requested format or operation lies outside the enabled raster profile.
    Unsupported,
    /// All bounded records are live or retained; no reservation was published.
    CapacityExceeded,
    /// An owner or slot generation cannot advance without wrapping.
    IdentityExhausted,
    /// Native allocation failed; no output ownership was published.
    OutOfMemory,
    /// The device was lost; recreate resources in a new session.
    DeviceLost,
};
/// Buffer creation role, fixed for its lifetime. Storage roles permit graph compute writes.
enum class BufferRole : ludus::foundation::uint8
{
    /// Per-vertex or per-instance attributes.
    Vertex,
    /// Mesh-local unsigned 16-bit indices.
    Index16,
    /// Mesh-local unsigned 32-bit indices.
    Index32,
    /// Uniform bytes with independently verified shader packing.
    Uniform,
    /// Mutable compute storage, initialized by complete creation bytes.
    Storage,
    /// Mutable compute storage also consumed as vertex/instance records.
    StorageVertex,
    /// Mutable compute storage also consumed as one indexed indirect command.
    StorageIndirect,
};
/// Fixed buffer allocation/role; complete initial bytes are consumed before return.
/// R8 RequestBufferUpdate permits completion-gated content changes; allocation size/role never change.
struct BufferDescription final
{
    /// Legal use of the created buffer.
    BufferRole Role = BufferRole::Vertex;
    /// Allocation and initial-content length in bytes; nonzero and bounded by capabilities.
    ludus::foundation::usize Size = 0;
};
/// Original-format sampled allocation or color attachment with private depth/stencil.
struct TextureDescription final
{
    /// Pixel width, nonzero and bounded by enabled limits.
    ludus::foundation::uint32 Width = 0;
    /// Pixel height, nonzero and bounded by enabled limits.
    ludus::foundation::uint32 Height = 0;
    /// Sampling conversion; no automatic upload conversion occurs.
    RasterFormat Format = RasterFormat::Rgba8Unorm;
    /// Allocate undefined color and private depth attachments instead of uploading bytes.
    /// Attachment textures remain sampleable; clear/store defines color contents.
    bool Attachment = false;
    /// Allocated mips, at most floor(log2(largest spatial extent))+1 and sixteen.
    /// D3 includes depth in that extent; array layers and cube face counts do not.
    /// Single-sample attachments allow mip chains; initial uploads define all unless DeferredInitialization.
    /// Each sampled view requires initialized contents only in its selected range.
    ludus::foundation::uint32 MipLevels = 1;
    /// Request nonblocking native upload readiness; Pending owns staging until completion.
    /// Legacy RGBA single-level creation defaults to its existing setup behavior.
    bool AsyncUpload = false;
    /// Opt-in copied CPU texels for fixed-mip reference shading. Uploaded R8/RG8/RGBA8
    /// only; attachments and RGBA16Float reject. Ordinary textures allocate no shadow.
    bool RetainCpuContent = false;
    /// Allocate undefined RGBA8Unorm sampled mips with an empty upload. R8 full-mip
    /// transfers initialize incrementally; full-resource sampled views require every mip.
    /// Attachments, non-D2 dimensions and retained CPU content reject this option.
    bool DeferredInitialization = false;
    /// Allocation dimension, fixed for its lifetime. Attachments exclude D3; CPU shadows require D2.
    TextureDimension Dimension = TextureDimension::D2;
    /// Array layers, six cube faces, or base volume depth; D2 requires one.
    ludus::foundation::uint32 DepthOrLayers = 1;
    /// Attachment sample count: one or capability-enabled four; uploads require one.
    /// Four samples require D2, one mip and one layer, and cannot be sampled directly.
    ludus::foundation::uint32 SampleCount = 1;
    /// Allocate a private depth/stencil image instead of depth alone; attachments only.
    /// Stencil support is independent of depth sampling and MSAA.
    bool Stencil = false;
};
/// Sampled color or private depth/stencil aspect; capability-gated independently.
enum class TextureAspect : ludus::foundation::uint8
{
    /// Color/data channels, including normalized and compressed formats.
    Color,
    /// Private depth component; scalar float loads, without comparison/filtering.
    Depth,
    /// Private stencil component; scalar unsigned integer loads when enabled.
    Stencil,
};
/// Immutable original-format sampled aspect or single-subresource writable attachment view.
/// Mip zero in a shader denotes BaseMip.
/// Zero counts select the remaining allocation range. D3 requires BaseLayer=0,
/// LayerCount=0 or 1; its volume slices are not array layers. No format reinterpretation.
struct TextureViewDescription final
{
    /// Shader-visible dimension; must match the allocation, except a single array/cube layer may use D2.
    TextureDimension Dimension = TextureDimension::D2;
    /// First allocated mip exposed to sampling.
    ludus::foundation::uint32 BaseMip = 0;
    /// Number of exposed mips; zero selects all remaining levels.
    ludus::foundation::uint32 MipCount = 0;
    /// First exposed array layer or cube face.
    ludus::foundation::uint32 BaseLayer = 0;
    /// Number of exposed layers; zero selects all remaining layers.
    ludus::foundation::uint32 LayerCount = 0;
    /// Original color or private depth/stencil aspect; depth/stencil require a D2 view.
    TextureAspect Aspect = TextureAspect::Color;
    /// Select a writable attachment view rather than a shader binding. Color only,
    /// exactly one mip/layer and D2; retains both color and its private depth/stencil.
    /// Undefined contents are legal until a pass clears them. Not bindable in a set.
    bool Attachment = false;
};
/// One complete initialized mip within the borrowed upload byte blob.
struct TextureMipUpload final
{
    /// Four-byte-aligned offset; levels must be tightly concatenated in ascending order.
    ludus::foundation::usize Offset = 0;
    /// Encoded bytes between rows or block rows; multiple of four and of the format block size.
    ludus::foundation::usize RowPitch = 0;
};
/// Borrowed complete original-format chain, consumed/copied before return.
/// Mips are concatenated in ascending order; each contains all layers/faces or
/// volume slices in ascending order. A slice has RowPitch * block-row-count bytes.
/// Compressed pitches count encoded block bytes, with rounded-up edge blocks.
struct TextureUpload final
{
    /// Rows in top-left image order, including optional padding between rows.
    std::span<const ludus::foundation::uint8> Bytes;
    /// Encoded row/block-row pitch, at least ceil(Width/blockWidth) * blockBytes.
    /// Must be a multiple of four and of the format block byte count (eight for HDR/BC1, sixteen for BC3/BC5).
    ludus::foundation::usize RowPitch = 0;
    /// Empty selects the legacy single-mip RowPitch. Otherwise exactly MipLevels
    /// descriptors; RowPitch must be zero. All metadata is consumed before return.
    // Explicit default preserves two-field aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::span<const TextureMipUpload> Mips{};
};
/// Borrowed immutable initialized texels of one mip, with original row padding.
/// Valid only while the queried texture/view incarnation remains retained. Copy
/// before releasing that owner; no GPU readback or implicit residency fallback.
struct TextureCpuContent final
{
    /// Borrowed complete rows for the selected mip.
    const ludus::foundation::uint8* Bytes = nullptr;
    /// Number of initialized bytes including row padding.
    ludus::foundation::usize Size = 0;
    /// Bytes between top-left-oriented rows.
    ludus::foundation::usize RowPitch = 0;
    /// Selected mip width in pixels.
    ludus::foundation::uint32 Width = 0;
    /// Selected mip height in pixels.
    ludus::foundation::uint32 Height = 0;
    /// Exact source format; decoding remains the consumer's declared shading policy.
    RasterFormat Format = RasterFormat::Rgba8Unorm;
};
/// Query an explicitly retained CPU shadow. No allocation/wait; failure preserves
/// output. Missing shadow returns Unsupported, pending upload NotReady, bad mip
/// InvalidDescription. Owner-thread only. The texture must remain retained.
[[nodiscard]] RasterStatus
GetTextureCpuContent(DeviceHandle, TextureHandle, ludus::foundation::uint32 mip, TextureCpuContent&) noexcept;
/// Query through a retained sampled view, including after its source texture owner
/// was destroyed. mip is relative to the view (zero selects BaseMip), and must
/// be less than its MipCount. Same readiness/failure rules; keep the view retained.
[[nodiscard]] RasterStatus
GetTextureCpuContent(DeviceHandle, TextureViewHandle, ludus::foundation::uint32 mip, TextureCpuContent&) noexcept;
/// Portable within-level minification/magnification filter; mip interpolation is separate.
enum class RasterFilter : ludus::foundation::uint8
{
    /// Select one texel.
    Nearest,
    /// Interpolate adjacent texels in linear light.
    Linear,
};
/// Portable address mode applied independently along each sampled texture axis.
enum class RasterAddress : ludus::foundation::uint8
{
    /// Clamp coordinates to edge texels.
    Clamp,
    /// Repeat the texture at integer coordinate boundaries.
    Repeat,
};
/// Independent minification selection across initialized mip levels.
enum class RasterMipFilter : ludus::foundation::uint8
{
    /// Clamp to level zero; preserves the legacy sampling profile.
    None,
    /// Select the nearest level.
    Nearest,
    /// Interpolate neighboring levels.
    Linear,
};
/// Immutable sampler state; anisotropy and comparison sampling are unsupported.
struct SamplerDescription final
{
    /// Magnification and minification filtering.
    RasterFilter Filter = RasterFilter::Nearest;
    /// Horizontal addressing.
    RasterAddress U = RasterAddress::Clamp;
    /// Vertical addressing.
    RasterAddress V = RasterAddress::Clamp;
    /// Mip selection; defaults to the legacy level-zero sampler.
    RasterMipFilter MipFilter = RasterMipFilter::None;
    /// Finite minimum LOD, 0..15; None requires zero.
    ludus::foundation::float32 MinLod = 0;
    /// Finite maximum LOD, MinLod..15; clamped to initialized texture levels by sampling.
    ludus::foundation::float32 MaxLod = 15;
    /// Volume-depth addressing; cube face selection is defined by direction vectors.
    RasterAddress W = RasterAddress::Clamp;
};
/// Resource kind reflected at a binding in group/set zero.
enum class RasterBindingKind : ludus::foundation::uint8
{
    /// Read-only uniform buffer range.
    UniformBuffer,
    /// Non-multisampled, float-sampled two-dimensional texture.
    Texture2D,
    /// Float-sampled fixed array of two-dimensional images.
    Texture2DArray,
    /// Float-sampled six-face cubemap.
    TextureCube,
    /// Float-sampled three-dimensional volume.
    Texture3D,
    /// Scalar float, unfiltered loads from a D2 private depth aspect; no sampler binding.
    TextureDepth2D,
    /// Scalar unsigned integer loads from a D2 private stencil aspect; no sampler binding.
    TextureStencil2D,
    /// Non-comparison sampler.
    Sampler,
    /// Read-only compute storage buffer range.
    StorageRead,
    /// Read/write compute storage buffer range; shader write races remain kernel obligations.
    StorageReadWrite,
    /// Immutable, fully built native TLAS; compute-stage ray queries only.
    RayScene,
};
/// Shader stage visibility mask.
enum class RasterVisibility : ludus::foundation::uint8
{
    /// Visible to the vertex stage only.
    Vertex = 1,
    /// Visible to the fragment stage only.
    Fragment = 2,
    /// Visible to both raster stages.
    Both = 3,
    /// Compute stage only; no raster/storage writes in this profile.
    Compute = 4,
};
/// Descriptor selection admitted by a reflected shader; array length alone grants no indexing mode.
enum class RasterBindingIndexing : ludus::foundation::uint8
{
    /// Constant array indices only; does not require dynamic descriptor indexing.
    Constant,
    /// A dynamically uniform index; native support is separately negotiated.
    Uniform,
    /// Per-invocation indices; requires explicitly requested optional bindless support.
    NonUniform,
};
/// One immutable layout entry; binding numbers must be unique and below the enabled Interface.MaxBindings.
struct RasterBinding final
{
    /// Group/set-zero binding number.
    ludus::foundation::uint32 Binding = 0;
    /// Resource type; storage textures remain outside the binding profile.
    RasterBindingKind Kind = RasterBindingKind::UniformBuffer;
    /// Stages allowed to use this entry.
    RasterVisibility Visibility = RasterVisibility::Both;
    /// Minimum occupied uniform bytes or storage element stride from target reflection;
    /// zero for textures/samplers/ray scenes. Storage ranges contain at least one element.
    ludus::foundation::usize MinSize = 0;
    /// Add the packet offset for this binding to the snapshot buffer offset.
    /// Buffer bindings only; alignment and the final range are checked before encoding.
    /// This layout property does not change shader reflection or buffer ownership.
    bool HasDynamicOffset = false;
    /// Fixed descriptor count, or the allocation ceiling for a runtime-sized binding.
    /// Must be positive; array kinds and counts obey the enabled binding profile.
    ludus::foundation::uint32 Count = 1;
    /// Required shader selection mode, independently verified by target reflection.
    RasterBindingIndexing Indexing = RasterBindingIndexing::Constant;
    /// Unsized shader array. Layout Count bounds allocation; shader Count is the
    /// verified minimum allocation required by its accesses. Vulkan-only, sampled
    /// color textures at RUNTIME_BINDING_ARRAY_SLOT. RUNTIME_BINDING_LENGTH_SLOT
    /// is reserved for an immutable RHI-owned length buffer, created per snapshot.
    bool IsRuntimeSized = false;
    /// Permit omitted array elements, with element zero required as a fallback.
    /// Holes sample/bind that retained fallback and cannot produce host index tokens.
    /// Allocation and descriptor budgets charge the complete table, including holes.
    bool HasPartialBinding = false;
};
/// Byte offsets indexed by group-zero binding number, copied into each accepted packet.
/// Non-dynamic and absent bindings require zero. Values are added to the snapshot
/// offset with checked arithmetic; the bound Size remains unchanged. Owner-thread only.
struct RasterBindingOffsets final
{
    /// Additional byte offset for each implemented binding number; unused entries must be zero.
    ludus::foundation::StaticArray<ludus::foundation::uint32, RASTER_BINDING_COUNT> Bytes{};
};
/// Portable float vertex attribute width.
enum class RasterVertexFormat : ludus::foundation::uint8
{
    /// Two IEEE binary32 components.
    Float2,
    /// Three IEEE binary32 components.
    Float3,
    /// Four IEEE binary32 components.
    Float4,
};
/// One reflected vertex-stage input; system-value inputs are excluded.
struct RasterShaderInput final
{
    /// Vertex attribute location, below eight.
    ludus::foundation::uint32 Location = 0;
    /// Exact float component count expected by the selected target artifact.
    RasterVertexFormat Format = RasterVertexFormat::Float3;
};
/// Bounded authored uniform shapes, independently reflected for each target.
/// Matrix order/array stride remain properties of the cooked target, not CPU math types.
enum class RasterUniformShape : ludus::foundation::uint8
{
    /// Unclassified shape; legacy/other schemas cannot satisfy a typed material ABI.
    Opaque,
    /// One IEEE binary32 value.
    Float32,
    /// Two IEEE binary32 components.
    Float2,
    /// Three IEEE binary32 components.
    Float3,
    /// Four IEEE binary32 components.
    Float4,
    /// Four by four IEEE binary32 matrix.
    Float4x4,
    /// Bounded fixed array of Float4 values.
    Float4Array,
    /// Bounded fixed array of Float4x4 values.
    Float4x4Array,
};
/// One independently reflected top-level uniform member offset and authored shape.
struct RasterUniformMember final
{
    /// Group-zero uniform binding containing the member.
    ludus::foundation::uint32 Binding = 0;
    /// Copied/consumed schema name, nonempty and at most 63 bytes.
    std::string_view Name;
    /// Byte offset inside the binding's occupied range, multiple of four.
    ludus::foundation::usize Offset = 0;
    /// Cooked authored numeric shape; Opaque retains legacy admission but rejects typed material ABI.
    RasterUniformShape Shape = RasterUniformShape::Opaque;
};
/// Per-target interface metadata emitted by the offline shader tool.
/// Hand-authored artifacts must supply independently verified matching reflection.
struct RasterShaderDescription final
{

    /// Target artifact and explicit entry; UniformSize is unused by the raster API.
    ShaderDescription Artifact{};
    /// Reflected float vertex inputs; empty for fragment stages. Copied before return.
    std::span<const RasterShaderInput> Inputs;
    /// Binding requirements for this stage; copied before return.
    std::span<const RasterBinding> Bindings;
    /// GLSL ES uniform block names, indexed by binding number; empty for other kinds.
    ludus::foundation::StaticArray<std::string_view, RASTER_BINDING_COUNT> UniformBlocks{};
    /// Combined GLSL ES sampled-uniform names, indexed by texture binding number.
    ludus::foundation::StaticArray<std::string_view, RASTER_BINDING_COUNT> TextureNames{};
    /// Sampler binding paired with each GLSL ES texture; ~uint32{0} denotes an unfiltered depth/stencil load.
    /// Cooked browser descriptions retain this metadata for target selection.
    ludus::foundation::StaticArray<ludus::foundation::uint32, RASTER_BINDING_COUNT> TextureSamplers{};
    /// Independently reflected local dimensions for Compute; ignored for raster.
    /// Positive dimensions/product must fit the negotiated compute limits.
    ludus::foundation::uint32 WorkgroupSize[3]{1, 1, 1};
    /// At most 64 unique (binding,name) top-level offsets. Empty preserves legacy
    /// artifacts; schema-aware consumers require a complete expected member set.
    /// Consumed before shader creation returns; never retained as borrowed names.
    // Explicit default preserves existing aggregate callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::span<const RasterUniformMember> UniformMembers{};
    /// The native array cooker verified every descriptor index as an in-range
    /// constant or bounded unsigned min/clamp in the emitted target. Required for
    /// shaders declaring arrays. Runtime shader Count certifies the minimum safe
    /// allocation. Hand-authored artifacts must independently verify this contract;
    /// false rejects before shader creation. Fixed counts must match exactly.
    bool HasVerifiedArrayBounds = false;
    /// Native development preparation copies sources and compiles on a dedicated worker.
    /// Pending owns the handle; poll GetStatus. Browser keeps its existing backend policy.
    bool ShouldPrepareAsynchronously = false;
};
/// One immutable binding snapshot entry; exactly the resource matching Kind is used.
struct RasterBindingResource final
{
    /// Layout binding number.
    ludus::foundation::uint32 Binding = 0;
    /// Uniform/storage buffer identity; ignored for texture/sampler entries.
    BufferHandle Buffer{};
    /// Buffer byte offset, aligned to the enabled uniform/storage range alignment.
    ludus::foundation::usize Offset = 0;
    /// Buffer byte range, nonzero and at least the layout's reflected minimum.
    ludus::foundation::usize Size = 0;
    /// Sampled view identity of the layout's exact dimension/aspect; writable views reject.
    TextureViewHandle Texture{};
    /// Sampler identity; ignored for other entries.
    SamplerHandle Sampler{};
    /// Zero-based descriptor array element; scalar entries require zero.
    ludus::foundation::uint32 ArrayElement = 0;
    /// Ready native ray scene, retained by the snapshot through all accepted GPU uses.
    /// RayScene entries require zero Offset/Size and no descriptor array or dynamic offset.
    RaySceneHandle Scene{};
};
/// Borrowed generation-checked descriptor index belonging to one immutable snapshot.
/// Copying does not retain the set. Resolve through GetBindingArrayElement before
/// publishing shader input; replacement snapshots never accept indices from older sets.
struct RasterBindingIndex final
{
private:
    BindingSetHandle mSet{};
    ludus::foundation::uint32 mBinding = 0;
    ludus::foundation::uint32 mElement = 0;
    /// Allows the shared registry to construct and validate this borrowed index.
    friend struct internal::RasterAccess;
};
/// One immutable vertex attribute; locations must be unique and below MaxVertexAttributes.
struct RasterVertexAttribute final
{
    /// Shader input location.
    ludus::foundation::uint32 Location = 0;
    /// Vertex stream index, zero or one.
    ludus::foundation::uint32 Stream = 0;
    /// Byte offset within a record, multiple of four.
    ludus::foundation::uint32 Offset = 0;
    /// Component count/type.
    RasterVertexFormat Format = RasterVertexFormat::Float3;
};
/// Immutable vertex-stream layout.
struct RasterVertexStream final
{
    /// Bytes between records, nonzero, multiple of four, at most 256.
    ludus::foundation::uint32 Stride = 0;
    /// False advances per vertex; true advances per instance with divisor one.
    bool PerInstance = false;
};
/// Color attachment compatibility baked into an immutable raster pipeline.
enum class RasterTarget : ludus::foundation::uint8
{
    /// Acquired session target's negotiated format.
    Surface,
    /// Linear RGBA8 offscreen color attachment.
    Rgba8Unorm,
    /// sRGB RGBA8 offscreen color attachment.
    Rgba8Srgb,
    /// Linear RGBA16F attachment; requires RasterCapabilities::HdrColor.
    Rgba16Float,
};
/// Canonical [0,1] depth comparison; reversal is explicit rather than inferred from a matrix.
enum class RasterDepthCompare : ludus::foundation::uint8
{
    /// Pass when incoming depth is strictly smaller (conventional depth).
    Less,
    /// Pass on smaller or equal depth.
    LessEqual,
    /// Pass when incoming depth is strictly larger (reverse-Z).
    Greater,
    /// Pass on larger or equal depth.
    GreaterEqual,
    /// Accept every depth value; DepthWrite still controls writes.
    Always,
};
/// Portable stencil comparison against the masked reference and stored value.
enum class RasterStencilCompare : ludus::foundation::uint8
{
    /// Never pass.
    Never,
    /// Reference is smaller than the stored value.
    Less,
    /// Reference equals the stored value.
    Equal,
    /// Reference is smaller than or equal to the stored value.
    LessEqual,
    /// Reference is greater than the stored value.
    Greater,
    /// Reference differs from the stored value.
    NotEqual,
    /// Reference is greater than or equal to the stored value.
    GreaterEqual,
    /// Always pass.
    Always,
};
/// Operation on an eight-bit stencil value, after masking by WriteMask.
enum class RasterStencilOperation : ludus::foundation::uint8
{
    /// Preserve the stored value.
    Keep,
    /// Write zero.
    Zero,
    /// Write Reference.
    Replace,
    /// Increment, saturating at 255.
    IncrementClamp,
    /// Decrement, saturating at zero.
    DecrementClamp,
    /// Invert all eight bits.
    Invert,
    /// Increment modulo 256.
    IncrementWrap,
    /// Decrement modulo 256.
    DecrementWrap,
};
/// Immutable per-face stencil state. Front/back share masks and reference in a pipeline.
struct RasterStencilFace final
{
    /// Masked comparison.
    RasterStencilCompare Compare = RasterStencilCompare::Always;
    /// Stencil comparison failed.
    RasterStencilOperation Fail = RasterStencilOperation::Keep;
    /// Stencil passed but depth failed.
    RasterStencilOperation DepthFail = RasterStencilOperation::Keep;
    /// Both enabled tests passed.
    RasterStencilOperation Pass = RasterStencilOperation::Keep;
};
/// Immutable triangle-list pipeline, independent of particular buffers or bindings.
/// Target selects the negotiated surface or an explicit offscreen color format,
/// with private depth/stencil. Explicit ColorFormats and SampleCount select R10 offscreen
/// compatibility. Dimensions are dynamic; format changes need
/// a compatible pipeline. Depth uses canonical [0,1] coordinates.
struct RasterPipelineDescription final
{
    /// Ready vertex stage retained by the pipeline.
    RasterShaderHandle Vertex{};
    /// Ready fragment stage retained by the pipeline.
    RasterShaderHandle Fragment{};
    /// Exact immutable binding-layout identity retained by the pipeline.
    BindingLayoutHandle Layout{};
    /// Up to two streams; copied before return.
    std::span<const RasterVertexStream> Streams;
    /// Up to MaxVertexAttributes (at most sixteen) attributes; copied before return.
    std::span<const RasterVertexAttribute> Attributes;
    /// Enable the explicit comparison and optional depth writes; false preserves depth.
    bool Depth = false;
    /// Enable premultiplied-alpha blending in the target's linear domain.
    bool Blend = false;
    /// Color format compatibility; dimensions are dynamic and not part of the key.
    RasterTarget Target = RasterTarget::Surface;
    /// When Depth is enabled, false tests depth without modifying it.
    bool DepthWrite = true;
    /// Comparison when Depth is enabled; defaults preserve R1 conventional depth.
    RasterDepthCompare DepthCompare = RasterDepthCompare::Less;
    /// Explicit offscreen color format sequence, one to four; empty uses Target.
    /// Copied before return. Every format and sample count must be enabled.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::span<const RasterFormat> ColorFormats{};
    /// One or enabled four samples; surface pipelines require one.
    ludus::foundation::uint32 SampleCount = 1;
    /// Require a depth/stencil attachment even when the stencil write mask is zero.
    bool Stencil = false;
    /// Stencil behavior for counter-clockwise triangles in canonical clip-space XY.
    RasterStencilFace StencilFront{};
    /// Back-facing triangle stencil behavior.
    RasterStencilFace StencilBack{};
    /// Eight-bit comparison mask shared by both faces.
    ludus::foundation::uint32 StencilReadMask = 255;
    /// Eight-bit write mask; zero permits a read-only stencil pass.
    ludus::foundation::uint32 StencilWriteMask = 255;
    /// Eight-bit reference, set by each accepted draw.
    ludus::foundation::uint32 StencilReference = 0;
    /// Queue native driver creation; also enabled by either asynchronous shader input.
    bool ShouldPrepareAsynchronously = false;
};
/// One vertex-stream slice, borrowed during DrawIndexed and retained through submission.
struct RasterVertexSlice final
{
    /// Vertex-role buffer identity.
    BufferHandle Buffer{};
    /// Byte offset, multiple of four.
    ludus::foundation::usize Offset = 0;
    /// Byte length of the accessible slice; no implicit access to the remaining buffer.
    ludus::foundation::usize Size = 0;
};
/// Complete indexed/instanced draw packet. First instance and base vertex are zero.
/// Validation precedes native calls; previous accepted draws are not rolled back
/// by a later rejected packet. Mesh-local indices must address VertexCount records.
struct RasterDraw final
{
    /// Ready immutable pipeline.
    RasterPipelineHandle Pipeline{};
    /// Ready snapshot with the exact pipeline layout.
    BindingSetHandle Bindings{};
    /// One slice for every pipeline stream, in stream order.
    std::span<const RasterVertexSlice> Vertices;
    /// Index16/Index32 buffer.
    BufferHandle Indices{};
    /// Byte offset into the index buffer, aligned to its element width.
    ludus::foundation::usize IndexOffset = 0;
    /// Number of indices, positive and a multiple of three.
    ludus::foundation::uint32 IndexCount = 0;
    /// Number of accessible vertex records; each index is checked at buffer creation/draw.
    ludus::foundation::uint32 VertexCount = 0;
    /// Number of instances, positive; instance streams need this many records.
    ludus::foundation::uint32 InstanceCount = 1;
    /// Optional StorageIndirect buffer. Non-null selects one indirect command;
    /// counts above are conservative bounds, not CPU copies of GPU arguments.
    BufferHandle Indirect{};
    /// Four-byte-aligned command offset, covering IndexedIndirectArguments.
    ludus::foundation::usize IndirectOffset = 0;
    /// Per-binding additional buffer offsets; copied for batches/graphs and submission.
    RasterBindingOffsets BindingOffsets{};
    /// Exhaustive reachable index declarations for every NonUniform array element.
    /// Tokens must belong to Bindings, be unique and cover every populated element
    /// of each NonUniform table. Fallback holes require no token.
    /// Consumed/validated before admission; GDI additionally requires all corresponding
    /// sampled uses. Empty is valid when the layout has no NonUniform arrays.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::span<const RasterBindingIndex> ReachableBindings{};
};
/// Query the enabled raster profile. Invalid owner preserves output; non-ready devices
/// return their state and zero the capability value. All operations are owner-thread only.
[[nodiscard]] RasterStatus GetRasterCapabilities(DeviceHandle, RasterCapabilities&) noexcept;
/// Query one known format's enabled uses and limits on the owner thread, including open frames.
/// Invalid owner or malformed format preserves output. Non-ready devices zero it;
/// valid but unavailable formats return Unsupported with zero output. No allocation or GPU wait.
[[nodiscard]] RasterStatus GetFormatCapabilities(DeviceHandle, RasterFormat, FormatCapabilities&) noexcept;
/// Create a buffer from exactly Size initial bytes; creation is forbidden in an open frame.
/// Failure preserves a null output. Index content is copied into bounded engine storage
/// for draw-range checks; vertex/uniform input is consumed by the backend before return.
[[nodiscard]] RasterStatus
CreateBuffer(DeviceHandle, const BufferDescription&, std::span<const ludus::foundation::uint8>, BufferHandle&) noexcept;
/// Create a sampled allocation from a complete original-format mip/slice chain, including enabled HDR/BC.
/// The borrowed blob must not exceed RasterCapabilities::MaxBufferSize; all logical backing consumes
/// StorageLimits::ResourceBytes. Empty upload/zero pitch selects an attachment or deferred RGBA8 D2
/// allocation. Owner-thread only, between frames. Ready/Pending publishes ownership and consumes
/// caller bytes/metadata before return; other results preserve output. Poll Pending with GetStatus.
[[nodiscard]] RasterStatus
CreateTexture(DeviceHandle, const TextureDescription&, const TextureUpload&, TextureHandle&) noexcept;
/// Return whether the texture identity is the null surface selector; no ownership/state query.
[[nodiscard]] bool IsNull(TextureHandle) noexcept;
/// Copy the immutable description of an owned texture; owner-thread only, including open frames.
/// Invalid/unready handles preserve output; does not expose native objects or extend ownership.
[[nodiscard]] RasterStatus GetTextureDescription(DeviceHandle, TextureHandle, TextureDescription&) noexcept;
/// Create a complete initialized-mip view in the allocation dimension and format, retaining its texture.
/// Use the description overload to select sampled subranges. Deferred sampled allocations
/// reject InvalidState until every mip is initialized. Failures preserve a null output; owner-thread only.
[[nodiscard]] RasterStatus CreateTextureView(DeviceHandle, TextureHandle, TextureViewHandle&) noexcept;
/// Create a bounded sampled aspect or writable attachment view on the owner thread between frames.
/// Only selected mips must be initialized (attachments retain existing clear/store rules).
/// Ready/Pending publishes ownership and retains the allocation; other results preserve output.
/// Unsupported denotes a disabled dimension/partial-view capability. GDI hazards and
/// mutation exclusion remain conservative for the whole allocation, including disjoint views.
[[nodiscard]] RasterStatus
CreateTextureView(DeviceHandle, TextureHandle, const TextureViewDescription&, TextureViewHandle&) noexcept;
/// Create immutable filter/address state; no engine allocations on a warmed resource path.
[[nodiscard]] RasterStatus CreateSampler(DeviceHandle, const SamplerDescription&, SamplerHandle&) noexcept;
/// Create a reflected stage. Pending owns its output; copied metadata survives caller storage.
[[nodiscard]] RasterStatus
CreateRasterShader(DeviceHandle, const RasterShaderDescription&, RasterShaderHandle&) noexcept;
/// Create an immutable group-zero layout from at most Interface.MaxBindings unique entries; per-stage counts are
/// checked independently. A runtime entry reserves the length slot and adds its
/// uniform entry when absent; conflicting consumer bindings reject.
[[nodiscard]] RasterStatus
CreateBindingLayout(DeviceHandle, std::span<const RasterBinding>, BindingLayoutHandle&) noexcept;
/// Create an immutable snapshot of the layout's full descriptor capacity.
/// Every element is required unless its binding enables HasPartialBinding; omitted
/// elements then use a required element-zero fallback. Duplicates always reject.
/// Runtime length metadata is supplied by RHI; consumer entries at its reserved
/// binding reject. Each runtime snapshot also owns one budgeted uniform buffer.
/// Creation consumes StorageLimits::BindingDescriptors until physical retirement.
/// It retains its layout and all resources even after public destruction.
[[nodiscard]] RasterStatus
CreateBindingSet(DeviceHandle, BindingLayoutHandle, std::span<const RasterBindingResource>, BindingSetHandle&) noexcept;
/// Allocate an explicitly sized snapshot of a runtime layout between frames.
/// @param runtimeCount Positive runtime array length, no greater than layout Count.
/// The borrowed entries are consumed before return. The RHI-owned length buffer
/// matches runtimeCount; verified runtime shaders clamp through runtime_bindings.slang.
/// Missing elements require
/// HasPartialBinding and populated element zero; otherwise every element is required.
/// Failure preserves output; Pending owns output and requires normal status polling.
/// Layouts without exactly one runtime binding reject. Owner-thread only.
[[nodiscard]] RasterStatus CreateRuntimeBindingSet(DeviceHandle,
                                                   BindingLayoutHandle,
                                                   ludus::foundation::BorrowedView<const RasterBindingResource>,
                                                   ludus::foundation::uint32 runtimeCount,
                                                   BindingSetHandle&) noexcept;
/// Publish a prepared replacement after previous work has bound the old snapshot.
/// Both handles must be owned, distinct, Ready and share the exact layout.
/// Owner-thread only, including during an open frame; prepare replacements before
/// the frame. Publication never allocates, collects or mutates native descriptors.
/// Ready consumes inoutReplacement, updates inoutCurrent and logically destroys
/// its old identity. Retained graphs/batches/submissions keep the previous backing
/// until completion. NotReady or other failure preserves both handles and bindings.
/// This snapshot operation does not enable Vulkan UPDATE_AFTER_BIND semantics.
[[nodiscard]] RasterStatus
PublishBindingSetUpdate(DeviceHandle, BindingSetHandle& inoutCurrent, BindingSetHandle& inoutReplacement) noexcept;
/// Obtain a borrowed index into a populated element of an owned, ready descriptor
/// array between or during frames. Logical holes and out-of-allocation indices reject.
/// Invalid/stale set returns InvalidHandle; bad binding/element InvalidDescription.
/// Failure preserves output. Does not retain resources or allocate. Owner-thread only.
[[nodiscard]] RasterStatus GetBindingIndex(DeviceHandle,
                                           BindingSetHandle,
                                           ludus::foundation::uint32 binding,
                                           ludus::foundation::uint32 element,
                                           RasterBindingIndex&) noexcept;
/// Resolve an index against the exact owned snapshot, preserving output on failure.
/// Stale/foreign/replaced indices return InvalidHandle. Ready writes the bounded shader
/// array element. Shaders must remain inside the allocated extent; logical holes
/// access the retained fallback. Replacing a snapshot invalidates its host tokens.
[[nodiscard]] RasterStatus
GetBindingArrayElement(DeviceHandle, BindingSetHandle, const RasterBindingIndex&, ludus::foundation::uint32&) noexcept;
/// Create an immutable triangle pipeline; dependencies must be ready. Binding contents
/// are supplied independently at draw time. Failure leaves null output and rolls back reservation.
[[nodiscard]] RasterStatus
CreateRasterPipeline(DeviceHandle, const RasterPipelineDescription&, RasterPipelineHandle&) noexcept;
/// Encode an indexed/instanced packet into the explicit device's open frame.
/// Retain all referenced records until backend completion; drawing cannot mix with
/// DrawFullscreen in the same frame. Rejected packets issue no native draw.
/// Sampled textures must have defined contents and a committed/draft sampled use
/// covering the binding visibility; otherwise InvalidDescription/InvalidState.
[[nodiscard]] RasterStatus DrawIndexed(DeviceHandle, const RasterDraw&) noexcept;
/// Query asynchronous validation for an owned Buffer record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, BufferHandle) noexcept;
/// Logically destroy an owned Buffer outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, BufferHandle&) noexcept;
/// Query asynchronous validation for an owned Texture record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, TextureHandle) noexcept;
/// Logically destroy an owned Texture outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, TextureHandle&) noexcept;
/// Query asynchronous validation for an owned TextureView record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, TextureViewHandle) noexcept;
/// Logically destroy an owned TextureView outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, TextureViewHandle&) noexcept;
/// Query asynchronous validation for an owned Sampler record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, SamplerHandle) noexcept;
/// Logically destroy an owned Sampler outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, SamplerHandle&) noexcept;
/// Query asynchronous validation for an owned RasterShader record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RasterShaderHandle) noexcept;
/// Logically destroy an owned RasterShader outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RasterShaderHandle&) noexcept;
/// Query asynchronous validation for an owned BindingLayout record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, BindingLayoutHandle) noexcept;
/// Logically destroy an owned BindingLayout outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, BindingLayoutHandle&) noexcept;
/// Query asynchronous validation for an owned BindingSet record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, BindingSetHandle) noexcept;
/// Logically destroy an owned BindingSet outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, BindingSetHandle&) noexcept;
/// Query asynchronous validation for an owned RasterPipeline record; stale/foreign handles are rejected.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RasterPipelineHandle) noexcept;
/// Logically destroy an owned RasterPipeline outside frames, zeroing the supplied handle.
/// Retained dependencies/GPU uses delay physical release. Invalid handles preserve output.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RasterPipelineHandle&) noexcept;
} // namespace ludus::graphics::rhi
