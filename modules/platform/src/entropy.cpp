#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/platform/entropy.h>

#if LUDUS_TARGET_OS == LUDUS_OS_WEB
#    define LUDUS_PLATFORM_HAS_ENTROPY 1
#    include <emscripten.h>
#elif LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_LINUX
#    define LUDUS_PLATFORM_HAS_ENTROPY 1
#    include <unistd.h>
#    if LUDUS_TARGET_OS == LUDUS_OS_MACOS
#        include <sys/random.h>
#    endif
#elif LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    define LUDUS_PLATFORM_HAS_ENTROPY 1
#    include <bcrypt.h>
#    include <windows.h>
#else
#    define LUDUS_PLATFORM_HAS_ENTROPY 0
#endif

namespace ludus::platform
{
namespace
{
using foundation::uint8;
using foundation::usize;

#if LUDUS_PLATFORM_HAS_ENTROPY
/// Largest single request: getentropy(3) rejects more on every supported native
/// target, and the browser path uses the same bound (Web Crypto allows 65536).
constexpr usize MAX_ENTROPY_REQUEST_BYTES = 256;
#endif

#if LUDUS_TARGET_OS == LUDUS_OS_WEB
// Emscripten's getentropy calls crypto.getRandomValues without a guard, so a
// missing or throwing Web Crypto would raise a JavaScript exception through
// wasm instead of reporting failure. Copy through a JavaScript buffer because
// Web Crypto rejects views of shared memory.
// clang-format off
EM_JS(foundation::int32, FillBrowserEntropy, (uint8* outBytes, usize size), {
    try {
        if (typeof globalThis.crypto == 'undefined' || typeof globalThis.crypto.getRandomValues != 'function') {
            return 0;
        }
        const bytes = new Uint8Array(size);
        globalThis.crypto.getRandomValues(bytes);
        HEAPU8.set(bytes, outBytes);
        return 1;
    } catch (_) {
        // JavaScript bridge errors never escape into the exception-free runtime.
        return 0;
    }
});
// clang-format on
#endif

#if LUDUS_PLATFORM_HAS_ENTROPY
bool RequestEntropy(uint8* outBytes, usize size) noexcept
{
#    if LUDUS_TARGET_OS == LUDUS_OS_WEB
    return FillBrowserEntropy(outBytes, size) == 1;
#    elif LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // Thanks to Microsoft, "BCryptGenRandom function", for the handle-free system RNG.
    // https://learn.microsoft.com/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom
    return BCryptGenRandom(nullptr, outBytes, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#    else
    return getentropy(outBytes, size) == 0;
#    endif
}
#endif

bool FillSystemEntropy(void* contextOrNull, uint8* outBytes, usize size) noexcept
{
    (void)contextOrNull;
    return TryFillSystemEntropy(outBytes, size);
}
} // namespace

bool TryFillSystemEntropy(uint8* outBytes, usize size) noexcept
{
    if (outBytes == nullptr)
    {
        return false;
    }
#if LUDUS_PLATFORM_HAS_ENTROPY
    usize offset = 0;
    while (offset < size)
    {
        const usize remaining = size - offset;
        const usize request = remaining < MAX_ENTROPY_REQUEST_BYTES ? remaining : MAX_ENTROPY_REQUEST_BYTES;
        if (RequestEntropy(outBytes + offset, request) == false)
        {
            return false;
        }
        offset += request;
    }
    return true;
#else
    // No audited operating-system source on this target yet: report unavailable
    // rather than substituting a weaker one.
    return size == 0;
#endif
}

foundation::identity::EntropySource GetSystemEntropySource() noexcept
{
    return foundation::identity::EntropySource{ .FillBytesOrNull = FillSystemEntropy, .ContextOrNull = nullptr };
}
} // namespace ludus::platform
