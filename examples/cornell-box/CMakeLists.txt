cmake_minimum_required(VERSION 3.29)
project(LudusCornellBox LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)

# Independent consumer: configure never downloads or builds engine sources.
find_package(Ludus CONFIG REQUIRED COMPONENTS FoundationBase FoundationMath FoundationLogging Platform GraphicsRhi GameplayCamera)
if(EMSCRIPTEN OR NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    message(FATAL_ERROR "This tutorial currently supports native Linux/Vulkan and macOS/Metal only")
endif()
add_executable(cornell_box src/main.cpp src/camera.cpp)
if(APPLE)
    set_target_properties(cornell_box PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.ludus.samples.cornell-box"
        MACOSX_BUNDLE_BUNDLE_NAME "Ludus Cornell Box"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
endif()
target_include_directories(cornell_box PRIVATE src)
target_link_libraries(cornell_box PRIVATE Ludus::FoundationBase Ludus::FoundationMath Ludus::FoundationLogging Ludus::Platform Ludus::GraphicsRhi Ludus::GameplayCamera)
ludus_compile_shader(TARGET cornell_box NAME cornell SOURCE shaders/cornell.slang
    VERTEX vertexMain FRAGMENT fragmentMain)
option(CORNELL_ENABLE_SANITIZERS "Instrument the sample with ASan and UBSan" OFF)
set(CORNELL_TARGETS cornell_box)
if(BUILD_TESTING)
    add_executable(cornell_camera_tests tests/camera_tests.cpp src/camera.cpp)
    target_include_directories(cornell_camera_tests PRIVATE src)
    target_link_libraries(cornell_camera_tests PRIVATE Ludus::FoundationMath Ludus::GameplayCamera)
    list(APPEND CORNELL_TARGETS cornell_camera_tests)
    add_test(NAME cornell_camera COMMAND cornell_camera_tests)
    add_test(NAME cornell_help COMMAND cornell_box --help)
    add_test(NAME cornell_reject_unbounded_headless COMMAND cornell_box --headless)
    set_tests_properties(cornell_reject_unbounded_headless PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cornell_render COMMAND cornell_box --headless --frames=3)
    add_test(NAME cornell_materials COMMAND cornell_box --headless --flat --frames=3)
    set_tests_properties(cornell_camera cornell_help cornell_reject_unbounded_headless cornell_render cornell_materials
        PROPERTIES TIMEOUT 30)
    if(CORNELL_ENABLE_SANITIZERS AND NOT APPLE)
        # Keep Vulkan drivers loaded until exit, so LeakSanitizer scans their globals and symbolizes their
        # stacks instead of reporting one-time driver allocations as leaks from unloaded modules.
        # Thanks to Khronos, "Vulkan Loader Interface Architecture", table of debug environment variables:
        # https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md#table-of-debug-environment-variables
        set_tests_properties(cornell_render cornell_materials
            PROPERTIES ENVIRONMENT "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1")
    endif()
endif()

foreach(target IN LISTS CORNELL_TARGETS)
    ludus_apply_app_policy(${target})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef
        -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Woverloaded-virtual -Wnon-virtual-dtor -Werror -ffp-contract=off)
    if(CORNELL_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
