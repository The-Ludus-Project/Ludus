#include "checker_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
using ludus::tooling::shader_language::CallKind;
using ludus::tooling::shader_language::Check;
using ludus::tooling::shader_language::DiagnosticCode;
using ludus::tooling::shader_language::NO_NODE;
using ludus::tooling::shader_language::NO_SYMBOL;
using ludus::tooling::shader_language::PreprocessRequest;
using ludus::tooling::shader_language::PreprocessResult;
using ludus::tooling::shader_language::SemanticModel;
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::SymbolKind;
using ludus::tooling::shader_language::SyntaxKind;
using ludus::tooling::shader_language::SyntaxTree;
using ludus::tooling::shader_language::test::CheckedSource;

/// Declarations most tests share.
constexpr std::string_view PRELUDE = R"(static const uint32 COUNT = 4;
struct Hit
{
    float32x3 PositionWorld;
    uint32 Count = 1;
};
enum class Mode : uint32 { Off, On, Debug = 7 };
extern slang "scene.slang"
{
    struct SceneContext;
    float32 Luminance(in float32x3 linearRgb);
}
)";

/// Check `body` inside `void F(...)` after the prelude; must succeed without diagnostics.
void RequireValid(const std::string& body)
{
    const CheckedSource checked(std::string(PRELUDE) +
                                "void F(float32 x, int32 i, uint32 u, float32x3 v, bool b, Hit h)\n{\n" + body +
                                "\n}\n");
    INFO(body << "\n" << checked.DescribeDiagnostics());
    REQUIRE(checked.GetParseStatus() == Status::Ready);
    CHECK(checked.GetStatus() == Status::Ready);
}

/// Check `body` inside the same function; must fail with `code`.
void RequireError(const std::string& body, DiagnosticCode code)
{
    const CheckedSource checked(std::string(PRELUDE) +
                                "void F(float32 x, int32 i, uint32 u, float32x3 v, bool b, Hit h)\n{\n" + body +
                                "\n}\n");
    INFO(body << "\n" << checked.DescribeDiagnostics());
    REQUIRE(checked.GetParseStatus() == Status::Ready);
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.HasDiagnostic(code) == true);
}

/// Check a whole translation unit after the prelude; must fail with `code`.
void RequireTopLevelError(const std::string& text, DiagnosticCode code)
{
    const CheckedSource checked(std::string(PRELUDE) + text);
    INFO(text << "\n" << checked.DescribeDiagnostics());
    REQUIRE(checked.GetParseStatus() == Status::Ready);
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.HasDiagnostic(code) == true);
}

void RequireTopLevelValid(const std::string& text)
{
    const CheckedSource checked(std::string(PRELUDE) + text);
    INFO(text << "\n" << checked.DescribeDiagnostics());
    REQUIRE(checked.GetParseStatus() == Status::Ready);
    CHECK(checked.GetStatus() == Status::Ready);
}
} // namespace

TEST_CASE("LUSL checker resolves built-in types and aliases", "[shader-language][checker]")
{
    RequireValid("float3 a = v; float32x3 c = a; half k = 0; float16 m = k; float4x4 n = 0; float32x4x4 o = n;");
    RequireValid("int j = i; uint w = u; bool2 p = 0; boolx2 q = p; double d = 0; float64 e = d;");
    RequireError("float32x5 a = 0;", DiagnosticCode::UnknownType);
    RequireError("Unknown a = 0;", DiagnosticCode::UnknownType);
}

TEST_CASE("LUSL checker resolves names in C order", "[shader-language][checker]")
{
    RequireError("y = 1;", DiagnosticCode::UnknownName);
    RequireError("float32 y = 0; float32 y = 1;", DiagnosticCode::Redeclaration);
    RequireValid("float32 y = 0; { float32 y = 1; y = 2; } y = 3;");
    RequireError("float32 y = z; float32 z = 0;", DiagnosticCode::UnknownName);
    RequireTopLevelError("void G() { H(); }\nvoid H() {}\n", DiagnosticCode::UnknownName);
    RequireValid("float32 y = Luminance(v);");
}

