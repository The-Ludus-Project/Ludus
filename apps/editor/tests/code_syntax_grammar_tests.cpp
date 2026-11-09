// The upstream grammars the syntax service links (docs/decisions/0026-tree-sitter-syntax-dependency.md):
// each pinned grammar loads into the pinned runtime and parses a representative snippet of its
// language without ERROR or MISSING nodes.

#include "internal/code_syntax_grammars.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <tree_sitter/api.h>

#include <cstdlib>

namespace
{
using ludus::foundation::uint32;
using ludus::foundation::usize;

/// Parses source with language and requires a tree whose root has no ERROR or MISSING descendants.
template <usize N>
void RequireParsesWithoutErrors(const TSLanguage* language, const char (&source)[N])
{
    TSParser* parserOrNull = ts_parser_new();
    REQUIRE(parserOrNull != nullptr);
    REQUIRE(ts_parser_set_language(parserOrNull, language) == true);
    TSTree* treeOrNull = ts_parser_parse_string(parserOrNull, nullptr, source, static_cast<uint32>(N - 1));
    REQUIRE(treeOrNull != nullptr);
    const TSNode root = ts_tree_root_node(treeOrNull);
    char* treeTextOrNull = ts_node_string(root);
    REQUIRE(treeTextOrNull != nullptr);
    INFO(treeTextOrNull);
    std::free(treeTextOrNull);
    CHECK(ts_node_has_error(root) == false);
    CHECK(ts_node_end_byte(root) > 0);
    ts_tree_delete(treeOrNull);
    ts_parser_delete(parserOrNull);
}
} // namespace

TEST_CASE("The pinned grammars load into the pinned runtime", "[editor][code-editor][syntax]")
{
    // ts_parser_set_language rejects a grammar generated for an ABI outside the runtime's range.
    for (const TSLanguage* language : {tree_sitter_c(),
                                       tree_sitter_cpp(),
                                       tree_sitter_slang(),
                                       tree_sitter_luau(),
                                       tree_sitter_json(),
                                       tree_sitter_cmake(),
                                       tree_sitter_markdown(),
                                       tree_sitter_markdown_inline()})
    {
        REQUIRE(language != nullptr);
        CHECK(ts_language_abi_version(language) >= TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION);
        CHECK(ts_language_abi_version(language) <= TREE_SITTER_LANGUAGE_VERSION);
    }
}

TEST_CASE("The C grammar parses C without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_c(), R"(#include <stdint.h>

typedef struct Point { float x; float y; } Point;

static inline float Dot(const Point* a, const Point* b) { return a->x * b->x + a->y * b->y; }

int main(void)
{
    Point points[2] = { { .x = 1.0f, .y = 0.0f }, { .x = 0.5f, .y = 2.0f } };
    for (int i = 0; i < 2; ++i) { if (Dot(&points[i], &points[0]) > 0.0f) { return i; } }
    return 0;
}
)");
}

TEST_CASE("The C++ grammar parses C++ without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_cpp(), R"(#pragma once

namespace ludus::editor
{
template <typename T> concept Numeric = requires(T value) { value + value; };

class Counter final
{
public:
    [[nodiscard]] constexpr int Get() const noexcept { return mValue; }
    void Add(Numeric auto amount) { mValue += static_cast<int>(amount); }

private:
    int mValue = 0;
};

inline auto MakeAdder(int base) { return [base](int value) { return base + value; }; }
} // namespace ludus::editor
)");
}

TEST_CASE("The Slang grammar parses Slang without errors", "[editor][code-editor][syntax]")
{
    // tree-sitter-slang 0.3.1 accepts one attribute per function and misreads a global generic
    // resource such as `RWTexture2D<float4> Output;` (see the manifest's known grammar gaps), so the
    // entry point has one attribute and takes its resource as a parameter.
    RequireParsesWithoutErrors(tree_sitter_slang(), R"(interface IMaterial
{
    float3 Evaluate(float3 directionWorld);
};

struct Lambertian : IMaterial
{
    float3 Albedo;
    float3 Evaluate(float3 directionWorld) { return Albedo * 0.5; }
};

[numthreads(8, 8, 1)]
void Main(uint3 threadId : SV_DispatchThreadID, RWTexture2D<float4> output)
{
    Lambertian material = { float3(1.0, 0.5, 0.25) };
    output[threadId.xy] = float4(material.Evaluate(float3(0.0, 1.0, 0.0)), 1.0);
}
)");
}

TEST_CASE("The Luau grammar parses Luau without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_luau(), R"(--!strict
type Point = { x: number, y: number }

local function Length(point: Point): number
    return math.sqrt(point.x * point.x + point.y * point.y)
end

local points: { Point } = { { x = 3, y = 4 } }
for index, point in points do
    print(`point {index} has length {Length(point)}`)
end
)");
}

TEST_CASE("The Luau grammar parses names longer than its scanner's initial buffer", "[editor][code-editor][syntax]")
{
    // Upstream's scanner grows its name buffer at 256 bytes but discarded the reallocated pointer;
    // EditorTreeSitter.cmake compiles a repaired copy. Sanitizer builds report a regression here.
    RequireParsesWithoutErrors(
        tree_sitter_luau(),
        "local a_name_that_is_much_longer_than_two_hundred_and_fifty_six_bytes_so_that_the_scanner_"
        "has_to_grow_its_buffer_while_reading_it_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa = 1\n");
}

TEST_CASE("The JSON grammar parses JSON without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_json(), R"({
  "name": "tree-sitter-json",
  "version": "0.24.8",
  "grammars": [{ "name": "json", "language_abi": 14 }],
  "escaped": "line\nbreak é",
  "values": [true, false, null, -1.5e3]
}
)");
}

TEST_CASE("The CMake grammar parses CMake without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_cmake(), R"(cmake_minimum_required(VERSION 3.28)
project(Sample LANGUAGES C CXX)

# A comment, then a bracket argument.
set(message [==[a [bracket] argument]==])
if(NOT DEFINED SAMPLE_OPTION AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SAMPLE_OPTION ON)
endif()
add_library(sample STATIC sample.c)
target_compile_options(sample PRIVATE $<$<C_COMPILER_ID:Clang>:-Wall> "${SAMPLE_FLAGS}")
foreach(item IN ITEMS a b c)
    message(STATUS "item ${item}")
endforeach()
)");
}

TEST_CASE("The Markdown grammars parse Markdown without errors", "[editor][code-editor][syntax]")
{
    RequireParsesWithoutErrors(tree_sitter_markdown(), R"(# Code editor

A paragraph with *emphasis*, `code` and a [link](../code-editor.md).

- A list item
- Another item

```cpp
int main() { return 0; }
```

| Column | Value |
| --- | --- |
| a | 1 |
)");
    RequireParsesWithoutErrors(tree_sitter_markdown_inline(),
                               "A paragraph with *emphasis*, `code`, **strong** text and a [link](../code-editor.md).");
}
