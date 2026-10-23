#include "internal/qt_accessibility.h"

#include <ludus/foundation/base/types.h>

#include <QAccessible>
#include <QByteArray>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstring>

namespace ludus::editor
{
namespace
{
using ReleaseElement = void (*)(id, SEL);
using RemoveElements = void (*)(id, SEL, NSArray*);

struct AccessibilityOwnershipHooks
{
    Ivar IdentityOrNull = nullptr;
    Ivar SynthesizedRoleOrNull = nullptr;
    ReleaseElement PreviousReleaseOrNull = nullptr;
    RemoveElements PreviousRemoveOrNull = nullptr;
};
AccessibilityOwnershipHooks ownershipHooks;
constexpr foundation::uint32 REMOVE_METHOD_ARGUMENT_COUNT = 3;

// Thanks to The Qt Company, qtbase 6.12.0, qcocoaaccessibilityelement.mm,
// initWithId:role:, populateTableRow:, removeElementsFromCache: and dealloc:
// https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/cocoa/qcocoaaccessibilityelement.mm
// Synthetic rows/columns/cells borrow the table's axid. Deleting that identity
// when a selected cell replaces a placeholder destroys the table and the other
// selected interfaces. Keep Qt's real-element cleanup, but never delete borrowed IDs.
void ReleaseAccessibleElement(id element, SEL selector) noexcept
{
    if (object_getIvar(element, ownershipHooks.SynthesizedRoleOrNull) != nil)
    {
        const QAccessible::Id identity = 0;
        std::memcpy(reinterpret_cast<foundation::uint8*>(element) + ivar_getOffset(ownershipHooks.IdentityOrNull),
                    &identity,
                    sizeof(identity));
    }
    ownershipHooks.PreviousReleaseOrNull(element, selector);
}

void RemoveOwnedAccessibleElements(id elementClass, SEL selector, NSArray* elements) noexcept
{
    NSMutableArray* owners = [NSMutableArray arrayWithCapacity:elements.count];
    for (id element in elements)
    {
        if (object_getIvar(element, ownershipHooks.SynthesizedRoleOrNull) == nil)
        {
            [owners addObject:element];
        }
    }
    ownershipHooks.PreviousRemoveOrNull(elementClass, selector, owners);
}
} // namespace

bool InstallQtAccessibilityOwnershipFix() noexcept
{
    if (qstrcmp(qVersion(), "6.12.0") != 0 || QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        ownershipHooks.PreviousReleaseOrNull != nullptr)
    {
        return true;
    }
    Class elementClassOrNull = NSClassFromString(@"QMacAccessibilityElement");
    if (elementClassOrNull == Nil)
    {
        return false;
    }
    Ivar identityOrNull = class_getInstanceVariable(elementClassOrNull, "axid");
    Ivar synthesizedRoleOrNull = class_getInstanceVariable(elementClassOrNull, "synthesizedRole");
    Method releaseOrNull = class_getInstanceMethod(elementClassOrNull, sel_registerName("dealloc"));
    Method removeOrNull = class_getClassMethod(elementClassOrNull, sel_registerName("removeElementsFromCache:"));
    if (identityOrNull == nullptr || synthesizedRoleOrNull == nullptr || releaseOrNull == nullptr ||
        removeOrNull == nullptr || qstrcmp(ivar_getTypeEncoding(identityOrNull), @encode(QAccessible::Id)) != 0 ||
        ivar_getTypeEncoding(synthesizedRoleOrNull)[0] != '@' || method_getNumberOfArguments(releaseOrNull) != 2 ||
        method_getNumberOfArguments(removeOrNull) != REMOVE_METHOD_ARGUMENT_COUNT)
    {
        return false;
    }
    ownershipHooks.IdentityOrNull = identityOrNull;
    ownershipHooks.SynthesizedRoleOrNull = synthesizedRoleOrNull;
    ownershipHooks.PreviousReleaseOrNull = reinterpret_cast<ReleaseElement>(
        method_setImplementation(releaseOrNull, reinterpret_cast<IMP>(&ReleaseAccessibleElement)));
    ownershipHooks.PreviousRemoveOrNull = reinterpret_cast<RemoveElements>(
        method_setImplementation(removeOrNull, reinterpret_cast<IMP>(&RemoveOwnedAccessibleElements)));
    return true;
}
} // namespace ludus::editor
