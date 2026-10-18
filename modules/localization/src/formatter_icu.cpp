// ICU4C 78.3 backend of the L2 formatter (LUDUS_BUILD_UNICODE builds).
//
// Thanks to Unicode, Inc. and the ICU project for ICU4C 78.3, particularly the
// MessageFormat API reference and the ICU User Guide, "Formatting Messages":
// https://unicode-org.github.io/icu/userguide/format_parse/messages/
// Ludus adopts ICU's named-argument MessageFormat 1 API unchanged. The adapter
// (validated, null-checked string adoption, isolate insertion, count-then-copy
// output) is original Ludus code first proven in tools/icu-probe/main.cpp.

#include "internal/formatter_backend.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/unicode/unicode.hpp>

#include "internal/catalog_access.hpp"
#include "internal/icu_host.hpp"

#include <new>
#include <utility>

#include <unicode/bytestream.h>
#include <unicode/fmtable.h>
#include <unicode/messagepattern.h>
#include <unicode/msgfmt.h>
#include <unicode/unistr.h>

#if defined(__cpp_exceptions)
#    error The ICU formatter backend must compile without C++ exceptions
#endif

namespace ludus::localization
{
namespace
{
using detail::CatalogAccess;
using detail::ResolvedArgument;
using foundation::int32;
namespace unicode = foundation::unicode;

constexpr UChar FIRST_STRONG_ISOLATE = 0x2068;
constexpr UChar POP_DIRECTIONAL_ISOLATE = 0x2069;

/// One prepared dynamic message: its record index and parsed ICU formatter.
struct PreparedMessage final
{
    uint32 Index{};
    foundation::UniquePtr<icu::MessageFormat> Formatter;
};

/// Copies text into a Formattable; ICU's own setString cannot report failure.
bool AdoptString(const icu::UnicodeString& text, icu::Formattable& outValue) noexcept
{
    // UnicodeString derives from UMemory: this new expression uses ICU's
    // allocator, whose Ludus hook terminates instead of returning nullptr.
    icu::UnicodeString* copyOrNull = new icu::UnicodeString(text); // NOLINT(cppcoreguidelines-owning-memory)
    if (copyOrNull == nullptr || copyOrNull->isBogus() == true || text.isBogus() == true)
    {
        delete copyOrNull; // NOLINT(cppcoreguidelines-owning-memory)
        return false;
    }
    outValue.adoptString(copyOrNull);
    return true;
}
} // namespace

namespace detail
{
/// Releases one host retention after every ICU object of its owner is gone.
struct IcuHostRetention final
{
    IcuHostRetention() noexcept = default;
    ~IcuHostRetention() noexcept
    {
        foundation::unicode::detail::ReleaseHost();
    }
    IcuHostRetention(const IcuHostRetention&) = delete;
    IcuHostRetention& operator=(const IcuHostRetention&) = delete;
    IcuHostRetention(IcuHostRetention&&) = delete;
    IcuHostRetention& operator=(IcuHostRetention&&) = delete;
};
} // namespace detail

/// Mutable ICU state of one formatter context: parsed formatters and UTF-16 scratch.
/// Members are destroyed in reverse order, so the retention is released last.
struct detail::FormatterBackend final
{
    FormatterBackend(const foundation::AllocationDomain& domain,
                     PreparedMessage* messagesOrNull,
                     usize messageCount) noexcept
        : Domain(domain), MessagesOrNull(messagesOrNull), MessageCount(messageCount)
    {
    }
    ~FormatterBackend() noexcept
    {
        for (usize i = 0; i < MessageCount; ++i)
        {
            MessagesOrNull[i].~PreparedMessage();
        }
        Domain.Free(MessagesOrNull, MessageCount * sizeof(PreparedMessage), alignof(PreparedMessage));
    }
    FormatterBackend(const FormatterBackend&) = delete;
    FormatterBackend& operator=(const FormatterBackend&) = delete;
    FormatterBackend(FormatterBackend&&) = delete;
    FormatterBackend& operator=(FormatterBackend&&) = delete;

