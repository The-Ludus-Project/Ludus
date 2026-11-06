#include "syntax_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
using ludus::tooling::shader_language::Diagnostic;
using ludus::tooling::shader_language::DiagnosticCode;
using ludus::tooling::shader_language::GetDiagnosticCodeName;
using ludus::tooling::shader_language::MAX_SYNTAX_DEPTH;
using ludus::tooling::shader_language::Parse;
using ludus::tooling::shader_language::PreprocessRequest;
using ludus::tooling::shader_language::PreprocessResult;
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::SyntaxKind;
using ludus::tooling::shader_language::SyntaxTree;
using ludus::tooling::shader_language::test::ParsedSource;

/// Parse `text` and return the dump of its first declaration; fails the test unless Ready.
std::string ParseFirst(const std::string& text)
{
    const ParsedSource parsed(text);
    INFO(text);
    for (const Diagnostic& diagnostic : parsed.GetTree().Diagnostics)
    {
        INFO(GetDiagnosticCodeName(diagnostic.Code)
             << " at " << diagnostic.Location.Line << ":" << diagnostic.Location.Column << ": "
             << diagnostic.Message.GetView());
        CHECK(false);
    }
    REQUIRE(parsed.GetStatus() == Status::Ready);
    return parsed.DumpFirstDeclaration();
}

/// Parse a function body and return the dump of its block.
std::string ParseBody(const std::string& body)
{
    const std::string dump = ParseFirst("void F()\n{\n" + body + "\n}\n");
    const std::string prefix = "(function-declaration F (type void) ";
    REQUIRE(dump.starts_with(prefix) == true);
    return dump.substr(prefix.size(), dump.size() - prefix.size() - 1);
}

/// Parse `text`, which must fail with `code`.
void RequireSyntaxError(const std::string& text, DiagnosticCode code)
{
    const ParsedSource parsed(text);
    INFO(text);
    REQUIRE(parsed.GetPreprocessStatus() == Status::Ready);
    CHECK(parsed.GetStatus() == Status::Failed);
    CHECK(parsed.HasDiagnostic(code) == true);
}
} // namespace

TEST_CASE("LUSL parser reads variable declarations", "[shader-language][parser]")
{
    CHECK(ParseFirst("static const uint32 COUNT = 4;") ==
          "(variable-declaration static const COUNT (type uint32) (number-literal 4))");
    CHECK(ParseFirst("float32 weights[COUNT][2] = 0;") ==
          "(variable-declaration weights (type float32) (array-size (name COUNT)) (array-size (number-literal 2)) "
          "(number-literal 0))");
    CHECK(ParseFirst("groupshared float32x4 tile[TILE] = void;") ==
          "(variable-declaration groupshared tile (type float32x4) (array-size (name TILE)) (void-initializer void))");
    CHECK(ParseFirst("Texture2D<float32x4> albedo;") ==
          "(variable-declaration albedo (type Texture2D (type float32x4)))");
    CHECK(ParseFirst("Outer<Inner<float32>, 4> nested;") ==
          "(variable-declaration nested (type Outer (type Inner (type float32)) (number-literal 4)))");
    CHECK(ParseFirst("Array<Array<uint32>> twice;") ==
          "(variable-declaration twice (type Array (type Array (type uint32))))");
    CHECK(ParseFirst("SceneQuery::Kind kind;") == "(variable-declaration kind (type Kind))");
}

TEST_CASE("LUSL parser reads functions, parameters and attributes", "[shader-language][parser]")
{
    CHECK(ParseFirst("[stage(compute), threads(8, 8, 1)]\nvoid Main(in uint32x3 id : DispatchThreadId) {}") ==
          "(function-declaration Main (attribute stage (name compute)) (attribute threads (number-literal 8) "
          "(number-literal 8) (number-literal 1)) (type void) (parameter in id (type uint32x3) (semantic "
          "DispatchThreadId)) (block))");
    CHECK(ParseFirst("bool Trace(SceneContext scene, out float32x3 radiance, inout uint32 seed = 1);") ==
          "(function-declaration Trace (type bool) (parameter scene (type SceneContext)) (parameter out radiance "
          "(type float32x3)) (parameter inout seed (type uint32) (number-literal 1)))");
}

