#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/diagnostic_output.hpp>

#include <cstdlib>

inline void ConfigureTestTransport()
{
    const char* descriptor = std::getenv("LUDUS_TEST_DIAGNOSTIC_FD");
    if (descriptor == nullptr)
    {
        return;
    }
    // The test driver passes a small decimal descriptor; reject anything else
    // rather than letting a malformed value silently select descriptor 0.
    constexpr ludus::foundation::int32 MAX_DESCRIPTOR = 1 << 20;
    ludus::foundation::int32 value = 0;
    bool isValid = descriptor[0] != '\0';
    for (const char* digit = descriptor; isValid == true && *digit != '\0'; ++digit)
    {
        isValid = *digit >= '0' && *digit <= '9' && value <= MAX_DESCRIPTOR;
        value = value * 10 + (*digit - '0');
    }
    if (isValid == false || ludus::foundation::diagnostics::ConfigureEmergencySocket(value) == false)
    {
        std::_Exit(79);
    }
}
