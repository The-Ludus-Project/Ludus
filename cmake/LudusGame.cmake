# LudusGame.cmake — helper for a project that consumes the installed Ludus SDK
# to build a gameplay module plus its host and shipping executables
# (project-live-reload design section 2). Installed with the SDK and included by
# LudusConfig.cmake, so a project does `find_package(Ludus CONFIG REQUIRED)` and
# then calls ludus_add_game(...).
#
# The same game source has ONE implementation. ludus_add_game builds:
#   <name>_module   — a MODULE library the dev host dlopens (reloadable).
#   <name>_host     — a project-owned host executable (links Ludus::GameHost).
#   <name>_shipping — a static executable linking the game sources directly
#                     (selecting static dispatch replaces only the module lookup).
#
# A gameplay module links ONLY Ludus::GameApi (no engine singleton runtime); the
# host and shipping executables link Ludus::GameHost. The helper stamps hidden
# visibility and exactly one exported entry on the module, matching the loader's
# RTLD_LOCAL contract.

include_guard(GLOBAL)

# Apply the required Ludus runtime policy to a gameplay target built against the
# installed SDK (design 2/15): C++23, no GNU extensions, no C++ exceptions,
# warning-clean, hidden visibility default. Gameplay/runtime targets are
# production targets and must carry the same policy as engine code; the SDK does
# not relax it for external consumers.
function(_ludus_apply_game_policy target)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL Ludus_SDK_COMPILER_ID OR
       NOT CMAKE_CXX_COMPILER_VERSION STREQUAL Ludus_SDK_COMPILER_VERSION)
        message(FATAL_ERROR "Gameplay toolchain must exactly match the SDK: ${Ludus_SDK_COMPILER_ID} ${Ludus_SDK_COMPILER_VERSION}")
    endif()
    target_compile_features(${target} PRIVATE cxx_std_23)
    # Ludus uses no C++20 named modules. Left on, CMake (CMP0155) runs
    # clang-scan-deps on every C++ source and collates each target before it
    # compiles; see docs/development/build-profiling.md. Set per target rather
    # than CMAKE_CXX_SCAN_FOR_MODULES so the project's own targets keep CMake's
    # default. Set the property back ON for a game that adopts named modules.
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -fno-exceptions
            -Wall -Wextra -Wpedantic -Werror)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c- /W4 /WX)
    endif()
    foreach(sanitizer ASAN UBSAN TSAN)
        if(Ludus_SDK_${sanitizer})
            if(sanitizer STREQUAL "ASAN")
                set(flag address)
            elseif(sanitizer STREQUAL "UBSAN")
                set(flag undefined)
            else()
                set(flag thread)
            endif()
            target_compile_options(${target} PRIVATE -fsanitize=${flag} -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=${flag})
        endif()
    endforeach()
endfunction()

function(ludus_add_game)
    set(options "")
    set(oneValue NAME)
    set(multiValue SOURCES)
    cmake_parse_arguments(GAME "${options}" "${oneValue}" "${multiValue}" ${ARGN})

    if(NOT GAME_NAME)
        message(FATAL_ERROR "ludus_add_game: NAME is required")
    endif()
    if(NOT GAME_SOURCES)
        message(FATAL_ERROR "ludus_add_game: SOURCES is required")
    endif()

    # --- Reloadable gameplay module (dev) ---
    add_library(${GAME_NAME}_module MODULE ${GAME_SOURCES})
    target_link_libraries(${GAME_NAME}_module PRIVATE Ludus::GameApi)
    if(DEFINED Ludus_SDK_IDENTITY)
        target_compile_definitions(${GAME_NAME}_module PRIVATE LUDUS_EXAMPLE_IDENTITY="${Ludus_SDK_IDENTITY}")
    endif()
    set_target_properties(${GAME_NAME}_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${GAME_NAME}"
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # Optional stateless helpers linked from static SDK archives must stay
    # private too; source visibility flags alone do not hide archive symbols.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${GAME_NAME}_module PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    _ludus_apply_game_policy(${GAME_NAME}_module)

    # --- Project-owned host executable ---
    if(TARGET Ludus::GameHost)
        add_executable(${GAME_NAME}_host "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LudusGameHostMain.cpp")
        target_link_libraries(${GAME_NAME}_host PRIVATE Ludus::GameHost)
        set_target_properties(${GAME_NAME}_host PROPERTIES OUTPUT_NAME "${GAME_NAME}_host")
        _ludus_apply_game_policy(${GAME_NAME}_host)
        # Do NOT set ENABLE_EXPORTS: the module calls back only through the
        # passed host service table, never host-exported C++ symbols, so the host
        # must not export its whole symbol table (design 6).

        # --- Static shipping executable: same game sources linked directly ---
        add_executable(${GAME_NAME}_shipping
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LudusGameShippingMain.cpp"
            ${GAME_SOURCES})
        target_link_libraries(${GAME_NAME}_shipping PRIVATE Ludus::GameHost Ludus::GameApi)
        target_compile_definitions(${GAME_NAME}_shipping PRIVATE LUDUS_GAME_STATIC_DISPATCH=1)
        if(DEFINED Ludus_SDK_IDENTITY)
            target_compile_definitions(${GAME_NAME}_shipping PRIVATE LUDUS_EXAMPLE_IDENTITY="${Ludus_SDK_IDENTITY}")
        endif()
        set_target_properties(${GAME_NAME}_shipping PROPERTIES OUTPUT_NAME "${GAME_NAME}")
        _ludus_apply_game_policy(${GAME_NAME}_shipping)
        if(WIN32)
            # The DLL and player share a base name. Separate the player's linked
            # PDB, import library and Agility runtime from the development build.
            set(shipping_directory "${CMAKE_CURRENT_BINARY_DIR}/shipping")
            set_target_properties(${GAME_NAME}_shipping PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${shipping_directory}"
                ARCHIVE_OUTPUT_DIRECTORY "${shipping_directory}"
                PDB_OUTPUT_DIRECTORY "${shipping_directory}")
            ludus_enable_d3d12_agility(${GAME_NAME}_host)
            ludus_enable_d3d12_agility(${GAME_NAME}_shipping)
        endif()
    endif()
endfunction()