TEST_CASE("LUSL parser reads structs, enums, extern blocks and static asserts", "[shader-language][parser]")
{
    CHECK(ParseFirst("struct Hit\n{\n    float32x3 PositionWorld;\n    uint32 Count = 1;\n    float32 Get() { return "
                     "1; }\n};") ==
          "(struct-declaration Hit (variable-declaration PositionWorld (type float32x3)) (variable-declaration Count "
          "(type uint32) (number-literal 1)) (function-declaration Get (type float32) (block (return-statement return "
          "(number-literal 1)))))");
    CHECK(ParseFirst("enum class Mode : uint32 { Off, On = 2, };") ==
          "(enum-declaration Mode (type uint32) (enumerator Off) (enumerator On (number-literal 2)))");
    CHECK(ParseFirst("extern slang \"math.slang\"\n{\n    static const float32 PI;\n    float32 Luminance(in "
                     "float32x3 rgb);\n}") ==
          "(extern-slang-block \"math.slang\" (variable-declaration static const PI (type float32)) "
          "(function-declaration Luminance (type float32) (parameter in rgb (type float32x3))))");
    CHECK(ParseFirst("static_assert(COUNT > 1, \"need two\");") ==
          "(static-assert static_assert (binary-expression > (name COUNT) (number-literal 1)) (string-literal \"need "
          "two\"))");
}

TEST_CASE("LUSL parser reads statements", "[shader-language][parser]")
{
    CHECK(ParseBody("float32 x = 0; x += 2; x = Foo(x); i++; --j;") ==
          "(block (variable-declaration x (type float32) (number-literal 0)) (assignment-statement += (name x) "
          "(number-literal 2)) (assignment-statement = (name x) (call-expression (name Foo) (argument (name x)))) "
          "(increment-statement ++ (name i)) (increment-statement -- (name j)))");
    CHECK(ParseBody("if (a == true) { Go(); } else if (b) Stop(); else {}") ==
          "(block (if-statement (binary-expression == (name a) (boolean-literal true)) (block (expression-statement "
          "(call-expression (name Go)))) (if-statement (name b) (expression-statement (call-expression (name Stop))) "
          "(block))))");
    CHECK(ParseBody("[unroll] for (uint32 i = 0; i < COUNT; i++) { continue; }") ==
          "(block (for-statement (attribute unroll) (variable-declaration i (type uint32) (number-literal 0)) "
          "(binary-expression < (name i) (name COUNT)) (increment-statement ++ (name i)) (block (continue-statement "
          "continue))))");
    CHECK(ParseBody("for (;;) { break; }") ==
          "(block (for-statement (empty) (empty) (empty) (block (break-statement break))))");
    CHECK(ParseBody("while (x < 1) x += 1; do { discard; } while (y);") ==
          "(block (while-statement (binary-expression < (name x) (number-literal 1)) (assignment-statement += (name x) "
          "(number-literal 1))) (do-statement (block (discard-statement discard)) (name y)))");
    CHECK(ParseBody("switch (mode) { case Mode::Off: [[fallthrough]]; case Mode::On: return; default: break; }") ==
          "(block (switch-statement (name mode) (case-clause case (scope-expression :: (name Mode) (name Off)) "
          "(fallthrough-statement fallthrough)) (case-clause case (scope-expression :: (name Mode) (name On)) "
          "(return-statement return)) (case-clause default (break-statement break))))");
}

