#include <ludus/foundation/parsing/json.hpp>

#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::parsing;

namespace
{
struct TrackedPool final
{
    alignas(uint64) uint8 Storage[65536]{};
    usize Attempts = 0;
    usize Frees = 0;
    usize Bytes = 0;
    usize Alignment = 0;
    bool Live = false;
    bool Fail = false;
    bool BadCall = false;

    static void* Allocate(void* context, usize bytes, usize alignment) noexcept
    {
        auto& pool = *static_cast<TrackedPool*>(context);
        ++pool.Attempts;
        pool.BadCall |= pool.Live || alignment > alignof(uint64);
        if (pool.Fail || pool.Live || bytes > sizeof(pool.Storage) || alignment > alignof(uint64))
        {
            return nullptr;
        }
        pool.Live = true;
        pool.Bytes = bytes;
        pool.Alignment = alignment;
        return pool.Storage;
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): fixed AllocationDomain callback signature.
    static void Free(void* context, void* pointer, usize bytes, usize alignment) noexcept
    {
        auto& pool = *static_cast<TrackedPool*>(context);
        ++pool.Frees;
        pool.BadCall |=
            pool.Live == false || pointer != pool.Storage || bytes != pool.Bytes || alignment != pool.Alignment;
        pool.Live = false;
    }
};
} // namespace

TEST_CASE("JSON is strict and never exposes a failed document", "[parsing][json]")
{
    JsonDocument document;
    ParseError error;
    const std::string_view invalid[] = {
        "",
        " ",
        "{",
        "[1,]",
        "{\"a\":1,}",
        "{}{}",
        "true false",
        "/*comment*/0",
        "01",
        "NaN",
        "Infinity",
        "1e999",
        R"("\ud800")",
        R"("\udc00")",
        R"("\u000g")",
        "\xef\xbb\xbf{}",
    };
    for (auto input : invalid)
    {
        INFO(input);
        REQUIRE(document.Read(input, error) == ParseStatus::InvalidSyntax);
        CHECK_FALSE(document.Root().Valid());
        CHECK(error.Offset <= input.size());
    }
    REQUIRE(document.Read("\"\xc0\x80\"", error) == ParseStatus::InvalidEncoding);
    CHECK(error.Offset == 1);
    CHECK_FALSE(document.Root().Valid());
    REQUIRE(document.Read(R"({"a":1,"\u0061":2})", error) == ParseStatus::DuplicateKey);
    CHECK(error.Offset == UNKNOWN_BYTE_OFFSET);
    CHECK_FALSE(document.Root().Valid());
    REQUIRE(document.Read(R"({"a\u0000b":1,"a\u0000b":2})", error) == ParseStatus::DuplicateKey);
    REQUIRE(document.Read(R"({"a\u0000b":1,"a":2,"":null})", error) == ParseStatus::Ok);
    uint64 integer = 0;
    CHECK(document.Root().Get(std::string_view{"a\0b", 3}).Integer(integer));
    CHECK(integer == 1);
    CHECK(document.Root().Get(std::string_view{}).Null());
    CHECK_FALSE(document.Root().Get(nullptr).Valid());
}

