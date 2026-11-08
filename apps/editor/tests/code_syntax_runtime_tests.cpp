// The tree-sitter runtime the syntax service links (docs/decisions/0026-tree-sitter-syntax-dependency.md):
// the pinned build compiles, links and supports the grammar ABI range the manifest records.

#include <catch2/catch_test_macros.hpp>

#include <tree_sitter/api.h>

namespace
{
/// Grammar ABI range of the pinned runtime, as third_party/tree-sitter/SOURCE-LOCK.json records it.
constexpr unsigned int PINNED_LANGUAGE_VERSION = 15;
constexpr unsigned int PINNED_MINIMUM_COMPATIBLE_LANGUAGE_VERSION = 13;
} // namespace

TEST_CASE("The pinned tree-sitter runtime accepts the recorded grammar ABI range", "[editor][code-editor][syntax]")
{
    // A grammar generated for an ABI outside this range is rejected by ts_parser_set_language, so the
    // grammar pins must stay inside it.
    STATIC_REQUIRE(TREE_SITTER_LANGUAGE_VERSION == PINNED_LANGUAGE_VERSION);
    STATIC_REQUIRE(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION == PINNED_MINIMUM_COMPATIBLE_LANGUAGE_VERSION);
}

TEST_CASE("The pinned tree-sitter runtime creates parsers and refuses to parse without a grammar",
          "[editor][code-editor][syntax]")
{
    TSParser* parserOrNull = ts_parser_new();
    REQUIRE(parserOrNull != nullptr);
    CHECK(ts_parser_language(parserOrNull) == nullptr);
    constexpr char SOURCE[] = "float32 x = 0;";
    TSTree* treeOrNull = ts_parser_parse_string(parserOrNull, nullptr, SOURCE, sizeof(SOURCE) - 1);
    CHECK(treeOrNull == nullptr);
    ts_parser_delete(parserOrNull);
}