    IcuHostRetention Retention;
    const foundation::AllocationDomain& Domain;
    PreparedMessage* MessagesOrNull; ///< MessageCount entries sorted by Index; null only when empty.
    usize MessageCount;
    icu::UnicodeString Scratch;
};

void detail::FormatterBackendDeleter::operator()(FormatterBackend* backend) const noexcept
{
    const foundation::AllocationDomain& domain = backend->Domain;
    backend->~FormatterBackend();
    domain.Free(backend, sizeof(FormatterBackend), alignof(FormatterBackend));
}

namespace detail
{
namespace
{
const PreparedMessage* FindPrepared(const FormatterBackend& backend, uint32 messageIndex) noexcept
{
    usize begin = 0;
    usize end = backend.MessageCount;
    while (begin < end)
    {
        const usize middle = begin + (end - begin) / 2;
        const uint32 index = backend.MessagesOrNull[middle].Index;
        if (index < messageIndex)
        {
            begin = middle + 1;
        }
        else if (index > messageIndex)
        {
            end = middle;
        }
        else
        {
            return &backend.MessagesOrNull[middle];
        }
    }
    return nullptr;
}

/// Parses one cooked pattern in its provider locale with DOUBLE_OPTIONAL apostrophes.
Status PrepareMessage(const Catalog& lease, uint32 index, PreparedMessage& outMessage) noexcept
{
    const MessageRecord record = CatalogAccess::GetMessage(lease, index);
    icu::Locale locale("und");
    if (U_FAILURE(unicode::detail::MakeLocale(CatalogAccess::GetProviderLocale(lease, record.Provider), locale)))
    {
        return Status::BackendFailure;
    }
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    icu::MessageFormat* formatterOrNull = new icu::MessageFormat( // NOLINT(cppcoreguidelines-owning-memory)
        unicode::detail::MakeUnicodeString(record.Value),
        locale,
        parseError,
        status);
    foundation::UniquePtr<icu::MessageFormat> formatter(formatterOrNull);
    if (formatter.Get() == nullptr || U_FAILURE(status) ||
        formatter.Get()->getApostropheMode() != UMSGPAT_APOS_DOUBLE_OPTIONAL)
    {
        return Status::BackendFailure;
    }
    outMessage.Index = index;
    outMessage.Formatter = std::move(formatter);
    return Status::Ok;
}
} // namespace

Status CheckFormatterBackend(std::string_view linguisticData) noexcept
{
    if (unicode::IsUnicodeHostInitialized() == false)
    {
        return Status::BackendUnavailable;
    }
    return linguisticData == unicode::GetLinguisticDataLabel() ? Status::Ok : Status::LinguisticDataMismatch;
}

Status CreateFormatterBackend(const Catalog& lease,
                              std::span<const uint32> messageIndices,
                              const foundation::AllocationDomain& domain,
                              FormatterBackendOwner& outBackend) noexcept
{
    if (unicode::detail::TryRetainHost() == false)
    {
        return Status::BackendUnavailable;
    }
    void* storageOrNull = domain.TryAllocate(sizeof(FormatterBackend), alignof(FormatterBackend));
    void* messagesOrNull =
        messageIndices.empty() == true
            ? nullptr
            : domain.TryAllocate(messageIndices.size() * sizeof(PreparedMessage), alignof(PreparedMessage));
    if (storageOrNull == nullptr || (messageIndices.empty() == false && messagesOrNull == nullptr))
    {
        domain.Free(storageOrNull, sizeof(FormatterBackend), alignof(FormatterBackend));
        domain.Free(messagesOrNull, messageIndices.size() * sizeof(PreparedMessage), alignof(PreparedMessage));
        unicode::detail::ReleaseHost();
        return Status::OutOfMemory;
    }
    PreparedMessage* preparedOrNull = static_cast<PreparedMessage*>(messagesOrNull);
    for (usize i = 0; i < messageIndices.size(); ++i)
    {
        ::new (&preparedOrNull[i]) PreparedMessage{};
    }
    // From here the owner releases the host retention, every message and both blocks.
    FormatterBackend* backend = ::new (storageOrNull) FormatterBackend(domain, preparedOrNull, messageIndices.size());
    FormatterBackendOwner candidate(backend, FormatterBackendDeleter{});
    for (usize i = 0; i < messageIndices.size(); ++i)
    {
        const Status prepared = PrepareMessage(lease, messageIndices[i], preparedOrNull[i]);
        if (prepared != Status::Ok)
        {
            return prepared;
        }
    }
    outBackend = std::move(candidate);
    return Status::Ok;
}

bool IsPrepared(const FormatterBackend& backend, uint32 messageIndex) noexcept
{
    return FindPrepared(backend, messageIndex) != nullptr;
}

Status FormatWithBackend(FormatterBackend& inoutBackend,
                         uint32 messageIndex,
                         std::span<const ResolvedArgument> arguments,
                         std::span<char> output,
                         usize& outRequiredBytes) noexcept
{
    const PreparedMessage* messageOrNull = FindPrepared(inoutBackend, messageIndex);
    if (messageOrNull == nullptr || arguments.size() > MAX_ARGUMENTS)
    {
        return Status::BackendFailure;
    }
    foundation::StaticArray<icu::UnicodeString, MAX_ARGUMENTS> names{};
    foundation::StaticArray<icu::Formattable, MAX_ARGUMENTS> values{};
    for (usize i = 0; i < arguments.size(); ++i)
    {
        const ResolvedArgument& argument = arguments[i];
        names[i] = unicode::detail::MakeUnicodeString(argument.Name);
        bool isAdopted = true;
        switch (argument.Type)
        {
            case ArgumentType::Text: {
                // Isolate inserted text so its direction cannot reorder the sentence.
                icu::UnicodeString text(FIRST_STRONG_ISOLATE);
                text.append(unicode::detail::MakeUnicodeString(argument.Text)).append(POP_DIRECTIONAL_ISOLATE);
                isAdopted = AdoptString(text, values[i]);
                break;
            }
            case ArgumentType::Count:
                values[i].setInt64(argument.Integer);
                break;
            case ArgumentType::Number:
                values[i].setDouble(argument.Number);
                break;
            case ArgumentType::Enum:
                isAdopted = AdoptString(unicode::detail::MakeUnicodeString(argument.Text), values[i]);
                break;
        }
        if (isAdopted == false || names[i].isBogus() == true)
        {
            return Status::BackendFailure;
        }
    }
    UErrorCode status = U_ZERO_ERROR;
    inoutBackend.Scratch.remove();
    messageOrNull->Formatter.Get()->format(names.GetData(),
                                           values.GetData(),
                                           static_cast<int32>(arguments.size()),
                                           inoutBackend.Scratch,
                                           status);
    if (U_FAILURE(status) || inoutBackend.Scratch.isBogus() == true)
    {
        return Status::BackendFailure;
    }
    // Count first, then copy only a complete result; never truncate.
    icu::CheckedArrayByteSink counter(nullptr, 0);
    inoutBackend.Scratch.toUTF8(counter);
    const usize required = static_cast<usize>(counter.NumberOfBytesAppended());
    outRequiredBytes = required;
    if (required > MAX_FORMATTED_BYTES)
    {
        return Status::Limit;
    }
    if (required > output.size())
    {
        return Status::BufferTooSmall;
    }
    icu::CheckedArrayByteSink sink(output.data(), static_cast<int32>(output.size()));
    inoutBackend.Scratch.toUTF8(sink);
    return sink.Overflowed() == true ? Status::BackendFailure : Status::Ok;
}
} // namespace detail
} // namespace ludus::localization