TEST_CASE("LUSL checker converts untyped constants only when they fit", "[shader-language][checker]")
{
    RequireValid("float32 a = 1; float32 c = 16777218; uint32 d = 0xffffffff; int16 e = -32768; float32 f = 1.5;");
    RequireValid("float32 g = .5; float32 k = 1.; float32 m = 2e-3; x = 1 + 2 * 3;");
    RequireError("uint32 a = -1;", DiagnosticCode::ConstantOutOfRange);
    RequireError("uint16 a = 70000;", DiagnosticCode::ConstantOutOfRange);
    RequireError("float32 a = 16777217;", DiagnosticCode::ConstantOutOfRange);
    RequireError("int32 a = 1.5;", DiagnosticCode::TypeMismatch);
    RequireError("bool a = 1;", DiagnosticCode::TypeMismatch);
    RequireError("float32x3 a = 1.0;", DiagnosticCode::TypeMismatch);
    RequireError("int32 a = 010;", DiagnosticCode::InvalidLiteral);
    RequireError("float32 a = 1.0f;", DiagnosticCode::InvalidLiteral);
    RequireError("uint32 a = 1u;", DiagnosticCode::InvalidLiteral);
    RequireError("int64 a = 0xffffffffffffffff;", DiagnosticCode::ConstantOutOfRange);
}

TEST_CASE("LUSL checker has no implicit conversions", "[shader-language][checker]")
{
    RequireError("float32 a = i;", DiagnosticCode::TypeMismatch);
    RequireError("uint32 a = i;", DiagnosticCode::TypeMismatch);
    RequireError("float32x3 a = float32x4(0, 0, 0, 0);", DiagnosticCode::TypeMismatch);
    RequireError("x = v;", DiagnosticCode::TypeMismatch);
    RequireError("Mode m = 1;", DiagnosticCode::TypeMismatch);
    RequireValid("float32 a = float32(i); int32 c = int32(x); uint32 d = uint32(Mode::On); Mode m = Mode(u);");
    RequireValid("float32x3 a = float32x3(int32x3(1, 2, 3)); uint32x3 c = uint32x3(v);");
    RequireError("bool a = bool(i);", DiagnosticCode::InvalidConstruction);
    RequireError("int32 a = int32(b);", DiagnosticCode::InvalidConstruction);
}

TEST_CASE("LUSL checker zero-fills any type from the literal 0", "[shader-language][checker]")
{
    RequireValid("Hit a = 0; float32x3 c = 0; float32x4x4 d = 0; float32 e[COUNT] = 0; a = 0; Mode m = 0;");
    RequireTopLevelError("void G() { SceneContext s = 0; }\n", DiagnosticCode::InvalidZeroInitializer);
    RequireError("Hit a = 0.0;", DiagnosticCode::TypeMismatch);
    RequireTopLevelError("void Take(Hit hit) {}\nvoid G() { Take(0); }\n", DiagnosticCode::TypeMismatch);
}

TEST_CASE("LUSL checker types operators", "[shader-language][checker]")
{
    RequireValid("float32x3 a = v * x; float32x3 c = x * v + v; float32x3 d = v * 2; float32x3 e = -v;");
    RequireValid("bool f = x < 1 && b == true; boolx3 g = v == v; uint32 k = u << 2 | 1; int32 m = ~i;");
    RequireValid("float32x4x4 n = 0; float32x4 p = n * float32x4(v, 1.0); float32x4x4 q = n * n * 2;");
    RequireError("float32x3 a = v + float32x4(0, 0, 0, 0);", DiagnosticCode::InvalidOperands);
    RequireError("float32x4x4 n = 0; float32x4 p = float32x4(v, 1.0) * n;", DiagnosticCode::InvalidOperands);
    RequireError("bool a = i && b;", DiagnosticCode::InvalidOperands);
    RequireError("float32 a = x & x;", DiagnosticCode::InvalidOperands);
    RequireError("float32 a = x + i;", DiagnosticCode::InvalidOperands);
    RequireError("bool a = b < b;", DiagnosticCode::InvalidOperands);
    RequireError("float32 a = ~x;", DiagnosticCode::InvalidOperands);
}

