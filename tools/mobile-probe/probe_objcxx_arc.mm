// Mobile M0 probe: an ARC Objective-C++ translation unit for Apple targets.
//
// It pins the flags a UIKit host would use (-fobjc-arc, -fno-objc-exceptions,
// -fno-exceptions) and, when the SDK provides UIKit, imports it to prove that the
// pinned upstream Clang accepts the iOS SDK headers and module maps. Build with
// -DLUDUS_PROBE_REQUIRE_UIKIT=1 against an iOS SDK to make a missing UIKit an
// error. See docs/development/mobile-support-plan.md#acceptance-probes.
#import <Foundation/Foundation.h>

#if __has_include(<UIKit/UIKit.h>)
#    import <UIKit/UIKit.h>
#    define LUDUS_PROBE_UIKIT_AVAILABLE 1
#else
#    define LUDUS_PROBE_UIKIT_AVAILABLE 0
#endif

#if defined(LUDUS_PROBE_REQUIRE_UIKIT) && LUDUS_PROBE_UIKIT_AVAILABLE == 0
#    error "The selected SDK has no UIKit; select an iphoneos or iphonesimulator SDK."
#endif

#if !__has_feature(objc_arc)
#    error "The probe must compile with -fobjc-arc."
#endif

#include <ludus/foundation/base/types.h>

using namespace ludus::foundation::core;

namespace
{
constexpr uint32 EXPECTED_LENGTH = 5;
}

/// Returns 0 when ARC allocation, autorelease pools and string handling work.
extern "C" int LudusMobileProbeObjectiveCxxArc() noexcept
{
    @autoreleasepool
    {
        NSString* text = [NSString stringWithUTF8String:"ludus"];
        const uint32 length = static_cast<uint32>([text length]);
#if LUDUS_PROBE_UIKIT_AVAILABLE
        UIScreen* screenOrNull = nil;
        if (screenOrNull != nil)
        {
            return 2;
        }
#endif
        return length == EXPECTED_LENGTH ? 0 : 1;
    }
}
