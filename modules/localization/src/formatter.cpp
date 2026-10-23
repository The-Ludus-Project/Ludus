#include <ludus/localization/formatter.hpp>

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include "internal/catalog_access.hpp"
#include "internal/formatter_backend.hpp"

#include <cstring>
#include <utility>

// Backend-independent L2 formatting: context lifetime, full schema validation
// before ICU and the complete-or-unchanged output contract. The boundary rules
// are specified in docs/architecture/localization-l0.md#message-profile-ludus-icu-mf1-v1
// and were first proven by the L0 probe's typed adapter (tools/icu-probe/main.cpp).

namespace ludus::localization
{
namespace
{
using detail::ArgumentRecord;
using detail::CatalogAccess;
using detail::MessageRecord;
using detail::ResolvedArgument;

constexpr std::string_view OTHER_TOKEN = "other";
/// UTF-8 lead bytes from which a sequence has four, three or two bytes, and the
/// scalar payload bits each lead and every continuation byte carries.
constexpr uint8 UTF8_FOUR_BYTE_LEAD = 0xf0;
constexpr uint8 UTF8_THREE_BYTE_LEAD = 0xe0;
constexpr uint8 UTF8_TWO_BYTE_LEAD = 0xc0;
constexpr uint32 UTF8_FOUR_BYTE_LEAD_PAYLOAD = 0x07U;
constexpr uint32 UTF8_THREE_BYTE_LEAD_PAYLOAD = 0x0fU;
constexpr uint32 UTF8_TWO_BYTE_LEAD_PAYLOAD = 0x1fU;
constexpr uint32 UTF8_CONTINUATION_PAYLOAD = 0x3fU;
constexpr uint32 UTF8_CONTINUATION_PAYLOAD_BITS = 6;
constexpr usize UTF8_FOUR_BYTE_LENGTH = 4;
constexpr usize UTF8_THREE_BYTE_LENGTH = 3;
constexpr usize UTF8_TWO_BYTE_LENGTH = 2;

/// Decodes one scalar of already validated UTF-8 starting at inoutOffset.
uint32 DecodeScalar(std::string_view text, usize& inoutOffset) noexcept
{
    const uint8 lead = static_cast<uint8>(text[inoutOffset]);
    usize length = 1;
    uint32 scalar = lead;
    if (lead >= UTF8_FOUR_BYTE_LEAD)
    {
        length = UTF8_FOUR_BYTE_LENGTH;
        scalar = lead & UTF8_FOUR_BYTE_LEAD_PAYLOAD;
    }
    else if (lead >= UTF8_THREE_BYTE_LEAD)
    {
        length = UTF8_THREE_BYTE_LENGTH;
        scalar = lead & UTF8_THREE_BYTE_LEAD_PAYLOAD;
    }
    else if (lead >= UTF8_TWO_BYTE_LEAD)
    {
        length = UTF8_TWO_BYTE_LENGTH;
        scalar = lead & UTF8_TWO_BYTE_LEAD_PAYLOAD;
    }
    for (usize i = 1; i < length; ++i)
    {
        scalar = (scalar << UTF8_CONTINUATION_PAYLOAD_BITS) |
                 (static_cast<uint8>(text[inoutOffset + i]) & UTF8_CONTINUATION_PAYLOAD);
    }
    inoutOffset += length;
    return scalar;
}

/// User text must not carry controls, bidi embeddings/overrides or isolates.
bool IsForbiddenUserScalar(uint32 scalar) noexcept
{
    const bool isControl = scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f);
    const bool isEmbeddingOrOverride = scalar >= 0x202a && scalar <= 0x202e;
    const bool isIsolate = scalar >= 0x2066 && scalar <= 0x2069;
    return isControl == true || isEmbeddingOrOverride == true || isIsolate == true;
}

bool IsValidUtf8(std::string_view text) noexcept
{
    foundation::Utf8View view;
    usize errorOffset = 0;
    return foundation::ValidateUtf8(text, view, errorOffset) == foundation::StringStatus::Ok;
}

bool IsValidTextArgument(const ArgumentRecord& schema, std::string_view text) noexcept
{
    if (text.size() > schema.MaxBytes || IsValidUtf8(text) == false)
    {
        return false;
    }
    for (usize offset = 0; offset < text.size();)
    {
        const uint32 scalar = DecodeScalar(text, offset);
        if (scalar == 0 || (schema.Origin == TextOrigin::User && IsForbiddenUserScalar(scalar) == true))
        {
            return false;
        }
    }
    return true;
}

/// Validates value against one declaration; writes the backend representation on success.
bool ResolveArgument(const Catalog& catalog,
                     const ArgumentRecord& schema,
                     const ArgumentValue& value,
                     ResolvedArgument& outResolved) noexcept
{
    ResolvedArgument resolved{ .Name = schema.Name, .Type = schema.Type, .Integer = 0, .Number = 0.0, .Text = {} };
    switch (schema.Type)
    {
        case ArgumentType::Text:
            if (value.Kind != ArgumentValueKind::Text || IsValidTextArgument(schema, value.Text) == false)
            {
                return false;
            }
            resolved.Text = value.Text;
            break;
        case ArgumentType::Count:
            if (value.Kind != ArgumentValueKind::Integer || value.Integer < schema.Minimum ||
                value.Integer > schema.Maximum)
            {
                return false;
            }
            resolved.Integer = value.Integer;
            break;
        case ArgumentType::Number:
            if (value.Kind == ArgumentValueKind::Integer)
            {
                if (value.Integer < schema.Minimum || value.Integer > schema.Maximum)
                {
                    return false;
                }
                resolved.Number = static_cast<float64>(value.Integer);
            }
            else
            {
                // Comparisons with NaN are false and the bounds are finite, so
                // every nonfinite value fails the range.
                const bool isInRange = value.Number >= static_cast<float64>(schema.Minimum) &&
                                       value.Number <= static_cast<float64>(schema.Maximum);
                if (value.Kind != ArgumentValueKind::Number || isInRange == false)
                {
                    return false;
                }
                resolved.Number = value.Number;
            }
            break;
        case ArgumentType::Enum: {
            if (value.Kind != ArgumentValueKind::Text)
            {
                return false;
            }
            bool isDeclared = false;
            for (uint32 i = 0; i < schema.ValueCount && isDeclared == false; ++i)
            {
                isDeclared = CatalogAccess::GetEnumValue(catalog, schema.FirstValue + i) == value.Text;
            }
            if (isDeclared == false && (schema.Unknown == UnknownPolicy::Reject || IsValidUtf8(value.Text) == false))
            {
                return false;
            }
            // An accepted unknown token selects the mandatory `other` branch.
            resolved.Text = isDeclared == true ? value.Text : OTHER_TOKEN;
            break;
        }
    }
    outResolved = resolved;
    return true;
}

/// Places every supplied value at its declaration; any schema violation fails as a whole.
bool ResolveArguments(const Catalog& catalog,
                      const MessageRecord& message,
                      std::span<const ArgumentValue> arguments,
                      std::span<ResolvedArgument> outResolved) noexcept
{
    if (arguments.size() != message.ArgumentCount)
    {
        return false;
    }
    foundation::StaticArray<bool, MAX_ARGUMENTS> isSeen{};
    for (const ArgumentValue& value : arguments)
    {
        bool isDeclared = false;
        for (uint32 i = 0; i < message.ArgumentCount && isDeclared == false; ++i)
        {
            const ArgumentRecord schema = CatalogAccess::GetArgument(catalog, message.FirstArgument + i);
            if (schema.Name != value.Name)
            {
                continue;
            }
            if (isSeen[i] == true || ResolveArgument(catalog, schema, value, outResolved[i]) == false)
            {
                return false;
            }
            isSeen[i] = true;
            isDeclared = true;
        }
        if (isDeclared == false)
        {
            return false;
        }
    }
    // Equal counts plus no duplicate or undeclared name means every declaration was supplied.
    return true;
}

/// RAII release of a temporary index array allocated from a domain.
class IndexScratch final
{
public:
    IndexScratch(const foundation::AllocationDomain& domain, usize count) noexcept
        : mDomain(domain), mCount(count),
          mIndicesOrNull(count == 0 ? nullptr
                                    : static_cast<uint32*>(domain.TryAllocate(count * sizeof(uint32), alignof(uint32))))
    {
    }
    ~IndexScratch() noexcept
    {
        mDomain.Free(mIndicesOrNull, mCount * sizeof(uint32), alignof(uint32));
    }
    IndexScratch(const IndexScratch&) = delete;
    IndexScratch& operator=(const IndexScratch&) = delete;
    IndexScratch(IndexScratch&&) = delete;
    IndexScratch& operator=(IndexScratch&&) = delete;