TEST_CASE("LUSL parser reads expressions with C precedence", "[shader-language][parser]")
{
    CHECK(ParseBody("x = a + b * c - d;") ==
          "(block (assignment-statement = (name x) (binary-expression - (binary-expression + (name a) "
          "(binary-expression * (name b) (name c))) (name d))))");
    CHECK(ParseBody("x = a || b && c == d << 1;") ==
          "(block (assignment-statement = (name x) (binary-expression || (name a) (binary-expression && (name b) "
          "(binary-expression == (name c) (binary-expression << (name d) (number-literal 1)))))))");
    CHECK(ParseBody("x = c ? -a : ~b;") ==
          "(block (assignment-statement = (name x) (conditional-expression (name c) (unary-expression - (name a)) "
          "(unary-expression ~ (name b)))))");
    CHECK(ParseBody("x = hit.Normals[i](1).xyz;") ==
          "(block (assignment-statement = (name x) (member-expression xyz (call-expression (index-expression "
          "(member-expression Normals (name hit)) (name i)) (argument (number-literal 1))))))");
    CHECK(ParseBody("x = (a + b) * float32(c);") ==
          "(block (assignment-statement = (name x) (binary-expression * (parenthesized-expression "
          "(binary-expression + (name a) (name b))) (call-expression (name float32) (argument (name c))))))");
}

TEST_CASE("LUSL parser reads named arguments, call marks and initializers", "[shader-language][parser]")
{
    CHECK(ParseBody("Trace(scene, inout seed, ray = r, outRadiance = out radiance);") ==
          "(block (expression-statement (call-expression (name Trace) (argument (name scene)) (argument inout (name "
          "seed)) (argument ray (name r)) (argument out outRadiance (name radiance)))))");
    CHECK(ParseBody("Hit hit = { .PositionWorld = p, .Count = 1, };") ==
          "(block (variable-declaration hit (type Hit) (initializer-list { (field-initializer PositionWorld (name p)) "
          "(field-initializer Count (number-literal 1)))))");
    CHECK(ParseBody("hit = {};") == "(block (assignment-statement = (name hit) (initializer-list {)))");
}

TEST_CASE("LUSL parser enforces LUSL syntax rules", "[shader-language][parser]")
{
    RequireSyntaxError("float32 a, b;", DiagnosticCode::MultipleDeclarators);
    RequireSyntaxError("void F() { if (a = b) {} }", DiagnosticCode::AssignmentInExpression);
    RequireSyntaxError("void F() { G(x += 1); }", DiagnosticCode::AssignmentInExpression);
    RequireSyntaxError("void F() { x = y++; }", DiagnosticCode::AssignmentInExpression);
    RequireSyntaxError("void F() { if (!a) {} }", DiagnosticCode::LogicalNot);
    RequireSyntaxError("void F() { x = (float32)y; }", DiagnosticCode::CStyleCast);
    RequireSyntaxError("void F() { a + b; }", DiagnosticCode::ExpressionWithoutEffect);
    RequireSyntaxError("void F() { ; }", DiagnosticCode::ExpressionWithoutEffect);
    RequireSyntaxError("void F() { G(a = 1, 2); }", DiagnosticCode::PositionalAfterNamed);
    RequireSyntaxError("enum Mode : uint32 { A };", DiagnosticCode::UnscopedEnum);
    RequireSyntaxError("concept Opaque<T> = true;", DiagnosticCode::LaterPhaseConstruct);
    RequireSyntaxError("dynamic Material m;", DiagnosticCode::LaterPhaseConstruct);
    RequireSyntaxError("template <Material M> struct Holder { };", DiagnosticCode::LaterPhaseConstruct);
    RequireSyntaxError("float32 return = 1;", DiagnosticCode::KeywordAsName);
    RequireSyntaxError("float32 x = 1", DiagnosticCode::UnexpectedToken);
    RequireSyntaxError("void F() { case 1: break; }", DiagnosticCode::UnexpectedToken);
    RequireSyntaxError("Hit hit = { 1, 2 };", DiagnosticCode::UnexpectedToken);
}

