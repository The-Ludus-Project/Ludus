#include "preprocessor_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
using ludus::tooling::shader_language::CONFIGURATION_SOURCE;
using ludus::tooling::shader_language::Diagnostic;
using ludus::tooling::shader_language::DiagnosticCode;
using ludus::tooling::shader_language::GetDiagnosticCodeName;
using ludus::tooling::shader_language::MAX_INCLUDE_DEPTH;
using ludus::tooling::shader_language::NO_EXPANSION;
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::Token;
using ludus::tooling::shader_language::test::Snapshot;

/// Preprocess one root file and return its output text; fails the test unless Ready.
std::string PreprocessText(const std::string& text)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", text);
    const Status status = snapshot.Run();
    INFO(text);
    for (const Diagnostic& diagnostic : snapshot.GetResult().Diagnostics)
    {
        INFO(GetDiagnosticCodeName(diagnostic.Code) << ": " << diagnostic.Message.GetView());
        CHECK(false);
    }
    REQUIRE(status == Status::Ready);
    return snapshot.GetText();
}

/// Preprocess one root file that must fail with `code`.
void RequireFailure(const std::string& text, DiagnosticCode code)
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", text);
    INFO(text);
    CHECK(snapshot.Run() == Status::Failed);
    CHECK(snapshot.HasDiagnostic(code) == true);
    CHECK(snapshot.GetResult().Tokens.IsEmpty() == true);
}
} // namespace

TEST_CASE("LUSL preprocessor passes plain tokens through without comments", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("float32 x = 1; // trailing\n/* block */ x += 2;") == "float32 x = 1 ; x += 2 ;");
    CHECK(PreprocessText("").empty() == true);
}

TEST_CASE("LUSL preprocessor expands object-like macros and stops self-reference", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#define SIZE 4\nfloat32 weights[SIZE];") == "float32 weights [ 4 ] ;");
    CHECK(PreprocessText("#define A B\n#define B C\nA") == "C");
    CHECK(PreprocessText("#define X X + 1\nX") == "X + 1");
    CHECK(PreprocessText("#define A B\n#define B A\nA B") == "A B");
    CHECK(PreprocessText("#define EMPTY\na EMPTY b") == "a b");
}

TEST_CASE("LUSL preprocessor expands function-like macros", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#define MAX(a, b) ((a) > (b) ? (a) : (b))\nMAX(x, f(y, z))") ==
          "( ( x ) > ( f ( y , z ) ) ? ( x ) : ( f ( y , z ) ) )");
    CHECK(PreprocessText("#define ZERO() 0\nZERO()") == "0");
    CHECK(PreprocessText("#define F(x) [x]\nF()") == "[ ]");
    CHECK(PreprocessText("#define F(x) x\nF") == "F");
    CHECK(PreprocessText("#define F(x) x\nF\n(1)") == "1");
    CHECK(PreprocessText("#define G F\n#define F(x) x * 2\nG(3)") == "3 * 2");
}

TEST_CASE("LUSL preprocessor follows the C rescanning rules", "[shader-language][preprocessor]")
{
    // A macro rescanned after its own expansion stays unexpanded, even inside a later invocation.
    CHECK(PreprocessText("#define f(a) a*g\n#define g(a) f(a)\nf(2)(9)") == "2 * 9 * g");
    // Arguments are expanded before substitution, except next to '#' or '##'.
    CHECK(PreprocessText("#define V 42\n#define STR(x) #x\n#define XSTR(x) STR(x)\nSTR(V) XSTR(V)") == "\"V\" \"42\"");
}

TEST_CASE("LUSL preprocessor stringifies arguments", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#define STR(x) #x\nSTR(  a   +  b )") == "\"a + b\"");
    CHECK(PreprocessText("#define STR(x) #x\nSTR()") == "\"\"");
    CHECK(PreprocessText("#define STR(x) #x\nSTR(\"q\\\"\")") == "\"\\\"q\\\\\\\"\\\"\"");
    RequireFailure("#define BAD(x) #y\n", DiagnosticCode::StringifyNotParameter);
}

TEST_CASE("LUSL preprocessor pastes tokens with placemarkers", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#define CAT(a, b) a ## b\nCAT(x, y) CAT(, y) CAT(x, ) CAT(, ) end") == "xy y x end");
    CHECK(PreprocessText("#define CAT(a, b) a ## b\nCAT(<<, =)") == "<<=");
    CHECK(PreprocessText("#define NAME(i) Light ## i ## Color\nNAME(2)") == "Light2Color");
    CHECK(PreprocessText("#define JOIN x ## 1\nJOIN") == "x1");
    RequireFailure("#define CAT(a, b) a ## b\nCAT(+, /)", DiagnosticCode::InvalidPaste);
    RequireFailure("#define CAT(a, b) a ## b\nCAT(/, /)", DiagnosticCode::InvalidPaste);
    RequireFailure("#define BAD ## x\n", DiagnosticCode::PasteAtEdge);
    RequireFailure("#define BAD(a) a ##\n", DiagnosticCode::PasteAtEdge);
}

