#include "internal/code_language_registry.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
/// Borrow a test string literal without its terminator.
[[nodiscard]] BorrowedView<const char> View(const char* text, usize length) noexcept
{
    return BorrowedView<const char>(text, length);
}
} // namespace

TEST_CASE("the registry describes every CodeLanguage in enum order", "[editor][code-editor]")
{
    const CodeLanguageRegistry& registry = CodeLanguageRegistry::Get();
    const BorrowedView<const CodeLanguageDescriptor> descriptors = registry.GetDescriptors();
    REQUIRE(descriptors.GetSize() == static_cast<usize>(CodeLanguage::Count));
    REQUIRE(descriptors[0].Language == CodeLanguage::PlainText);
    REQUIRE(descriptors[1].Language == CodeLanguage::Slang);
    REQUIRE(descriptors[2].Language == CodeLanguage::Lusl);
    REQUIRE(registry.Describe(CodeLanguage::Lusl).Language == CodeLanguage::Lusl);
    REQUIRE(registry.Describe(CodeLanguage::PlainText).Language == CodeLanguage::PlainText);
    REQUIRE(registry.Describe(CodeLanguage::Slang).Language == CodeLanguage::Slang);
}

TEST_CASE("plain text keeps the current no-coloring, no-structural-editing profile", "[editor][code-editor]")
{
    const CodeLanguageDescriptor& plain = CodeLanguageRegistry::Get().Describe(CodeLanguage::PlainText);
    REQUIRE(plain.HasLexicalColoring == false);
    REQUIRE(plain.HasStructuralEditing == false);
    REQUIRE(plain.LineComment.IsEmpty() == true);
    REQUIRE(plain.SuffixPatterns.IsEmpty() == true);
    REQUIRE(plain.BracketPairs.IsEmpty() == true);
}

TEST_CASE("Slang keeps lexical coloring, structural editing and the line comment", "[editor][code-editor]")
{
    const CodeLanguageDescriptor& slang = CodeLanguageRegistry::Get().Describe(CodeLanguage::Slang);
    REQUIRE(slang.HasLexicalColoring == true);
    REQUIRE(slang.HasStructuralEditing == true);
    REQUIRE(slang.LineComment.GetSize() == 2);
    REQUIRE(slang.LineComment[0] == '/');
    REQUIRE(slang.LineComment[1] == '/');
    REQUIRE(slang.BracketPairs.GetSize() == 3);
}

TEST_CASE("LUSL colors, edits structurally and has a language service", "[editor][code-editor]")
{
    const CodeLanguageDescriptor& lusl = CodeLanguageRegistry::Get().Describe(CodeLanguage::Lusl);
    REQUIRE(lusl.HasLexicalColoring == true);
    REQUIRE(lusl.HasStructuralEditing == true);
    REQUIRE(lusl.HasLanguageService == true);
    REQUIRE(lusl.LineComment.GetSize() == 2);
    REQUIRE(lusl.BracketPairs.GetSize() == 3);
    REQUIRE(HasCodeLanguageService(CodeLanguage::Slang) == true);
    REQUIRE(HasCodeLanguageService(CodeLanguage::PlainText) == false);
    const CodeLanguageRegistry& registry = CodeLanguageRegistry::Get();
    const char sourcePath[] = "main_shaders/miss.lusl";
    REQUIRE(registry.ClassifyFileName(View(sourcePath, sizeof(sourcePath) - 1)) == CodeLanguage::Lusl);
    const char headerPath[] = "shaders/RAY_SHADER.LUSLH";
    REQUIRE(registry.ClassifyFileName(View(headerPath, sizeof(headerPath) - 1)) == CodeLanguage::Lusl);
    // The generated Slang module of a LUSL source stays Slang.
    const char generatedPath[] = "main_shaders/miss.lusl.slang";
    REQUIRE(registry.ClassifyFileName(View(generatedPath, sizeof(generatedPath) - 1)) == CodeLanguage::Slang);
}

TEST_CASE("file classification preserves the .slang mapping", "[editor][code-editor]")
{
    const CodeLanguageRegistry& registry = CodeLanguageRegistry::Get();

    const char slangPath[] = "shaders/lighting.slang";
    REQUIRE(registry.ClassifyFileName(View(slangPath, sizeof(slangPath) - 1)) == CodeLanguage::Slang);

    const char slanghPath[] = "shaders/common.slangh";
    REQUIRE(registry.ClassifyFileName(View(slanghPath, sizeof(slanghPath) - 1)) == CodeLanguage::Slang);

    const char upperPath[] = "SHADERS/LIGHTING.SLANG";
    REQUIRE(registry.ClassifyFileName(View(upperPath, sizeof(upperPath) - 1)) == CodeLanguage::Slang);

    const char textPath[] = "notes/readme.txt";
    REQUIRE(registry.ClassifyFileName(View(textPath, sizeof(textPath) - 1)) == CodeLanguage::PlainText);

    const char cmakePath[] = "CMakeLists.txt";
    REQUIRE(registry.ClassifyFileName(View(cmakePath, sizeof(cmakePath) - 1)) == CodeLanguage::CMake);
    const char prefixedCMakePath[] = "project/notCMakeLists.txt";
    REQUIRE(registry.ClassifyFileName(View(prefixedCMakePath, sizeof(prefixedCMakePath) - 1)) ==
            CodeLanguage::PlainText);

    const char empty[] = "";
    REQUIRE(registry.ClassifyFileName(View(empty, 0)) == CodeLanguage::PlainText);

    // A bare suffix-like name that is shorter than the suffix must not match.
    const char shortName[] = ".sla";
    REQUIRE(registry.ClassifyFileName(View(shortName, sizeof(shortName) - 1)) == CodeLanguage::PlainText);
}
