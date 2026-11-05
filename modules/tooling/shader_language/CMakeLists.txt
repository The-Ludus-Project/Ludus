# Ludus::ShaderLanguage: the LUSL front end (docs/architecture/shading-language.md). L1a provides
# the preprocessor; later stages lower LUSL to Slang for Ludus::ShaderCompiler.
add_library(ludus_shader_language STATIC
    src/condition.cpp src/diagnostic.cpp src/hide_set.cpp src/include_path.cpp src/lexer.cpp
    src/macro_expander.cpp src/macro_table.cpp src/preprocessor.cpp src/token_text.cpp)
add_library(Ludus::ShaderLanguage ALIAS ludus_shader_language)
set_target_properties(ludus_shader_language PROPERTIES EXPORT_NAME ShaderLanguage)
target_compile_features(ludus_shader_language PUBLIC cxx_std_23)
target_sources(ludus_shader_language PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/tooling/shader_language/diagnostic.hpp
    include/ludus/tooling/shader_language/limits.hpp
    include/ludus/tooling/shader_language/preprocessor.hpp
    include/ludus/tooling/shader_language/token.hpp)
target_include_directories(ludus_shader_language PRIVATE src)
target_link_libraries(ludus_shader_language
    PUBLIC Ludus::FoundationContainers Ludus::FoundationStrings Ludus::ShaderCompilerClient
    PRIVATE Ludus::FoundationMemory)
ludus_apply_project_defaults(ludus_shader_language)
install(TARGETS ludus_shader_language EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_shader_language_tests
        tests/lexer_tests.cpp tests/preprocessor_tests.cpp tests/preprocessor_randomized_tests.cpp)
    # Lexer tests reach the internal lexer directly.
    target_include_directories(ludus_shader_language_tests PRIVATE src)
    target_link_libraries(ludus_shader_language_tests PRIVATE
        Ludus::ShaderLanguage Ludus::FoundationMath Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_shader_language_tests)
    ludus_enable_test_exceptions(ludus_shader_language_tests)
    add_test(NAME ludus_shader_language_tests COMMAND ludus_shader_language_tests)
    set_tests_properties(ludus_shader_language_tests PROPERTIES LABELS "unit;shader;tooling" TIMEOUT 120)
endif()

# libFuzzer target over the preprocessor (native Clang only).
option(LUDUS_BUILD_SHADER_LANGUAGE_FUZZERS "Build the instrumented LUSL preprocessor fuzz target" OFF)
if(LUDUS_BUILD_SHADER_LANGUAGE_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LUSL fuzzing requires native Clang/libFuzzer")
    endif()
    target_compile_options(ludus_shader_language PRIVATE -fsanitize=fuzzer-no-link)
    add_executable(ludus_shader_language_preprocessor_fuzzer tests/preprocessor_fuzzer.cpp)
    target_link_libraries(ludus_shader_language_preprocessor_fuzzer PRIVATE Ludus::ShaderLanguage)
    ludus_apply_project_defaults(ludus_shader_language_preprocessor_fuzzer)
    target_compile_options(ludus_shader_language_preprocessor_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(ludus_shader_language_preprocessor_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