TEST_CASE("LUSL preprocessor reports pasting errors at the definition and the use", "[shader-language][preprocessor]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "#define CAT(a, b) a ## b\nfloat32 x;\n  CAT(+, /)\n");
    CHECK(snapshot.Run() == Status::Failed);
    const Diagnostic* diagnosticOrNull = snapshot.FindDiagnosticOrNull(DiagnosticCode::InvalidPaste);
    REQUIRE(diagnosticOrNull != nullptr);
    // The '##' in the definition, and the use that expanded it.
    CHECK(diagnosticOrNull->Location.Line == 1);
    CHECK(diagnosticOrNull->Location.Column == 21);
    REQUIRE(diagnosticOrNull->ExpansionLocation.IsValid() == true);
    CHECK(diagnosticOrNull->ExpansionLocation.Line == 3);
    CHECK(diagnosticOrNull->ExpansionLocation.Column == 3);
}

TEST_CASE("LUSL preprocessor records the outermost expansion of each token", "[shader-language][preprocessor]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "#define INNER value\n#define OUTER INNER\nplain OUTER\n");
    REQUIRE(snapshot.Run() == Status::Ready);
    const ludus::foundation::Array<Token>& tokens = snapshot.GetResult().Tokens;
    REQUIRE(tokens.GetSize() == 2);
    CHECK(tokens[0].ExpansionIndex == NO_EXPANSION);
    REQUIRE(tokens[1].ExpansionIndex != NO_EXPANSION);
    const Token& expanded = tokens[1];
    CHECK(expanded.Location.Line == 1);
    CHECK(snapshot.GetResult().Expansions[expanded.ExpansionIndex].Line == 3);
    CHECK(snapshot.GetResult().Expansions[expanded.ExpansionIndex].Column == 7);
}

TEST_CASE("LUSL preprocessor reports invalid macro definitions and calls", "[shader-language][preprocessor]")
{
    RequireFailure("#define F(a, a) a\n", DiagnosticCode::DuplicateMacroParameter);
    RequireFailure("#define F(...) 0\n", DiagnosticCode::VariadicMacro);
    RequireFailure("#define F(a...) 0\n", DiagnosticCode::VariadicMacro);
    RequireFailure("#define F(a 0\n", DiagnosticCode::MalformedDirective);
    RequireFailure("#define defined 1\n", DiagnosticCode::ReservedMacroName);
    RequireFailure("#define F(a, b) a\nF(1)", DiagnosticCode::MacroArgumentCount);
    RequireFailure("#define F() 0\nF(1)", DiagnosticCode::MacroArgumentCount);
    RequireFailure("#define F(a) a\nF(1", DiagnosticCode::UnterminatedMacroInvocation);
    RequireFailure("#define F(a) a\nF(1,\n#define G\n2)", DiagnosticCode::DirectiveInMacroArguments);
    RequireFailure("#define X 1\n#define X 2\n", DiagnosticCode::MacroRedefined);
    RequireFailure("#define F(a) a\n#define F a\n", DiagnosticCode::MacroRedefined);
    CHECK(PreprocessText("#define X 1 + 2\n#define X 1 + 2\nX") == "1 + 2");
    CHECK(PreprocessText("#define X 1\n#undef X\n#define X 2\nX") == "2");
}

TEST_CASE("LUSL preprocessor selects conditional groups", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#if 1\na\n#elif 1\nb\n#else\nc\n#endif") == "a");
    CHECK(PreprocessText("#if 0\na\n#elif 1\nb\n#else\nc\n#endif") == "b");
    CHECK(PreprocessText("#if 0\na\n#elif 0\nb\n#else\nc\n#endif") == "c");
    CHECK(PreprocessText("#if 1\n#if 0\na\n#else\nb\n#endif\n#endif") == "b");
    // Skipped groups ignore their directives, including invalid ones.
    CHECK(PreprocessText("#if 0\n#error no\n#bogus\n#if UNKNOWN\n#endif\n#endif\nok") == "ok");
    // A taken branch stops later conditions from being evaluated.
    CHECK(PreprocessText("#if 1\na\n#elif 1 / 0\nb\n#endif") == "a");
    CHECK(PreprocessText("#define X\n#ifdef X\na\n#endif\n#ifndef X\nb\n#endif") == "a");
}

