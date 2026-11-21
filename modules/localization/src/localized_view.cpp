#include "internal/localized_view.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/strings/status.hpp>

#include <span>
#include <string_view>

namespace ludus::localization::detail
{
const Catalog* CatalogGeneration::FindCatalogOrNull(std::string_view name) const noexcept
{
    for (const DomainCatalog& domain : Catalogs)
    {
        if (domain.Name.GetView() == name)
        {
            return &domain.Package;
        }
    }
    return nullptr;
}

namespace
{
using foundation::BorrowedView;
using foundation::OwnershipStatus;
using foundation::StaticArray;
using foundation::StringStatus;

struct BoundMessage final
{
    const Catalog* CatalogOrNull{}; // Only dynamic messages are collected for ICU preparation.
    MessageBinding Binding;
};

using BoundMessages = StaticArray<BoundMessage, MAX_VISIBLE_MESSAGES>;
using FormatBuffer = StaticArray<char, MAX_FORMATTED_BYTES>;

Status BindVisibleMessages(const CatalogGeneration& generation,
                           BorrowedView<const VisibleMessage> messages,
                           LocalizedView& outView,
                           BoundMessages& outDynamicMessages,
                           bool& outHasDynamicMessages) noexcept
{
    outHasDynamicMessages = false;
    for (usize i = 0; i < messages.GetSize(); ++i)
    {
        const VisibleMessage& message = messages[i];
        if (message.Arguments.IsValid() == false)
        {
            return Status::Invalid;
        }
        const Catalog* catalogOrNull = generation.FindCatalogOrNull(message.Descriptor.Key.Domain);
        if (catalogOrNull == nullptr)
        {
            return Status::NotFound;
        }
        MessageBinding binding;
        const Status bound = catalogOrNull->BindMessage(message.Descriptor, binding);
        if (bound != Status::Ok)
        {
            return bound;
        }
        if (message.Descriptor.Arguments.empty() == false)
        {
            outDynamicMessages[i] = {catalogOrNull, binding};
            outHasDynamicMessages = true;
            continue;
        }
        if (message.Arguments.IsEmpty() == false)
        {
            return Status::InvalidArguments;
        }
        const Status resolved = catalogOrNull->ResolveStatic(binding, outView.Entries[i].Resolved);
        if (resolved != Status::Ok)
        {
            return resolved;
        }
    }
    return Status::Ok;
}

Status FormatDomain(const Catalog& catalog,
                    BorrowedView<const VisibleMessage> messages,
                    const BoundMessages& dynamicMessages,
                    const foundation::AllocationDomain& domain,
                    FormatBuffer& inoutBuffer,
                    LocalizedView& inoutView) noexcept
{
    StaticArray<MessageBinding, MAX_VISIBLE_MESSAGES> bindings;
    StaticArray<usize, MAX_VISIBLE_MESSAGES> indices;
    usize count = 0;
    for (usize i = 0; i < messages.GetSize(); ++i)
    {
        if (dynamicMessages[i].CatalogOrNull == &catalog)
        {
            bindings[count] = dynamicMessages[i].Binding;
            indices[count] = i;
            ++count;
        }
    }
    if (count == 0)
    {
        return Status::Ok;
    }
    FormatterContext formatter;
    const Status prepared = PrepareFormatterContext(catalog, {bindings.GetData(), count}, domain, formatter);
    if (prepared != Status::Ok)
    {
        return prepared;
    }
    for (usize i = 0; i < count; ++i)
    {
        const usize index = indices[i];
        const BorrowedView<const ArgumentValue> arguments = messages[index].Arguments;
        FormatResult result;
        const Status formatted = Format(formatter,
                                        bindings[i],
                                        {arguments.GetDataOrNull(), arguments.GetSize()},
                                        {inoutBuffer.GetData(), inoutBuffer.GetSize()},
                                        result);
        if (formatted != Status::Ok)
        {
            return formatted;
        }
        LocalizedEntry& entry = inoutView.Entries[index];
        if (foundation::CreateShared({inoutBuffer.GetData(), result.Written}, domain, entry.Formatted) !=
            StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
        entry.Resolved =
        {
            .Text = entry.Formatted.GetView(),
            .Provenance = result.Provenance,
            .ProviderLocale = result.ProviderLocale,
        };
    }
    return Status::Ok;
}

Status FormatVisibleMessages(const CatalogGeneration& generation,
                             BorrowedView<const VisibleMessage> messages,
                             const BoundMessages& dynamicMessages,
                             const foundation::AllocationDomain& domain,
                             LocalizedView& inoutView) noexcept
{
    foundation::UniquePtr<FormatBuffer, foundation::DomainDeleter<FormatBuffer>> buffer;
    if (foundation::TryCreateOwned(domain, buffer) != OwnershipStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    for (const DomainCatalog& catalog : generation.Catalogs)
    {
        const Status formatted = FormatDomain(catalog.Package, messages, dynamicMessages, domain, *buffer, inoutView);
        if (formatted != Status::Ok)
        {
            return formatted;
        }
    }
    return Status::Ok;
}
} // namespace

Status PrepareLocalizedView(const foundation::SharedPtr<const CatalogGeneration>& generation,
                            uint64 viewRevision,
                            BorrowedView<const VisibleMessage> messages,
                            const foundation::AllocationDomain& domain,
                            foundation::SharedPtr<const LocalizedView>& outView) noexcept
{
    if (messages.GetSize() > MAX_VISIBLE_MESSAGES)
    {
        return Status::Limit;
    }
    if (viewRevision == 0 || messages.IsValid() == false)
    {
        return Status::Invalid;
    }
    foundation::SharedPtr<LocalizedView> view;
    if (foundation::TryMakeShared(domain, view) != OwnershipStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    view->Generation = generation;
    view->Revision = viewRevision;
    view->MessageCount = messages.GetSize();
    BoundMessages dynamicMessages;
    bool hasDynamicMessages = false;
    const Status bound = BindVisibleMessages(*generation, messages, *view, dynamicMessages, hasDynamicMessages);
    if (bound != Status::Ok)
    {
        return bound;
    }
    if (hasDynamicMessages == true)
    {
        const Status formatted = FormatVisibleMessages(*generation, messages, dynamicMessages, domain, *view);
        if (formatted != Status::Ok)
        {
            return formatted;
        }
    }
    outView = foundation::Move(view);
    return Status::Ok;
}
} // namespace ludus::localization::detail
