#include <ludus/foundation/config/json.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::config;
TEST_CASE("Config bundle validates full layer before publication and writes sparse overrides")
{
    const Descriptor schema[]{
        { .Name = "a", .Default = Value::FromUint64(5), .Help = {}, .Units = {}, .Choices = {}, .Persistent = true },
        { .Name = "b", .Default = Value::FromBool(false), .Help = {}, .Units = {}, .Choices = {}, .Persistent = true }};
    Context context;
    Diagnostic error;
    const auto& domain = GetSystemAllocationDomain();
    REQUIRE(context.Initialize(schema, domain, error) == Status::Ok);
    const auto input =
        R"({"version":1,"schema":"test.v1","layer":"preference","values":[{"key":"a","type":"uint64","value":"18446744073709551615","source":"player","line":12}]})";
    REQUIRE(PrepareJson(context, input, Layer::Preference, "test.v1", context.Revision(), domain, error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    uint8 buffer[4096]{};
    std::span<const uint8> output;
    REQUIRE(WriteLayer(context, Layer::Preference, "test.v1", buffer, output) == Status::Ok);
    const std::string_view text{reinterpret_cast<const char*>(output.data()), output.size()};
    CHECK(text.find("18446744073709551615") != std::string_view::npos);
    CHECK(text.find("\"key\":\"b\"") == std::string_view::npos);
    Context roundTrip;
    REQUIRE(roundTrip.Initialize(schema, domain, error) == Status::Ok);
    REQUIRE(PrepareJson(roundTrip, text, Layer::Preference, "test.v1", roundTrip.Revision(), domain, error) ==
            Status::Ok);
    REQUIRE(roundTrip.Commit(roundTrip.Revision(), error) == Status::Ok);
    CHECK(PrepareJson(context, input, Layer::Preference, "wrong.v1", context.Revision(), domain, error) ==
          Status::InvalidBundle);
    CHECK(PrepareJson(context, input, Layer::Project, "test.v1", context.Revision(), domain, error) ==
          Status::InvalidBundle);
    CHECK(PrepareJson(context,
                      R"({"version":1,"version":1})",
                      Layer::Preference,
                      "test.v1",
                      context.Revision(),
                      domain,
                      error) == Status::InvalidBundle);
    const auto preserved = output;
    uint8 tiny[2]{};
    CHECK(WriteLayer(context, Layer::Preference, "test.v1", tiny, output) == Status::LimitExceeded);
    CHECK(output.data() == preserved.data());
}
TEST_CASE("Config integer decoding rejects coercion, overflow, noncanonical strings and preserves outputs")
{
    parsing::JsonDocument document;
    parsing::ParseError error;
    const std::string_view invalid[]{"1", "\"01\"", "\"-0\"", "\"+1\"", "\"18446744073709551616\"", "true", "1.0"};
    for (auto input : invalid)
    {
        REQUIRE(document.Read(input, error) == parsing::ParseStatus::Ok);
        Value value = Value::FromBool(true);
        CHECK(DecodeValue(document.Root(), Type::Uint64, value) == Status::InvalidValue);
        CHECK(value.Kind == Type::Bool);
    }
    REQUIRE(document.Read("\"-9223372036854775808\"", error) == parsing::ParseStatus::Ok);
    Value value;
    REQUIRE(DecodeValue(document.Root(), Type::Int64, value) == Status::Ok);
    CHECK(value.Signed == (-9223372036854775807LL - 1));
}

TEST_CASE("Config JSON adapter rejects wrong shapes, unknown keys, source positions and resource failures")
{
    const Descriptor schema[]{
        { .Name = "a", .Default = Value::FromBool(false), .Help = {}, .Units = {}, .Choices = {}, .Persistent = true }};
    Context context;
    Diagnostic error;
    const auto& domain = GetSystemAllocationDomain();
    REQUIRE(context.Initialize(schema, domain, error) == Status::Ok);
    const std::string_view malformed[]{
        R"({"version":2,"schema":"s","layer":"project","values":[]})",
        R"({"version":1,"schema":"s","layer":"project","values":[],"extra":true})",
        R"({"version":1,"schema":"s","layer":"project","values":[{"key":"z","type":"bool","value":true,"source":"x","line":0}]})",
        R"({"version":1,"schema":"s","layer":"project","values":[{"key":"a","type":"bool","value":true,"source":"x","line":4294967296}]})",
        R"({"version":1,"schema":"s","layer":"project","values":[{"key":"a","type":"bool","value":1,"source":"x","line":0}]})",
    };
    const auto revision = context.Revision();
    for (auto input : malformed)
    {
        CHECK(PrepareJson(context, input, Layer::Project, "s", revision, domain, error) != Status::Ok);
        CHECK(context.Revision() == revision);
        CHECK(context.Commit(revision, error) == Status::InvalidState);
    }
    CHECK(PrepareJson(context, "{", Layer::Project, "s", revision, domain, error) == Status::InvalidBundle);
    CHECK(error.ByteOffset != UNKNOWN_BYTE_OFFSET);
    CHECK(error.Record == 0);
    const AllocationDomain failure(
        nullptr,
        [](void*, usize, usize) noexcept -> void* { return nullptr; },
        [](void*, void*, usize, usize) noexcept {});
    CHECK(PrepareJson(context, "{}", Layer::Project, "s", revision, failure, error) == Status::OutOfMemory);
}

TEST_CASE("Config writer rejects layers exceeding the loader budget even with a larger destination")
{
    constexpr usize kCount = 800;
    StaticString<16> names[kCount];
    Descriptor schema[kCount];
    Assignment edits[kCount];
    char textBytes[256];
    char sourceBytes[128];
    for (auto& byte : textBytes)
    {
        byte = 'x';
    }
    for (auto& byte : sourceBytes)
    {
        byte = 's';
    }
    Value value;
    REQUIRE(Value::FromText(Type::String, {textBytes, sizeof(textBytes)}, value) == Status::Ok);
    for (usize i = 0; i < kCount; ++i)
    {
        uint8 digits[32]{};
        parsing::JsonWriter writer(digits);
        std::span<const uint8> output;
        writer.Integer(i);
        REQUIRE(writer.Finish(output) == parsing::ParseStatus::Ok);
        REQUIRE(names[i].TryAssign("k") == StringStatus::Ok);
        REQUIRE(names[i].TryAppend({reinterpret_cast<const char*>(output.data()), output.size() - 1}) ==
                StringStatus::Ok);
        schema[i].Name = names[i].GetView();
        schema[i].Default = value;
        schema[i].Persistent = true;
        edits[i].Name = names[i].GetView();
        edits[i].Data = value;
        REQUIRE(edits[i].From.Source.TryAssign({sourceBytes, sizeof(sourceBytes)}) == StringStatus::Ok);
    }
    Context context;
    Diagnostic error;
    Limits limits;
    limits.MaxSettings = kCount;
    limits.MaxEdits = kCount;
    REQUIRE(context.Initialize(schema, GetSystemAllocationDomain(), error, limits) == Status::Ok);
    REQUIRE(context.Prepare(Layer::Preference, edits, true, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    uint8 large[MAX_BUNDLE_BYTES * 2]{};
    uint8 marker[1]{};
    std::span<const uint8> output = marker;
    CHECK(WriteLayer(context, Layer::Preference, "big.v1", large, output) == Status::LimitExceeded);
    CHECK(output.data() == marker);
}

TEST_CASE("Exported configuration schema matches native descriptor validation")
{
    constexpr int64 MIN_SIGNED = -9223372036854775807LL - 1;
    constexpr int64 MAX_SIGNED = 9223372036854775807LL;
    constexpr uint64 MAX_UNSIGNED = ~uint64{0};
    constexpr uint64 MIN_UNSIGNED = MAX_UNSIGNED - 2;
    constexpr uint32 SOURCES = SourceMask(Layer::Project) | SourceMask(Layer::Launch);
    constexpr uint32 GROUP = 5;
    constexpr uint32 MAX_TEXT_BYTES = 3;
    Value text;
    REQUIRE(Value::FromText(Type::String, "", text) == Status::Ok);
    const Descriptor schema[]{
        {
            .Name = "signed",
            .Default = Value::FromInt64(0),
            .Help = {},
            .Units = {},
            .Choices = {},
            .MinSigned = MIN_SIGNED,
            .MaxSigned = MAX_SIGNED,
        },
        {
            .Name = "unsigned",
            .Default = Value::FromUint64(MAX_UNSIGNED),
            .Help = {},
            .Units = {},
            .Choices = {},
            .MinUnsigned = MIN_UNSIGNED,
            .MaxUnsigned = MAX_UNSIGNED,
        },
        {
            .Name = "text",
            .Default = text,
            .Help = {},
            .Units = {},
            .Choices = {},
            .AllowedSources = SOURCES,
            .Group = GROUP,
            .MaxTextBytes = MAX_TEXT_BYTES,
        },
        {
            .Name = "real",
            .Default = Value::FromFloat32(0),
            .Help = {},
            .Units = {},
            .Choices = {},
            .MinReal = -0.5,
            .MaxReal = 0.5,
        },
        { .Name = "flag", .Default = Value::FromBool(false), .Help = {}, .Units = {}, .Choices = {} },
    };
    Context context;
    Diagnostic error;
    REQUIRE(context.Initialize(schema, GetSystemAllocationDomain(), error) == Status::Ok);
    constexpr usize SCHEMA_BUFFER_BYTES = 4096;
    uint8 buffer[SCHEMA_BUFFER_BYTES]{};
    std::span<const uint8> output;
    REQUIRE(WriteSchema(context, "bounds.v1", buffer, output) == Status::Ok);
    parsing::JsonDocument document;
    parsing::ParseError parseError;
    REQUIRE(document.Read({reinterpret_cast<const char*>(output.data()), output.size()}, parseError) ==
            parsing::ParseStatus::Ok);
    const parsing::JsonValue settings = document.Root().Get("settings");
    REQUIRE(settings.Array());
    REQUIRE(settings.Count() == context.Schema().size());
    for (usize index = 0; index < settings.Count(); ++index)
    {
        const Descriptor& descriptor = schema[index];
        const parsing::JsonValue setting = settings.At(index);
        std::string_view name;
        REQUIRE(setting.Get("key").String(name));
        CHECK(name == descriptor.Name);
        uint64 sourceMask = 0;
        uint64 group = 0;
        REQUIRE(setting.Get("allowed_sources").Integer(sourceMask));
        REQUIRE(setting.Get("group").Integer(group));
        CHECK(sourceMask == descriptor.AllowedSources);
        CHECK(group == descriptor.Group);
    }

    const struct BoundCase
    {
        usize SettingIndex;
        const char* Field;
        std::string_view Decimal;
        Value Expected;
    } bounds[]{
        {0, "min", "-9223372036854775808", Value::FromInt64(MIN_SIGNED)},
        {0, "max", "9223372036854775807", Value::FromInt64(MAX_SIGNED)},
        {1, "min", "18446744073709551613", Value::FromUint64(MIN_UNSIGNED)},
        {1, "max", "18446744073709551615", Value::FromUint64(MAX_UNSIGNED)},
    };
    const uint64 revision = context.Revision();
    for (const BoundCase& bound : bounds)
    {
        const Descriptor& descriptor = schema[bound.SettingIndex];
        const parsing::JsonValue node = settings.At(bound.SettingIndex).Get(bound.Field);
        std::string_view decimal;
        REQUIRE(node.String(decimal));
        CHECK(decimal == bound.Decimal);
        Assignment edit{ .Name = descriptor.Name, .From = {} };
        REQUIRE(DecodeValue(node, descriptor.Default.Kind, edit.Data) == Status::Ok);
        CHECK(edit.Data == bound.Expected);
        REQUIRE(context.Prepare(Layer::Project, {&edit, 1}, false, revision, error) == Status::Ok);
        context.Discard();
    }

    const Assignment belowUnsigned
    {
        .Name = "unsigned",
        .Data = Value::FromUint64(MIN_UNSIGNED - 1),
        .From = {},
    };
    CHECK(context.Prepare(Layer::Project, {&belowUnsigned, 1}, false, revision, error) == Status::InvalidValue);

    const parsing::JsonValue textSetting = settings.At(2);
    uint64 textLimit = 0;
    REQUIRE(textSetting.Get("max_text_bytes").Integer(textLimit));
    CHECK(textLimit == MAX_TEXT_BYTES);
    CHECK(textSetting.Get("min").Valid() == false);
    Assignment textEdit{ .Name = "text", .From = {} };
    REQUIRE(Value::FromText(Type::String, "éx", textEdit.Data) == Status::Ok);
    REQUIRE(context.Prepare(Layer::Project, {&textEdit, 1}, false, revision, error) == Status::Ok);
    context.Discard();
    CHECK(context.Prepare(Layer::Engine, {&textEdit, 1}, false, revision, error) == Status::InvalidSource);
    REQUIRE(Value::FromText(Type::String, "éxx", textEdit.Data) == Status::Ok);
    CHECK(context.Prepare(Layer::Project, {&textEdit, 1}, false, revision, error) == Status::InvalidValue);

    const parsing::JsonValue real = settings.At(3);
    float64 minimum = 0;
    float64 maximum = 0;
    REQUIRE(real.Get("min").Number(minimum));
    REQUIRE(real.Get("max").Number(maximum));
    CHECK(minimum == schema[3].MinReal);
    CHECK(maximum == schema[3].MaxReal);
    CHECK(real.Get("max_text_bytes").Valid() == false);
    const parsing::JsonValue flag = settings.At(4);
    CHECK(flag.Get("min").Valid() == false);
    CHECK(flag.Get("max").Valid() == false);
    CHECK(flag.Get("max_text_bytes").Valid() == false);

    uint8 tiny[1]{};
    const std::span<const uint8> preserved = output;
    CHECK(WriteSchema(context, "bounds.v1", tiny, output) == Status::LimitExceeded);
    CHECK(output.data() == preserved.data());
    CHECK(output.size() == preserved.size());
}
