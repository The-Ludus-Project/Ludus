#pragma once

/// @file
/// @brief Typed `ludus-icu-mf1-v1` formatting of cooked package version 2 messages (phase L2).
///
/// The formatter validates the complete argument schema before ICU sees any
/// value, isolates inserted text, formats in the provider locale with the pinned
/// ICU4C backend and copies only complete UTF-8. Builds without
/// `LUDUS_BUILD_UNICODE` keep this API and report BackendUnavailable. See
/// docs/modules/localization.md ("Formatting messages with arguments").

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/localization/catalog.hpp>

#include <span>
#include <string_view>

namespace ludus::localization
{
using foundation::float64;

/// Largest formatted message, in UTF-8 bytes; a longer result returns Limit.
inline constexpr usize MAX_FORMATTED_BYTES = usize{64} * 1024;
/// Most bindings one formatter context may prepare.
inline constexpr usize MAX_PREPARED_MESSAGES = 4096;

/// Value representation supplied for one argument; the schema decides which kinds it accepts.
enum class ArgumentValueKind : uint8
{
    Text,    ///< Counted UTF-8: a Text argument or an Enum token.
    Integer, ///< `int64`: a Count, or a Number with an integral value.
    Number,  ///< Finite `float64`: a Number only.
};

/// @brief One named argument value; views borrow caller storage until Format returns.
///
/// Prefer the Make*Argument helpers or the generated typed helpers to spelling
/// the fields directly.
struct ArgumentValue final
{
    std::string_view Name;                           ///< Declared argument name.
    ArgumentValueKind Kind{ArgumentValueKind::Text}; ///< Which value field is meaningful.
    int64 Integer{};                                 ///< Integer kind only.
    float64 Number{};                                ///< Number kind only.
    std::string_view Text;                           ///< Text kind only: literal text or enum token.
};

/// Text argument: literal UTF-8, never interpreted as a message.
[[nodiscard]] constexpr ArgumentValue MakeTextArgument(std::string_view name, std::string_view text) noexcept
{
    return { .Name = name, .Kind = ArgumentValueKind::Text, .Integer = 0, .Number = 0.0, .Text = text };
}

/// Enum argument: one declared token, or any token for an `unknown: other` declaration.
[[nodiscard]] constexpr ArgumentValue MakeEnumArgument(std::string_view name, std::string_view token) noexcept
{
    return { .Name = name, .Kind = ArgumentValueKind::Text, .Integer = 0, .Number = 0.0, .Text = token };
}

/// Count argument: a plural/ordinal selector or integer display within the declared bounds.
[[nodiscard]] constexpr ArgumentValue MakeCountArgument(std::string_view name, int64 count) noexcept
{
    return { .Name = name, .Kind = ArgumentValueKind::Integer, .Integer = count, .Number = 0.0, .Text = {} };
}

/// Number argument: a finite measurement within the declared bounds; display only.
[[nodiscard]] constexpr ArgumentValue MakeNumberArgument(std::string_view name, float64 number) noexcept
{
    return { .Name = name, .Kind = ArgumentValueKind::Number, .Integer = 0, .Number = number, .Text = {} };
}

/// Outcome details of Format; reset on every call, so stale values never survive a failure.
struct FormatResult final
{
    usize Written{};                   ///< Bytes copied to the output on Ok; zero otherwise.
    usize RequiredBytes{};             ///< Complete UTF-8 size on Ok or BufferTooSmall; zero otherwise.
    Origin Provenance{Origin::Source}; ///< Which catalog provided the message on Ok.
    std::string_view ProviderLocale;   ///< Locale whose grammar formatted it on Ok; borrowed from the context's lease.
};

namespace detail
{
/// Private backend state of a formatter context (ICU objects and scratch).
struct FormatterBackend;

/// Destroys backend state through the domain that created it; defined by the backend.
struct FormatterBackendDeleter final
{
    void operator()(FormatterBackend* backend) const noexcept;
};
} // namespace detail

/// @brief Prepared formatter state for a declared set of messages of one catalog lease.
///
/// Owns its parsed ICU formatters and UTF-16 scratch and retains a lease of its
/// catalog, so views in FormatResult stay valid while the context lives. It is
/// neither reentrant nor shareable: one thread owns it at a time, and a worker
/// needs its own prepared context. Moved-from contexts are invalid. Destroy every
/// context before `ShutdownUnicodeHost`.
class FormatterContext final
{
public:
    /// Creates an invalid context; Format reports Invalid until PrepareFormatterContext replaces it.
    FormatterContext() noexcept = default;
    /// Destroys the ICU state and its host retention, then releases the retained lease.
    ~FormatterContext() noexcept = default;
    /// Deleted: ICU formatter state is owned by exactly one context.
    FormatterContext(const FormatterContext&) = delete;
    /// Deleted: ICU formatter state is owned by exactly one context.
    FormatterContext& operator=(const FormatterContext&) = delete;
    /// Transfers the prepared state; other becomes invalid.
    FormatterContext(FormatterContext&& other) noexcept;
    /// Destroys this state, then transfers other's; other becomes invalid.
    FormatterContext& operator=(FormatterContext&& other) noexcept;