TEST_CASE("LUSL preprocessor evaluates integer conditions", "[shader-language][preprocessor]")
{
    CHECK(PreprocessText("#if 2 + 3 * 4 == 14 && (1 << 4) == 0x10 && -3 / 2 == -1 && 7 % 4 == 3\nyes\n#endif") ==
          "yes");
    CHECK(PreprocessText("#if (1 ? 2 : 3) == 2 && ~0 == -1 && 5 > 4 && 4 >= 4 && 3 < 4 && (6 & 3) == 2\nyes\n#endif") ==
          "yes");
    CHECK(PreprocessText("#if (6 | 1) == 7 && (6 ^ 3) == 5 && (16 >> 2) == 4 && 1 != 2\nyes\n#endif") == "yes");
    CHECK(PreprocessText("#if true && false == 0\nyes\n#endif") == "yes");
    // Skipped operands are not evaluated.
    CHECK(PreprocessText("#if 0 && (1 / 0)\nno\n#else\nyes\n#endif") == "yes");
    CHECK(PreprocessText("#if 1 || (1 / 0)\nyes\n#endif") == "yes");
    CHECK(PreprocessText("#if 1 ? 1 : 1 / 0\nyes\n#endif") == "yes");
    RequireFailure("#if 1 / 0\n#endif", DiagnosticCode::ConditionDivisionByZero);
    RequireFailure("#if 0x7fffffffffffffff + 1\n#endif", DiagnosticCode::ConditionOverflow);
    RequireFailure("#if 1 << 63\n#endif", DiagnosticCode::ConditionOverflow);
    RequireFailure("#if 010\n#endif", DiagnosticCode::InvalidCondition);
    RequireFailure("#if 1u\n#endif", DiagnosticCode::InvalidCondition);
    RequireFailure("#if\n#endif", DiagnosticCode::InvalidCondition);
    RequireFailure("#if (1\n#endif", DiagnosticCode::InvalidCondition);
    RequireFailure("#if 1 2\n#endif", DiagnosticCode::InvalidCondition);
}

TEST_CASE("LUSL preprocessor rejects unknown names and logical not", "[shader-language][preprocessor]")
{
    RequireFailure("#if UNKNOWN\n#endif", DiagnosticCode::UndefinedIdentifierInCondition);
    RequireFailure("#if defined(UNKNOWN)\n#endif", DiagnosticCode::UnknownMacroName);
    RequireFailure("#ifdef UNKNOWN\n#endif", DiagnosticCode::UnknownMacroName);
    RequireFailure("#define X\n#if !defined(X)\n#endif", DiagnosticCode::LogicalNot);
    RequireFailure("#define D defined\n#define X\n#if D(X)\n#endif", DiagnosticCode::DefinedFromExpansion);
    // A name defined or undefined earlier is known, even when it is no longer defined.
    CHECK(PreprocessText("#define X\n#undef X\n#if defined(X) == 0 && defined X == 0\nyes\n#endif") == "yes");
}

TEST_CASE("LUSL preprocessor reports conditional structure errors", "[shader-language][preprocessor]")
{
    RequireFailure("#if 1\n", DiagnosticCode::UnterminatedConditional);
    RequireFailure("#endif\n", DiagnosticCode::UnmatchedConditional);
    RequireFailure("#else\n", DiagnosticCode::UnmatchedConditional);
    RequireFailure("#if 1\n#else\n#elif 1\n#endif", DiagnosticCode::UnmatchedConditional);
    RequireFailure("#if 1\n#else\n#else\n#endif", DiagnosticCode::UnmatchedConditional);
    RequireFailure("#if 1\n#endif extra", DiagnosticCode::MalformedDirective);
    RequireFailure("#ifdef\n#endif", DiagnosticCode::MalformedDirective);
}

TEST_CASE("LUSL preprocessor reports other directives", "[shader-language][preprocessor]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "#error unsupported   target\n");
    CHECK(snapshot.Run() == Status::Failed);
    const Diagnostic* errorOrNull = snapshot.FindDiagnosticOrNull(DiagnosticCode::ErrorDirective);
    REQUIRE(errorOrNull != nullptr);
    CHECK(errorOrNull->Message.GetView() == "#error unsupported target");

    RequireFailure("#line 4\n", DiagnosticCode::LineDirective);
    RequireFailure("#pragma optimize\n", DiagnosticCode::UnknownPragma);
    RequireFailure("#version 450\n", DiagnosticCode::UnknownDirective);
    RequireFailure("# 5\n", DiagnosticCode::MalformedDirective);
    CHECK(PreprocessText("#\nx") == "x");
    // A '#' that does not start a line is an ordinary token.
    CHECK(PreprocessText("a # b") == "a # b");
}