    [[nodiscard]] bool IsAllocated() const noexcept
    {
        return mCount == 0 || mIndicesOrNull != nullptr;
    }
    [[nodiscard]] uint32* GetData() const noexcept
    {
        return mIndicesOrNull;
    }

private:
    const foundation::AllocationDomain& mDomain;
    usize mCount;
    uint32* mIndicesOrNull;
};
} // namespace

FormatterContext::FormatterContext(FormatterContext&& other) noexcept
    : mLease(std::move(other.mLease)), mBackend(std::move(other.mBackend)),
      mPreparedCount(std::exchange(other.mPreparedCount, 0))
{
}

FormatterContext& FormatterContext::operator=(FormatterContext&& other) noexcept
{
    if (this != &other)
    {
        // Destroy the ICU state before releasing the lease it formats from.
        mBackend = std::move(other.mBackend);
        mLease = std::move(other.mLease);
        mPreparedCount = std::exchange(other.mPreparedCount, 0);
    }
    return *this;
}

bool FormatterContext::IsValid() const noexcept
{
    return mBackend.Get() != nullptr;
}

usize FormatterContext::GetPreparedCount() const noexcept
{
    return mPreparedCount;
}

Status PrepareFormatterContext(const Catalog& lease,
                               std::span<const MessageBinding> messages,
                               const foundation::AllocationDomain& domain,
                               FormatterContext& outContext) noexcept
{
    if (lease.IsValid() == false)
    {
        return Status::Invalid;
    }
    if (CatalogAccess::GetVersion(lease) != 2)
    {
        return Status::Unsupported;
    }
    if (messages.size() > MAX_PREPARED_MESSAGES)
    {
        return Status::Limit;
    }
    for (const MessageBinding binding : messages)
    {
        if (CatalogAccess::IsBindingOf(lease, binding) == false)
        {
            return Status::WrongCatalog;
        }
    }
    const Status available = detail::CheckFormatterBackend(lease.GetLinguisticData());
    if (available != Status::Ok)
    {
        return available;
    }
    IndexScratch scratch(domain, messages.size());
    if (scratch.IsAllocated() == false)
    {
        return Status::OutOfMemory;
    }
    usize count = 0;
    for (const MessageBinding binding : messages)
    {
        const uint32 index = CatalogAccess::GetIndex(binding);
        if (CatalogAccess::GetMessage(lease, index).IsDynamic == true)
        {
            scratch.GetData()[count++] = index;
        }
    }
    foundation::Sort(scratch.GetData(), count, [](uint32 left, uint32 right) noexcept { return left < right; });
    usize distinct = 0;
    for (usize i = 0; i < count; ++i)
    {
        if (distinct == 0 || scratch.GetData()[distinct - 1] != scratch.GetData()[i])
        {
            scratch.GetData()[distinct++] = scratch.GetData()[i];
        }
    }
    FormatterContext candidate;
    candidate.mLease = lease;
    const Status created = detail::CreateFormatterBackend(candidate.mLease,
                                                          std::span<const uint32>(scratch.GetData(), distinct),
                                                          domain,
                                                          candidate.mBackend);
    if (created != Status::Ok)
    {
        return created;
    }
    candidate.mPreparedCount = distinct;
    outContext = std::move(candidate);
    return Status::Ok;
}