TEST_CASE("JSON owns copied input and keeps exact integer conversions", "[parsing][json]")
{
    char input[] =
        R"({"text":"hi","max":18446744073709551615,"min":-9223372036854775808,"over":18446744073709551616,"float":1.25,"flag":true,"list":[null,1]})";
    JsonDocument document;
    ParseError error;
    REQUIRE(document.Read({input, sizeof(input) - 1}, error) == ParseStatus::Ok);
    for (char& ch : input)
    {
        ch = 'x';
    }
    const JsonValue root = document.Root();
    REQUIRE(root.Fields({"text", "max", "min", "over", "float", "flag", "list"}));
    CHECK_FALSE(root.Fields({"text"}));
    std::string_view text;
    REQUIRE(root.Get("text").String(text));
    CHECK(text == "hi");
    uint64 number = 7;
    REQUIRE(root.Get("max").Integer(number));
    CHECK(number == std::numeric_limits<uint64>::max());
    int64 signedNumber = 7;
    CHECK_FALSE(root.Get("max").SignedInteger(signedNumber));
    CHECK(signedNumber == 7);
    REQUIRE(root.Get("min").SignedInteger(signedNumber));
    CHECK(signedNumber == std::numeric_limits<int64>::min());
    CHECK_FALSE(root.Get("min").Integer(number));
    CHECK(number == std::numeric_limits<uint64>::max());
    CHECK_FALSE(root.Get("over").Integer(number));
    float64 real = 0;
    REQUIRE(root.Get("over").Number(real)); // Pinned codec falls back to float64 on uint64 overflow.
    CHECK(real > 0);
    REQUIRE(root.Get("float").Number(real));
    CHECK(real == 1.25);
    bool boolean = false;
    REQUIRE(root.Get("flag").Boolean(boolean));
    CHECK(boolean);
    CHECK_FALSE(root.Get("text").Boolean(boolean));
    CHECK(boolean);
    REQUIRE(root.Get("list").Array());
    CHECK(root.Get("list").Count() == 2);
    CHECK(root.Get("list").At(0).Null());
    CHECK_FALSE(root.Get("list").At(2).Valid());
    CHECK_FALSE(root.Get("missing").String(text));
    CHECK(text == "hi");
}