TEST_CASE("LUSL checker requires bool conditions", "[shader-language][checker]")
{
    RequireError("if (i) {}", DiagnosticCode::NonBoolCondition);
    RequireError("while (1) {}", DiagnosticCode::NonBoolCondition);
    RequireError("float32 a = i ? 1 : 2;", DiagnosticCode::NonBoolCondition);
    RequireValid("if (i != 0) {} while (b == true) { break; } for (uint32 k = 0; k < COUNT; k++) {}");
}

TEST_CASE("LUSL checker folds integer constants exactly", "[shader-language][checker]")
{
    RequireTopLevelValid("static const int32 HALF = 1 / 2;\nstatic_assert(HALF == 0, \"untyped division\");\n"
                         "static_assert(COUNT * 2 == 8 && -7 / 2 == -3 && (1 << 4) == 16, \"folding\");\n");
    RequireTopLevelError("static_assert(COUNT == 5, \"wrong count\");\n", DiagnosticCode::StaticAssertFailed);
    RequireTopLevelError("static const int64 BIG = 0x7fffffffffffffff + 1;\n", DiagnosticCode::ConstantOverflow);
    RequireTopLevelError("static const int32 BAD = 1 / 0;\n", DiagnosticCode::ConstantOverflow);
    RequireTopLevelError("static const uint32 WRAP = COUNT - 5;\n", DiagnosticCode::ConstantOverflow);
    RequireTopLevelError("static_assert(1.0 == 1.0, \"float\");\n", DiagnosticCode::NotConstant);
}

TEST_CASE("LUSL checker requires named, positive array sizes", "[shader-language][checker]")
{
    RequireValid("float32 w[COUNT] = 0; float32 m[COUNT][COUNT * 2] = 0; w[0] = m[1][7];");
    RequireError("float32 w[4] = 0;", DiagnosticCode::ArraySizeLiteral);
    RequireError("float32 w[u] = 0;", DiagnosticCode::NotConstant);
    RequireTopLevelError("static const int32 NONE = 0;\nvoid G() { float32 w[NONE] = 0; }\n",
                         DiagnosticCode::InvalidArraySize);
    RequireError("float32 w[COUNT] = 0; x = w[COUNT];", DiagnosticCode::ConstantOutOfRange);
    RequireError("float32 w[COUNT] = 0; x = w[x];", DiagnosticCode::TypeMismatch);
}

TEST_CASE("LUSL checker checks structs, members and initializers", "[shader-language][checker]")
{
    RequireValid("Hit a = { .PositionWorld = v, .Count = 2 }; uint32 c = a.Count; float32x2 d = a.PositionWorld.xy;");
    RequireValid("Hit a = {}; a = { .Count = 0 }; float32x4 c = v.xyzx; v.zyx = v; float32x3 d = x.xxx;");
    RequireError("x = h.Missing;", DiagnosticCode::UnknownMember);
    RequireError("float32x2 a = v.xq;", DiagnosticCode::UnknownMember);
    RequireError("float32 a = v.w;", DiagnosticCode::UnknownMember);
    RequireError("v.xx = float32x2(1.0);", DiagnosticCode::NotAssignable);
    RequireError("Hit a = { .Missing = 1 };", DiagnosticCode::InvalidInitializer);
    RequireError("Hit a = { .Count = 1, .Count = 2 };", DiagnosticCode::InvalidInitializer);
    RequireError("x = { .Count = 1 };", DiagnosticCode::InvalidInitializer);
    RequireError("Hit a = Hit(v, 1);", DiagnosticCode::InvalidConstruction);
    RequireTopLevelError("struct Bad { Bad Inner; };\n", DiagnosticCode::TypeMismatch);
    RequireTopLevelError("struct Bad { float32 A = COUNT2; };\n", DiagnosticCode::UnknownName);
    RequireTopLevelError("struct Repeat { float32 A; float32 A; };\n", DiagnosticCode::Redeclaration);
}

