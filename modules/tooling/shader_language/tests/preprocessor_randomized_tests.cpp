// Seeded robustness test: random token soups with directives must never crash, must keep the
// diagnostic bound, and must give the same result twice. The libFuzzer target
// (preprocessor_fuzzer.cpp) explores further when LUDUS_BUILD_SHADER_LANGUAGE_FUZZERS is on.

#include "preprocessor_test_support.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
using ludus::foundation::uint32;
using ludus::foundation::usize;
using ludus::tooling::shader_language::MAX_DIAGNOSTICS;
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::test::Snapshot;

/// Number of random cases; small enough for a sanitizer build.
constexpr uint32 CASE_COUNT = 1500;
/// Most fragments per random file.
constexpr uint32 MAX_FRAGMENTS = 48;
/// Number of fragments in FRAGMENTS.
constexpr usize FRAGMENT_COUNT = 40;

constexpr ludus::foundation::StaticArray<std::string_view, FRAGMENT_COUNT> FRAGMENTS = {{
    "\n#define A B\n",
    "\n#define B(x) x ## A\n",
    "\n#define C(x, y) #x y\n",
    "\n#define D A A\n",
    "\n#undef A\n",
    "\n#if A\n",
    "\n#if defined(A)\n",
    "\n#ifdef B\n",
    "\n#elif 1\n",
    "\n#else\n",
    "\n#endif\n",
    "\n#include \"h.luslh\"\n",
    "\n#include \"main.lusl\"\n",
    "\n#pragma once\n",
    "\n#error e\n",
    "\n#line 3\n",
    "A",
    "B",
    "C",
    "D",
    "MODE",
    "(",
    ")",
    ",",
    "##",
    "#",
    "!",
    "1",
    "0x7fffffffffffffff",
    "/",
    "\"s\"",
    "'c'",
    "/*",
    "*/",
    "//",
    "\\\n",
    "@",
    "\xc3\xa9",
    " ",
    "\n",
}};

[[nodiscard]] std::string MakeSource(ludus::foundation::math::RandomStream& inoutRandom)
{
    std::string text;
    const uint32 count = inoutRandom.NextUInt32() % MAX_FRAGMENTS;
    for (uint32 i = 0; i < count; ++i)
    {
        text += FRAGMENTS[inoutRandom.NextUInt32() % FRAGMENT_COUNT];
        text += ' ';
    }
    return text;
}
} // namespace

TEST_CASE("LUSL preprocessor survives random sources deterministically", "[shader-language][preprocessor][random]")
{
    ludus::foundation::math::RandomStream random;
    REQUIRE(random.TryReseed(0x4c55534cU, 1) == true);
    for (uint32 i = 0; i < CASE_COUNT; ++i)
    {
        const std::string main = MakeSource(random);
        const std::string header = MakeSource(random);
        Snapshot first;
        first.AddFile("main.lusl", main).AddFile("h.luslh", header).AddConfiguration("MODE", "A B");
        Snapshot second;
        second.AddFile("main.lusl", main).AddFile("h.luslh", header).AddConfiguration("MODE", "A B");
        const Status firstStatus = first.Run();
        const Status secondStatus = second.Run();
        INFO("case " << i << "\nmain:\n" << main << "\nheader:\n" << header);
        CHECK(firstStatus == secondStatus);
        CHECK((firstStatus == Status::Ready || firstStatus == Status::Failed) == true);
        CHECK(first.GetResult().Diagnostics.GetSize() <= MAX_DIAGNOSTICS);
        CHECK(first.GetText() == second.GetText());
        CHECK(first.GetResult().Diagnostics.GetSize() == second.GetResult().Diagnostics.GetSize());
    }
}
