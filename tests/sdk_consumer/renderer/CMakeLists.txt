cmake_minimum_required(VERSION 3.29)
project(LudusRendererConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Ludus CONFIG REQUIRED COMPONENTS Resources GraphicsRenderer GraphicsText FoundationTime)
add_executable(ludus_sdk_renderer_consumer ../renderer.cpp)
target_link_libraries(ludus_sdk_renderer_consumer PRIVATE Ludus::Resources Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_consumer SHADERS renderer_flat)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_consumer PROPERTIES OUTPUT_NAME renderer SUFFIX ".js")
    configure_file(../renderer.html renderer.html COPYONLY)
endif()
add_executable(ludus_sdk_renderer_views_consumer ../renderer_views.cpp)
target_link_libraries(ludus_sdk_renderer_views_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_views_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_views_consumer SHADERS renderer_flat)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_views_consumer SHADERS renderer_composite)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_views_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_views_consumer PROPERTIES OUTPUT_NAME renderer-views SUFFIX ".js")
    configure_file(../renderer-views.html renderer-views.html COPYONLY)
endif()

add_executable(ludus_sdk_renderer_overlays_consumer ../renderer_overlays.cpp)
target_link_libraries(ludus_sdk_renderer_overlays_consumer PRIVATE Ludus::GraphicsText Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_overlays_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_overlays_consumer SHADERS
    renderer_flat renderer_composite renderer_overlay)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_overlays_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_overlays_consumer PROPERTIES OUTPUT_NAME renderer-overlays SUFFIX ".js")
    configure_file(../renderer-overlays.html renderer-overlays.html COPYONLY)
endif()

add_executable(ludus_sdk_renderer_materials_consumer ../renderer_materials.cpp)
target_link_libraries(ludus_sdk_renderer_materials_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_materials_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_materials_consumer SHADERS
    renderer_flat renderer_composite renderer_material)
ludus_compile_shader(TARGET ludus_sdk_renderer_materials_consumer NAME l3_layout RASTER
    SOURCE ../l3_layout.slang VERTEX vertexMain FRAGMENT fragmentMain)
ludus_cook_texture(TARGET ludus_sdk_renderer_materials_consumer NAME l3_color SOURCE ../l3-color.texture.json)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_materials_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_materials_consumer PROPERTIES OUTPUT_NAME renderer-materials SUFFIX ".js")
    configure_file(../renderer-materials.html renderer-materials.html COPYONLY)
endif()

add_executable(ludus_sdk_renderer_lighting_consumer ../renderer_lighting.cpp)
target_link_libraries(ludus_sdk_renderer_lighting_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_lighting_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_lighting_consumer SHADERS
    renderer_flat renderer_composite renderer_lit renderer_shadow renderer_output)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_lighting_consumer SHADERS renderer_lit_shadow)
ludus_cook_texture(TARGET ludus_sdk_renderer_lighting_consumer NAME l4_bake SOURCE ../l4-bake.texture.json)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_lighting_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_lighting_consumer PROPERTIES OUTPUT_NAME renderer-lighting SUFFIX ".js")
    configure_file(../renderer-lighting.html renderer-lighting.html COPYONLY)
endif()

add_executable(ludus_sdk_renderer_scaling_consumer ../renderer_scaling.cpp)
target_link_libraries(ludus_sdk_renderer_scaling_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_renderer_scaling_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_scaling_consumer SHADERS
    renderer_flat renderer_composite renderer_clustered renderer_shadow renderer_output)
ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_scaling_consumer SHADERS renderer_clustered_shadow)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_renderer_scaling_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_renderer_scaling_consumer PROPERTIES OUTPUT_NAME renderer-scaling SUFFIX ".js")
    configure_file(../renderer-scaling.html renderer-scaling.html COPYONLY)
endif()