TEST_CASE("LUSL checker checks enums and switches", "[shader-language][checker]")
{
    RequireValid("Mode m = Mode::Debug; switch (m) { case Mode::Off: break; case Mode::On: [[fallthrough]]; "
                 "default: break; }");
    RequireValid("switch (u) { case 1: break; case COUNT: break; }");
    RequireError("Mode m = Mode::Missing;", DiagnosticCode::UnknownMember);
    RequireError("switch (u) { case 1: break; case 1: break; }", DiagnosticCode::InvalidSwitch);
    RequireError("switch (x) { default: break; }", DiagnosticCode::InvalidSwitch);
    RequireError("switch (u) { case u: break; }", DiagnosticCode::InvalidSwitch);
    RequireError("switch (u) { case 1: [[fallthrough]]; x = 1; }", DiagnosticCode::InvalidSwitch);
    RequireTopLevelError("enum class Repeat : uint32 { A, A };\n", DiagnosticCode::Redeclaration);
    RequireTopLevelError("enum class Small : uint16 { A = 70000 };\n", DiagnosticCode::ConstantOutOfRange);
    RequireTopLevelError("enum class Floaty : float32 { A };\n", DiagnosticCode::TypeMismatch);
}

TEST_CASE("LUSL checker resolves overloads with named arguments and defaults", "[shader-language][checker]")
{
    const std::string functions = R"(float32 Scale(in float32 value, in float32 factor = 2.0) { return value * factor; }
int32 Scale(in int32 value, in int32 factor = 2) { return value * factor; }
float32 Mix(in float32 low, in float32 high, in float32 weight) { return low + (high - low) * weight; }
)";
    RequireTopLevelValid(functions + "void G(float32 x, int32 i)\n{\n    float32 a = Scale(x);\n    int32 c = Scale(i, "
                                     "factor = 3);\n    float32 d = Mix(weight = 0.5, low = x, high = 1.0);\n}\n");
    RequireTopLevelError(functions + "void G() { float32 a = Scale(1); }\n", DiagnosticCode::AmbiguousCall);
    RequireTopLevelError(functions + "void G(float32 x) { float32 a = Mix(x, 1.0); }\n",
                         DiagnosticCode::MissingArgument);
    RequireTopLevelError(functions + "void G(float32 x) { float32 a = Mix(x, 1.0, 0.5, 1.0); }\n",
                         DiagnosticCode::NoMatchingOverload);
    RequireTopLevelError(functions + "void G(float32 x) { float32 a = Mix(x, 1.0, alpha = 0.5); }\n",
                         DiagnosticCode::UnknownNamedArgument);
    RequireTopLevelError(functions + "void G(float32 x) { float32 a = Mix(x, 1.0, low = 0.5); }\n",
                         DiagnosticCode::DuplicateArgument);
    RequireTopLevelError(functions + "void G(bool b) { float32 a = Scale(b); }\n", DiagnosticCode::NoMatchingOverload);
    RequireTopLevelError("float32 One(in float32 a) { return a; }\nvoid G(int32 i) { float32 a = One(i); }\n",
                         DiagnosticCode::TypeMismatch);
    RequireTopLevelError("void G(float32 x) { x(); }\n", DiagnosticCode::NotCallable);
}

