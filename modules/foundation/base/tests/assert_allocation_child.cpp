#include <ludus/foundation/base/assert_format.hpp>

#include "assert_transport.hpp"
#include "internal/diagnostic_format.hpp"

#include <bit>

#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <unistd.h>

using ludus::foundation::usize;

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
bool ludus_assert_test_watching = false;
}
namespace
{
void AllocationAttempt()
{
    if (ludus_assert_test_watching)
    {
        std::_Exit(96);
    }
}
} // namespace

// The linker --wrap ABI mandates these exact symbol names. This exception is
// confined to the test allocation interposer, never engine APIs.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void* __real_malloc(usize);
extern "C" void* __real_calloc(usize, usize);
extern "C" void* __real_realloc(void*, usize);
extern "C" void* __wrap_malloc(usize size)
{
    AllocationAttempt();
    return __real_malloc(size);
}
extern "C" void* __wrap_calloc(usize count, usize size)
{
    AllocationAttempt();
    return __real_calloc(count, size);
}
extern "C" void* __wrap_realloc(void* data, usize size)
{
    AllocationAttempt();
    return __real_realloc(data, size);
}
// NOLINTEND(bugprone-reserved-identifier)

// TSan owns strong new/delete replacements. C allocator wrapping remains active;
// unsanitized and ASan jobs separately exercise our C++ allocation interposer.
#if !__has_feature(thread_sanitizer)
void* operator new(usize size)
{
    AllocationAttempt();
    void* result = __real_malloc(size == 0 ? 1 : size);
    if (result == nullptr)
    {
        std::abort();
    }
    return result;
}
void* operator new[](usize size)
{
    return ::operator new(size);
}
void operator delete(void* data) noexcept
{
    std::free(data);
}
void operator delete[](void* data) noexcept
{
    std::free(data);
}

#endif

int main(int argc, char** argv)
{
    const bool closed = argc > 1 && std::strncmp(argv[1], "closed", 6) == 0;
    if (closed)
    {
        int endpoints[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, endpoints) != 0 ||
            ludus::foundation::diagnostics::ConfigureEmergencySocket(endpoints[0]) == false)
        {
            return 78;
        }
        (void)close(endpoints[0]);
        (void)close(endpoints[1]);
    }
    else
    {
        ConfigureTestTransport();
    }
    ludus_assert_test_watching = true;
    LUDUS_ASSERT(true);
    LUDUS_REQUIRE(true);
    if (LUDUS_CHECK(true) == false)
    {
        return 97;
    }
    if (argc > 1 && std::strcmp(argv[1], "closed") != 0)
    {
        LUDUS_FATAL_F("first fatal allocation probe {}", ludus::foundation::float64{1.234e300});
    }
    if (LUDUS_CHECK_F(false,
                      "{} {} {} {} {} {} {} {}",
                      ludus::foundation::int64{-1},
                      ludus::foundation::uint64{99},
                      ludus::foundation::float64{1.234e-300},
                      true,
                      ludus::foundation::diagnostics::DiagnosticText{"bytes", 5},
                      "literal",
                      ludus::foundation::diagnostics::DiagnosticCString("c-string"),
                      ludus::foundation::diagnostics::DiagnosticAddress(&ludus_assert_test_watching)))
    {
        return 77;
    }
    // Exercise shared libstdc++ float conversion across exponents/mantissas,
    // including exceptional encodings, while allocation interception is active.
    ludus::foundation::uint64 bits = 1;
    for (int i = 0; i < 10000; ++i)
    {
        bits = bits * 6364136223846793005ULL + 1;
        const auto value = std::bit_cast<ludus::foundation::float64>(bits);
        const auto argument = ludus::foundation::diagnostics::detail::MakeDiagnosticArg(value);
        char output[128];
        ludus::foundation::diagnostics::internal::TextWriter writer(output, sizeof(output));
        ludus::foundation::diagnostics::internal::FormatDiagnostic(writer, {"{}", 3}, &argument, 1);
        (void)writer.Finish();
    }
    // First plain failure: no logger or prior diagnostic/TLS initialization.
    if (LUDUS_CHECK(false, "first Check allocation probe"))
    {
        return 98;
    }
    ludus_assert_test_watching = false;
    return 0;
}
