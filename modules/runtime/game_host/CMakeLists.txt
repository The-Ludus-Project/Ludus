# Ludus::GameHost — Qt-free static runtime library (design section 2).
#
# Owns engine lifecycles, the game window, the RHI session and ONE play session.
# Loads a project's native gameplay module (dlopen) or dispatches to a statically
# linked implementation. The editor never links this; a project-owned host
# executable and the static shipping executable both reuse this library's
# entry/lifecycle logic. No Qt dependency anywhere in its closure.

add_library(ludus_runtime_game_host STATIC)
add_library(Ludus::GameHost ALIAS ludus_runtime_game_host)

set_target_properties(ludus_runtime_game_host PROPERTIES
    OUTPUT_NAME ludus_runtime_game_host
    EXPORT_NAME GameHost
)

# Generate the host/module ABI identity header from ACTUAL build inputs. The
# string both host and module stamp and the host compares exactly before Create.
set(LUDUS_GAME_ABI_MAJOR 1)
set(LUDUS_GAME_ABI_MINOR 1)
if(NOT DEFINED LUDUS_TARGET_TRIPLE OR LUDUS_TARGET_TRIPLE STREQUAL "")
    set(LUDUS_TARGET_TRIPLE "${CMAKE_CXX_COMPILER_TARGET}")