TEST_CASE("LUSL checker matches call marks and rejects aliasing", "[shader-language][checker]")
{
    const std::string functions = R"(void Store(in float32 value, out float32 outValue) { outValue = value; }
void Swap(inout float32 a, inout float32 b) { float32 t = a; a = b; b = t; }
void Fill(out float32x3 outA, out float32x3 outB) { outA = 0; outB = 0; }
)";
    RequireTopLevelValid(functions +
                         "void G(float32x3 v)\n{\n    float32 a = 0;\n    float32 c = 0;\n    Store(1.0, out "
                         "a);\n    Swap(inout a, inout c);\n    Swap(inout v.x, inout v.y);\n}\n");
    RequireTopLevelError(functions + "void G() { float32 a = 0; Store(1.0, a); }\n", DiagnosticCode::DirectionMismatch);
    RequireTopLevelError(functions + "void G() { float32 a = 0; Store(out 1.0, out a); }\n",
                         DiagnosticCode::DirectionMismatch);
    RequireTopLevelError(functions + "void G() { float32 a = 0; Swap(out a, inout a); }\n",
                         DiagnosticCode::DirectionMismatch);
    RequireTopLevelError(functions + "void G() { Store(1.0, out COUNT2); }\nstatic const float32 COUNT2 = 1;\n",
                         DiagnosticCode::UnknownName);
    RequireTopLevelError(functions + "static const float32 FIXED = 1;\nvoid G() { Store(1.0, out FIXED); }\n",
                         DiagnosticCode::NotAssignable);
    RequireTopLevelError(functions + "void G() { float32 a = 0; Swap(inout a, inout a); }\n",
                         DiagnosticCode::AliasedArguments);
    RequireTopLevelError(functions + "void G(float32x3 v) { Fill(out v, out v); }\n", DiagnosticCode::AliasedArguments);
    RequireTopLevelError(functions + "void G(float32x3 v) { Swap(inout v.x, inout v.x); }\n",
                         DiagnosticCode::AliasedArguments);
    RequireTopLevelError(
        "void Pair(out float32 a, out float32 c) { a = 0; c = 0; }\nvoid G(uint32 i, uint32 j)\n{\n    "
        "float32 w[COUNT] = 0;\n    Pair(out w[i], out w[j]);\n}\n",
        DiagnosticCode::AliasedArguments);
    RequireTopLevelValid(
        "void Pair(out float32 a, out float32 c) { a = 0; c = 0; }\nvoid G()\n{\n    float32 w[COUNT] = "
        "0;\n    Pair(out w[0], out w[1]);\n}\n");
}

TEST_CASE("LUSL checker resolves built-in functions", "[shader-language][checker]")
{
    RequireValid("float32 a = dot(v, v); float32x3 c = normalize(v); float32 d = max(x, 0); float32x3 e = lerp(v, v, "
                 "0.5); float32x3 f2 = max(v, 0); float32x3 g2 = clamp(v, x, 1.0);");
    RequireValid("float32x3 f = cross(v, v); uint32 g = asuint(x); float32 k = asfloat(u); bool m = any(v == v);");
    RequireValid("float32 n = length(v); int32 p = clamp(i, 0, 7); uint32 q = countbits(u); float32 r = sqrt(2);");
    RequireError("float32 a = normalize(x);", DiagnosticCode::NoMatchingOverload);
    RequireError("float32 a = dot(v, x);", DiagnosticCode::TypeMismatch);
    RequireError("float32 a = sqrt(i);", DiagnosticCode::NoMatchingOverload);
    RequireError("float32 a = max(x);", DiagnosticCode::NoMatchingOverload);
    RequireError("float32 a = sqrt(value = x);", DiagnosticCode::InvalidConstruction);
}

TEST_CASE("LUSL checker builds vectors and matrices", "[shader-language][checker]")
{
    RequireValid("float32x3 a = float32x3(1.0); float32x4 c = float32x4(v, 1); float32x4 d = float32x4(x, x, v.xy);");
    RequireValid("float32x2x2 m = float32x2x2(1, 0, 0, 1); float32x2x2 n = float32x2x2(v.xy, v.yz);");
    RequireError("float32x3 a = float32x3(1.0, 2.0);", DiagnosticCode::InvalidConstruction);
    RequireError("float32x3 a = float32x3(i, i, i);", DiagnosticCode::InvalidConstruction);
    RequireError("float32x3 a = float32x3(x, x, ray = 1.0);", DiagnosticCode::InvalidConstruction);
}

TEST_CASE("LUSL checker checks methods and mutation", "[shader-language][checker]")
{
    const std::string counter = R"(struct Counter
{
    uint32 Value;
    uint32 Get() { return Value + Step(); }
    uint32 Step() { return 1; }
    [mutating] void Increment() { Value += Step(); }
};
)";
    RequireTopLevelValid(counter + "void G()\n{\n    Counter c = 0;\n    c.Increment();\n    uint32 v = c.Get();\n}\n");
    RequireTopLevelError("struct S { uint32 Value; void Reset() { Value = 0; } };\n", DiagnosticCode::NotAssignable);
    RequireTopLevelError(counter + "struct T { Counter Inner; void Touch() { Inner.Increment(); } };\n",
                         DiagnosticCode::NotAssignable);
    RequireTopLevelError("[mutating] void Free() {}\n", DiagnosticCode::InvalidAttribute);
    RequireTopLevelError(counter + "void G() { Counter c = 0; c.Missing(); }\n", DiagnosticCode::UnknownMember);
    RequireTopLevelError(counter + "void G() { Counter c = 0; uint32 v = c.Get; }\n", DiagnosticCode::TypeMismatch);
}

