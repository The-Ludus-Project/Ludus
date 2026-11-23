cmake_minimum_required(VERSION 3.29)
project(hair_skin_bust LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only against the installed SDK. GraphicsRenderer brings in the renderer closure, Platform and
# GraphicsRhi (Metal on macOS); no engine source is compiled.
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer Resources)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

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
target_link_libraries(hair_skin_capture PRIVATE Ludus::GraphicsRenderer Ludus::Resources Qt6::Core Qt6::Gui)
target_compile_definitions(hair_skin_capture PRIVATE
    LUDUS_SDK_VERSION="${_hsb_sdk_version}"
    LUDUS_SDK_SOURCE_REVISION="${Ludus_SOURCE_REVISION}"
    LUDUS_SDK_TARGET="${Ludus_TARGET_TRIPLE}")

# The forward lit profile carries the authored hair-card strand highlight; the clustered deferred
# default profile carries the real-time skin diffusion passes. The fixture compiles both so one
# executable can render either feature of the saved World.
ludus_compile_renderer_shaders(TARGET hair_skin_capture SHADERS
    renderer_flat renderer_composite renderer_output renderer_shadow
    renderer_lit renderer_lit_shadow
    renderer_clustered renderer_clustered_shadow
    renderer_skin_deferred renderer_skin_blur renderer_skin_composite)

# Cook the installed forward lit template exactly as a project would copy it, selecting the Metal and
# WGSL targets on Apple and SPIR-V and WGSL elsewhere.
set(_hsb_forward_source "${Ludus_DIR}/render_graph/templates/forward_lit")
set(_hsb_forward_staged "${CMAKE_CURRENT_BINARY_DIR}/forward_lit_source")
foreach(file main.lurender.json rasterize.lurender.json main_shaders/geometry.slang main_shaders/color.slang)
    configure_file("${_hsb_forward_source}/${file}" "${_hsb_forward_staged}/${file}" COPYONLY)
endforeach()
if(APPLE)
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
else()
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"spirv\", \"wgsl\"]")
endif()
configure_file("${_hsb_forward_source}/main.lushader.json.in" "${_hsb_forward_staged}/main.lushader.json" @ONLY)
ludus_compile_render_graph(TARGET hair_skin_capture NAME forward_lit
    MANIFEST "${_hsb_forward_staged}/main.lushader.json" RUNTIME_PACKAGE)

# The RGBA hair opacity texture (infinite-head/hair-cards.png) is an imported alpha-masked map, not a
# raw-texel recipe, so it is staged beside the app by verify.py (like raster_baseline stages its
# fixtures) rather than cooked here. The packaged game therefore carries the strand mask with no
# engine-source path.

# Install the executable and its bundled runtime so packaging can relocate it. On macOS the renderer
# closure pulls in the Metal runtime dylibs; INSTALL_RPATH keeps them beside the executable.
set_target_properties(hair_skin_capture PROPERTIES INSTALL_RPATH "@executable_path/../lib")
install(TARGETS hair_skin_capture RUNTIME DESTINATION .)
ludus_install_denoiser_runtime(TARGET hair_skin_capture DESTINATION .)

unset(_hsb_manifest)
unset(_hsb_sdk_version)
