#include <ludus/foundation/base/assert.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(LUDUS_TEST_EXPECT_DEBUG)
static_assert(LUDUS_ENABLE_ASSERTS == 1);
static_assert(LUDUS_BREAK_ON_CHECK == 1);
static_assert(LUDUS_ASSERT_DIALOGS_AVAILABLE == 0);
#    if LUDUS_TEST_EXPECT_DEBUG
static_assert(LUDUS_BUILD_FLAVOR_ID == 1);
#        if !defined(LUDUS_BUILD_DEBUG) || !defined(_DEBUG) || !defined(_DLL) || defined(NDEBUG)
#            error "The Visual Studio Debug build must select Debug policy and the debug dynamic CRT"
#        endif
#    else
static_assert(LUDUS_BUILD_FLAVOR_ID == 2);
#        if !defined(LUDUS_BUILD_DEVELOPMENT) || defined(_DEBUG) || !defined(_DLL) || !defined(NDEBUG)
#            error "The Visual Studio Development build must select Development policy and the release dynamic CRT"
#        endif
#    endif
#endif

namespace
{
struct ExplicitBool
{
    bool Value;
    explicit constexpr operator bool() const noexcept
    {
        return Value;
    }
    bool operator!() const = delete;
};

constexpr bool ConstantPass()
{
    LUDUS_ASSERT(true);
    LUDUS_REQUIRE(true);
    return LUDUS_CHECK(ExplicitBool{true});
}
static_assert(ConstantPass());
} // namespace

TEST_CASE("assertion conditions evaluate once and success does not evaluate messages", "[assertions]")
{
    int conditions = 0;
    int messages = 0;
    LUDUS_ASSERT(++conditions == 1, (++messages, "unused assert message"));
    CHECK(conditions == LUDUS_ENABLE_ASSERTS);
    conditions = 0;
    LUDUS_REQUIRE(++conditions == 1, (++messages, "unused require message"));
    CHECK(conditions == 1);
    conditions = 0;
    CHECK(LUDUS_CHECK(++conditions == 1, (++messages, "unused check message")));
    CHECK(conditions == 1);
    CHECK(messages == 0);
}

TEST_CASE("failed Check evaluates condition and message once and returns false", "[assertions]")
{
    int conditions = 0;
    int messages = 0;
    CHECK_FALSE(LUDUS_CHECK(++conditions == 2, (++messages, "expected Check failure")));
    CHECK(conditions == 1);
    CHECK(messages == 1);
    CHECK_FALSE(LUDUS_CHECK(false));
    CHECK_FALSE(LUDUS_CHECK(false, nullptr));
}

TEST_CASE("assertion macros accept explicit bool and are single statements", "[assertions]")
{
    LUDUS_ASSERT(ExplicitBool{true});
    LUDUS_REQUIRE(ExplicitBool{true});
    CHECK(LUDUS_CHECK(ExplicitBool{true}));
    CHECK_FALSE(LUDUS_CHECK(ExplicitBool{false}));
    bool wrong_else = false;
    const bool choose_assert = ConstantPass();
    // Deliberately omit braces to catch a macro that steals the caller's else.
    // NOLINTBEGIN(readability-braces-around-statements)
    if (choose_assert)
        LUDUS_ASSERT(true);
    else
        wrong_else = true;
    if (choose_assert)
        LUDUS_REQUIRE(true);
    else
        wrong_else = true;
    // NOLINTEND(readability-braces-around-statements)
    CHECK_FALSE(wrong_else);
    int comma = 0;
    CHECK(LUDUS_CHECK((++comma, true)));
    CHECK(comma == 1);
}

#if !LUDUS_ENABLE_ASSERTS
TEST_CASE("disabled Assert neither evaluates nor semantically compiles arguments", "[assertions]")
{
    int conditions = 0;
    int messages = 0;
    LUDUS_ASSERT(++conditions == 2, (++messages, "disabled"));
    LUDUS_ASSERT(identifier_that_does_not_exist(), absent_message->member);
    CHECK(conditions == 0);
    CHECK(messages == 0);
}
#endif
