add_library(ludus_foundation_base STATIC)
add_library(Ludus::FoundationBase ALIAS ludus_foundation_base)

if((NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin|Windows)$" AND NOT EMSCRIPTEN) OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Ludus assertions currently require the Linux/macOS/Windows Clang or pinned Emscripten backend")
endif()

set_target_properties(
    ludus_foundation_base
    PROPERTIES
        OUTPUT_NAME ludus_foundation_base
        EXPORT_NAME FoundationBase
)

find_package(Git QUIET)
set(LUDUS_GIT_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE LUDUS_GIT_REVISION_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(LUDUS_GIT_REVISION_OUTPUT)
        set(LUDUS_GIT_REVISION "${LUDUS_GIT_REVISION_OUTPUT}")
    endif()
endif()

set(LUDUS_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(LUDUS_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/build_metadata.hpp.in"
    "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}/ludus/foundation/base/build_metadata.hpp"
    @ONLY
    NEWLINE_STYLE UNIX
)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/assert_config.hpp.in"
    "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}/ludus/foundation/base/assert_config.hpp"
    @ONLY
    NEWLINE_STYLE UNIX
)

target_sources(
    ludus_foundation_base
    PRIVATE
        src/version.cpp
        src/types.cpp
        src/parse_number.cpp
        src/assert.cpp
        src/diagnostic_format.cpp
        src/internal/diagnostic_finish.hpp
        src/internal/diagnostic_format.hpp
        src/internal/diagnostic_text.hpp
        src/diagnostic_control_codec.cpp
        src/internal/diagnostic_platform.hpp
        src/internal/diagnostic_record.hpp
        src/diagnostic.cpp
        src/internal/version_format.hpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
            "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}"
        FILES
            include/ludus/foundation/base/core.h
            include/ludus/foundation/base/config.h
            include/ludus/foundation/base/compiler.h
            include/ludus/foundation/base/target.hpp
            include/ludus/foundation/base/assert.hpp
            include/ludus/foundation/base/assert_format.hpp
            include/ludus/foundation/base/diagnostic_output.hpp
            include/ludus/foundation/base/types.h
            include/ludus/foundation/base/parse_number.hpp
            include/ludus/foundation/base/checked_integer.hpp
            include/ludus/foundation/base/byte_order.hpp
            include/ludus/foundation/base/version.hpp
            "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}/ludus/foundation/base/build_metadata.hpp"
            "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}/ludus/foundation/base/assert_config.hpp"
            include/ludus/foundation/base/pointer.hpp
            include/ludus/foundation/base/diagnostic.hpp
)

target_include_directories(
    ludus_foundation_base
    PRIVATE
        src
)

if(EMSCRIPTEN)
    target_sources(ludus_foundation_base PRIVATE src/diagnostics_web.cpp src/diagnostic_output_web.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Report-only stderr backend; the POSIX socket/helper transports do not exist here.
    target_sources(ludus_foundation_base PRIVATE src/diagnostic_output_windows.cpp src/diagnostics_windows.cpp)
    set_source_files_properties(src/diagnostics_windows.cpp PROPERTIES COMPILE_OPTIONS "-fno-optimize-sibling-calls")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(ludus_foundation_base PRIVATE src/diagnostic_output.cpp src/diagnostics_macos.cpp)
    set_source_files_properties(src/diagnostics_macos.cpp PROPERTIES COMPILE_OPTIONS "-fno-optimize-sibling-calls")
else()
    target_sources(ludus_foundation_base PRIVATE src/diagnostic_output.cpp src/diagnostics_linux.cpp)
    set_source_files_properties(src/diagnostics_linux.cpp PROPERTIES COMPILE_OPTIONS "-fno-optimize-sibling-calls")
endif()

ludus_apply_project_defaults(ludus_foundation_base)
ludus_configure_target_identity(ludus_foundation_base PUBLIC)
set_source_files_properties(src/assert.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-optimize-sibling-calls")

install(
    TARGETS ludus_foundation_base
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_foundation_base_tests)
    target_sources(ludus_foundation_base_tests PRIVATE
        tests/version_tests.cpp tests/assert_tests.cpp tests/assert_format_tests.cpp
        tests/parse_number_tests.cpp tests/checked_integer_tests.cpp tests/byte_order_tests.cpp tests/control_codec_tests.cpp
        tests/pointer_tests.cpp)
    target_include_directories(ludus_foundation_base_tests PRIVATE src)
    target_link_libraries(
        ludus_foundation_base_tests
        PRIVATE
            Ludus::FoundationBase
            Catch2::Catch2WithMain
    )
    ludus_apply_project_defaults(ludus_foundation_base_tests)
    ludus_enable_test_exceptions(ludus_foundation_base_tests)

    add_test(NAME ludus_foundation_base_tests COMMAND ludus_foundation_base_tests)
    set_tests_properties(ludus_foundation_base_tests PROPERTIES LABELS "unit;foundation")

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    find_package(Threads REQUIRED)
    # Both backends use identical driver code and compiler policy. Compile its
    # concurrency-heavy includes once, but keep each runtime in its own binary.
    # Do not link Base here: the fake executable provides a separate runtime.
    add_library(ludus_assert_test_driver OBJECT tests/assert_child.cpp)
    target_include_directories(ludus_assert_test_driver PRIVATE src include "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}")
    target_link_libraries(ludus_assert_test_driver PRIVATE Threads::Threads)
    ludus_apply_project_defaults(ludus_assert_test_driver)

    add_executable(ludus_assert_child $<TARGET_OBJECTS:ludus_assert_test_driver>)
    target_link_libraries(ludus_assert_child PRIVATE Ludus::FoundationBase Threads::Threads)
    ludus_apply_project_defaults(ludus_assert_child)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(ludus_assert_allocation_child tests/assert_allocation_child.cpp)
        target_include_directories(ludus_assert_allocation_child PRIVATE src)
        target_link_libraries(ludus_assert_allocation_child PRIVATE Ludus::FoundationBase)
        target_link_options(ludus_assert_allocation_child PRIVATE
            -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--export-dynamic-symbol=ludus_assert_test_watching)
        ludus_apply_project_defaults(ludus_assert_allocation_child)
        add_test(NAME ludus_assert_allocation
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/assertions/allocation_tests.py"
                "$<TARGET_FILE:ludus_assert_allocation_child>")
        set_tests_properties(ludus_assert_allocation PROPERTIES LABELS "foundation;assert-death" TIMEOUT 30)

        if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
            add_library(ludus_assert_allocation_interposer SHARED tests/assert_allocation_interposer.cpp)
            target_include_directories(ludus_assert_allocation_interposer PRIVATE include)
            ludus_apply_project_defaults(ludus_assert_allocation_interposer)
            add_test(NAME ludus_assert_shared_allocation
                COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=$<TARGET_FILE:ludus_assert_allocation_interposer>"
                    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/assertions/allocation_tests.py"
                    "$<TARGET_FILE:ludus_assert_allocation_child>")
            set_tests_properties(ludus_assert_shared_allocation PROPERTIES LABELS "foundation;assert-death" TIMEOUT 30)
        endif()

        add_test(NAME ludus_assert_blocked_output
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/assertions/blocked_output_tests.py"
                "$<TARGET_FILE:ludus_assert_allocation_child>")
        set_tests_properties(ludus_assert_blocked_output PROPERTIES LABELS "foundation;assert-death" TIMEOUT 30)

        add_executable(ludus_assert_fake_child $<TARGET_OBJECTS:ludus_assert_test_driver> tests/assert_platform_fake.cpp
            src/assert.cpp src/diagnostic_output.cpp src/diagnostic_control_codec.cpp src/diagnostic_format.cpp)
        target_include_directories(ludus_assert_fake_child PRIVATE src include "${LUDUS_FOUNDATION_BASE_GENERATED_INCLUDE_DIR}")
        target_link_libraries(ludus_assert_fake_child PRIVATE Threads::Threads)
        target_link_options(ludus_assert_fake_child PRIVATE -Wl,--wrap=sigtimedwait,--wrap=send)
        ludus_apply_project_defaults(ludus_assert_fake_child)

        foreach(backend native fake)
            if(backend STREQUAL "native")
                set(child ludus_assert_child)
            else()
                set(child ludus_assert_fake_child)
            endif()
            add_test(NAME ludus_assert_${backend}_death
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/assertions/death_tests.py"
                    "$<TARGET_FILE:${child}>" "${LUDUS_ENABLE_ASSERTS}" "${LUDUS_BREAK_ON_CHECK}" "${backend}"
                    "${LUDUS_ASSERT_DIALOGS_AVAILABLE}")
            set_tests_properties(ludus_assert_${backend}_death
                PROPERTIES LABELS "foundation;assert-death;assert-concurrency" TIMEOUT 120)
        endforeach()
    endif() # Linux linker interposition/death harness
endif()