Status Format(FormatterContext& inoutContext,
              MessageBinding binding,
              std::span<const ArgumentValue> arguments,
              std::span<char> output,
              FormatResult& outResult) noexcept
{
    outResult = {};
    if (inoutContext.IsValid() == false)
    {
        return Status::Invalid;
    }
    const Catalog& lease = inoutContext.mLease;
    if (CatalogAccess::IsBindingOf(lease, binding) == false)
    {
        return Status::WrongCatalog;
    }
    const uint32 index = CatalogAccess::GetIndex(binding);
    const MessageRecord message = CatalogAccess::GetMessage(lease, index);
    if (message.IsDynamic == true && detail::IsPrepared(*inoutContext.mBackend.Get(), index) == false)
    {
        return Status::NotPrepared;
    }
    foundation::StaticArray<ResolvedArgument, MAX_ARGUMENTS> resolved{};
    if (ResolveArguments(lease, message, arguments, std::span<ResolvedArgument>(resolved.GetData(), MAX_ARGUMENTS)) ==
        false)
    {
        return Status::InvalidArguments;
    }
    usize required = 0;
    if (message.IsDynamic == false)
    {
        // A static record is the cooked ICU-decoded text; no backend is involved.
        required = message.Value.size();
        if (required > output.size())
        {
            outResult.RequiredBytes = required;
            return Status::BufferTooSmall;
        }
        if (required != 0)
        {
            std::memcpy(output.data(), message.Value.data(), required);
        }
    }
    else
    {
        const Status formatted =
            detail::FormatWithBackend(*inoutContext.mBackend.Get(),
                                      index,
                                      std::span<const ResolvedArgument>(resolved.GetData(), message.ArgumentCount),
                                      output,
                                      required);
        if (formatted != Status::Ok)
        {
            outResult.RequiredBytes = formatted == Status::BufferTooSmall ? required : 0;
            return formatted;
        }
    }
    outResult =
    {
        .Written = required,
        .RequiredBytes = required,
        .Provenance = CatalogAccess::GetOrigin(lease, message.Provider),
        .ProviderLocale = CatalogAccess::GetProviderLocale(lease, message.Provider),
    };
    return Status::Ok;
}
} // namespace ludus::localization
