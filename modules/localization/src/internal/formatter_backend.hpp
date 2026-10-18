#pragma once

// Boundary between backend-independent formatting (formatter.cpp: lifetime,
// schema validation, output contract) and its backend: formatter_icu.cpp with
// LUDUS_BUILD_UNICODE, formatter_unavailable.cpp without. No ICU type crosses it.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/localization/catalog.hpp>
#include <ludus/localization/formatter.hpp>

#include <span>
#include <string_view>

namespace ludus::localization::detail
{
/// One argument after schema validation, in declaration order.
struct ResolvedArgument final
{
    std::string_view Name;                 ///< Declared name.
    ArgumentType Type{ArgumentType::Text}; ///< Declared type.
    int64 Integer{};                       ///< Count value.
    float64 Number{};                      ///< Number value, already converted from an Integer kind.
    std::string_view Text;                 ///< Text value, or the enum token to select (`other` for unknowns).
};

/// Owner of backend state; destroyed through its creating domain.
using FormatterBackendOwner = foundation::UniquePtr<FormatterBackend, FormatterBackendDeleter>;

/// @brief Reports whether a backend can format packages labelled linguisticData.
/// @return Ok, BackendUnavailable or LinguisticDataMismatch.
[[nodiscard]] Status CheckFormatterBackend(std::string_view linguisticData) noexcept;

/// @brief Parses each dynamic message of messageIndices in its provider locale.
///
/// @param messageIndices Distinct dynamic record indices of lease, ascending.
/// @param outBackend Written only on Ok.
/// @return Ok; BackendUnavailable; OutOfMemory for domain storage; BackendFailure.
[[nodiscard]] Status CreateFormatterBackend(const Catalog& lease,
                                            std::span<const uint32> messageIndices,
                                            const foundation::AllocationDomain& domain,
                                            FormatterBackendOwner& outBackend) noexcept;

/// Reports whether messageIndex was prepared by backend.
[[nodiscard]] bool IsPrepared(const FormatterBackend& backend, uint32 messageIndex) noexcept;

/// @brief Formats a prepared message into scratch and copies it only when complete.
///
/// @param arguments Validated values in declaration order.
/// @param output Written only on Ok.
/// @param outRequiredBytes Complete UTF-8 size on Ok, BufferTooSmall and Limit.
/// @return Ok, BufferTooSmall, Limit or BackendFailure.
[[nodiscard]] Status FormatWithBackend(FormatterBackend& inoutBackend,
                                       uint32 messageIndex,
                                       std::span<const ResolvedArgument> arguments,
                                       std::span<char> output,
                                       usize& outRequiredBytes) noexcept;
} // namespace ludus::localization::detail
