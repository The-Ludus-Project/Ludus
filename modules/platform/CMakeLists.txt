# The entropy adapter is its own small target so tools that need only UUIDv4
# entropy (ludus-cook) do not link the window backends, AppKit or Wayland
# (docs/architecture/object-handles-uuid.md). Ludus::Platform re-exports it.
add_library(ludus_platform_entropy STATIC)
add_library(Ludus::PlatformEntropy ALIAS ludus_platform_entropy)
set_target_properties(ludus_platform_entropy PROPERTIES
    OUTPUT_NAME ludus_platform_entropy EXPORT_NAME PlatformEntropy)
target_sources(ludus_platform_entropy
    PRIVATE src/entropy.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
        FILES include/ludus/platform/entropy.h)
# FoundationIdentity owns the EntropySource contract; Identity never depends on Platform.
target_link_libraries(ludus_platform_entropy PUBLIC Ludus::FoundationBase Ludus::FoundationIdentity)
ludus_apply_project_defaults(ludus_platform_entropy)

# Native frame pacing is its own small target so frame loops and their tests can
# use the policy without linking window backends
# (docs/architecture/high-resolution-time.md, "Native frame pacing").
add_library(ludus_platform_pacing STATIC)
add_library(Ludus::PlatformPacing ALIAS ludus_platform_pacing)
set_target_properties(ludus_platform_pacing PROPERTIES
    OUTPUT_NAME ludus_platform_pacing EXPORT_NAME PlatformPacing)
target_sources(ludus_platform_pacing
    PRIVATE src/frame_pacing.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
        FILES include/ludus/platform/frame_pacing.h)
target_link_libraries(ludus_platform_pacing PUBLIC Ludus::FoundationBase Ludus::FoundationTime)
ludus_apply_project_defaults(ludus_platform_pacing)

add_library(ludus_platform STATIC)
add_library(Ludus::Platform ALIAS ludus_platform)

set_target_properties(
    ludus_platform
    PROPERTIES
        OUTPUT_NAME ludus_platform
        EXPORT_NAME Platform
)