TEST_CASE("JSON admission rejects excessive work before allocating", "[parsing][json]")
{
    TrackedPool pool;
    AllocationDomain domain(&pool, TrackedPool::Allocate, TrackedPool::Free);
    JsonDocument document(domain);
    ParseError error;
    JsonLimits limits;
    limits.MaxInputBytes = 1;
    REQUIRE(document.Read("{}", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::InputBytes);
    CHECK(error.Budget == 1);
    CHECK(error.Observed == 2);
    limits = {};
    limits.MaxWorkspaceBytes = 1;
    REQUIRE(document.Read("{}", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::WorkspaceBytes);
    limits = {};
    const std::string deep(1000, '[');
    REQUIRE(document.Read(deep, error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::NestingDepth);
    CHECK(error.Offset == 33);
    limits.MaxDepth = MAX_JSON_DEPTH + 1;
    REQUIRE(document.Read("0", error, limits) == ParseStatus::LimitExceeded);
    limits = {};
    limits.MaxObjectMembers = MAX_JSON_OBJECT_MEMBERS + 1;
    REQUIRE(document.Read("{}", error, limits) == ParseStatus::LimitExceeded);
    CHECK(pool.Attempts == 0);
    CHECK_FALSE(document.Root().Valid());
}

TEST_CASE("JSON structural limits have exact boundaries", "[parsing][json]")
{
    JsonDocument document;
    ParseError error;
    JsonLimits limits;
    limits.MaxDepth = 0;
    REQUIRE(document.Read("{}", error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read("[0]", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::NestingDepth);
    CHECK(error.Offset == UNKNOWN_BYTE_OFFSET);
    REQUIRE(document.Read(R"("[{\"}]")", error, limits) == ParseStatus::Ok);
    limits = {};
    const std::string exact = std::string(32, '[') + "0" + std::string(32, ']');
    REQUIRE(document.Read(exact, error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read("[" + exact + "]", error, limits) == ParseStatus::LimitExceeded);
    limits.MaxDepth = MAX_JSON_DEPTH;
    REQUIRE(document.Read(std::string(64, '[') + "0" + std::string(64, ']'), error, limits) == ParseStatus::Ok);
    limits = {};
    limits.MaxObjectMembers = 1;
    REQUIRE(document.Read(R"({"a":0})", error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read(R"({"a":0,"b":0})", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::ObjectMembers);
    CHECK(error.Observed == 2);
    limits = {};
    limits.MaxArrayElements = 1;
    REQUIRE(document.Read("[0]", error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read("[0,1]", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::ArrayElements);
    limits = {};
    limits.MaxValues = 2;
    REQUIRE(document.Read("[0]", error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read("[0,1]", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::Values);
    limits = {};
    limits.MaxStringBytes = 2;
    REQUIRE(document.Read(R"("\u00a2")", error, limits) == ParseStatus::Ok);
    REQUIRE(document.Read(R"({"abc":0})", error, limits) == ParseStatus::LimitExceeded);
    CHECK(error.Limit == ParseLimit::StringBytes);
    CHECK_FALSE(document.Root().Valid());
}

TEST_CASE("JSON reuses storage, reports OOM, and frees through the owning domain", "[parsing][json]")
{
    TrackedPool pool;
    AllocationDomain domain(&pool, TrackedPool::Allocate, TrackedPool::Free);
    {
        JsonDocument document(domain);
        ParseError error;
        pool.Fail = true;
        REQUIRE(document.Read("{}", error) == ParseStatus::OutOfMemory);
        CHECK(document.WorkspaceBytes() == 0);
        CHECK_FALSE(document.Root().Valid());
        pool.Fail = false;
        REQUIRE(document.Read(R"({"a":true})", error) == ParseStatus::Ok);
        const usize capacity = document.WorkspaceBytes();
        CHECK(capacity == pool.Bytes);
        REQUIRE(document.Read("{", error) == ParseStatus::InvalidSyntax);
        CHECK_FALSE(document.Root().Valid());
        REQUIRE(document.Read("{}", error) == ParseStatus::Ok);
        CHECK(document.WorkspaceBytes() == capacity);
        CHECK(pool.Attempts == 2);
        document.Reset();
        CHECK_FALSE(document.Root().Valid());
        CHECK(pool.Live);
        REQUIRE(document.Read(R"({"a":true,"b":false})", error) == ParseStatus::Ok);
        CHECK(pool.Attempts == 3); // Growth frees before allocating, keeping peak storage bounded.
        CHECK(pool.Frees == 1);
        JsonDocument moved(std::move(document));
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        CHECK_FALSE(document.Root().Valid());
        CHECK(moved.Root().Object());
        JsonDocument assigned;
        REQUIRE(assigned.Read("0", error) == ParseStatus::Ok);
        assigned = std::move(moved);
        CHECK(assigned.Root().Object());
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        CHECK_FALSE(moved.Root().Valid());
        JsonLimits small;
        small.MaxWorkspaceBytes = 1;
        REQUIRE(assigned.Read("0", error, small) == ParseStatus::LimitExceeded);
        CHECK(assigned.WorkspaceBytes() == 0);
        CHECK_FALSE(pool.Live);
        REQUIRE(assigned.Read("0", error) == ParseStatus::Ok);
        assigned.Release();
        CHECK_FALSE(pool.Live);
        REQUIRE(assigned.Read("null", error) == ParseStatus::Ok);
    }
    CHECK_FALSE(pool.Live);
    CHECK_FALSE(pool.BadCall);
    CHECK(pool.Frees == pool.Attempts - 1);
}

TEST_CASE("Bounded JSON writer round-trips and does not publish failed prefixes", "[parsing][json]")
{
    uint8 storage[256]{};
    JsonWriter writer(storage);
    writer.Raw("[");
    writer.String({"a\0\"\\\n", 5});
    writer.Raw(",");
    writer.Integer(std::numeric_limits<uint64>::max());
    writer.Raw(",");
    writer.Number(-0.125);
    writer.Raw(",");
    writer.Boolean(true);
    writer.Raw("]");
    std::span<const uint8> output;
    REQUIRE(writer.Finish(output) == ParseStatus::Ok);
    const usize count = output.size();
    REQUIRE(writer.Finish(output) == ParseStatus::Ok);
    CHECK(output.size() == count);
    JsonDocument document;
    ParseError error;
    REQUIRE(document.Read({reinterpret_cast<const char*>(output.data()), output.size()}, error) == ParseStatus::Ok);
    std::string_view text;
    REQUIRE(document.Root().At(0).String(text));
    CHECK(text == std::string_view{"a\0\"\\\n", 5});
    writer.Raw("0");
    CHECK(writer.Finish(output) == ParseStatus::InvalidState);

    uint8 exact[2]{};
    JsonWriter exactWriter(exact);
    exactWriter.Integer(0);
    REQUIRE(exactWriter.Finish(output) == ParseStatus::Ok);
    CHECK(output.size() == 2);
    JsonWriter shortWriter(std::span<uint8>{exact, 1});
    shortWriter.Integer(0);
    CHECK(shortWriter.Finish(output) == ParseStatus::LimitExceeded);
    CHECK(output.size() == 2);
    CHECK(output.data() == exact);
    JsonWriter invalid(storage);
    invalid.String("\xff");
    CHECK(invalid.Finish(output) == ParseStatus::InvalidEncoding);
    JsonWriter nonfinite(storage);
    nonfinite.Number(std::numeric_limits<float64>::infinity());
    CHECK(nonfinite.Finish(output) == ParseStatus::InvalidSyntax);
}

TEST_CASE("Truncated documents and byte mutations remain bounded and reusable", "[parsing][json]")
{
    constexpr std::string_view source = R"({"key":[1,true,null,"\u00a2"],"other":{"a":2}})";
    JsonDocument document;
    ParseError error;
    for (usize length = 0; length < source.size(); ++length)
    {
        REQUIRE(document.Read(source.substr(0, length), error) != ParseStatus::Ok);
        CHECK_FALSE(document.Root().Valid());
    }
    std::string mutation(source);
    for (usize offset = 0; offset < source.size(); ++offset)
    {
        for (uint32 byte = 0; byte <= 255; ++byte)
        {
            mutation[offset] = static_cast<char>(byte);
            const auto status = document.Read(mutation, error);
            CHECK(document.Root().Valid() == (status == ParseStatus::Ok));
            CHECK((error.Offset == UNKNOWN_BYTE_OFFSET || error.Offset <= mutation.size()));
        }
        mutation[offset] = source[offset];
    }
    REQUIRE(document.Read(source, error) == ParseStatus::Ok);
}

TEST_CASE("JSON allocation failure during growth leaves no stale document", "[parsing][json]")
{
    TrackedPool pool;
    AllocationDomain domain(&pool, TrackedPool::Allocate, TrackedPool::Free);
    JsonDocument document(domain);
    ParseError error;
    REQUIRE(document.Read("{}", error) == ParseStatus::Ok);
    pool.Fail = true;
    REQUIRE(document.Read("[1,2,3,4]", error) == ParseStatus::OutOfMemory);
    CHECK_FALSE(document.Root().Valid());
    CHECK(document.WorkspaceBytes() == 0);
    CHECK_FALSE(pool.Live);
    CHECK_FALSE(pool.BadCall);
    pool.Fail = false;
    REQUIRE(document.Read("{}", error) == ParseStatus::Ok);
    document.Release();
    CHECK(pool.Frees == 2);
    CHECK(pool.Attempts == 3);
}

TEST_CASE("Object iteration is bounded and preserves outputs on failure", "[parsing][json]")
{
    JsonDocument document;
    ParseError error;
    REQUIRE(document.Read(R"({"a":true,"b":7})", error) == ParseStatus::Ok);
    const auto root = document.Root();
    CHECK(root.MemberCount() == 2);
    std::string_view key = "sentinel";
    JsonValue value = root;
    CHECK_FALSE(root.MemberAt(2, key, value));
    CHECK(key == "sentinel");
    CHECK(value.Value == root.Value);
    REQUIRE(root.MemberAt(1, key, value));
    CHECK(key == "b");
    uint64 integer = 0;
    REQUIRE(value.Integer(integer));
    CHECK(integer == 7);
    CHECK(value.MemberCount() == 0);
    CHECK_FALSE(value.MemberAt(0, key, value));
    CHECK(key == "b");
}
