#include "preprocessor_test_support.hpp"

#include <ludus/tooling/shader_language/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
namespace foundation = ludus::foundation;
namespace language = ludus::tooling::shader_language;
using language::test::Snapshot;

/// The result of lowering a snapshot, with its text and diagnostics as strings.
struct Lowered final
{
    language::Status Outcome = language::Status::InvalidRequest;
    std::string Text;
    std::string Diagnostics;
    language::LoweringResult Result;
};

Lowered LowerSnapshot(Snapshot& inoutSnapshot,
                      std::string_view root = "main.lusl",
                      const language::LoweringOptions& options = language::LoweringOptions{})
{
    // Run builds the request's views; LowerToSlang preprocesses it again.
    (void)inoutSnapshot.Run(root);
    Lowered lowered;
    lowered.Outcome = language::LowerToSlang(inoutSnapshot.GetRequest(), options, lowered.Result);
    lowered.Text.assign(lowered.Result.Text.GetData(), lowered.Result.Text.GetSize());
    for (const language::Diagnostic& diagnostic : lowered.Result.Diagnostics)
    {
        lowered.Diagnostics += language::GetDiagnosticCodeName(diagnostic.Code);
        lowered.Diagnostics += ": ";
        lowered.Diagnostics += diagnostic.Message.GetView();
        lowered.Diagnostics += '\n';
    }
    return lowered;
}

Lowered Lower(std::string text, const language::LoweringOptions& options = language::LoweringOptions{})
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", std::move(text));
    return LowerSnapshot(snapshot, "main.lusl", options);
}

bool Contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

bool HasCode(const Lowered& lowered, language::DiagnosticCode code)
{
    for (const language::Diagnostic& diagnostic : lowered.Result.Diagnostics)
    {
        if (diagnostic.Code == code)
        {
            return true;
        }
    }
    return false;
}

language::LoweringOptions WithBoundsChecks()
{
    language::LoweringOptions options;
    options.Emit.ShouldCheckBounds = true;
    return options;
}
} // namespace

TEST_CASE("LUSL lowering spells types, constants and literals", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(static const uint32 COUNT = 4;
static const int32 LOWEST = -2147483647 - 1;
static const int64 BIG = 9000000000;
static const float32 HALF = 0.5;
static const float64 PRECISE = 0.1;
static const float16 SMALL = 2;
static const uint32 MASK = COUNT * 2 + 1;
float3 Make(in float32 x, in uint32x2 pair, in float32x4x4 transform)
{
    int16 small = int16(pair.x);
    return float32x3(x, 1, float32(small)) * (transform * float32x4(x)).xyz;
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text, "static const uint COUNT = 4u;"));
    CHECK(Contains(lowered.Text, "static const int LOWEST = (-2147483647 - 1);"));
    CHECK(Contains(lowered.Text, "static const int64_t BIG = 9000000000ll;"));
    CHECK(Contains(lowered.Text, "static const float HALF = 0.5f;"));
    CHECK(Contains(lowered.Text, "static const double PRECISE = 0.1l;"));
    CHECK(Contains(lowered.Text, "static const half SMALL = 2.0h;"));
    // Constant expressions are emitted as their exact folded value.
    CHECK(Contains(lowered.Text, "static const uint MASK = 9u;"));
    CHECK(Contains(lowered.Text, "float3 Make(float x, uint2 pair, float4x4 transform)"));
    CHECK(Contains(lowered.Text, "int16_t small = int16_t(pair.x);"));
    // Matrix-vector products become mul; component-wise vector products stay `*`.
    CHECK(Contains(lowered.Text, "return float3(x, 1.0f, float(small)) * mul(transform, float4(x)).xyz;"));
}

