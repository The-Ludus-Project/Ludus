#include <catch2/catch_test_macros.hpp>

#if !defined(__cpp_exceptions)
#    error "Catch2 test targets must enable exceptions"
#endif

TEST_CASE("test-only exception policy permits unwinding", "[build-contract]")
{
    bool destroyed = false;
    struct Cleanup
    {
        bool& Destroyed;
        ~Cleanup()
        {
            Destroyed = true;
        }
    };

    const auto fail = [&destroyed] {
        const Cleanup cleanup{destroyed};
        // This test deliberately throws a primitive to verify the test-only
        // exception boundary. Production exceptions remain compile-time errors.
        throw 7; // NOLINT(bugprone-std-exception-baseclass)
    };
    CHECK_THROWS_AS(fail(), int);
    CHECK(destroyed);
}
