#include "checker_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
using ludus::tooling::shader_language::CheckOptions;
using ludus::tooling::shader_language::Diagnostic;
using ludus::tooling::shader_language::DiagnosticCode;
using ludus::tooling::shader_language::Severity;
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::StyleSetting;
using ludus::tooling::shader_language::test::CheckedSource;

/// Declarations shared by the flow tests; their names follow the style rules.
constexpr std::string_view PRELUDE = R"(static const uint32 COUNT2 = 4;
void Store(in float32 value, out float32 outValue) { outValue = value; }
void Swap(inout float32 inoutA, inout float32 inoutB) { float32 t = inoutA; inoutA = inoutB; inoutB = t; }
void Take(in float32x3 value) {}
)";

[[nodiscard]] std::string MakeFunction(std::string_view body)
{
    return std::string(PRELUDE) + "void F(float32 x, int32 i, uint32 u, float32x3 v, bool isOn, bool isOff)\n{\n" +
           std::string(body) + "\n}\n";
}

void RequireFlowValid(std::string_view body)
{
    const CheckedSource checked(MakeFunction(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}

void RequireFlowError(std::string_view body, DiagnosticCode code)
{
    const CheckedSource checked(MakeFunction(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.HasDiagnostic(code) == true);
}

void RequireTopLevel(std::string_view text, DiagnosticCode code)
{
    const CheckedSource checked(std::string(PRELUDE) + std::string(text));
    INFO(text << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    CHECK(checked.HasDiagnostic(code) == true);
}

void RequireTopLevelValid(std::string_view text)
{
    const CheckedSource checked(std::string(PRELUDE) + std::string(text));
    INFO(text << "\n" << checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}

/// Apply the fix of the first `code` diagnostic of `body`, require `expected` in the result, and
/// require that the fixed source no longer reports `code`.
void RequireFix(std::string_view body, DiagnosticCode code, std::string_view expected)
{
    const CheckedSource checked(MakeFunction(body));
    INFO(body << "\n" << checked.DescribeDiagnostics());
    const Diagnostic* diagnosticOrNull = checked.FindDiagnosticOrNull(code);
    REQUIRE(diagnosticOrNull != nullptr);
    REQUIRE(diagnosticOrNull->Fix.EditCount > 0);
    const std::string fixed = checked.ApplyFix(diagnosticOrNull->Fix);
    INFO("fixed:\n" << fixed);
    CHECK(fixed.find(expected) != std::string::npos);
    const CheckedSource recheck(fixed);
    INFO(recheck.DescribeDiagnostics());
    CHECK(recheck.FindDiagnosticOrNull(code) == nullptr);
}

void RequireNoFix(std::string_view text, DiagnosticCode code)
{
    const CheckedSource checked{std::string(text)};
    INFO(text << "\n" << checked.DescribeDiagnostics());
    const Diagnostic* diagnosticOrNull = checked.FindDiagnosticOrNull(code);
    REQUIRE(diagnosticOrNull != nullptr);
    CHECK(diagnosticOrNull->Fix.EditCount == 0);
}
} // namespace

TEST_CASE("LUSL flow analysis requires definite assignment", "[shader-language][flow]")
{
    RequireFlowValid("float32 a; a = 1; x = a;");
    RequireFlowValid("float32 a; if (isOn == true) { a = 1; } else { a = 2; } x = a;");
    RequireFlowValid("float32 a; while (true) { a = 1; break; } x = a;");
    RequireFlowValid("float32 a; Store(1.0, out a); x = a;");
    RequireFlowValid("float32x3 a = void; x = a.x;");
    RequireFlowValid("float32 a; switch (u) { case 1: a = 1; break; default: a = 2; break; } x = a;");
    RequireFlowValid("float32 a; do { a = 1; } while (isOn == true); x = a;");
    RequireFlowError("float32 a; x = a;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; if (isOn == true) { a = 1; } x = a;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; while (isOn == true) { a = 1; } x = a;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; for (uint32 k = 0; k < u; k++) { a = 1; } x = a;",
                     DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; switch (u) { case 1: a = 1; break; } x = a;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; a += 1;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; float32 c = 0; Swap(inout a, inout c);", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32x3 a; a.x = 1;", DiagnosticCode::UseBeforeAssignment);
    RequireFlowError("float32 a; x = isOn == true ? a : 1;", DiagnosticCode::UseBeforeAssignment);
}

TEST_CASE("LUSL flow analysis checks out parameters and returns", "[shader-language][flow]")
{
    RequireTopLevelValid("void Get(out float32 outValue) { outValue = 1; }\n");
    RequireTopLevelValid("float32 Pick(bool isOn) { if (isOn == true) { return 1; } return 2; }\n");
    RequireTopLevelValid("float32 Forever() { while (true) {} }\n");
    RequireTopLevelValid("float32 Choose(uint32 u) { switch (u) { case 1: return 1; default: return 2; } }\n");
    RequireTopLevelValid("void Shade(out float32 outValue) { discard; }\n");
    RequireTopLevel("void Get(out float32 outValue) {}\n", DiagnosticCode::OutParameterNotAssigned);
    RequireTopLevel("void Get(bool isOn, out float32 outValue) { if (isOn == true) { return; } outValue = 1; }\n",
                    DiagnosticCode::OutParameterNotAssigned);
    RequireTopLevel("void Get(out float32 outValue) { float32 a = outValue; outValue = a; }\n",
                    DiagnosticCode::UseBeforeAssignment);
    RequireTopLevel("float32 Pick(bool isOn) { if (isOn == true) { return 1; } }\n", DiagnosticCode::MissingReturn);
    RequireTopLevel("float32 Choose(uint32 u) { switch (u) { case 1: return 1; } }\n", DiagnosticCode::MissingReturn);
    RequireTopLevel("float32 Loop(bool isOn) { while (isOn == true) { return 1; } }\n", DiagnosticCode::MissingReturn);
}

TEST_CASE("LUSL flow analysis requires explicit case endings", "[shader-language][flow]")
{
    RequireFlowValid("switch (u) { case 1: [[fallthrough]]; case 2: x = 1; break; default: x = 2; }");
    RequireFlowError("switch (u) { case 1: x = 1; case 2: break; }", DiagnosticCode::InvalidSwitch);
}

TEST_CASE("LUSL flow analysis rejects recursion", "[shader-language][flow]")
{
    RequireTopLevel("uint32 Count(uint32 n) { return Count(n); }\n", DiagnosticCode::Recursion);
    RequireTopLevel("void Ping(uint32 n);\nvoid Pong(uint32 n) { Ping(n); }\nvoid Ping(uint32 n) { Pong(n); }\n",
                    DiagnosticCode::Recursion);
    RequireTopLevel("struct Walker { uint32 Step() { return Again(); } uint32 Again() { return Step(); } };\n",
                    DiagnosticCode::Recursion);
    RequireTopLevelValid("uint32 Leaf() { return 1; }\nuint32 Mid() { return Leaf() + Leaf(); }\nuint32 Top() { return "
                         "Mid(); }\n");
}

TEST_CASE("LUSL flow analysis requires groupshared initializers", "[shader-language][flow]")
{
    RequireTopLevelValid("static const uint32 TILE = 64;\ngroupshared float32 tile[TILE] = 0;\ngroupshared uint32 "
                         "counter = void;\n");
    RequireTopLevel("groupshared uint32 counter;\n", DiagnosticCode::GroupsharedInitializer);
    RequireTopLevel("groupshared uint32 counter = 1;\n", DiagnosticCode::GroupsharedInitializer);
}

TEST_CASE("LUSL flow analysis runs only after types check", "[shader-language][flow]")
{
    const CheckedSource checked(MakeFunction("float32 a; x = a; float32 c = i;"));
    CHECK(checked.HasDiagnostic(DiagnosticCode::TypeMismatch) == true);
    CHECK(checked.HasDiagnostic(DiagnosticCode::UseBeforeAssignment) == false);
}

TEST_CASE("LUSL style rules check names", "[shader-language][style]")
{
    const std::string_view text = R"(struct hit_record { uint32 count; bool Visible; };
enum class Mode : uint32 { off };
static const uint32 maxCount = 4;
void do_work(in float32 Value, out float32 value, inout uint32 seed, out bool outVisible)
{
    float32 MyValue = Value;
    bool visible = true;
    value = MyValue;
    outVisible = visible;
}
)";
    const CheckedSource checked{std::string(text)};
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Failed);
    const std::string described = checked.DescribeDiagnostics();
    for (const std::string_view suggestion : {"'HitRecord'",
                                              "'Count'",
                                              "'IsVisible'",
                                              "'Off'",
                                              "'MAX_COUNT'",
                                              "'DoWork'",
                                              "'value'",
                                              "'outValue'",
                                              "'inoutSeed'",
                                              "'outIsVisible'",
                                              "'myValue'",
                                              "'isVisible'"})
    {
        INFO(suggestion);
        CHECK(described.find(suggestion) != std::string::npos);
    }
    CHECK(checked.CountDiagnostics(DiagnosticCode::NamingStyle) == 12);
}

TEST_CASE("LUSL style rules skip extern slang names", "[shader-language][style]")
{
    const CheckedSource checked{std::string("extern slang \"color.slang\"\n{\n    float32 luminance(in float32x3 "
                                            "linear_rgb);\n    struct scene_data;\n}\n")};
    INFO(checked.DescribeDiagnostics());
    CHECK(checked.GetStatus() == Status::Ready);
}

TEST_CASE("LUSL style rules require explicit bool comparisons", "[shader-language][style]")
{
    RequireFlowError("if (isOn) {}", DiagnosticCode::BareBoolOperand);
    RequireFlowError("x = isOn ? 1 : 2;", DiagnosticCode::BareBoolOperand);
    RequireFlowError("bool isBoth = isOn && isOff == true;", DiagnosticCode::BareBoolOperand);
    RequireFlowError("while (isOn) { break; }", DiagnosticCode::BareBoolOperand);
    RequireFlowValid("if (isOn == true && isOff == false) {} bool isAny = isOn == true || x < 1;");
    RequireFlowValid("bool isCopy = isOn; if (true) {}");
    const CheckedSource both(MakeFunction("if (isOn && isOff) {}"));
    CHECK(both.CountDiagnostics(DiagnosticCode::BareBoolOperand) == 2);
}

TEST_CASE("LUSL style setting controls style severity", "[shader-language][style]")
{
    const std::string text = MakeFunction("if (isOn) {} float32 Bad = 0;");
    CheckOptions warn;
    warn.Style = StyleSetting::Warn;
    const CheckedSource warned(text, warn);
    CHECK(warned.GetStatus() == Status::Ready);
    REQUIRE(warned.FindDiagnosticOrNull(DiagnosticCode::BareBoolOperand) != nullptr);
    CHECK(warned.FindDiagnosticOrNull(DiagnosticCode::BareBoolOperand)->Level == Severity::Warning);
    CHECK(warned.FindDiagnosticOrNull(DiagnosticCode::NamingStyle)->Level == Severity::Warning);

    CheckOptions off;
    off.Style = StyleSetting::Off;
    const CheckedSource silent(text, off);
    CHECK(silent.GetStatus() == Status::Ready);
    CHECK(silent.GetModel().Diagnostics.IsEmpty() == true);

    const CheckedSource strict(text);
    CHECK(strict.GetStatus() == Status::Failed);
    CHECK(strict.FindDiagnosticOrNull(DiagnosticCode::BareBoolOperand)->Level == Severity::Error);

    // Language rules do not depend on the setting.
    const CheckedSource language(MakeFunction("float32 a = i;"), off);
    CHECK(language.GetStatus() == Status::Failed);
}

TEST_CASE("LUSL diagnostics carry fixes that repair the source", "[shader-language][fix]")
{
    RequireFix("if (!isOn) {}", DiagnosticCode::LogicalNot, "if (isOn == false) {}");
    RequireFix("bool isSame = !isOn == isOff;", DiagnosticCode::LogicalNot, "(isOn == false) == isOff");
    RequireFix("if (!Take2(v).x) {}", DiagnosticCode::LogicalNot, "if (Take2(v).x == false) {}");
    RequireFix("float32 a = i;", DiagnosticCode::TypeMismatch, "float32 a = float32(i);");
    RequireFix("float32x3 a = 1.0;", DiagnosticCode::TypeMismatch, "float32x3 a = float32x3(1.0);");
    RequireFix("if (i) {}", DiagnosticCode::NonBoolCondition, "if (i != 0) {}");
    RequireFix("if (u + 1) {}", DiagnosticCode::NonBoolCondition, "if (u + 1 != 0) {}");
    RequireFix("float32 a = 0; Store(1.0, a);", DiagnosticCode::DirectionMismatch, "Store(1.0, out a);");
    RequireFix("float32 a = 0; float32 c = 0; Swap(out a, inout c);",
               DiagnosticCode::DirectionMismatch,
               "Swap(inout a, inout c);");
    RequireFix("Take(out v);", DiagnosticCode::DirectionMismatch, "Take(v);");
    RequireFix("float32 a; x = a;", DiagnosticCode::UseBeforeAssignment, "float32 a = 0;");
    RequireFix("float32 w[COUNT2]; x = w[0];", DiagnosticCode::UseBeforeAssignment, "float32 w[COUNT2] = 0;");
    RequireFix("if (isOn) {}", DiagnosticCode::BareBoolOperand, "if (isOn == true) {}");
    RequireFix("if ((isOn)) {}", DiagnosticCode::BareBoolOperand, "if ((isOn) == true) {}");
}

TEST_CASE("LUSL diagnostics offer no fix where one is unsafe", "[shader-language][fix]")
{
    RequireNoFix(MakeFunction("if (u & 1) {}"), DiagnosticCode::NonBoolCondition);
    RequireNoFix(MakeFunction("bool isB = true; float32 a = isB;"), DiagnosticCode::TypeMismatch);
    RequireNoFix("#define READ(value) (value)\nvoid F(int32 i) { float32 a = READ(i); }\n",
                 DiagnosticCode::TypeMismatch);
    RequireNoFix("groupshared uint32 counter = 1;\n", DiagnosticCode::GroupsharedInitializer);
}