TEST_CASE("LUSL lowering writes explicit zero values", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(static const uint32 COUNT = 3;
struct Inner
{
    float32x2 Uv;
};
struct Hit
{
    Inner Part;
    float32 Weights[COUNT];
    float32x3x3 Basis;
    bool IsFront;
};
float32 Read()
{
    Hit hit = 0;
    float32x3 v = 0;
    float32 w[COUNT] = 0;
    hit = 0;
    return hit.Weights[0] + v.x + w[1];
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text, "Inner _luslZero"));
    CHECK(Contains(lowered.Text, "    value.Uv = float2(0.0f);\n"));
    CHECK(Contains(lowered.Text, "    value.Basis = float3x3(0.0f);\n"));
    CHECK(Contains(lowered.Text, "    value.IsFront = false;\n"));
    CHECK(Contains(lowered.Text, "        value[i] = 0.0f;\n"));
    CHECK(Contains(lowered.Text, "float3 v = float3(0.0f);"));
    CHECK(Contains(lowered.Text, "float w[3] = _luslZero"));
    CHECK(Contains(lowered.Text, "typealias _luslArray"));
}

TEST_CASE("LUSL lowering builds designated initializers with defaults and zeros", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(struct Surface
{
    float32x3 Albedo = float32x3(0.5);
    float32 Roughness;
    uint32 Id = 7;
};
Surface Make(in float32 roughness)
{
    Surface first = { .Roughness = roughness };
    Surface second = { .Roughness = 1.0 };
    Surface third = { .Id = 2, .Roughness = 0.25 };
    first.Id = second.Id + third.Id;
    return first;
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    // One helper per distinct field list; omitted fields take their default, else zero.
    CHECK(Contains(lowered.Text,
                   "Surface _luslInit0(float _luslParameter0)\n{\n    Surface value;\n"
                   "    value.Albedo = float3(0.5f);\n    value.Roughness = _luslParameter0;\n"
                   "    value.Id = 7u;\n"));
    CHECK(Contains(lowered.Text, "Surface first = _luslInit0(roughness);"));
    CHECK(Contains(lowered.Text, "Surface second = _luslInit0(1.0f);"));
    // Written order is parameter order, so values are evaluated as written.
    CHECK(Contains(lowered.Text, "Surface _luslInit1(uint _luslParameter0, float _luslParameter1)"));
    CHECK(Contains(lowered.Text, "Surface third = _luslInit1(2u, 0.25f);"));
}

TEST_CASE("LUSL lowering binds named arguments and defaults positionally", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(float32 Scale(in float32 value, in float32 factor = 2.0, in float32 bias = 0.0)
{
    return value * factor + bias;
}
float32 Sample()
{
    return 1.0;
}
struct Lobe
{
    float32 Weight;
    float32 Evaluate(in float32 cosine, in float32 scale = 1.0)
    {
        return Weight * cosine * scale;
    }
    float32 Twice()
    {
        return Evaluate(scale = Sample(), cosine = 0.5);
    }
};
float32 Use(in float32 x)
{
    float32 a = Scale(x, bias = 1.0);
    float32 b = Scale(factor = 3.0, value = x);
    float32 c = Scale(factor = 3.0, value = Sample());
    Lobe lobe = { .Weight = x };
    return a + b + c + lobe.Evaluate(scale = Sample(), cosine = x) + lobe.Twice();
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text, "float Scale(float value, float factor, float bias)"));
    CHECK(Contains(lowered.Text, "float a = Scale(x, 2.0f, 1.0f);"));
    // Reordered arguments without calls cannot observe the order: they are reordered directly.
    CHECK(Contains(lowered.Text, "float b = Scale(x, 3.0f, 0.0f);"));
    // With a call, an adapter takes them in written order.
    CHECK(Contains(lowered.Text, "float c = _luslCall"));
    CHECK(Contains(lowered.Text, "(3.0f, Sample());"));
    CHECK(Contains(lowered.Text, "return Scale(_luslParameter1, _luslParameter0, 0.0f);"));
    // Method adapters live in an extension of their struct and serve implicit `this` calls too.
    CHECK(Contains(lowered.Text, "extension Lobe\n{\n"));
    CHECK(Contains(lowered.Text, "return this.Evaluate(_luslParameter1, _luslParameter0);"));
    CHECK(Contains(lowered.Text, "lobe._luslCall"));
    CHECK(Contains(lowered.Text, "return this._luslCall"));
}

TEST_CASE("LUSL lowering wraps signed arithmetic and masks shifts", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(int32 Wrap(in int32 a, in int32 b, in uint32 u, in uint32 count)
{
    int32 c = a * b - a;
    c += b;
    c++;
    int32x2 v = int32x2(a, b) * 3;
    uint32 plain = u * 2 + u;
    plain <<= count;
    uint32 fixed = u << 3;
    return -c + v.x + int32(plain >> count) + int32(fixed);
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text,
                   "    __target_switch\n    {\n    case metal:\n"
                   "        return asint(asuint(a) * asuint(b));\n    default:\n"
                   "        return a * b;\n"));
    CHECK(Contains(lowered.Text, "target = asint(asuint(target) + asuint(value));"));
    CHECK(Contains(lowered.Text, "target = asint(asuint(target) + 1u);"));
    CHECK(Contains(lowered.Text, "int2 _luslWrap"));
    CHECK(Contains(lowered.Text, "return asint(0u - asuint(a));"));
    // Unsigned arithmetic already wraps on every target.
    CHECK(Contains(lowered.Text, "uint plain = (u * 2u) + u;"));
    CHECK(Contains(lowered.Text, "plain <<= (count & 31u);"));
    CHECK(Contains(lowered.Text, "uint fixed = u << 3u;"));
    CHECK(Contains(lowered.Text, "int(plain >> (count & 31u))"));
}

TEST_CASE("LUSL lowering checks runtime indices only in development builds", "[shader-language][emitter]")
{
    const std::string source = R"(static const uint32 COUNT = 4;
float32 Read(in uint32 index, in int32 signedIndex)
{
    float32 w[COUNT] = 0;
    float32x3 v = 0;
    w[index] = 1.0;
    return w[2] + w[signedIndex] + v[index];
}
)";
    const Lowered release = Lower(source);
    INFO(release.Diagnostics);
    REQUIRE(release.Outcome == language::Status::Ready);
    CHECK(Contains(release.Text, "_luslCheckIndex") == false);
    CHECK(Contains(release.Text, language::FAILURE_CODE_VARIABLE) == false);
    CHECK(release.Result.BoundsChecks.IsEmpty() == true);

    const Lowered development = Lower(source, WithBoundsChecks());
    REQUIRE(development.Outcome == language::Status::Ready);
    CHECK(Contains(development.Text, "static uint _luslFailureCode = 0u;"));
    CHECK(Contains(development.Text, "w[_luslCheckIndex1(uint(index), 4u, 1u)] = 1.0f;"));
    // Constant indices are checked by the checker instead.
    CHECK(Contains(development.Text, "w[2u]"));
    CHECK(Contains(development.Text, "w[_luslCheckIndex1(uint(signedIndex), 4u, 2u)]"));
    CHECK(Contains(development.Text, "v[_luslCheckIndex1(uint(index), 3u, 3u)]"));
    REQUIRE(development.Result.BoundsChecks.GetSize() == 3);
    CHECK(development.Result.BoundsChecks[0].FailureCode == 1);
    CHECK(development.Result.BoundsChecks[0].Location.Line == 6);
    CHECK(development.Result.BoundsChecks[0].Location.Column == 7);
    CHECK(development.Result.BoundsChecks[2].Location.Line == 7);
}

TEST_CASE("LUSL lowering clears groupshared memory in the entry points that reach it", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(static const uint32 ROWS = 2;
static const uint32 COLUMNS = 4;
groupshared float32 tile[ROWS][COLUMNS] = 0;
groupshared uint32 counter = 0;
groupshared float32 scratch[COLUMNS] = void;
void Store(in uint32 x, in uint32 y)
{
    tile[y][x] = 1.0;
}
[stage(compute), threads(COLUMNS, ROWS, 1)]
void Fill(in uint32x3 groupThread : GroupThreadId)
{
    Store(groupThread.x, groupThread.y);
    scratch[groupThread.x] = 0.0;
}
[stage(compute), threads(COLUMNS, 1, 1)]
void Count(in uint32 groupIndex : GroupIndex)
{
    counter = groupIndex;
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text, "groupshared float tile[2][4];"));
    CHECK(Contains(lowered.Text,
                   "void Fill(uint3 groupThread : SV_GroupThreadID, uint _luslGroupIndex : SV_GroupIndex)\n{\n"
                   "    for (uint _luslClear = _luslGroupIndex; _luslClear < 8u; _luslClear += 8u)\n"
                   "    {\n        tile[(_luslClear / 4u) % 2u][(_luslClear / 1u) % 4u] = 0.0f;\n    }\n"
                   "    GroupMemoryBarrierWithGroupSync();\n"));
    // An existing GroupIndex parameter is reused; `= void` storage is not cleared.
    CHECK(Contains(lowered.Text, "void Count(uint groupIndex : SV_GroupIndex)\n{\n    if (groupIndex == 0u)\n"));
    CHECK(Contains(lowered.Text, "scratch[(_luslClear") == false);
    CHECK(Contains(lowered.Text, "counter = 0u;"));
    CHECK(Contains(lowered.Text, "[numthreads(4, 2, 1)]"));
}

TEST_CASE("LUSL lowering copies fallthrough cases", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(enum class Mode : uint32
{
    Off,
    On = 3,
};
float32 Pick(in Mode mode)
{
    float32 total = 0;
    switch (mode)
    {
        case Mode::Off:
            total = 1;
            [[fallthrough]];
        case Mode::On:
            total += 2;
            break;
        default:
            break;
    }
    return total;
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text, "enum class Mode : uint\n{\n    Off = 0u,\n    On = 3u,\n};"));
    CHECK(Contains(lowered.Text, "    case Mode::Off:\n        {\n"));
    CHECK(Contains(lowered.Text,
                   "            total = 1.0f;\n        }\n        {\n            total += 2.0f;\n"
                   "            break;\n        }\n"));
    CHECK(Contains(lowered.Text, "[[fallthrough]]") == false);
}

TEST_CASE("LUSL lowering includes extern slang files relative to the generated module", "[shader-language][emitter]")
{
    Snapshot snapshot;
    snapshot.AddFile("shaders/main.lusl",
                     "#include \"../include/math.luslh\"\n#include \"local.luslh\"\nfloat32 Use(in float32x3 c)\n{\n"
                     "    return ComputeLuminance(c) + Twice(1.0);\n}\n");
    snapshot.AddFile(
        "include/math.luslh",
        "#pragma once\nextern slang \"math.slang\"\n{\n    float32 ComputeLuminance(in float32x3 linearRgb);\n};\n"
        "extern slang \"math.slang\"\n{\n    float32 Twice(in float32 x);\n};\n");
    snapshot.AddFile("include/math.slang", "float ComputeLuminance(float3 c) { return c.x; }\n");
    snapshot.AddFile("shaders/local.luslh", "extern slang \"helpers/local.slang\"\n{\n    struct Opaque;\n};\n");
    snapshot.AddFile("helpers/local.slang", "struct Opaque { float x; };\n");
    const Lowered lowered = LowerSnapshot(snapshot, "shaders/main.lusl");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    // Beside the declaring file first, then from the snapshot root; each file once.
    CHECK(Contains(lowered.Text, "#include \"../include/math.slang\"\n#include \"../helpers/local.slang\"\n"));
    CHECK(Contains(lowered.Text, "float ComputeLuminance") == false);

    Snapshot missing;
    missing.AddFile("main.lusl", "extern slang \"absent.slang\"\n{\n    float32 Gone(in float32 x);\n};\n");
    const Lowered failed = LowerSnapshot(missing);
    CHECK(failed.Outcome == language::Status::Failed);
    CHECK(HasCode(failed, language::DiagnosticCode::ExternSlangNotFound) == true);
    CHECK(failed.Text.empty() == true);
}

TEST_CASE("LUSL lowering maps generated lines back to LUSL", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(#define DECLARE(name) float32 name = x * 2.0;

float32 Use(in float32 x)
{
    DECLARE(y)

    return y;
}
)");
    INFO(lowered.Diagnostics);
    REQUIRE(lowered.Outcome == language::Status::Ready);
    CHECK(Contains(lowered.Text,
                   "#line 3 \"main.lusl\"\nfloat Use(float x)\n{\n    float y = x * 2.0f;\n"
                   "#line 7 \"main.lusl\"\n    return y;\n"));
    const foundation::Array<language::SourceMapEntry>& map = lowered.Result.SourceMap;
    REQUIRE(map.GetSize() == 3);
    CHECK(map[0].Location.Line == 3);
    // A statement from a macro maps to the macro's use.
    CHECK(map[1].Location.Line == 5);
    CHECK(map[1].Location.Column == 5);
    CHECK(map[2].Location.Line == 7);
    CHECK(map[2].GeneratedLine == map[1].GeneratedLine + 2);
}

TEST_CASE("LUSL lowering is deterministic", "[shader-language][emitter]")
{
    const std::string source = R"(static const uint32 COUNT = 2;
struct Hit
{
    float32x3 NormalWorld = float32x3(0.0, 0.0, 1.0);
    int32 Count;
};
int32 Use(in int32 a, in uint32 index)
{
    Hit hit = { .Count = a * 2 };
    float32 w[COUNT] = 0;
    return hit.Count + int32(w[index]);
}
)";
    const Lowered first = Lower(source, WithBoundsChecks());
    const Lowered second = Lower(source, WithBoundsChecks());
    INFO(first.Diagnostics);
    REQUIRE(first.Outcome == language::Status::Ready);
    CHECK(first.Text == second.Text);
}

