#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
namespace internal
{
struct EnvironmentData;
struct Access;
} // namespace internal
/// Maximum reconstructed linear HDR environment radiance per RGB channel.
/// Separate from bounded material emission and constant-environment multipliers.
inline constexpr ludus::foundation::float32 MAX_ENVIRONMENT_RADIANCE = 1e12F;
/// Borrowed linear HDR latitude/longitude image; Initialize copies all pixels.
/// +Y is north, longitude zero is +X, increasing longitude turns toward +Z.
struct EnvironmentDescription final
{
    /// Tightly packed top-left rows, RGB radiance [0,1e12]; alpha is ignored.
    /// Initialize requires Width*Height records and a nonnull pointer; it copies
    /// them during the call without retaining the caller's storage.
    const ludus::foundation::math::Vector4* PixelsOrNull = nullptr;
    /// Positive source dimensions, each at most 4096; need not be powers of two.
    ludus::foundation::uint32 Width = 0;
    /// Positive source height; top row borders the north pole.
    ludus::foundation::uint32 Height = 0;
    /// Power-of-two prefiltered width [16,512]; height is half this value.
    ludus::foundation::uint32 FilterWidth = 128;
    /// Deterministic GGX samples per filtered texel, [16,1024]. Preparation only.
    ludus::foundation::uint32 FilterSamples = 128;
    /// Explicit CPU preparation/retention budget in bytes; zero rejects.
    ludus::foundation::usize CpuByteBudget = 0;
};
/// Immutable HDR source, importance distribution, diffuse SH and GGX mip chain.
/// Move-only owner; accepted materials/captures retain the exact data after Reset.
/// Initialize, Reset, moves and destruction require a closed RHI frame.
/// All operations and destruction use the renderer/device thread. Reset before
/// device destruction, and keep that device alive until retained uses finish.
class EnvironmentMap final
{
public:
    /// Construct an empty owner without allocating or accessing the device.
    EnvironmentMap() noexcept = default;
    /// Release this owner; retained renderer uses remain valid.
    ~EnvironmentMap() noexcept;
    /// Copying ownership is forbidden; pass a borrowed pointer to material/capture creation.
    EnvironmentMap(const EnvironmentMap&) = delete;
    /// Copy assignment is forbidden; move to transfer this owner.
    EnvironmentMap& operator=(const EnvironmentMap&) = delete;
    /// Transfer ownership, leaving other empty.
    EnvironmentMap(EnvironmentMap&& other) noexcept;
    /// Release this owner and transfer other, leaving other empty.
    EnvironmentMap& operator=(EnvironmentMap&& other) noexcept;
    /// Prepare CPU data and upload RGBA16F. Empty owner required. Ready/Pending
    /// accepts ownership; every other status preserves this empty owner.
    /// HDR unsupported devices return Unsupported; no LDR fallback or exposure.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const EnvironmentDescription& description) noexcept;
    /// Upload readiness; empty returns InvalidHandle. Does not block.
    [[nodiscard]] rhi::RasterStatus GetStatus() const noexcept;
    /// Detach ownership; safe for an empty owner. No allocation or blocking.
    void Reset() noexcept;

private:
    internal::EnvironmentData* mDataOrNull = nullptr;
    /// Grants the renderer access to retain this exact data incarnation.
    friend struct internal::Access;
};
} // namespace ludus::graphics::renderer
