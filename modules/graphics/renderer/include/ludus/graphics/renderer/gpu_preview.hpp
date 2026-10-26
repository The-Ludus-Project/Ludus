#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/rhi/lifetime.h>

namespace ludus::graphics::renderer
{
/// Optional progressive GPU path-tracing settings. No CPU transport or readback fallback.
/// Static RGB reflection/emission uses RT1's admitted surface profile; this is a
/// progressive, finite-depth reference mode using native ray queries, without denoising.
/// Use RasterViewport for ordinary interactive World navigation; this mode freezes
/// scene/camera input, and requires native hardware ray support, compute and retained
/// reference inputs. Native-query artifacts require native ray support. Triangle graph
/// kernels may instead supply the software compute ABI, which needs ordinary compute
/// and retained scene inputs but creates no acceleration structures. Unsupported
/// devices/APIs return Unsupported; no CPU transport fallback is selected.
struct GpuPreviewOptions final
{
    /// Setup/retained geometry budget, including BVH construction and packing/upload scratch. Positive.
    /// Excludes RHI-retained input texels/registries and driver staging.
    ludus::foundation::usize CpuByteBudget = 0;
    /// Known preview buffers and two RGBA8/private-depth output images across live
    /// previews, including native BLAS/TLAS inputs, structures and build scratch, and
    /// cancelled jobs with pending producers/acquired frames.
    /// Excludes renderer/RHI registries, upload staging and opaque driver allocations.
    ludus::foundation::usize GpuByteBudget = 0;
    /// Positive finite display exposure. Output applies Reinhard then sRGB encoding.
    ludus::foundation::float32 Exposure = 1;
    /// Optional native graph storage; copied by EnableGpuPreview/EnableTriangleRayPipeline.
    /// Slots/strides/access must match the compute shader. Each job owns zeroed buffers
    /// until release; Count==0 follows its pixel extent. Budgets include their bytes.
    // Keep partial aggregate callers warning-clean when this optional field is omitted.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const content::render_graph_format::StorageBuffer> StorageBuffers{};
};
/// Renderer-owned immutable GPU scene and progressive accumulation. Copies borrow;
/// Release cancels future dispatches, while acquired output frames retain the job.
struct GpuPreview final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the renderer access to the opaque ownership identity.
    friend struct internal::Access;
};
/// Owned read-only output lease. Release only after every external GPU consumer
/// has completed and released its RHI interoperability lease. Copies borrow.
/// Reset/destruction invalidates renderer identities; an RHI external lease must
/// independently retain backing through foreign completion before RHI shutdown.
struct GpuPreviewFrame final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants the renderer access to the opaque ownership identity.
    friend struct internal::Access;
};
/// Borrowed output of one acquired lease; never destroy or mutate Texture.
/// Texture is full initialized RGBA8Unorm, top-left, opaque, Reinhard/sRGB encoded.
/// Qt must not tone-map/encode it again. Import the exact version on the same
/// native logical device/backend, order sampling after Producer, and retain the frame through
/// external completion. Matching a physical adapter alone is insufficient.
/// Width/Height are physical pixels, not logical UI units.
struct GpuPreviewImage final
{
    rhi::TextureHandle Texture{};          ///< Read-only texture valid through its frame lease.
    rhi::SubmissionToken Producer{};       ///< Completed producer work; not display completion.
    ludus::foundation::uint32 Width = 0;   ///< Positive physical width.
    ludus::foundation::uint32 Height = 0;  ///< Positive physical height.
    ludus::foundation::uint32 Samples = 0; ///< Completed dispatched samples per pixel.
};
/// CPU-observable submission progress; does not read GPU radiance/counters.
/// Numerical validity remains visible through error pixels and SDK acceptance;
/// completion alone is not a claim of CPU-oracle parity or convergence.
struct GpuPreviewInfo final
{
    ludus::foundation::uint32 CompletedSamples = 0;     ///< GPU-completed full-image dispatch count.
    ludus::foundation::uint32 TargetSamples = 0;        ///< Frozen positive stop target.
    ludus::foundation::uint64 SceneFingerprintLow = 0;  ///< Captured profile/content identity, low word.
    ludus::foundation::uint64 SceneFingerprintHigh = 0; ///< Captured identity, high word.
    bool Pending = false;                               ///< Producer commands outstanding; poll without waiting.
    bool Complete = false; ///< All requested dispatches completed, excluding consumer presentation.
};
} // namespace ludus::graphics::renderer