TEST_CASE("LUSL checker reserves names the generated Slang cannot use", "[shader-language][emitter]")
{
    const Lowered keyword = Lower("void Use()\n{\n    int32 let = 0;\n}\n", language::LoweringOptions{});
    CHECK(keyword.Outcome == language::Status::Failed);
    CHECK(HasCode(keyword, language::DiagnosticCode::ReservedName) == true);
    const Lowered prefix = Lower("static const uint32 _luslCount = 1;\n");
    CHECK(HasCode(prefix, language::DiagnosticCode::ReservedName) == true);
    // extern slang declarations mirror Slang names, so they may use any valid Slang name.
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "extern slang \"lib.slang\"\n{\n    float32 __Exported(in float32 value);\n};\n");
    snapshot.AddFile("lib.slang", "float __Exported(float value) { return value; }\n");
    CHECK(HasCode(LowerSnapshot(snapshot), language::DiagnosticCode::ReservedName) == false);
}

TEST_CASE("LUSL checker keeps struct members contiguous across method parameters", "[shader-language][emitter]")
{
    // Regression: method parameters were appended among the struct's members, hiding later methods.
    const Lowered lowered = Lower(R"(struct Lobe
{
    float32 Weight;
    float32 Evaluate(in float32 cosine, in float32 scale)
    {
        return Weight * cosine * scale + Bias();
    }
    float32 Bias()
    {
        return 0.5;
    }
};
float32 Use(in Lobe lobe)
{
    return lobe.Bias() + lobe.Evaluate(1.0, 2.0);
}
)");
    INFO(lowered.Diagnostics);
    CHECK(lowered.Outcome == language::Status::Ready);
}

TEST_CASE("LUSL checker rejects overloads that differ only in directions", "[shader-language][emitter]")
{
    const Lowered lowered = Lower(R"(void Pick(in float32 a, out float32 outB)
{
    outB = a;
}
void Pick(out float32 outA, in float32 b)
{
    outA = b;
}
)");
    CHECK(lowered.Outcome == language::Status::Failed);
    CHECK(HasCode(lowered, language::DiagnosticCode::Redeclaration) == true);
}

TEST_CASE("LUSL EmitSlang rejects inputs that did not check", "[shader-language][emitter]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "void Use()\n{\n}\n");
    REQUIRE(snapshot.Run() == language::Status::Ready);
    language::SyntaxTree tree;
    REQUIRE(language::Parse(snapshot.GetRequest(), snapshot.GetResult(), tree) == language::Status::Ready);
    const language::SemanticModel unchecked;
    language::EmitResult result;
    CHECK(language::EmitSlang(snapshot.GetRequest(),
                              snapshot.GetResult(),
                              tree,
                              unchecked,
                              language::EmitOptions{},
                              result) == language::Status::InvalidRequest);
    CHECK(result.Text.IsEmpty() == true);
}
