#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/graphics/rhi/rhi.h>

// R14a raw adapter catalog. The catalog reports what the native graphics API says
// about each adapter; it is separate from the enabled DeviceProfile, which alone
// governs admission. See docs/development/fullscreen-rendering.md, "Adapter
// catalog and selection (R14a)".
namespace ludus::graphics::rhi
{
/// Bytes in an AdapterIdentity: a Vulkan device UUID, or a little-endian Direct3D 12
/// LUID or Metal registry ID zero padded to this width.
inline constexpr ludus::foundation::usize ADAPTER_IDENTITY_BYTES = 16;
/// Maximum UTF-8 bytes kept from an adapter's reported name; longer names are
/// shortened at a code point boundary, and a name with ill-formed UTF-8 keeps only
/// the well-formed bytes before the first ill-formed sequence.
inline constexpr ludus::foundation::usize MAX_ADAPTER_NAME_BYTES = 255;
/// Maximum adapters one catalog holds; AdapterCatalog::ReportedCount shows truncation.
inline constexpr ludus::foundation::usize MAX_CATALOG_ADAPTERS = 16;

/// Bounded adapter name as reported by the graphics API.
using AdapterName = ludus::foundation::StaticString<MAX_ADAPTER_NAME_BYTES>;

/// Adapter class reported by the graphics API; Unknown when the API does not say.
enum class AdapterKind : ludus::foundation::uint8
{
    /// The API reports no class (Direct3D 12 hardware adapters, Vulkan "other").
    Unknown,
    /// A GPU sharing memory with the CPU (Vulkan integrated, Metal unified memory).
    Integrated,
    /// A GPU with its own memory, including removable external GPUs.
    Discrete,
    /// A virtualized GPU (Vulkan virtual).
    Virtual,
    /// A CPU implementation (Vulkan CPU, Direct3D 12 WARP).
    Software,
};

/// @brief Stable identity of one adapter within one native backend; never a pointer.
///
/// Survives process restarts while the driver and hardware are unchanged. Vulkan uses
/// VkPhysicalDeviceIDProperties::deviceUUID; Direct3D 12 the adapter LUID (valid until
/// reboot); Metal the MTLDevice registry ID. All-zero bytes mean the automatic choice.
struct AdapterIdentity final
{
    /// Backend whose API produced Bytes; an identity is never valid on another backend.
    Backend Api = Backend::Vulkan;
    /// Backend identity bytes; all zero selects the automatic adapter choice.
    ludus::foundation::StaticArray<ludus::foundation::uint8, ADAPTER_IDENTITY_BYTES> Bytes{};

    /// True when every identity byte is zero, the automatic adapter choice.
    [[nodiscard]] LUDUS_INLINE bool IsAutomatic() const noexcept
    {
        for (const ludus::foundation::uint8 byte : Bytes)
        {
            if (byte != 0)
            {
                return false;
            }
        }
        return true;
    }
    /// Compare backend and bytes; equal identities name the same adapter.
    [[nodiscard]] LUDUS_INLINE bool operator==(const AdapterIdentity&) const noexcept = default;
};

/// @brief Raw claims of one adapter as reported by the graphics API.
///
/// These values describe hardware, not enabled operations. Admission, limits and
/// formats come only from DeviceInfo::Profile after a device is Ready; a large raw
/// value here never raises an enabled limit.
struct AdapterInfo final
{
    /// Stable identity to pass in DeviceDescription::Adapter.
    AdapterIdentity Identity{};
    /// API-reported name, for diagnostics and user choice; not unique or stable.
    AdapterName Name{};
    /// API-reported class.
    AdapterKind Kind = AdapterKind::Unknown;
    /// PCI vendor ID; zero when the API does not report one (Metal).
    ludus::foundation::uint32 VendorId = 0;
    /// PCI device ID; zero when the API does not report one (Metal).
    ludus::foundation::uint32 DeviceId = 0;
    /// Bytes the API reports for the adapter: Vulkan device-local heaps, Direct3D 12
    /// dedicated video memory, Metal's recommended working set. Not a budget.
    ludus::foundation::uint64 LocalMemoryBytes = 0;
    /// Raw 2D texture edge claim in texels; zero when unknown. Not an enabled limit.
    ludus::foundation::uint32 MaxTextureDimension2D = 0;
    /// True when the adapter meets this backend's minimum (Vulkan 1.1 with a graphics
    /// queue, Metal Mac2/Apple4 family, Direct3D 12 feature level 11.0). Required
    /// workloads may still reject it during device creation.
    bool HasBackendBaseline = false;
};

/// Bounded snapshot of the adapters one native backend reports, in API order.
struct AdapterCatalog final
{
    /// Backend that produced every entry.
    Backend Api = Backend::Vulkan;
    /// The first Count entries are valid; the rest are default values.
    ludus::foundation::StaticArray<AdapterInfo, MAX_CATALOG_ADAPTERS> Adapters{};
    /// Valid entries, at most MAX_CATALOG_ADAPTERS.
    ludus::foundation::usize Count = 0;
    /// Adapters the API reported; greater than Count when the catalog was truncated.
    ludus::foundation::usize ReportedCount = 0;
};

/// Result of an adapter catalog query.
enum class AdapterStatus : ludus::foundation::uint8
{
    /// The catalog was written; it may be empty.
    Ready,
    /// This build's backend has no adapter catalog (browser WebGPU/WebGL 2).
    Unsupported,
    /// The graphics API could not be loaded or enumerated.
    Failed,
};

/// @brief Enumerate this build's native adapters without starting a device.
///
/// Works with or without a live device session and never changes it: Vulkan reuses the
/// session instance or creates and destroys a temporary one, Direct3D 12 likewise a DXGI
/// factory, and Metal copies the system device list. Owner (main) thread only.
/// @param outCatalog Receives the catalog on Ready; preserved on every other result.
/// @return Ready, Unsupported on browser builds, or Failed when the API is unavailable.
[[nodiscard]] AdapterStatus GetAdapterCatalog(AdapterCatalog& outCatalog) noexcept;
} // namespace ludus::graphics::rhi
