// Byte-identity tests for the native scene cooker against the Phase 0 goldens
// captured from the retired Python 3.12 pipeline (tests/goldens/scene_pipeline).

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/math/summation.hpp>

#include "python_compat.h"
#include "scratch_directory.h"

#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/time/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace content = ludus::content;
namespace foundation = ludus::foundation;
namespace detail = ludus::content::scene_cook::detail;
using namespace ludus::content::scene_cook;
namespace timing = ludus::foundation::time;

constexpr std::string_view GOLDENS = LUDUS_SCENE_GOLDENS_DIR;

std::string Repr(float64 value)
{
    foundation::Array<uint8> bytes;
    detail::ByteWriter writer(bytes);
    detail::AppendFloatRepr(writer, value);
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

std::string Hex(const content::Digest& digest)
{
    foundation::Array<uint8> bytes;
    detail::ByteWriter writer(bytes);
    detail::AppendHex(writer, digest.Data);
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

std::string ReadGolden(std::string_view relative)
{
    content::Bytes bytes;
    REQUIRE(content::ReadFile(GOLDENS, relative, MAX_SOURCE_BYTES, bytes) == content::Status::Ok);
    return std::string(bytes.String());
}

std::string_view ViewAsText(const foundation::Array<uint8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

struct OutputRecord
{
    std::string Sha256;
    uint64 Bytes = 0;
};

OutputRecord GoldenOutput(std::string_view name)
{
    const std::string text = ReadGolden("expected/goldens.json");
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError error{};
    foundation::parsing::JsonLimits limits;
    limits.MaxObjectMembers = foundation::parsing::MAX_JSON_OBJECT_MEMBERS;
    limits.MaxInputBytes = usize{16} << 20;
    limits.MaxDepth = 64;
    const auto status = document.Read(text, error, limits);
    INFO(static_cast<int>(error.Limit));
    REQUIRE(status == foundation::parsing::ParseStatus::Ok);
    const auto record = document.Root().Get("outputs").Get(name);
    std::string_view sha;
    OutputRecord output;
    REQUIRE(record.Get("sha256").String(sha));
    REQUIRE(record.Get("bytes").Integer(output.Bytes));
    output.Sha256 = std::string(sha);
    return output;
}
} // namespace

TEST_CASE("Python float repr is reproduced", "[scene_cook]")
{
    CHECK(Repr(0.0) == "0.0");
    CHECK(Repr(-0.0) == "-0.0");
    CHECK(Repr(1.0) == "1.0");
    CHECK(Repr(45.0) == "45.0");
    CHECK(Repr(0.1) == "0.1");
    CHECK(Repr(-2.5) == "-2.5");
    CHECK(Repr(1e-05) == "1e-05");
    CHECK(Repr(0.0001) == "0.0001");
    CHECK(Repr(1e16) == "1e+16");
    CHECK(Repr(1234567890123456.0) == "1234567890123456.0");
    CHECK(Repr(0.30000000000000004) == "0.30000000000000004");
    CHECK(Repr(1.5e300) == "1.5e+300");
    CHECK(Repr(5e-324) == "5e-324");
    CHECK(Repr(123.456) == "123.456");
}

TEST_CASE("Python sum() compensation is reproduced", "[scene_cook]")
{
    math::CompensatedSum sum;
    for (const float64 value : {1e100, 1.0, -1e100, 1.0})
    {
        sum.Add(value);
    }
    CHECK(sum.GetResult() == 2.0);
    math::CompensatedSum zero;
    zero.Add(-0.0);
    CHECK(Repr(zero.GetResult()) == "0.0");
}

TEST_CASE("Python float() and int() syntax is accepted", "[scene_cook]")
{
    foundation::Array<float64> values;
    const std::string_view tokens[] = {"+1", "007.5", ".5", "5.", "1_000.25", "-0", "1e3", "2E-2_0"};
    REQUIRE(detail::ParseFloats(tokens, values) == detail::NumberResult::Ok);
    CHECK(values[0] == 1.0);
    CHECK(values[1] == 7.5);
    CHECK(values[2] == 0.5);
    CHECK(values[3] == 5.0);
    CHECK(values[4] == 1000.25);
    CHECK(Repr(values[5]) == "-0.0");
    CHECK(values[6] == 1000.0);
    CHECK(values[7] == 2e-20);
    const std::string_view nonfinite[] = {"1", "inf"};
    CHECK(detail::ParseFloats(nonfinite, values) == detail::NumberResult::Nonfinite);
    const std::string_view overflow[] = {"1e400"};
    CHECK(detail::ParseFloats(overflow, values) == detail::NumberResult::Nonfinite);
    const std::string_view invalid[] = {"1__0"};
    CHECK(detail::ParseFloats(invalid, values) == detail::NumberResult::Syntax);
    int64 integer = 0;
    CHECK(detail::ParseInt("-0_7", integer));
    CHECK(integer == -7);
    CHECK_FALSE(detail::ParseInt("1.0", integer));
}

TEST_CASE("Cooked fixture is byte-identical to the Python goldens", "[scene_cook][goldens]")
{
    const std::string source = std::string(GOLDENS) + "/source";
    CookResult result;
    CookError error;
    const bool isCooked = Cook(source, result, error);
    INFO(std::string(error.Message.View()));
    REQUIRE(isCooked);

    const OutputRecord bundle = GoldenOutput("cook/scene.luscn");
    CHECK(result.Bundle.GetSize() == bundle.Bytes);
    CHECK(Hex(content::Hash(result.Bundle.AsSpan())) == bundle.Sha256);
    CHECK(ViewAsText(result.Manifest) == ReadGolden("expected/files/cook/manifest.json"));
    CHECK(ViewAsText(result.License) == ReadGolden("expected/files/cook/LICENSE.txt"));
}

TEST_CASE("NamedMaterial selects its own slot after an inline Material", "[scene_cook]")
{
    // An inline Material ahead of MakeNamedMaterial takes a material slot but no
    // name; NamedMaterial must still resolve to the named declaration's slot.
    const std::filesystem::path directory = ludus::scene_cook_tests::MakeScratchDirectory("ludus-scene-cook");
    const std::filesystem::path source = directory / "source";
    std::filesystem::copy(std::string(GOLDENS) + "/source", source, std::filesystem::copy_options::recursive);
    std::ifstream input(source / "scene-v4.pbrt", std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::string scene = buffer.str();
    const std::string::size_type first = scene.find("MakeNamedMaterial");
    REQUIRE(first != std::string::npos);
    scene.insert(first, "Material \"diffuse\" \"rgb reflectance\" [ 0.5 0.5 0.5 ]\n");
    std::ofstream(source / "scene-v4.pbrt", std::ios::binary | std::ios::trunc) << scene;

    CookResult result;
    CookError error;
    const bool isCooked = Cook(source.string(), result, error);
    INFO(std::string(error.Message.View()));
    REQUIRE(isCooked);
    const OutputRecord bundle = GoldenOutput("cook/scene.luscn");
    CHECK(Hex(content::Hash(result.Bundle.AsSpan())) == bundle.Sha256);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Cook writes the three outputs atomically", "[scene_cook]")
{
    const std::string source = std::string(GOLDENS) + "/source";
    CookResult result;
    CookError error;
    REQUIRE(Cook(source, result, error));
    const std::filesystem::path directory = ludus::scene_cook_tests::MakeScratchDirectory("ludus-scene-cook");
    const std::string output = (directory / "nested" / "out").string();
    REQUIRE(WriteCook(output, result, error));
    content::Bytes manifest;
    REQUIRE(content::ReadFile(output, "manifest.json", usize{1} << 20, manifest) == content::Status::Ok);
    CHECK(manifest.String() == ViewAsText(result.Manifest));
    content::Bytes bundle;
    REQUIRE(content::ReadFile(output, "scene.luscn", usize{64} << 20, bundle) == content::Status::Ok);
    CHECK(bundle.Data().size() == result.Bundle.GetSize());
}

TEST_CASE("Cancellation stops the cook", "[scene_cook]")
{
    const std::string source = std::string(GOLDENS) + "/source";
    Cancellation cancellation;
    cancellation.Requested = [](void*) noexcept { return true; };
    CookResult result;
    CookError error;
    CHECK_FALSE(Cook(source, result, error, &cancellation));
    CHECK(error.Result == content::Status::Cancelled);
    CHECK(error.Message.View() == "Scene conversion cancelled");
}

TEST_CASE("Profile violations report the reference messages", "[scene_cook]")
{
    CookResult result;
    CookError error;
    CHECK_FALSE(Cook("/nonexistent/ludus-scene-cook", result, error));
    CHECK(error.Result == content::Status::NotFound);
}

TEST_CASE("ByteWriter appends of a large bundle stay linear", "[scene_cook]")
{
    // Regression: the macOS editor dry run of staircase2 hung in PackBundle because
    // every 4-byte append reallocated the whole bundle (quadratic in its size).
    foundation::Array<uint8> bytes;
    detail::ByteWriter writer(bytes);
    constexpr usize FLOAT_COUNT = usize{16} << 20U; // 64 MiB of float32
    const timing::Timestamp start = timing::Now();
    for (usize index = 0; index < FLOAT_COUNT; ++index)
    {
        writer.AppendFloat32(static_cast<float64>(index));
    }
    timing::Duration elapsed;
    REQUIRE(timing::TryElapsed(start, timing::Now(), elapsed) == timing::TimeStatus::Ok);
    REQUIRE(writer.IsOk() == true);
    CHECK(bytes.GetSize() == FLOAT_COUNT * sizeof(float32));
    // Linear appends take well under a second even in sanitizer builds; quadratic growth takes hours.
    CHECK(elapsed.Nanoseconds < timing::Seconds(30).Nanoseconds);
}
