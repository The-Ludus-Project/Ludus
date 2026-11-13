# The tree-sitter C runtime and the syntax service's grammars as the editor-private static target
# ludus_editor_tree_sitter (docs/decisions/0026-tree-sitter-syntax-dependency.md). Thanks to Max
# Brunsfeld and the tree-sitter contributors for the incremental parsing library,
# https://tree-sitter.github.io/, and to the grammar authors named in each third_party/tree-sitter-*/LICENSE:
# Max Brunsfeld (C, C++, JSON), Stephan Seitz (Slang), polychromatist (Luau), Uy Ha (CMake) and
# Matthias Deiml (Markdown).
#
# Each pin lives in third_party/<component>/SOURCE-LOCK.json. CMake downloads that exact archive,
# verifies its SHA-256 and its LICENSE against the committed copy, and compiles only the runtime's
# lib/src/lib.c amalgamation and each grammar's upstream-generated parser.c and hand-written
# scanner.c, natively and with Emscripten alike. Upstream's own build files, the tree-sitter CLI,
# bindings and the Wasm-store feature are not used, and no grammar is regenerated. Nothing here is
# installed.
include_guard(GLOBAL)
include(FetchContent)
enable_language(C)

# Downloads the archive that third_party/<component>/SOURCE-LOCK.json pins, checks its LICENSE and
# returns the extracted source directory and the lock's JSON text.
function(ludus_editor_fetch_tree_sitter_component component out_source out_lock)
    set(lock_path "${PROJECT_SOURCE_DIR}/third_party/${component}/SOURCE-LOCK.json")
    set(license_path "${PROJECT_SOURCE_DIR}/third_party/${component}/LICENSE")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock_path}" "${license_path}")
    file(READ "${lock_path}" lock)
    string(JSON version GET "${lock}" version)
    string(JSON archive_url GET "${lock}" archive_url)
    string(JSON archive_sha256 GET "${lock}" archive_sha256)
    string(REPLACE "-" "_" content_name "ludus_${component}_source")
    FetchContent_Declare(${content_name} URL "${archive_url}"
        URL_HASH "SHA256=${archive_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(${content_name})
    if(NOT ${content_name}_POPULATED)
        FetchContent_Populate(${content_name})
    endif()
    set(source "${${content_name}_SOURCE_DIR}")
    file(SHA256 "${license_path}" expected_license)
    file(SHA256 "${source}/LICENSE" actual_license)
    if(NOT actual_license STREQUAL expected_license)
        message(FATAL_ERROR "${component} ${version}: the archive's LICENSE differs from "
            "third_party/${component}/LICENSE; re-verify the pin before updating either")
    endif()
    set(${out_source} "${source}" PARENT_SCOPE)
    set(${out_lock} "${lock}" PARENT_SCOPE)
endfunction()

# The project warning set's C subset (EngineCompilerWarnings.cmake minus its C++-only flags), with
# warnings-as-errors when LUDUS_WARNINGS_AS_ERRORS is on. Sanitizers and the macro prefix map come
# from ludus_project_options; its C++-only exception flags do not affect C.
function(ludus_editor_apply_tree_sitter_c_options target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${target} PRIVATE ludus_project_options)
    set(c_warnings -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef -Wnull-dereference
        -Wdouble-promotion -Wimplicit-fallthrough)
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:${c_warnings}>
        $<$<C_COMPILER_ID:MSVC>:/W4>
        $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:Clang,AppleClang,GNU>>:-Werror>
        $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:MSVC>>:/WX>)
endfunction()

# Upstream polychromatist/tree-sitter-luau's process_name discards ts_realloc's result when a name
# reaches 256 bytes, then writes through the freed buffer. Compile a copy of scanner.c that keeps the
# result; the downloaded tree stays untouched. Configuration fails if upstream changes the line.
function(ludus_editor_fix_luau_scanner source_directory out_scanner)
    set(broken_line "      ts_realloc(name, name_capacity * sizeof(char));")
    set(fixed_line "      name = ts_realloc(name, name_capacity * sizeof(char));")
    file(READ "${source_directory}/scanner.c" scanner)
    string(FIND "${scanner}" "${broken_line}" first)
    string(FIND "${scanner}" "${broken_line}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "tree-sitter-luau: scanner.c no longer contains the one discarded "
            "ts_realloc this build repairs; re-verify the pin and remove or update the repair")
    endif()
    string(REPLACE "${broken_line}" "${fixed_line}" scanner "${scanner}")
    set(fixed_path "${CMAKE_CURRENT_BINARY_DIR}/tree-sitter-luau/scanner.c")
    set(existing "")
    if(EXISTS "${fixed_path}")
        file(READ "${fixed_path}" existing)
    endif()
    if(NOT existing STREQUAL scanner)
        file(WRITE "${fixed_path}" "${scanner}")
    endif()
    set(${out_scanner} "${fixed_path}" PARENT_SCOPE)