endif()
if(LUDUS_TARGET_TRIPLE STREQUAL "")
    set(LUDUS_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
endif()
set(LUDUS_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(LUDUS_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

set(LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/game_host_identity.hpp.in"
    "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}/ludus/runtime/game_host/game_host_identity.hpp"
    @ONLY
    NEWLINE_STYLE UNIX
)

target_sources(ludus_runtime_game_host
    PRIVATE
        src/host.cpp
        src/entry.cpp
        src/frame_presenter.cpp
        src/internal/frame_presenter.h
        src/host_services.cpp
        src/host_session.cpp
        src/property_exchange.cpp
        src/identity.cpp
        src/module_loader.cpp
        src/protocol.cpp
        src/protocol_codec.cpp
        src/timing_telemetry.cpp
        src/internal/host_services.h
        src/internal/host_session.h
        src/internal/identity.h
        src/internal/module_loader.h
        src/internal/protocol_codec.h
        src/internal/timing_telemetry.h
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES
            include/ludus/runtime/game_host/host.h
            include/ludus/runtime/game_host/protocol.h
)

target_include_directories(ludus_runtime_game_host
    PRIVATE
        src
        "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}"
)

target_link_libraries(ludus_runtime_game_host
    PUBLIC
        Ludus::FoundationBase
        Ludus::GameApi
    PRIVATE
        Ludus::FoundationLogging
        Ludus::FoundationConfigJson
        Ludus::RuntimeConfiguration
        Ludus::FoundationContainers
        Ludus::FoundationTime
        Ludus::Input
        Ludus::Platform
        Ludus::PlatformPacing
        Ludus::GraphicsRhi
        ${CMAKE_DL_LIBS}
)

ludus_apply_project_defaults(ludus_runtime_game_host)

install(TARGETS ludus_runtime_game_host
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# L0 two-generation feasibility: build two tiny gameplay fixture modules (A and
# B) as MODULE libraries, plus a module that exports no entry and one with a
# wrong identity, then prove concurrent load, hidden symbols, correct code
# dispatch, and rejection of missing/incompatible modules.
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)

    # Fixture module A and B share one source but differ by a compile define, so
    # each has its own code and the test can prove A and B run their own copy.
    foreach(variant A B C D H)
        add_library(ludus_game_fixture_${variant} MODULE tests/fixture_module.cpp)
        target_link_libraries(ludus_game_fixture_${variant} PRIVATE Ludus::GameApi Threads::Threads)
        target_compile_definitions(ludus_game_fixture_${variant} PRIVATE
            LUDUS_FIXTURE_VARIANT=${variant}
            LUDUS_FIXTURE_IDENTITY="${LUDUS_GAME_COMPATIBILITY_IDENTITY}")
        ludus_apply_project_defaults(ludus_game_fixture_${variant})
        # Publication acceptance verifies embedded symbols in every policy
        # profile, including optimized Release. Keep the full acceptance there;
        # these test-only images retain symbols without changing optimization.
        target_compile_options(ludus_game_fixture_${variant} PRIVATE -g)
        set_target_properties(ludus_game_fixture_${variant} PROPERTIES
            PREFIX "" OUTPUT_NAME "game_fixture_${variant}"
            C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    endforeach()

    # A module whose identity is deliberately wrong (rejected before Create).
    add_library(ludus_game_fixture_badid MODULE tests/fixture_module.cpp)
    target_link_libraries(ludus_game_fixture_badid PRIVATE Ludus::GameApi Threads::Threads)
    target_compile_definitions(ludus_game_fixture_badid PRIVATE
        LUDUS_FIXTURE_VARIANT=A
        LUDUS_FIXTURE_IDENTITY="wrong-sdk-identity-should-be-rejected")
    ludus_apply_project_defaults(ludus_game_fixture_badid)
    set_target_properties(ludus_game_fixture_badid PROPERTIES
        PREFIX "" OUTPUT_NAME "game_fixture_badid"
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

    # A module that exports no entry symbol (EntryMissing).
    add_library(ludus_game_fixture_noentry MODULE tests/fixture_noentry.cpp)
    ludus_apply_project_defaults(ludus_game_fixture_noentry)
    set_target_properties(ludus_game_fixture_noentry PROPERTIES
        PREFIX "" OUTPUT_NAME "game_fixture_noentry"
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

    add_executable(ludus_game_host_loader_tests tests/loader_tests.cpp)
    target_include_directories(ludus_game_host_loader_tests PRIVATE src "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}")
    target_link_libraries(ludus_game_host_loader_tests PRIVATE
        Ludus::GameHost Ludus::GameApi Ludus::FoundationLogging Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    ludus_apply_project_defaults(ludus_game_host_loader_tests)
    ludus_enable_test_exceptions(ludus_game_host_loader_tests)
    add_dependencies(ludus_game_host_loader_tests
        ludus_game_fixture_A ludus_game_fixture_B ludus_game_fixture_badid ludus_game_fixture_noentry)
    target_compile_definitions(ludus_game_host_loader_tests PRIVATE
        LUDUS_FIXTURE_A_PATH="$<TARGET_FILE:ludus_game_fixture_A>"
        LUDUS_FIXTURE_B_PATH="$<TARGET_FILE:ludus_game_fixture_B>"
        LUDUS_FIXTURE_BADID_PATH="$<TARGET_FILE:ludus_game_fixture_badid>"
        LUDUS_FIXTURE_NOENTRY_PATH="$<TARGET_FILE:ludus_game_fixture_noentry>")
    add_test(NAME ludus_game_host_loader_tests COMMAND ludus_game_host_loader_tests)
    set_tests_properties(ludus_game_host_loader_tests PROPERTIES TIMEOUT 120 LABELS "unit;runtime;loader")

    add_executable(ludus_game_host_protocol_tests tests/protocol_tests.cpp)
    target_include_directories(ludus_game_host_protocol_tests PRIVATE src)
    target_link_libraries(ludus_game_host_protocol_tests PRIVATE
        Ludus::GameHost Ludus::FoundationContainers Ludus::FoundationLogging Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_game_host_protocol_tests)
    ludus_enable_test_exceptions(ludus_game_host_protocol_tests)
    add_test(NAME ludus_game_host_protocol_tests COMMAND ludus_game_host_protocol_tests)
    set_tests_properties(ludus_game_host_protocol_tests PROPERTIES TIMEOUT 60 LABELS "unit;runtime;protocol")

    add_executable(ludus_game_host_timing_tests tests/timing_tests.cpp)
    target_include_directories(ludus_game_host_timing_tests PRIVATE src)
    target_link_libraries(ludus_game_host_timing_tests PRIVATE
        Ludus::GameHost Ludus::FoundationContainers Ludus::FoundationTime Ludus::PlatformPacing Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_game_host_timing_tests)
    ludus_enable_test_exceptions(ludus_game_host_timing_tests)
    add_test(NAME ludus_game_host_timing_tests COMMAND ludus_game_host_timing_tests)
    set_tests_properties(ludus_game_host_timing_tests PROPERTIES TIMEOUT 60 LABELS "unit;runtime;timing")

    add_executable(ludus_game_host_session_tests tests/session_tests.cpp)
    target_include_directories(ludus_game_host_session_tests PRIVATE src "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}")
    target_link_libraries(ludus_game_host_session_tests PRIVATE
        Ludus::GameHost Ludus::GameApi Ludus::FoundationContainers Ludus::FoundationLogging Ludus::FoundationTime
        Ludus::PlatformPacing Catch2::Catch2WithMain Threads::Threads ${CMAKE_DL_LIBS})
    ludus_apply_project_defaults(ludus_game_host_session_tests)
    ludus_enable_test_exceptions(ludus_game_host_session_tests)
    add_dependencies(ludus_game_host_session_tests ludus_game_fixture_A ludus_game_fixture_B ludus_game_fixture_badid)
    target_compile_definitions(ludus_game_host_session_tests PRIVATE
        LUDUS_FIXTURE_A_PATH="$<TARGET_FILE:ludus_game_fixture_A>"
        LUDUS_FIXTURE_B_PATH="$<TARGET_FILE:ludus_game_fixture_B>"
        LUDUS_FIXTURE_BADID_PATH="$<TARGET_FILE:ludus_game_fixture_badid>")
    add_test(NAME ludus_game_host_session_tests COMMAND ludus_game_host_session_tests)
    set_tests_properties(ludus_game_host_session_tests PROPERTIES TIMEOUT 120 LABELS "unit;runtime;session;reload")

    add_executable(ludus_game_host_reload_tests tests/reload_tests.cpp)
    target_include_directories(ludus_game_host_reload_tests PRIVATE src "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}")
    target_link_libraries(ludus_game_host_reload_tests PRIVATE
        Ludus::GameHost Ludus::GameApi Ludus::FoundationContainers Ludus::FoundationLogging Ludus::FoundationTime
        Ludus::PlatformPacing Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    ludus_apply_project_defaults(ludus_game_host_reload_tests)
    ludus_enable_test_exceptions(ludus_game_host_reload_tests)
    add_dependencies(ludus_game_host_reload_tests ludus_game_fixture_A ludus_game_fixture_B)
    target_compile_definitions(ludus_game_host_reload_tests PRIVATE
        LUDUS_FIXTURE_A_PATH="$<TARGET_FILE:ludus_game_fixture_A>"
        LUDUS_FIXTURE_B_PATH="$<TARGET_FILE:ludus_game_fixture_B>")
    add_test(NAME ludus_game_host_reload_tests COMMAND ludus_game_host_reload_tests)
    set_tests_properties(ludus_game_host_reload_tests PROPERTIES TIMEOUT 180 LABELS "unit;runtime;reload;stress")

    # Debug-only native source stepping acceptance. Release/ASan run all
    # runtime regressions; this journey requires usable Debug source/locals.
    find_program(LUDUS_GDB NAMES gdb)
    find_package(Python3 3.10 COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "Native play-supervisor acceptance requires Python 3.10+")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
        add_test(NAME ludus_game_host_configuration_acceptance
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/configuration_acceptance.py"
                "$<TARGET_FILE:ludus_game_host_app>" "$<TARGET_FILE:ludus_game_fixture_A>")
        set_tests_properties(ludus_game_host_configuration_acceptance PROPERTIES
            TIMEOUT 60 LABELS "runtime;configuration;acceptance")
        add_test(NAME ludus_game_host_supervisor_acceptance
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/python/test_play_host_acceptance.py"
                --root "${PROJECT_SOURCE_DIR}"
                --host "$<TARGET_FILE:ludus_game_host_app>"
                --a "$<TARGET_FILE:ludus_game_fixture_A>"
                --b "$<TARGET_FILE:ludus_game_fixture_B>"
                --crash "$<TARGET_FILE:ludus_game_fixture_C>"
                --hang "$<TARGET_FILE:ludus_game_fixture_H>"
                --destructor-crash "$<TARGET_FILE:ludus_game_fixture_D>")
        set_tests_properties(ludus_game_host_supervisor_acceptance PROPERTIES
            TIMEOUT 120 LABELS "runtime;session;supervisor;reload")
        if(APPLE)
            get_filename_component(_ludus_compiler_real "${CMAKE_CXX_COMPILER}" REALPATH)
            get_filename_component(_ludus_compiler_bin "${_ludus_compiler_real}" DIRECTORY)
            find_program(LUDUS_DSYMUTIL NAMES dsymutil HINTS "${_ludus_compiler_bin}" NO_DEFAULT_PATH REQUIRED)
            set_tests_properties(ludus_game_host_supervisor_acceptance PROPERTIES
                ENVIRONMENT "LUDUS_DSYMUTIL=${LUDUS_DSYMUTIL}")
        endif()
    endif() # Native process supervisor/configuration journeys
    set(_ludus_debugger_eligible OFF)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug"
       AND NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        set(_ludus_debugger_eligible ON)
    endif()
    if(_ludus_debugger_eligible)
        if(NOT LUDUS_GDB OR NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR "Debug gameplay acceptance requires GDB and Python 3.10+; install these tools")
        endif()
        # Single-process same-session reload journey harness the debugger steps.
        add_executable(ludus_game_host_debugger_journey tests/debugger_journey.cpp)
        target_include_directories(ludus_game_host_debugger_journey PRIVATE src "${LUDUS_GAME_HOST_GENERATED_INCLUDE_DIR}")
        target_link_libraries(ludus_game_host_debugger_journey PRIVATE
            Ludus::GameHost Ludus::GameApi Ludus::FoundationLogging Ludus::FoundationContainers Ludus::FoundationTime
            Ludus::PlatformPacing ${CMAKE_DL_LIBS})
        ludus_apply_project_defaults(ludus_game_host_debugger_journey)
        add_dependencies(ludus_game_host_debugger_journey ludus_game_fixture_A ludus_game_fixture_B)

        if(LUDUS_GDB)
            add_test(NAME ludus_game_host_debugger_acceptance
                COMMAND Python3::Interpreter
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdb_acceptance.py"
                    "${LUDUS_GDB}"
                    "$<TARGET_FILE:ludus_game_host_debugger_journey>"
                    "$<TARGET_FILE:ludus_game_fixture_A>"
                    "$<TARGET_FILE:ludus_game_fixture_B>")
        endif()
        if(LUDUS_GDB)
            set_tests_properties(ludus_game_host_debugger_acceptance PROPERTIES
                TIMEOUT 180 LABELS "runtime;debugger;acceptance")
            add_test(NAME ludus_game_host_debugger_attach_acceptance
                COMMAND Python3::Interpreter
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdb_acceptance.py"
                    "${LUDUS_GDB}"
                    "$<TARGET_FILE:ludus_game_host_debugger_journey>"
                    "$<TARGET_FILE:ludus_game_fixture_A>"
                    "$<TARGET_FILE:ludus_game_fixture_B>" --attach)
            set_tests_properties(ludus_game_host_debugger_attach_acceptance PROPERTIES
                TIMEOUT 180 LABELS "runtime;debugger;acceptance")
        endif()
    endif()
endif()
