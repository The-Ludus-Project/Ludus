// Mobile M0 probe runner: calls the probe translation units and reports a result.
//
// CTest launches this executable through CMAKE_CROSSCOMPILING_EMULATOR, so it
// only runs on a device or simulator selected with LUDUS_MOBILE_DEVICE.
#include <ludus/foundation/base/types.h>

using namespace ludus::foundation::core;

extern "C" int LudusMobileProbeCxx23() noexcept;
#if defined(LUDUS_MOBILE_PROBE_HAS_OBJCXX)
extern "C" int LudusMobileProbeObjectiveCxxArc() noexcept;
#endif

int main()
{
    int32 result = LudusMobileProbeCxx23();
#if defined(LUDUS_MOBILE_PROBE_HAS_OBJCXX)
    result |= LudusMobileProbeObjectiveCxxArc();
#endif
    return result;
}