endfunction()

function(ludus_editor_add_tree_sitter)
    ludus_editor_fetch_tree_sitter_component(tree-sitter source lock)
    set(runtime_source "${source}")
    add_library(ludus_editor_tree_sitter STATIC "${source}/lib/src/lib.c")
    ludus_editor_apply_tree_sitter_c_options(ludus_editor_tree_sitter)
    # The feature macros upstream's build defines for its POSIX and BSD declarations.
    target_compile_definitions(ludus_editor_tree_sitter PRIVATE
        _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE _DARWIN_C_SOURCE)
    target_include_directories(ludus_editor_tree_sitter PRIVATE "${source}/lib/src")
    target_include_directories(ludus_editor_tree_sitter SYSTEM PUBLIC "${source}/lib/include")
    # Upstream-only suppressions, scoped to this target (ADR 0026, "Compilation and warnings"):
    # - -Wundef: the runtime's trimmed ICU headers (lib/src/unicode/umachine.h) test platform macros
    #   such as U_PLATFORM that the trimmed copy never defines; ICU relies on them evaluating to 0.
    # - -Wmissing-format-attribute and -Wformat-nonliteral (from -Wformat=2): lexer.c's internal
    #   ts_lexer__log forwards a printf format to vsnprintf without declaring the attribute.
    # - MSVC C4232: alloc.c initializes allocator callbacks with imported CRT function addresses.
    # - MSVC C4244/C4018: node.c and query.c narrow internal symbol/capture indices and compare
    #   signed capture bounds with array counts. These upstream C diagnostics stay local here.
    # - MSVC C4701: node.c reads last_iterator only while has_last_iterator is true, set alongside
    #   its initialization. MSVC's optimizer does not preserve that guard correlation.
    target_compile_options(ludus_editor_tree_sitter PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-undef -Wno-missing-format-attribute -Wno-format-nonliteral>
        $<$<C_COMPILER_ID:MSVC>:/wd4018 /wd4232 /wd4244 /wd4701>)

    # One object library per grammar, because each grammar compiles against its own generated
    # src/tree_sitter/parser.h. Their objects join ludus_editor_tree_sitter, so consumers link one
    # target and reach each grammar through its tree_sitter_<name>() entry point.
    foreach(component IN ITEMS tree-sitter-c tree-sitter-cpp tree-sitter-slang tree-sitter-luau
            tree-sitter-json tree-sitter-cmake tree-sitter-markdown)
        ludus_editor_fetch_tree_sitter_component(${component} source lock)
        string(JSON grammar_count LENGTH "${lock}" grammars)
        math(EXPR last_grammar "${grammar_count} - 1")
        foreach(grammar_index RANGE ${last_grammar})
            string(JSON grammar GET "${lock}" grammars ${grammar_index} name)
            string(JSON directory GET "${lock}" grammars ${grammar_index} directory)
            string(JSON source_count LENGTH "${lock}" grammars ${grammar_index} sources)
            math(EXPR last_source "${source_count} - 1")
            set(grammar_sources "")
            foreach(source_index RANGE ${last_source})
                string(JSON source_file GET "${lock}" grammars ${grammar_index} sources ${source_index})
                list(APPEND grammar_sources "${source}/${directory}/${source_file}")
            endforeach()
            if(grammar STREQUAL "luau")
                ludus_editor_fix_luau_scanner("${source}/${directory}" fixed_scanner)
                list(REMOVE_ITEM grammar_sources "${source}/${directory}/scanner.c")
                list(APPEND grammar_sources "${fixed_scanner}")
            endif()
            set(target ludus_editor_tree_sitter_${grammar})
            add_library(${target} OBJECT ${grammar_sources})
            ludus_editor_apply_tree_sitter_c_options(${target})
            target_include_directories(${target} PRIVATE "${source}/${directory}")
            target_sources(ludus_editor_tree_sitter PRIVATE $<TARGET_OBJECTS:${target}>)
        endforeach()
    endforeach()

    # LUSL is a repository-owned grammar. Its generated ABI header is identical
    # to the pinned runtime's parser.h, so both native and Wasm builds reuse it.
    set(lusl_grammar "${PROJECT_SOURCE_DIR}/apps/editor/syntax/lusl")
    set(lusl_parser "${lusl_grammar}/src/parser.c")
    file(READ "${PROJECT_SOURCE_DIR}/config/tool_versions.json" versions)
    string(JSON generator_version GET "${versions}" quality tree-sitter)
    file(READ "${PROJECT_SOURCE_DIR}/third_party/tree-sitter/SOURCE-LOCK.json" runtime_lock)
    string(JSON runtime_version GET "${runtime_lock}" version)
    if(NOT generator_version STREQUAL runtime_version)
        message(FATAL_ERROR "The LUSL generator and tree-sitter runtime pins must agree")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lusl_grammar}/grammar.js" "${lusl_parser}")
    file(SHA256 "${lusl_grammar}/grammar.js" lusl_grammar_hash)
    file(STRINGS "${lusl_parser}" stamp LIMIT_COUNT 1)
    if(NOT stamp STREQUAL "// Tree-sitter ${generator_version}, grammar.js SHA-256: ${lusl_grammar_hash}")
        message(FATAL_ERROR "Stale LUSL parser. Run scripts/regenerate-lusl-grammar")
    endif()
    set(lusl_headers "${CMAKE_CURRENT_BINARY_DIR}/tree-sitter-lusl")
    configure_file("${runtime_source}/lib/src/parser.h" "${lusl_headers}/tree_sitter/parser.h" COPYONLY)
    add_library(ludus_editor_tree_sitter_lusl OBJECT "${lusl_parser}")
    ludus_editor_apply_tree_sitter_c_options(ludus_editor_tree_sitter_lusl)
    target_include_directories(ludus_editor_tree_sitter_lusl PRIVATE "${lusl_headers}")
    target_sources(ludus_editor_tree_sitter PRIVATE $<TARGET_OBJECTS:ludus_editor_tree_sitter_lusl>)

    # These scanners define tree_sitter_<name>_external_scanner_create() with an empty parameter list
    # instead of (void), while the runtime calls it through a prototyped `void *(*)(void)` slot. In C11
    # that definition is unprototyped, a different function type: Clang's function sanitizer (part of
    # the UBSan presets) aborts on the first parse. C23 reads an empty list as (void), so these grammar
    # objects compile as C23 and every sanitizer check stays on. MSVC uses the
    # C11 dialect from the common options and has no Clang function sanitizer.
    #
    # Upstream-only grammar suppressions, scoped to the grammar that needs them (ADR 0026):
    # - -Wstrict-prototypes: the same empty parameter lists; tree-sitter's generated declaration has
    #   the prototype.
    # - Luau's scanner also leaves switch fallthrough unannotated, keeps an unused skip_if helper and
    #   ignores valid_symbols in two token readers (-Wimplicit-fallthrough, -Wunused-function,
    #   -Wunused-parameter).
    foreach(grammar IN ITEMS cpp slang luau cmake markdown_inline)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            set_target_properties(ludus_editor_tree_sitter_${grammar} PROPERTIES C_STANDARD 23)
        endif()
        target_compile_options(ludus_editor_tree_sitter_${grammar} PRIVATE
            $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-strict-prototypes>)
    endforeach()
    target_compile_options(ludus_editor_tree_sitter_luau PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-implicit-fallthrough -Wno-unused-function -Wno-unused-parameter>)
    # MSVC scanner-only diagnostics. C4244 covers the upstream wctype calls with UTF-32 lookahead
    # on Windows' 16-bit wint_t. C4267 covers the C++/Slang serialized delimiter length and the
    # Markdown indentation counters. Luau's C4146 is intentional unsigned hash negation and its
    # C4100 mirrors the unused valid_symbols parameters documented above. Markdown inline's
    # C4005 is the scanner redefining parser.h's UNUSED macro for MSVC.
    foreach(grammar IN ITEMS cpp slang luau cmake)
        target_compile_options(ludus_editor_tree_sitter_${grammar} PRIVATE $<$<C_COMPILER_ID:MSVC>:/wd4244>)
    endforeach()
    foreach(grammar IN ITEMS cpp slang markdown)
        target_compile_options(ludus_editor_tree_sitter_${grammar} PRIVATE $<$<C_COMPILER_ID:MSVC>:/wd4267>)
    endforeach()
    target_compile_options(ludus_editor_tree_sitter_luau PRIVATE $<$<C_COMPILER_ID:MSVC>:/wd4100 /wd4146>)
    target_compile_options(ludus_editor_tree_sitter_markdown_inline PRIVATE $<$<C_COMPILER_ID:MSVC>:/wd4005>)
endfunction()
