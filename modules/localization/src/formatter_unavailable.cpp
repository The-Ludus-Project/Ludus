#include "internal/formatter_backend.hpp"

// Backend of builds without LUDUS_BUILD_UNICODE: the formatter API exists, but
// no context can be prepared, so no other entry point is reached.

namespace ludus::localization
{
void detail::FormatterBackendDeleter::operator()(FormatterBackend* /*backend*/) const noexcept {}

namespace detail
{
Status CheckFormatterBackend(std::string_view /*linguisticData*/) noexcept
{
    return Status::BackendUnavailable;
}

Status CreateFormatterBackend(const Catalog& /*lease*/,
                              std::span<const uint32> /*messageIndices*/,
                              const foundation::AllocationDomain& /*domain*/,
                              FormatterBackendOwner& /*outBackend*/) noexcept
{
    return Status::BackendUnavailable;
}

bool IsPrepared(const FormatterBackend& /*backend*/, uint32 /*messageIndex*/) noexcept
{
    return false;
}

Status FormatWithBackend(FormatterBackend& /*inoutBackend*/,
                         uint32 /*messageIndex*/,
                         std::span<const ResolvedArgument> /*arguments*/,
                         std::span<char> /*output*/,
                         usize& /*outRequiredBytes*/) noexcept
{
    return Status::BackendUnavailable;
}
} // namespace detail
} // namespace ludus::localization
