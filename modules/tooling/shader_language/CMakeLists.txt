# Ludus::ShaderLanguage: the LUSL front end (docs/architecture/shading-language.md). L1a provides
# the preprocessor, L1b the parser, L1c the semantic checker and L1d the lowering to Slang for Ludus::ShaderCompiler.
add_library(ludus_shader_language STATIC
    src/access_path.cpp src/call_binding.cpp src/checker.cpp src/condition.cpp src/diagnostic.cpp src/emitter.cpp src/fix_builder.cpp src/flow_analysis.cpp src/formatter.cpp
    src/hide_set.cpp src/include_path.cpp src/intrinsics.cpp src/lexer.cpp
    src/lowering.cpp src/macro_expander.cpp src/macro_table.cpp src/parser.cpp src/pipeline.cpp src/preprocessor.cpp src/scope_table.cpp src/slang_emitter.cpp src/style_rules.cpp src/token_text.cpp
    src/type_table.cpp)
add_library(Ludus::ShaderLanguage ALIAS ludus_shader_language)
set_target_properties(ludus_shader_language PROPERTIES EXPORT_NAME ShaderLanguage)
target_compile_features(ludus_shader_language PUBLIC cxx_std_23)
target_sources(ludus_shader_language PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/tooling/shader_language/diagnostic.hpp
    include/ludus/tooling/shader_language/emitter.hpp
    include/ludus/tooling/shader_language/formatter.hpp
    include/ludus/tooling/shader_language/limits.hpp
    include/ludus/tooling/shader_language/pipeline.hpp
    include/ludus/tooling/shader_language/preprocessor.hpp
    include/ludus/tooling/shader_language/semantic.hpp
    include/ludus/tooling/shader_language/status.hpp
    include/ludus/tooling/shader_language/syntax.hpp
    include/ludus/tooling/shader_language/token.hpp)
target_include_directories(ludus_shader_language PRIVATE src)
target_link_libraries(ludus_shader_language
    PUBLIC Ludus::FoundationContainers Ludus::FoundationStrings Ludus::ShaderCompilerClient
    PRIVATE Ludus::FoundationMemory)
ludus_apply_project_defaults(ludus_shader_language)
# The optional native compiler library lowers LUSL in process; as a shared library it needs its
# static closure position-independent (Windows links it statically).
if(TARGET ludus_shader_compiler AND NOT WIN32)
    set_target_properties(ludus_shader_language PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
install(TARGETS ludus_shader_language EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_shader_language_tests
        tests/checker_tests.cpp tests/emitter_tests.cpp tests/flow_style_tests.cpp tests/formatter_tests.cpp tests/interface_tests.cpp tests/lexer_tests.cpp tests/parser_tests.cpp tests/preprocessor_tests.cpp
        tests/preprocessor_randomized_tests.cpp)
    # Lexer tests reach the internal lexer directly.
    target_include_directories(ludus_shader_language_tests PRIVATE src)
    target_link_libraries(ludus_shader_language_tests PRIVATE
        Ludus::ShaderLanguage Ludus::FoundationMath Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_shader_language_tests)
    ludus_enable_test_exceptions(ludus_shader_language_tests)
    add_test(NAME ludus_shader_language_tests COMMAND ludus_shader_language_tests)
    set_tests_properties(ludus_shader_language_tests PROPERTIES LABELS "unit;shader;tooling" TIMEOUT 120)
endif()

# libFuzzer target over the preprocessor, parser and formatter (native Clang only).
option(LUDUS_BUILD_SHADER_LANGUAGE_FUZZERS "Build the instrumented LUSL front-end fuzz target" OFF)
if(LUDUS_BUILD_SHADER_LANGUAGE_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LUSL fuzzing requires native Clang/libFuzzer")
    endif()
    target_compile_options(ludus_shader_language PRIVATE -fsanitize=fuzzer-no-link)
    add_executable(ludus_shader_language_fuzzer tests/front_end_fuzzer.cpp)
    target_link_libraries(ludus_shader_language_fuzzer PRIVATE Ludus::ShaderLanguage)
    ludus_apply_project_defaults(ludus_shader_language_fuzzer)
    target_compile_options(ludus_shader_language_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(ludus_shader_language_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Target tests compile lowered LUSL with the pinned Slang in process, so they need the optional
# native compiler library built with the shader worker.
if(LUDUS_BUILD_TESTS AND TARGET ludus_shader_compiler)
    add_executable(ludus_shader_language_target_tests tests/emitter_target_tests.cpp)
    target_link_libraries(ludus_shader_language_target_tests PRIVATE
        Ludus::ShaderLanguage Ludus::ShaderCompiler Ludus::FoundationFilesystem Catch2::Catch2WithMain)
    # The SDK's LUSL declarations are compiled against the Slang files they declare.
    target_compile_definitions(ludus_shader_language_target_tests PRIVATE
        LUDUS_MATH_SHADER_DIRECTORY="${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders")
    ludus_apply_project_defaults(ludus_shader_language_target_tests)
    ludus_enable_test_exceptions(ludus_shader_language_target_tests)
    if(WIN32)
        # Slang and DXC load from the worker's directory, where it deploys them.
        add_dependencies(ludus_shader_language_target_tests ludus-shader-worker)
        set_target_properties(ludus_shader_language_target_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ludus-shader-worker>")
    else()
        # Slang dlopens its SPIR-V optimizer beside itself; name that directory as the compiler tests do.
        set_target_properties(ludus_shader_language_target_tests PROPERTIES BUILD_RPATH "${LUDUS_SLANG_ROOT}/lib")
    endif()
    add_test(NAME ludus_shader_language_target_tests COMMAND ludus_shader_language_target_tests)
    set_tests_properties(ludus_shader_language_target_tests PROPERTIES LABELS "unit;shader;tooling" TIMEOUT 120)
endif()
