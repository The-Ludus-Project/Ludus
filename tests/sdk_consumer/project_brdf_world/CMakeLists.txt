cmake_minimum_required(VERSION 3.29)
project(project_brdf_world LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only against the installed SDK, and link no Qt: the app loads the Editor-exported scene through
# Ludus::Resources, renders through Ludus::GraphicsRenderer and reads and writes files through Ludus::Content.
# No engine source is compiled. See docs/development/project-sdk-workflow.md#qualify-a-project-brdf-provider.
find_package(Ludus CONFIG REQUIRED COMPONENTS
    GraphicsRenderer Resources Content FoundationTime FoundationLogging FoundationStrings)

# Record the SDK identity from the installed manifest and the exported toolchain variables.
file(READ "${Ludus_SDK_MANIFEST}" _pbw_manifest)
string(JSON _pbw_sdk_version ERROR_VARIABLE _pbw_version_error GET "${_pbw_manifest}" "version")
if(_pbw_version_error OR _pbw_sdk_version STREQUAL "")
    set(_pbw_sdk_version "unknown")
endif()

add_executable(project_brdf_capture capture.cpp)
target_compile_features(project_brdf_capture PRIVATE cxx_std_23)
set_target_properties(project_brdf_capture PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(project_brdf_capture PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
target_link_libraries(project_brdf_capture PRIVATE
    Ludus::GraphicsRenderer Ludus::Resources Ludus::Content
    Ludus::FoundationTime Ludus::FoundationLogging Ludus::FoundationStrings)
target_compile_definitions(project_brdf_capture PRIVATE
    LUDUS_SDK_VERSION="${_pbw_sdk_version}"
    LUDUS_SDK_SOURCE_REVISION="${Ludus_SOURCE_REVISION}"
    LUDUS_SDK_TARGET="${Ludus_TARGET_TRIPLE}")
# The graph's composite pass presents through the renderer's composite recipe.
ludus_compile_renderer_shaders(TARGET project_brdf_capture SHADERS renderer_composite)

if(APPLE)
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
else()
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"spirv\", \"wgsl\"]")
endif()

# Cook the installed compute ray-tracing template, unchanged, as a project copies it: the general graph that draws
# the built-in control and the project's closure program alike. verify.py stages the cooked package beside the app
# as compute_ray_tracing.lurgrt.
set(_pbw_template "${Ludus_DIR}/render_graph/templates/compute_ray_tracing")
set(_pbw_staged "${CMAKE_CURRENT_BINARY_DIR}/compute_ray_tracing_source")
file(GLOB_RECURSE _pbw_template_files RELATIVE "${_pbw_template}" "${_pbw_template}/*")
foreach(file IN LISTS _pbw_template_files)
    if(NOT file STREQUAL "main.lushader.json.in")
        configure_file("${_pbw_template}/${file}" "${_pbw_staged}/${file}" COPYONLY)
    endif()
endforeach()
configure_file("${_pbw_template}/main.lushader.json.in" "${_pbw_staged}/main.lushader.json" @ONLY)
ludus_compile_render_graph(TARGET project_brdf_capture NAME compute_ray_tracing
    MANIFEST "${_pbw_staged}/main.lushader.json" RUNTIME_PACKAGE)

if(APPLE)
    set_target_properties(project_brdf_capture PROPERTIES INSTALL_RPATH "@executable_path/../lib")
else()
    set_target_properties(project_brdf_capture PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS project_brdf_capture RUNTIME DESTINATION bin)
install(FILES "${compute_ray_tracing_RUNTIME_PACKAGE}" DESTINATION . RENAME compute_ray_tracing.lurgrt)
ludus_install_denoiser_runtime(TARGET project_brdf_capture DESTINATION lib)

unset(_pbw_manifest)
unset(_pbw_sdk_version)
