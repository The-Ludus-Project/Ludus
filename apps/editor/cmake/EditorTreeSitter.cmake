# The tree-sitter C runtime as the editor-private static target ludus_editor_tree_sitter
# (docs/decisions/0026-tree-sitter-syntax-dependency.md). Thanks to Max Brunsfeld and the
# tree-sitter contributors for the incremental parsing library, https://tree-sitter.github.io/.
#
# The pin lives in third_party/tree-sitter/SOURCE-LOCK.json. CMake downloads that exact tagged
# archive, verifies its SHA-256 and its LICENSE against the committed copy, and compiles only the
# runtime's lib/src/lib.c amalgamation, natively and with Emscripten alike. Upstream's own CMake
# build, Rust CLI, bindings and Wasm-store feature are not used. Nothing here is installed.
include_guard(GLOBAL)
include(FetchContent)
enable_language(C)

function(ludus_editor_add_tree_sitter)
    set(lock_path "${PROJECT_SOURCE_DIR}/third_party/tree-sitter/SOURCE-LOCK.json")
    set(license_path "${PROJECT_SOURCE_DIR}/third_party/tree-sitter/LICENSE")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock_path}" "${license_path}")
    file(READ "${lock_path}" lock)
    string(JSON version GET "${lock}" version)
    string(JSON archive_url GET "${lock}" archive_url)
    string(JSON archive_sha256 GET "${lock}" archive_sha256)
    FetchContent_Declare(ludus_tree_sitter_source URL "${archive_url}"
        URL_HASH "SHA256=${archive_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(ludus_tree_sitter_source)
    if(NOT ludus_tree_sitter_source_POPULATED)
        FetchContent_Populate(ludus_tree_sitter_source)
    endif()
    set(source "${ludus_tree_sitter_source_SOURCE_DIR}")
    file(SHA256 "${license_path}" expected_license)
    file(SHA256 "${source}/LICENSE" actual_license)
    if(NOT actual_license STREQUAL expected_license)
        message(FATAL_ERROR "tree-sitter ${version}: the archive's LICENSE differs from "
            "third_party/tree-sitter/LICENSE; re-verify the pin before updating either")
    endif()

    add_library(ludus_editor_tree_sitter STATIC "${source}/lib/src/lib.c")
    set_target_properties(ludus_editor_tree_sitter PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    # The feature macros upstream's build defines for its POSIX and BSD declarations.
    target_compile_definitions(ludus_editor_tree_sitter PRIVATE
        _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE _DARWIN_C_SOURCE)
    target_include_directories(ludus_editor_tree_sitter PRIVATE "${source}/lib/src")
    target_include_directories(ludus_editor_tree_sitter SYSTEM PUBLIC "${source}/lib/include")
    # Sanitizers and the macro prefix map; the C++-only exception flags do not affect C.
    target_link_libraries(ludus_editor_tree_sitter PRIVATE ludus_project_options)
    # The project warning set's C subset (EngineCompilerWarnings.cmake minus its C++-only flags).
    set(c_warnings -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef -Wnull-dereference
        -Wdouble-promotion -Wimplicit-fallthrough)
    target_compile_options(ludus_editor_tree_sitter PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:${c_warnings}>
        $<$<C_COMPILER_ID:MSVC>:/W4>
        $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:Clang,AppleClang,GNU>>:-Werror>
        $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:MSVC>>:/WX>)
    # Upstream-only suppressions, scoped to this target (ADR 0026, "Compilation and warnings"):
    # - -Wundef: the runtime's trimmed ICU headers (lib/src/unicode/umachine.h) test platform macros
    #   such as U_PLATFORM that the trimmed copy never defines; ICU relies on them evaluating to 0.
    # - -Wmissing-format-attribute and -Wformat-nonliteral (from -Wformat=2): lexer.c's internal
    #   ts_lexer__log forwards a printf format to vsnprintf without declaring the attribute.
    target_compile_options(ludus_editor_tree_sitter PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-undef -Wno-missing-format-attribute -Wno-format-nonliteral>)
endfunction()