TEST_CASE("LUSL parser recovers and reports independent errors", "[shader-language][parser]")
{
    const ParsedSource parsed("float32 a, b;\nvoid F()\n{\n    x = (float32)y;\n    Good();\n    a + b;\n}\nuint32 "
                              "after = 1;\n");
    CHECK(parsed.GetStatus() == Status::Failed);
    CHECK(parsed.HasDiagnostic(DiagnosticCode::MultipleDeclarators) == true);
    CHECK(parsed.HasDiagnostic(DiagnosticCode::CStyleCast) == true);
    CHECK(parsed.HasDiagnostic(DiagnosticCode::ExpressionWithoutEffect) == true);
    CHECK(parsed.GetTree().Diagnostics.GetSize() == 3);
    // The declaration after the errors is still parsed.
    CHECK(parsed.Dump().find("(variable-declaration after (type uint32) (number-literal 1))") != std::string::npos);
    CHECK(parsed.Dump().find("(expression-statement (call-expression (name Good)))") != std::string::npos);
}

TEST_CASE("LUSL parser skips the rest of a call after an argument error", "[shader-language][parser]")
{
    const ParsedSource parsed("void F()\n{\n    G(a = 1, 2, (3));\n    H();\n}\n");
    CHECK(parsed.GetStatus() == Status::Failed);
    REQUIRE(parsed.GetTree().Diagnostics.GetSize() == 1);
    CHECK(parsed.GetTree().Diagnostics[0].Code == DiagnosticCode::PositionalAfterNamed);
    CHECK(parsed.Dump().find("(expression-statement (call-expression (name H)))") != std::string::npos);
}

TEST_CASE("LUSL parser records token ranges and locations", "[shader-language][parser]")
{
    const ParsedSource parsed("\nvoid F()\n{\n    float32 x = 1;\n}\n");
    REQUIRE(parsed.GetStatus() == Status::Ready);
    const SyntaxTree& tree = parsed.GetTree();
    const ludus::foundation::uint32 function = tree.Nodes[0].FirstChild;
    CHECK(parsed.Spell(tree.Nodes[function].FirstToken) == "void");
    CHECK(parsed.Spell(tree.Nodes[function].LastToken) == "}");
    CHECK(tree.Nodes[0].FirstToken == 0);
    CHECK(parsed.Spell(tree.Nodes[0].LastToken) == "}");

    const ParsedSource failing("void F()\n{\n    x = (float32)y;\n}\n");
    REQUIRE(failing.GetTree().Diagnostics.GetSize() == 1);
    CHECK(failing.GetTree().Diagnostics[0].Location.Line == 3);
}

TEST_CASE("LUSL parser bounds its nesting", "[shader-language][parser]")
{
    std::string deep = "void F() { x = ";
    for (ludus::foundation::usize i = 0; i < MAX_SYNTAX_DEPTH * 2; ++i)
    {
        deep += '(';
    }
    deep += '1';
    for (ludus::foundation::usize i = 0; i < MAX_SYNTAX_DEPTH * 2; ++i)
    {
        deep += ')';
    }
    deep += "; }";
    RequireSyntaxError(deep, DiagnosticCode::SyntaxDepthLimit);
}

TEST_CASE("LUSL parser needs a successful preprocessing result", "[shader-language][parser]")
{
    const PreprocessRequest request;
    const PreprocessResult preprocessed;
    SyntaxTree tree;
    CHECK(Parse(request, preprocessed, tree) == Status::InvalidRequest);
    CHECK(tree.Diagnostics.GetSize() == 1);

    const ParsedSource expanded("#define DECLARE(type, name) type name = 0;\nDECLARE(float32, x)\n");
    REQUIRE(expanded.GetStatus() == Status::Ready);
    CHECK(expanded.DumpFirstDeclaration() == "(variable-declaration x (type float32) (number-literal 0))");
    const SyntaxTree& expandedTree = expanded.GetTree();
    CHECK(expandedTree.Nodes[expandedTree.Nodes[0].FirstChild].Kind == SyntaxKind::VariableDeclaration);
}