TEST_CASE("LUSL checker checks returns and jumps", "[shader-language][checker]")
{
    RequireTopLevelValid("Hit Make(float32x3 p) { return { .PositionWorld = p }; }\nfloat32 Half(float32 x) { return x "
                         "* 0.5; }\n");
    RequireTopLevelError("void G() { return 1; }\n", DiagnosticCode::InvalidReturn);
    RequireTopLevelError("float32 G() { return; }\n", DiagnosticCode::InvalidReturn);
    RequireTopLevelError("float32 G(int32 i) { return i; }\n", DiagnosticCode::TypeMismatch);
    RequireError("break;", DiagnosticCode::InvalidReturn);
    RequireError("switch (u) { default: continue; }", DiagnosticCode::InvalidReturn);
    RequireError("x = F2();", DiagnosticCode::UnknownName);
    RequireTopLevelError("void Nothing() {}\nvoid G() { float32 a = Nothing(); }\n", DiagnosticCode::VoidValue);
}

TEST_CASE("LUSL checker checks top-level declarations", "[shader-language][checker]")
{
    RequireTopLevelError("float32 global;\n", DiagnosticCode::InvalidTopLevelVariable);
    RequireTopLevelError("static const float32 NONE;\n", DiagnosticCode::NotConstant);
    RequireTopLevelError("float32 Compute() { return 1; }\nstatic const float32 RUNTIME = Compute();\n",
                         DiagnosticCode::NotConstant);
    RequireTopLevelValid("groupshared float32 tile[COUNT];\nstatic const float32x3 UP = float32x3(0, 1, 0);\n");
    RequireTopLevelError("void Later();\n", DiagnosticCode::UndefinedFunction);
    RequireTopLevelValid("float32 Later(float32 x);\nvoid G() { float32 a = Later(1.0); }\nfloat32 Later(float32 x) { "
                         "return x; }\n");
    RequireTopLevelError("void Twice() {}\nvoid Twice() {}\n", DiagnosticCode::Redeclaration);
    RequireTopLevelError("struct Opaque;\n", DiagnosticCode::InvalidInitializer);
    RequireTopLevelError("void G() { SceneContext s; float32 a = s.Field; }\n", DiagnosticCode::UnknownMember);
}

TEST_CASE("LUSL checker checks entry points and attributes", "[shader-language][checker]")
{
    RequireTopLevelValid(
        "[stage(compute), threads(8, 8, 1)]\nvoid Main(in uint32x3 id : DispatchThreadId, uint32 index "
        ": GroupIndex) {}\n");
    RequireTopLevelError("[stage(compute)]\nvoid Main(in uint32x3 id : DispatchThreadId) {}\n",
                         DiagnosticCode::InvalidAttribute);
    RequireTopLevelError("[stage(vertex), threads(1, 1, 1)]\nvoid Main() {}\n", DiagnosticCode::InvalidAttribute);
    RequireTopLevelError("[stage(compute), threads(8, 0, 1)]\nvoid Main() {}\n", DiagnosticCode::InvalidAttribute);
    RequireTopLevelError("[stage(compute), threads(8, 8, 1)]\nvoid Main(in float32x3 id : DispatchThreadId) {}\n",
                         DiagnosticCode::InvalidSemantic);
    RequireTopLevelError("[stage(compute), threads(8, 8, 1)]\nvoid Main(in uint32x3 id : Position) {}\n",
                         DiagnosticCode::InvalidSemantic);
    RequireTopLevelError("[stage(compute), threads(8, 8, 1)]\nvoid Main(in uint32x3 id) {}\n",
                         DiagnosticCode::InvalidSemantic);
    RequireTopLevelError("void G(in uint32x3 id : DispatchThreadId) {}\n", DiagnosticCode::InvalidSemantic);
    RequireTopLevelError("[stage(compute), threads(8, 8, 1)]\nfloat32 Main() { return 0; }\n",
                         DiagnosticCode::InvalidReturn);
    RequireTopLevelError("[shiny] void G() {}\n", DiagnosticCode::InvalidAttribute);
    RequireError("[unroll] if (b == true) {}", DiagnosticCode::InvalidAttribute);
    RequireValid("[unroll(COUNT)] for (uint32 k = 0; k < COUNT; k++) {} [branch] if (b == true) {}");
}