TEST_CASE("LUSL preprocessor exposes configuration values as read-only macros", "[shader-language][preprocessor]")
{
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", "#if SHADOW_MODE == 2\nshadows\n#endif\nfloat32 x = SCALE;\n");
    snapshot.AddConfiguration("SHADOW_MODE", "2").AddConfiguration("SCALE", "1.5").AddConfiguration("UNUSED", "0");
    REQUIRE(snapshot.Run() == Status::Ready);
    CHECK(snapshot.GetText() == "shadows float32 x = 1.5 ;");
    const ludus::foundation::Array<ludus::foundation::uint8>& reads = snapshot.GetResult().ConfigurationReadMask;
    REQUIRE(reads.GetSize() == 3);
    CHECK(reads[0] == 1);
    CHECK(reads[1] == 1);
    CHECK(reads[2] == 0);
    CHECK(snapshot.GetResult().Tokens[4].Location.FileIndex == CONFIGURATION_SOURCE);
    CHECK(snapshot.GetResult().Tokens[4].Location.Line == 2);

    Snapshot redefined;
    redefined.AddFile("main.lusl", "#define MODE 3\n").AddConfiguration("MODE", "1");
    CHECK(redefined.Run() == Status::Failed);
    CHECK(redefined.HasDiagnostic(DiagnosticCode::ConfigurationModified) == true);

    Snapshot undefined;
    undefined.AddFile("main.lusl", "#undef MODE\n").AddConfiguration("MODE", "1");
    CHECK(undefined.Run() == Status::Failed);
    CHECK(undefined.HasDiagnostic(DiagnosticCode::ConfigurationModified) == true);

    Snapshot tested;
    tested.AddFile("main.lusl", "#ifdef MODE\nyes\n#endif\n").AddConfiguration("MODE", "");
    REQUIRE(tested.Run() == Status::Ready);
    CHECK(tested.GetText() == "yes");
    CHECK(tested.GetResult().ConfigurationReadMask[0] == 1);
}

TEST_CASE("LUSL preprocessor resolves includes inside the snapshot", "[shader-language][preprocessor]")
{
    Snapshot snapshot;
    snapshot.AddFile("shaders/main.lusl", "#include \"common/math.luslh\"\n#include \"lights.luslh\"\nmain\n")
        .AddFile("shaders/common/math.luslh", "#pragma once\nmath\n#include \"../common/math.luslh\"\n")
        .AddFile("lights.luslh", "#include \"shaders/common/math.luslh\"\nlights\n")
        .AddFile("unused.luslh", "unused\n")
        .AddFile("shaders/slang.slang", "slang\n");
    REQUIRE(snapshot.Run("shaders/main.lusl") == Status::Ready);
    CHECK(snapshot.GetText() == "math lights main");
    const ludus::foundation::Array<ludus::foundation::uint8>& read = snapshot.GetResult().SourceReadMask;
    REQUIRE(read.GetSize() == 5);
    CHECK(read[0] == 1);
    CHECK(read[1] == 1);
    CHECK(read[2] == 1);
    CHECK(read[3] == 0);
    CHECK(read[4] == 0);
}

TEST_CASE("LUSL preprocessor reports include errors", "[shader-language][preprocessor]")
{
    Snapshot recursive;
    recursive.AddFile("main.lusl", "#include \"loop.luslh\"\n").AddFile("loop.luslh", "#include \"loop.luslh\"\n");
    CHECK(recursive.Run() == Status::Failed);
    CHECK(recursive.HasDiagnostic(DiagnosticCode::RecursiveInclude) == true);

    Snapshot slang;
    slang.AddFile("main.lusl", "#include \"math.slang\"\n").AddFile("math.slang", "float f;\n");
    CHECK(slang.Run() == Status::Failed);
    CHECK(slang.HasDiagnostic(DiagnosticCode::IncludeNotHeader) == true);

    RequireFailure("#include \"missing.luslh\"\n", DiagnosticCode::IncludeNotFound);
    RequireFailure("#include \"../outside.luslh\"\n", DiagnosticCode::InvalidIncludePath);
    RequireFailure("#include \"/absolute.luslh\"\n", DiagnosticCode::InvalidIncludePath);
    RequireFailure("#include <angle.luslh>\n", DiagnosticCode::MalformedDirective);

    Snapshot deep;
    std::string chain = "#include \"0.luslh\"\n";
    deep.AddFile("main.lusl", chain);
    for (ludus::foundation::usize i = 0; i <= MAX_INCLUDE_DEPTH; ++i)
    {
        deep.AddFile(std::to_string(i) + ".luslh", "#include \"" + std::to_string(i + 1) + ".luslh\"\n");
    }
    deep.AddFile(std::to_string(MAX_INCLUDE_DEPTH + 1) + ".luslh", "end\n");
    CHECK(deep.Run() == Status::Failed);
    CHECK(deep.HasDiagnostic(DiagnosticCode::IncludeDepthLimit) == true);
}

