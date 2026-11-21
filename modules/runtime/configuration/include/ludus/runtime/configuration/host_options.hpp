#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/config/config.hpp>

namespace ludus::runtime::configuration
{
/// First migrated module-owned schema; bump identity for incompatible contracts.
inline constexpr std::string_view HOST_SCHEMA = "ludus.host.v1";
/// Plain startup options copied from active host configuration.
struct HostOptions final
{
    /// Run without a presentation window.
    bool Headless = false;
    foundation::uint64 MaxFrames = 0; ///< Zero means unlimited.
};
/// Borrow the immutable host descriptors for the lifetime of this module.
[[nodiscard]] std::span<const foundation::config::Descriptor> HostSchema() noexcept;
/// Copy active values from a context initialized with the host schema.
/// Returns InvalidSchema and preserves the output if required settings are absent or mistyped.
[[nodiscard]] foundation::config::Status ReadHostOptions(const foundation::config::Context& context,
                                                         HostOptions& output) noexcept;
} // namespace ludus::runtime::configuration