# Wayland is the preferred Linux windowing backend, but it is optional at build
# time: if the development packages, protocol definitions, and wayland-scanner
# are all present we compile the Wayland backend; otherwise the module still
# builds against a headless null backend (see src/window.cpp). This keeps a
# clean clone buildable on headless CI runners and hosts without Wayland.
set(LUDUS_PLATFORM_WAYLAND_AVAILABLE OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
        find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)

        if(WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
            set(LUDUS_PLATFORM_WAYLAND_AVAILABLE ON)
        endif()
    endif()
endif()

option(LUDUS_PLATFORM_ENABLE_WAYLAND "Build the Wayland windowing backend" ${LUDUS_PLATFORM_WAYLAND_AVAILABLE})

if(LUDUS_PLATFORM_ENABLE_WAYLAND AND NOT LUDUS_PLATFORM_WAYLAND_AVAILABLE)
    message(FATAL_ERROR
        "LUDUS_PLATFORM_ENABLE_WAYLAND is ON but the Wayland build prerequisites were not found. "
        "Install the Wayland client dev headers, wayland-protocols, and wayland-scanner "
        "(see AGENTS.md / ./init.sh), or leave the option unset to build the headless backend.")
endif()

target_sources(
    ludus_platform
    PRIVATE
        src/window.cpp
        src/window_base.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/platform/browser/window.h
            include/ludus/platform/config.h
            include/ludus/platform/keyboard_sink.h
            include/ludus/platform/log_categories.h
            include/ludus/platform/native_window.h
            include/ludus/platform/base/window.h
            include/ludus/platform/wayland/window.h
            include/ludus/platform/headless/window.h
)

target_include_directories(
    ludus_platform
    PRIVATE
        src
)

target_link_libraries(
    ludus_platform
    PUBLIC
        Ludus::FoundationBase
        Ludus::PlatformEntropy
        Ludus::PlatformPacing
        Ludus::FoundationLogging
        # One-way dependency: Platform depends on Input for the normalized
        # keyboard record/sink vocabulary. Input never depends on Platform.
        Ludus::Input
)

if(EMSCRIPTEN)
    message(STATUS "Ludus platform: browser canvas backend enabled")
    target_sources(ludus_platform PRIVATE src/window_web.cpp)
    target_compile_definitions(ludus_platform PRIVATE LUDUS_PLATFORM_BROWSER=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    ludus_get_pinned_llvm_major(ludus_llvm_major)
    if(NOT CMAKE_OBJCXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_OBJCXX_COMPILER_VERSION MATCHES "^${ludus_llvm_major}\\.")
        message(FATAL_ERROR "The Cocoa backend requires the pinned upstream Clang ${ludus_llvm_major} Objective-C++ compiler")
    endif()
    function(ludus_configure_cocoa_target target)
        set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;-fno-objc-exceptions>)
    endfunction()
    message(STATUS "Ludus platform: Cocoa backend enabled")
    target_sources(ludus_platform PRIVATE src/window_cocoa.mm)
    target_compile_definitions(ludus_platform PRIVATE LUDUS_PLATFORM_COCOA=1)
    target_link_libraries(ludus_platform PRIVATE "-framework AppKit" "-framework CoreFoundation" "-framework CoreGraphics")
    ludus_configure_cocoa_target(ludus_platform)
elseif(LUDUS_PLATFORM_ENABLE_WAYLAND)
    message(STATUS "Ludus platform: Wayland backend enabled")

    pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)

    set(LUDUS_PLATFORM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(LUDUS_PLATFORM_WAYLAND_GENERATED_DIR "${LUDUS_PLATFORM_GENERATED_DIR}/wayland")
    set(LUDUS_PLATFORM_XDG_SHELL_XML "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml")
    set(LUDUS_PLATFORM_XDG_SHELL_HEADER "${LUDUS_PLATFORM_WAYLAND_GENERATED_DIR}/xdg-shell-client-protocol.h")
    set(LUDUS_PLATFORM_XDG_SHELL_CODE "${LUDUS_PLATFORM_WAYLAND_GENERATED_DIR}/xdg-shell-protocol.cpp")

    add_custom_command(
        OUTPUT
            "${LUDUS_PLATFORM_XDG_SHELL_HEADER}"
            "${LUDUS_PLATFORM_XDG_SHELL_CODE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LUDUS_PLATFORM_WAYLAND_GENERATED_DIR}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${LUDUS_PLATFORM_XDG_SHELL_XML}" "${LUDUS_PLATFORM_XDG_SHELL_HEADER}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${LUDUS_PLATFORM_XDG_SHELL_XML}" "${LUDUS_PLATFORM_XDG_SHELL_CODE}"
        DEPENDS
            "${LUDUS_PLATFORM_XDG_SHELL_XML}"
        VERBATIM
    )

    add_custom_target(
        ludus_platform_wayland_protocols
        DEPENDS
            "${LUDUS_PLATFORM_XDG_SHELL_HEADER}"
            "${LUDUS_PLATFORM_XDG_SHELL_CODE}"
    )

    target_sources(ludus_platform PRIVATE src/window_wayland.cpp)
    target_include_directories(ludus_platform PRIVATE "${LUDUS_PLATFORM_WAYLAND_GENERATED_DIR}")
    add_dependencies(ludus_platform ludus_platform_wayland_protocols)
    target_link_libraries(ludus_platform PRIVATE PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(ludus_platform PRIVATE LUDUS_PLATFORM_WAYLAND=1)
else()
    message(STATUS "Ludus platform: native window backend unavailable, building headless window backend")
    target_sources(ludus_platform PRIVATE src/window_headless.cpp)
    target_compile_definitions(ludus_platform PRIVATE LUDUS_PLATFORM_HEADLESS=1)
endif()

ludus_apply_project_defaults(ludus_platform)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_platform_headless_tests tests/headless_tests.cpp src/window_headless.cpp)
    target_include_directories(ludus_platform_headless_tests PRIVATE include)
    # Compile the headless backend independently while reusing WindowBase's
    # exception-free implementation from the production platform library.
    target_link_libraries(ludus_platform_headless_tests PRIVATE Ludus::Platform Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_platform_headless_tests)
    ludus_enable_test_exceptions(ludus_platform_headless_tests)
    add_test(NAME ludus_platform_headless_tests COMMAND ludus_platform_headless_tests)
    set_tests_properties(ludus_platform_headless_tests PROPERTIES LABELS "unit;platform")

    add_executable(ludus_platform_entropy_tests tests/entropy_tests.cpp)
    target_link_libraries(ludus_platform_entropy_tests PRIVATE
        Ludus::PlatformEntropy Ludus::FoundationIdentity Ludus::FoundationContainers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_platform_entropy_tests)
    ludus_enable_test_exceptions(ludus_platform_entropy_tests)
    add_test(NAME ludus_platform_entropy_tests COMMAND ludus_platform_entropy_tests)
    set_tests_properties(ludus_platform_entropy_tests PROPERTIES LABELS "unit;platform;identity")

    add_executable(ludus_platform_pacing_tests tests/frame_pacing_tests.cpp)
    target_link_libraries(ludus_platform_pacing_tests PRIVATE Ludus::PlatformPacing Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_platform_pacing_tests)
    ludus_enable_test_exceptions(ludus_platform_pacing_tests)
    add_test(NAME ludus_platform_pacing_tests COMMAND ludus_platform_pacing_tests)
    set_tests_properties(ludus_platform_pacing_tests PROPERTIES LABELS "unit;platform;time")

    add_executable(ludus_platform_cocoa_keymap_tests tests/cocoa_keymap_tests.cpp)
    target_include_directories(ludus_platform_cocoa_keymap_tests PRIVATE src)
    target_link_libraries(ludus_platform_cocoa_keymap_tests PRIVATE Ludus::Input Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_platform_cocoa_keymap_tests)
    ludus_enable_test_exceptions(ludus_platform_cocoa_keymap_tests)
    add_test(NAME ludus_platform_cocoa_keymap_tests COMMAND ludus_platform_cocoa_keymap_tests)
    set_tests_properties(ludus_platform_cocoa_keymap_tests PROPERTIES LABELS "unit;platform;input")
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(ludus_platform_cocoa_tests tests/cocoa_tests.mm)
    target_link_libraries(ludus_platform_cocoa_tests PRIVATE Ludus::Platform Catch2::Catch2WithMain "-framework AppKit")
    ludus_apply_project_defaults(ludus_platform_cocoa_tests)
    ludus_enable_test_exceptions(ludus_platform_cocoa_tests)
    ludus_configure_cocoa_target(ludus_platform_cocoa_tests)
    add_test(NAME ludus_platform_cocoa_tests COMMAND ludus_platform_cocoa_tests)
    set_tests_properties(ludus_platform_cocoa_tests PROPERTIES LABELS "platform;macos;integration" TIMEOUT 30 RUN_SERIAL TRUE SKIP_RETURN_CODE 4)
endif()

if(LUDUS_BUILD_TESTS AND LUDUS_PLATFORM_ENABLE_WAYLAND)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_platform_wayland_tests tests/wayland_event_tests.cpp)
    # The visibility cases name xdg_toplevel_state values from the generated protocol header.
    target_include_directories(ludus_platform_wayland_tests PRIVATE "${LUDUS_PLATFORM_WAYLAND_GENERATED_DIR}")
    add_dependencies(ludus_platform_wayland_tests ludus_platform_wayland_protocols)
    target_link_libraries(ludus_platform_wayland_tests PRIVATE
        Ludus::Platform Catch2::Catch2WithMain PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(ludus_platform_wayland_tests PRIVATE LUDUS_PLATFORM_WAYLAND=1)
    ludus_apply_project_defaults(ludus_platform_wayland_tests)
    ludus_enable_test_exceptions(ludus_platform_wayland_tests)
    add_test(NAME ludus_platform_wayland_tests COMMAND ludus_platform_wayland_tests)
    set_tests_properties(ludus_platform_wayland_tests PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 4)

    # Compositor-INDEPENDENT keyboard-normalization tests: exercise the private
    # evdev->Key table and the sink seam without a running compositor. These
    # always run (no SKIP) since they need only Linux input-event-codes + Input.
    add_executable(ludus_platform_keyboard_tests tests/wayland_keyboard_tests.cpp)
    target_include_directories(ludus_platform_keyboard_tests PRIVATE src include)
    target_link_libraries(ludus_platform_keyboard_tests PRIVATE
        Ludus::Platform Ludus::Input Catch2::Catch2WithMain PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(ludus_platform_keyboard_tests PRIVATE LUDUS_PLATFORM_WAYLAND=1)
    ludus_apply_project_defaults(ludus_platform_keyboard_tests)
    ludus_enable_test_exceptions(ludus_platform_keyboard_tests)
    add_test(NAME ludus_platform_keyboard_tests COMMAND ludus_platform_keyboard_tests)
    set_tests_properties(ludus_platform_keyboard_tests PROPERTIES LABELS "unit;platform;input")

    # LIVE compositor keyboard lifecycle. Skips (code 4) unless LUDUS_TEST_WAYLAND
    # is set with a running compositor.
    add_executable(ludus_platform_live_keyboard_tests tests/wayland_live_keyboard_tests.cpp)
    target_link_libraries(ludus_platform_live_keyboard_tests PRIVATE
        Ludus::Platform Ludus::Input Catch2::Catch2WithMain PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(ludus_platform_live_keyboard_tests PRIVATE LUDUS_PLATFORM_WAYLAND=1)
    ludus_apply_project_defaults(ludus_platform_live_keyboard_tests)
    ludus_enable_test_exceptions(ludus_platform_live_keyboard_tests)
    add_test(NAME ludus_platform_live_keyboard_tests COMMAND ludus_platform_live_keyboard_tests)
    set_tests_properties(ludus_platform_live_keyboard_tests PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 4)
endif()

install(
    TARGETS ludus_platform_entropy ludus_platform_pacing ludus_platform
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