TEST_CASE("LUSL preprocessor validates its request", "[shader-language][preprocessor]")
{
    Snapshot missingRoot;
    missingRoot.AddFile("other.lusl", "");
    CHECK(missingRoot.Run() == Status::InvalidRequest);
    CHECK(missingRoot.HasDiagnostic(DiagnosticCode::InvalidRequest) == true);

    Snapshot header;
    header.AddFile("main.luslh", "");
    CHECK(header.Run("main.luslh") == Status::InvalidRequest);

    Snapshot badPath;
    badPath.AddFile("main.lusl", "").AddFile("a/../b.luslh", "");
    CHECK(badPath.Run() == Status::InvalidRequest);

    Snapshot duplicate;
    duplicate.AddFile("main.lusl", "").AddFile("main.lusl", "");
    CHECK(duplicate.Run() == Status::InvalidRequest);

    Snapshot zeroByte;
    zeroByte.AddFile("main.lusl", std::string("a\0b", 3));
    CHECK(zeroByte.Run() == Status::InvalidRequest);

    Snapshot badName;
    badName.AddFile("main.lusl", "").AddConfiguration("1MODE", "1");
    CHECK(badName.Run() == Status::InvalidRequest);

    Snapshot repeated;
    repeated.AddFile("main.lusl", "").AddConfiguration("MODE", "1").AddConfiguration("MODE", "2");
    CHECK(repeated.Run() == Status::InvalidRequest);

    Snapshot badValue;
    badValue.AddFile("main.lusl", "").AddConfiguration("MODE", "\"open");
    CHECK(badValue.Run() == Status::InvalidRequest);

    Snapshot multiline;
    multiline.AddFile("main.lusl", "").AddConfiguration("MODE", "1\n2");
    CHECK(multiline.Run() == Status::InvalidRequest);
}

TEST_CASE("LUSL preprocessor stops at expansion limits", "[shader-language][preprocessor]")
{
    // Each level doubles the output, so the last level exceeds MAX_OUTPUT_TOKENS.
    std::string text = "#define L0 x x\n";
    constexpr ludus::foundation::int32 LEVELS = 21;
    for (ludus::foundation::int32 i = 1; i <= LEVELS; ++i)
    {
        text += "#define L" + std::to_string(i) + " L" + std::to_string(i - 1) + " L" + std::to_string(i - 1) + "\n";
    }
    text += "L" + std::to_string(LEVELS) + "\n";
    Snapshot snapshot;
    snapshot.AddFile("main.lusl", text);
    CHECK(snapshot.Run() == Status::Failed);
    const bool hasLimit = snapshot.HasDiagnostic(DiagnosticCode::OutputTokenLimit) == true ||
                          snapshot.HasDiagnostic(DiagnosticCode::MacroExpansionLimit) == true;
    CHECK(hasLimit == true);
    CHECK(snapshot.GetResult().Tokens.IsEmpty() == true);
}

TEST_CASE("LUSL preprocessor is deterministic", "[shader-language][preprocessor]")
{
    const std::string text = "#define CAT(a, b) a ## b\n#define STR(x) #x\nCAT(Light, 1) STR(CAT(a, b))\n";
    Snapshot first;
    first.AddFile("main.lusl", text);
    Snapshot second;
    second.AddFile("main.lusl", text);
    REQUIRE(first.Run() == Status::Ready);
    REQUIRE(second.Run() == Status::Ready);
    CHECK(first.GetText() == second.GetText());
    CHECK(first.GetText() == "Light1 \"CAT(a, b)\"");
    REQUIRE(first.GetResult().Tokens.GetSize() == second.GetResult().Tokens.GetSize());
    for (ludus::foundation::usize i = 0; i < first.GetResult().Tokens.GetSize(); ++i)
    {
        const Token& left = first.GetResult().Tokens[i];
        const Token& right = second.GetResult().Tokens[i];
        CHECK(left.SpellingOffset == right.SpellingOffset);
        CHECK(left.Location.ByteOffset == right.Location.ByteOffset);
        CHECK(left.ExpansionIndex == right.ExpansionIndex);
    }
}