    /// Reports whether PrepareFormatterContext produced this context.
    [[nodiscard]] bool IsValid() const noexcept;
    /// Number of distinct dynamic messages prepared; static messages need no preparation.
    [[nodiscard]] usize GetPreparedCount() const noexcept;

private:
    Catalog mLease;
    foundation::UniquePtr<detail::FormatterBackend, detail::FormatterBackendDeleter> mBackend;
    usize mPreparedCount{};
    friend Status PrepareFormatterContext(const Catalog&,
                                          std::span<const MessageBinding>,
                                          const foundation::AllocationDomain&,
                                          FormatterContext&) noexcept;
    friend Status
    Format(FormatterContext&, MessageBinding, std::span<const ArgumentValue>, std::span<char>, FormatResult&) noexcept;
};

/// @brief Parses the dynamic patterns of messages once, in their provider locales.
///
/// A cold operation: prepare a view's dynamic set before it becomes visible.
/// Static bindings and duplicates are accepted and need no preparation. Checks,
/// in order: a valid version 2 lease (Invalid, Unsupported), the message count
/// (Limit), every binding against this lease (WrongCatalog), a running Unicode
/// host (BackendUnavailable, also in builds without ICU), the package's
/// linguistic-data label against the running ICU (LinguisticDataMismatch), the
/// context storage from domain (OutOfMemory) and ICU pattern construction
/// (BackendFailure). ICU allocation failure is fatal, never reported.
///
/// @param lease Catalog to retain; copied, so the caller may replace its own.
/// @param messages Bindings of lease to prepare, at most MAX_PREPARED_MESSAGES.
/// @param domain Storage for the context state. Borrowed; it must outlive the context.
/// @param outContext Replaced only on Ok; every failure leaves it unchanged.
[[nodiscard]] Status PrepareFormatterContext(const Catalog& lease,
                                             std::span<const MessageBinding> messages,
                                             const foundation::AllocationDomain& domain,
                                             FormatterContext& outContext) noexcept;

/// @brief Formats one message with its complete argument schema into caller bytes.
///
/// Validates before ICU: every declared argument exactly once with an accepted
/// kind; Count and Number within bounds, Number finite; Text valid UTF-8 without
/// NUL within `max_bytes`, and `user` text without C0/C1 controls, bidi
/// embeddings/overrides (U+202A-U+202E) or isolates (U+2066-U+2069); Enum tokens
/// declared, or mapped to `other` for `unknown: other`. Inserted text is wrapped
/// in FSI (U+2068) ... PDI (U+2069). A static record copies its cooked text
/// without ICU; it still rejects arguments.
///
/// @param inoutContext Prepared context; its ICU scratch is reused.
/// @param binding Binding made by the context's lease (or a copy of it).
/// @param arguments Values in any order.
/// @param output Receives complete UTF-8 on Ok only; never NUL-terminated or truncated.
/// @param outResult Reset on every call; see FormatResult.
/// @return Ok; Invalid for an invalid context; WrongCatalog; NotPrepared for an
///         unprepared dynamic message; InvalidArguments; BufferTooSmall with
///         RequiredBytes; Limit above MAX_FORMATTED_BYTES; BackendFailure. Every
///         failure leaves the output bytes unchanged.
[[nodiscard]] Status Format(FormatterContext& inoutContext,
                            MessageBinding binding,
                            std::span<const ArgumentValue> arguments,
                            std::span<char> output,
                            FormatResult& outResult) noexcept;
} // namespace ludus::localization