TEST_CASE("LUSL checker records types, symbols and bindings", "[shader-language][checker]")
{
    const CheckedSource checked(std::string(PRELUDE) +
                                R"(float32 Weight(in float32 value, in float32 scale = 1.0) { return value * scale; }
void G(float32x3 v)
{
    float32 total = Weight(scale = 2.0, value = v.x);
    uint32 doubled = COUNT * 2;
    float32x4 wide = float32x4(v, 1);
    Mode mode = Mode::Debug;
}
)");
    INFO(checked.DescribeDiagnostics());
    REQUIRE(checked.GetStatus() == Status::Ready);
    const SemanticModel& model = checked.GetModel();
    const SyntaxTree& tree = checked.GetTree();

    const ludus::foundation::uint32 total = checked.FindNode(SyntaxKind::VariableDeclaration, "total");
    REQUIRE(total != NO_NODE);
    CHECK(checked.TypeOf(total) == "float32");

    ludus::foundation::uint32 weightCall = NO_NODE;
    for (ludus::foundation::uint32 i = 0; i < tree.Nodes.GetSize(); ++i)
    {
        if (tree.Nodes[i].Kind == SyntaxKind::CallExpression && model.NodeCallKinds[i] == CallKind::Function)
        {
            weightCall = i;
        }
    }
    REQUIRE(weightCall != NO_NODE);
    REQUIRE(model.NodeSymbols[weightCall] != NO_SYMBOL);
    CHECK(model.Symbols[model.NodeSymbols[weightCall]].Kind == SymbolKind::Function);
    // `scale = 2.0` binds parameter 1, `value = v.x` parameter 0.
    const ludus::foundation::uint32 first = tree.Nodes[tree.Nodes[weightCall].FirstChild].NextSibling;
    const ludus::foundation::uint32 second = tree.Nodes[first].NextSibling;
    CHECK(model.NodeParameters[first] == 1);
    CHECK(model.NodeParameters[second] == 0);
    CHECK(checked.TypeOf(tree.Nodes[first].FirstChild) == "float32");

    const ludus::foundation::uint32 doubled = checked.FindNode(SyntaxKind::VariableDeclaration, "doubled");
    const ludus::foundation::uint32 product = tree.Nodes[tree.Nodes[doubled].FirstChild].NextSibling;
    CHECK(model.NodeIntegerConstants[product].IsKnown == true);
    CHECK(model.NodeIntegerConstants[product].Value == 8);
    CHECK(checked.TypeOf(product) == "uint32");

    const ludus::foundation::uint32 wide = checked.FindNode(SyntaxKind::VariableDeclaration, "wide");
    const ludus::foundation::uint32 construction = tree.Nodes[tree.Nodes[wide].FirstChild].NextSibling;
    CHECK(model.NodeCallKinds[construction] == CallKind::Construction);
    CHECK(checked.TypeOf(construction) == "float32x4");

    const ludus::foundation::uint32 mode = checked.FindNode(SyntaxKind::VariableDeclaration, "mode");
    const ludus::foundation::uint32 enumerator = tree.Nodes[tree.Nodes[mode].FirstChild].NextSibling;
    CHECK(checked.TypeOf(enumerator) == "Mode");
    CHECK(model.NodeIntegerConstants[enumerator].Value == 7);
}

TEST_CASE("LUSL checker needs successful earlier stages", "[shader-language][checker]")
{
    const PreprocessRequest request;
    const PreprocessResult preprocessed;
    const SyntaxTree tree;
    SemanticModel model;
    CHECK(Check(request, preprocessed, tree, model) == Status::InvalidRequest);
    CHECK(model.Diagnostics.GetSize() == 1);
}