if(NOT EMSCRIPTEN AND LUDUS_SPIRV_CROSS)
    find_package(Ludus CONFIG REQUIRED COMPONENTS FoundationFilesystem)
    add_executable(ludus_sdk_reference_capture ../reference_capture.cpp)
    target_link_libraries(ludus_sdk_reference_capture PRIVATE Ludus::GraphicsRenderer Ludus::FoundationFilesystem Ludus::FoundationTime)
    target_compile_options(ludus_sdk_reference_capture PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_renderer_shaders(TARGET ludus_sdk_reference_capture SHADERS
        renderer_flat renderer_composite renderer_lit renderer_shadow renderer_output)
    ludus_compile_renderer_shaders(TARGET ludus_sdk_reference_capture SHADERS renderer_lit_shadow)
    ludus_compile_ray_queries(TARGET ludus_sdk_reference_capture NAME ray_query)
endif()

# Build the real offline scene consumer through the same installed-SDK boundary.
if(NOT EMSCRIPTEN AND LUDUS_SPIRV_CROSS)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/reference-scene" reference-scene)
endif()

# Installed authored/HDR declarations and recipe dependencies must compile together.
if(NOT EMSCRIPTEN)
    add_executable(ludus_sdk_renderer_authored_consumer ../renderer_authored.cpp)
    target_link_libraries(ludus_sdk_renderer_authored_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
    target_compile_options(ludus_sdk_renderer_authored_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_renderer_shaders(TARGET ludus_sdk_renderer_authored_consumer SHADERS
        renderer_flat renderer_composite renderer_output renderer_shadow renderer_authored renderer_authored_shadow)
endif()

# RG3: the installed render-graph helper cooks two offscreen graphs into compiled headers and their
# `.lurgrt` runtime generations, here for this platform's only native or browser target: the opaque
# static graph and the masked graph with per-draw uniform values. The consumer activates each
# generation twice and requires byte-identical exported pixels (render_graph_runtime.cpp).
if(EMSCRIPTEN)
    set(RG2_TEST_PROFILES "[\"wgsl\"]")
elseif(APPLE)
    set(RG2_TEST_PROFILES "[\"metal\"]")
else()
    set(RG2_TEST_PROFILES "[\"spirv\"]")
endif()
# The installed scene helper, shared version header and generated API must agree.
add_executable(ludus_sdk_scene_shader_contract_consumer ../scene_shader_contract.cpp)
target_link_libraries(ludus_sdk_scene_shader_contract_consumer PRIVATE Ludus::GraphicsRenderer)
target_compile_options(ludus_sdk_scene_shader_contract_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
set(scene_ray_source "${CMAKE_CURRENT_SOURCE_DIR}/../rg2/scene_ray")
set(scene_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg3-inputs/scene_ray")
file(COPY "${scene_ray_source}/" DESTINATION "${scene_ray_staged}" PATTERN "*.in" EXCLUDE)
configure_file("${scene_ray_source}/main.lushader.json.in" "${scene_ray_staged}/main.lushader.json" @ONLY)
ludus_compile_render_graph(TARGET ludus_sdk_scene_shader_contract_consumer NAME rg2_scene_ray
    MANIFEST "${scene_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)

add_executable(ludus_sdk_render_graph_runtime_consumer ../render_graph_runtime.cpp)
target_link_libraries(ludus_sdk_render_graph_runtime_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_render_graph_runtime_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_renderer_shaders(TARGET ludus_sdk_render_graph_runtime_consumer SHADERS renderer_composite)
set(rg3_embedded "${CMAKE_CURRENT_BINARY_DIR}/rg3-embedded")
file(MAKE_DIRECTORY "${rg3_embedded}")
foreach(rg3_fixture offscreen masked_color)
    if(rg3_fixture STREQUAL "offscreen")
        set(rg3_name rg3_runtime)
    else()
        set(rg3_name rg3_masked)
    endif()
    set(rg3_input "${CMAKE_CURRENT_SOURCE_DIR}/../rg2/${rg3_fixture}")
    set(rg3_staged "${CMAKE_CURRENT_BINARY_DIR}/rg3-inputs/${rg3_fixture}")
    file(COPY "${rg3_input}/" DESTINATION "${rg3_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${rg3_input}/main.lushader.json.in" "${rg3_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_sdk_render_graph_runtime_consumer NAME ${rg3_name}
        MANIFEST "${rg3_staged}/main.lushader.json" RUNTIME_PACKAGE)
    string(TOUPPER "${rg3_name}" rg3_constant)
    add_custom_command(OUTPUT "${rg3_embedded}/${rg3_name}_package.h"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${${rg3_name}_RUNTIME_PACKAGE}"
            "-DOUTPUT=${rg3_embedded}/${rg3_name}_package.h" -DNAME=${rg3_constant}_PACKAGE
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_bytes.cmake"
        DEPENDS "${${rg3_name}_RUNTIME_PACKAGE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_bytes.cmake"
        VERBATIM)
    target_sources(ludus_sdk_render_graph_runtime_consumer PRIVATE "${rg3_embedded}/${rg3_name}_package.h")
endforeach()
target_include_directories(ludus_sdk_render_graph_runtime_consumer PRIVATE "${rg3_embedded}")
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_render_graph_runtime_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_render_graph_runtime_consumer PROPERTIES OUTPUT_NAME render-graph-runtime SUFFIX ".js")
    configure_file(../render-graph-runtime.html render-graph-runtime.html COPYONLY)
endif()

# RG4: exact-descriptor transient pooling evidence. The consumer rewrites the embedded offscreen
# generation into its scratch workload, compares serial-control and pooled exports byte for byte,
# and logs allocation counts, RHI telemetry bytes and CPU p50/p95 times (render_graph_pooling.cpp).
add_executable(ludus_sdk_render_graph_pooling_consumer ../render_graph_pooling.cpp "${rg3_embedded}/rg3_runtime_package.h")
target_link_libraries(ludus_sdk_render_graph_pooling_consumer PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime)
target_compile_options(ludus_sdk_render_graph_pooling_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
target_include_directories(ludus_sdk_render_graph_pooling_consumer PRIVATE "${rg3_embedded}")
ludus_compile_renderer_shaders(TARGET ludus_sdk_render_graph_pooling_consumer SHADERS renderer_composite)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_render_graph_pooling_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_render_graph_pooling_consumer PROPERTIES OUTPUT_NAME render-graph-pooling SUFFIX ".js")
    configure_file(../render-graph-pooling.html render-graph-pooling.html COPYONLY)
endif()
