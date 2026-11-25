cmake_minimum_required(VERSION 3.29)
project(hair_skin_bust LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only against the installed SDK, and link NO Qt: the app loads the cooked scene through
# Ludus::Resources, renders through Ludus::GraphicsRenderer, and reads/writes files through
# Ludus::Content, so the packaged app has no Homebrew-Qt runtime dependency. GraphicsRenderer brings
# in the renderer closure, Platform and GraphicsRhi (Metal on macOS). No engine source is compiled.
find_package(Ludus CONFIG REQUIRED COMPONENTS
    GraphicsRenderer Resources Content FoundationFilesystem FoundationTime FoundationLogging FoundationStrings)

# Record the SDK identity for the capture provenance from the installed manifest and the exported
# toolchain variables, never from a version string guessed by the consumer.
file(READ "${Ludus_SDK_MANIFEST}" _hsb_manifest)
string(JSON _hsb_sdk_version ERROR_VARIABLE _hsb_version_error GET "${_hsb_manifest}" "version")
if(_hsb_version_error OR _hsb_sdk_version STREQUAL "")
    set(_hsb_sdk_version "unknown")
endif()

add_executable(hair_skin_capture capture.cpp)
target_compile_features(hair_skin_capture PRIVATE cxx_std_23)
set_target_properties(hair_skin_capture PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(hair_skin_capture PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
target_link_libraries(hair_skin_capture PRIVATE
    Ludus::GraphicsRenderer Ludus::Resources Ludus::Content
    Ludus::FoundationFilesystem Ludus::FoundationTime Ludus::FoundationLogging Ludus::FoundationStrings)
target_compile_definitions(hair_skin_capture PRIVATE
    LUDUS_SDK_VERSION="${_hsb_sdk_version}"
    LUDUS_SDK_SOURCE_REVISION="${Ludus_SOURCE_REVISION}"
    LUDUS_SDK_TARGET="${Ludus_TARGET_TRIPLE}")

# The runtime needs the forward-lit recipes (hair cards ride the lit path) and the clustered-deferred
# recipes plus the three skin SSS passes (skin diffusion). Compile every recipe both packaged graphs
# can activate so one executable renders either feature.
ludus_compile_renderer_shaders(TARGET hair_skin_capture SHADERS
    renderer_flat renderer_composite renderer_output renderer_shadow
    renderer_lit renderer_lit_shadow
    renderer_clustered renderer_clustered_shadow
    renderer_skin_deferred renderer_skin_blur renderer_skin_composite)

if(APPLE)
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
else()
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"spirv\", \"wgsl\"]")
endif()

# Cook BOTH installed render-graph templates to .lurgrt runtime packages, exactly as a project would
# copy them: the forward-lit template (hair cards) and the clustered-deferred default template (skin).
# verify.py stages each cooked .lurgrt beside the app as forward_lit.lurgrt / clustered_deferred.lurgrt
# and the app selects the matching one per capture.
function(hair_skin_cook_graph template name)
    set(source "${Ludus_DIR}/render_graph/templates/${template}")
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/${template}_source")
    file(GLOB_RECURSE _hsb_template_files RELATIVE "${source}" "${source}/*")
    foreach(file IN LISTS _hsb_template_files)
        if(NOT file STREQUAL "main.lushader.json.in")
            configure_file("${source}/${file}" "${staged}/${file}" COPYONLY)
        endif()
    endforeach()
    configure_file("${source}/main.lushader.json.in" "${staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET hair_skin_capture NAME ${name}
        MANIFEST "${staged}/main.lushader.json" RUNTIME_PACKAGE)
    # Surface the cooked package path so the CI action / verify.py can stage it beside the app.
    set(${name}_RUNTIME_PACKAGE "${${name}_RUNTIME_PACKAGE}" PARENT_SCOPE)
endfunction()

hair_skin_cook_graph(forward_lit forward_lit)
hair_skin_cook_graph(default clustered_deferred)

# The RGBA hair opacity texture (infinite-head/hair-cards.png) rides inside the cooked scene.luscn as
# the Hair material's alpha-masked texture, so the packaged app carries the strand mask with no
# engine-source path; verify.py cooks that scene from the authored level before staging.

# Install the executable to bin and its bundled runtime to lib so packaging can relocate it. On macOS
# the renderer closure pulls in the Metal runtime dylibs; the RPATH keeps them beside the executable.
if(APPLE)
    set_target_properties(hair_skin_capture PROPERTIES INSTALL_RPATH "@executable_path/../lib")
else()
    set_target_properties(hair_skin_capture PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS hair_skin_capture RUNTIME DESTINATION bin)
ludus_install_denoiser_runtime(TARGET hair_skin_capture DESTINATION lib)

unset(_hsb_manifest)
unset(_hsb_sdk_version)
