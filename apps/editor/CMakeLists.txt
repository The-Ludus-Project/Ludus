# Optional Qt Widgets editor workspace (native E0 and browser preview).
#
# This file is only entered when LUDUS_BUILD_EDITOR=ON (see the top-level
# CMakeLists.txt). All Qt discovery is local to this directory so a default
# (Editor OFF) native/browser configure never searches for Qt (E01). Qt headers
# stay in private app headers and .cpp files; no Editor target or header is added
# to the installed Ludus SDK export set.

# --- Native-host gating ------------------------------------------------------
# Native hosts share Qt Widgets and the owned POSIX process adapter.
if(NOT EMSCRIPTEN)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        # Linux reference host.
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|x86_64|amd64|AMD64)$")
        # Cocoa Qt plugin; Metal belongs to the separately launched game.
    else()
        message(FATAL_ERROR "The native Editor supports Linux x64 and macOS arm64/x64; browser builds require the pinned Qt WebAssembly toolchain.")
    endif()
endif()

if(EMSCRIPTEN)
    set(editor_process_sources src/browser_processes.cpp)
    set(editor_audio_source src/audio_preview_browser.cpp)
else()
    set(editor_process_sources src/tool_process.cpp src/play_process.cpp src/source_watch.cpp)
    # The startup sound needs a device that may start without a user gesture: native only.
    set(editor_audio_source src/audio_preview.cpp src/startup_sound_player.cpp)
endif()

# --- Qt discovery (local to the enabled editor only) -------------------------
# Use APIs available in Qt 6.4; require at least 6.4. Core/Gui/Widgets only, as
# native system libraries or the pinned source-built WebAssembly Qt.
# Qt is installed only by opted-in editor setup;
# it is never added to the engine Conan recipe or SDK.
if(EMSCRIPTEN)
    find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Widgets)
else()
    find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Widgets Concurrent)
endif()
if(NOT Qt6_FOUND)
    ludus_dependency_error("Editor workspace" "Qt 6.4+ Core, Gui, Widgets and native Concurrent"
        "Qt development packages are missing or incompatible. Qt6_DIR='${Qt6_DIR}'."
        "For native Linux setup, run ./init.sh --cli --with-editor from the repository root. For browser setup, follow docs/development/browser-editor.md. For an existing Qt installation set Qt6_DIR; or disable the editor with -DLUDUS_BUILD_EDITOR=OFF.")
endif()

# QT_NO_KEYWORDS forbids the unprefixed signals/slots/emit macros so Qt's macro
# vocabulary cannot collide with engine/standard identifiers; use Q_SIGNALS/
# Q_SLOTS/Q_EMIT and the typed (pointer-to-member) connect overloads instead.
# AUTOMOC is enabled per target below, not globally.

# --- Build-service policy limits ----------------------------------------------
# Keep UI bounds and document admission tied to the owning build-service policy.
# Read after Qt discovery so a missing Qt still reports its setup diagnostic.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(scene_query_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_ray.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scene_query_policy}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import json,sys; sys.path.insert(0, sys.argv[1]); from ludus_tools.render_graph_ray import SCENE_QUERY_LIMITS; print(json.dumps(SCENE_QUERY_LIMITS))"
        "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE scene_query_status OUTPUT_VARIABLE scene_query_limits ERROR_VARIABLE scene_query_error)
if(NOT scene_query_status EQUAL 0)
    message(FATAL_ERROR "Cannot read scene query limits: ${scene_query_error}")
endif()
set(scene_query_definitions)
foreach(limit MIN_SURFACE_QUERIES MAX_SURFACE_QUERIES MIN_VISIBILITY_QUERIES MAX_VISIBILITY_QUERIES)
    string(JSON scene_query_limit GET "${scene_query_limits}" "${limit}")
    list(APPEND scene_query_definitions "LUDUS_EDITOR_${limit}=${scene_query_limit}")
endforeach()
set_source_files_properties(src/render_graph_scene_authoring.cpp
    PROPERTIES COMPILE_DEFINITIONS "${scene_query_definitions}")

# Import the compiler's admitted workgroup shapes into the shared canvas/Properties form.
set(scene_compute_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_ray.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scene_compute_policy}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import sys; sys.path.insert(0, sys.argv[1]); import json; from ludus_tools.render_graph_ray import SUPPORTED_WORKGROUP_SIZES; print(json.dumps(SUPPORTED_WORKGROUP_SIZES,separators=(',',':')))"
        "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE scene_compute_status OUTPUT_VARIABLE scene_compute_group_width ERROR_VARIABLE scene_compute_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT scene_compute_status EQUAL 0)
    message(FATAL_ERROR "Cannot read the scene compute group size: ${scene_compute_error}")
endif()
set_source_files_properties(src/render_graph_pipeline_node.cpp
    PROPERTIES COMPILE_DEFINITIONS "LUDUS_EDITOR_SCENE_COMPUTE_GROUPS=R\"policy(${scene_compute_group_width})policy\"")

# The settings form mirrors the build service's admission policy, not a second copy.
set(graph_settings_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_settings.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${graph_settings_policy}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import json,sys; sys.path.insert(0, sys.argv[1]); from ludus_tools import render_graph_settings as policy; print(json.dumps({key:getattr(policy,key) for key in ('MAX_SETTINGS','MAX_NUMERIC_VALUE','MAX_SETTING_LABEL_LENGTH')}))"
        "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE graph_settings_status OUTPUT_VARIABLE graph_settings_limits ERROR_VARIABLE graph_settings_error)
if(NOT graph_settings_status EQUAL 0)
    message(FATAL_ERROR "Cannot read graph settings limits: ${graph_settings_error}")
endif()
set(graph_settings_definitions)
foreach(limit MAX_SETTINGS MAX_NUMERIC_VALUE MAX_SETTING_LABEL_LENGTH)
    string(JSON graph_settings_limit GET "${graph_settings_limits}" "${limit}")
    list(APPEND graph_settings_definitions "LUDUS_EDITOR_${limit}=${graph_settings_limit}")
endforeach()
set_source_files_properties(src/render_graph_settings_editor.cpp src/render_graph_configuration_template.cpp
    tests/render_graph_template_tests.cpp
    PROPERTIES COMPILE_DEFINITIONS "${graph_settings_definitions}")

# Shader authoring shares the build service's target-artifact and pipeline ceilings
# (render_graph_build_limits.h). Both counts follow from the manifest; no authored budget bounds them.
set(shader_build_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_build.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader_build_policy}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import sys; sys.path.insert(0,sys.argv[1]); from ludus_tools.render_graph_build import LIMITS; print(LIMITS['artifacts'], LIMITS['pipelines'])"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE shader_build_status OUTPUT_VARIABLE shader_build_limits ERROR_VARIABLE shader_build_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT shader_build_status EQUAL 0)
    message(FATAL_ERROR "Cannot read shader build limits: ${shader_build_error}")
endif()
separate_arguments(shader_build_limits)
list(GET shader_build_limits 0 shader_artifact_limit)
list(GET shader_build_limits 1 shader_pipeline_limit)
set_property(SOURCE src/render_graph_shader_creation.cpp src/render_graph_configuration_template.cpp
    src/render_graph_techniques.cpp tests/render_graph_template_tests.cpp tests/render_graph_pipeline_tests.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS
    "LUDUS_EDITOR_MAX_SHADER_ARTIFACTS=${shader_artifact_limit}" "LUDUS_EDITOR_MAX_SHADER_PIPELINES=${shader_pipeline_limit}")

# Native snapshot transport uses the backend owner's encoded admission policy.
set(shader_context_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/shader_context.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader_context_policy}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import sys; sys.path.insert(0,sys.argv[1]); from ludus_tools.shader_context import MAX_DRAFT_INPUT_BYTES; print(MAX_DRAFT_INPUT_BYTES)"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE shader_context_status OUTPUT_VARIABLE shader_context_input_limit ERROR_VARIABLE shader_context_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT shader_context_status EQUAL 0)
    message(FATAL_ERROR "Cannot read shader context wire limit: ${shader_context_error}")
endif()
set_source_files_properties(src/render_graph_shader_assistance.cpp PROPERTIES
    COMPILE_DEFINITIONS "LUDUS_EDITOR_SHADER_CONTEXT_INPUT_BYTES=${shader_context_input_limit}")

# Finite option admission has one owner shared with installed host tooling.
set(graph_options_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_options.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${graph_options_policy}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import json,sys; sys.path.insert(0,sys.argv[1]); from ludus_tools import render_graph_options as policy; print(json.dumps({key:getattr(policy,key) for key in ('MAX_OPTIONS','MAX_VALUES','MAX_COMBINATIONS','MAX_LABEL_LENGTH','MAX_ENUM_LENGTH','MAX_NUMERIC_VALUE')}))"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE graph_options_status OUTPUT_VARIABLE graph_options_limits ERROR_VARIABLE graph_options_error)
if(NOT graph_options_status EQUAL 0)
    message(FATAL_ERROR "Cannot read option limits: ${graph_options_error}")
endif()
set(graph_options_definitions)
foreach(limit MAX_OPTIONS MAX_VALUES MAX_COMBINATIONS MAX_LABEL_LENGTH MAX_ENUM_LENGTH MAX_NUMERIC_VALUE)
    string(JSON graph_options_limit GET "${graph_options_limits}" "${limit}")
    list(APPEND graph_options_definitions "LUDUS_EDITOR_OPTIONS_${limit}=${graph_options_limit}")
endforeach()
set_source_files_properties(src/render_graph_options.cpp PROPERTIES COMPILE_DEFINITIONS "${graph_options_definitions}")

# The offline layout schema owns admission limits used by the Qt editor as well.
set(graph_layout_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_layout.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${graph_layout_policy}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import json,sys; sys.path.insert(0,sys.argv[1]); from ludus_tools import render_graph_layout as policy; print(json.dumps({key:value for key,value in vars(policy).items() if key.isupper()}))"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE graph_layout_status OUTPUT_VARIABLE graph_layout_limits ERROR_VARIABLE graph_layout_error)
if(NOT graph_layout_status EQUAL 0)
    message(FATAL_ERROR "Cannot read canvas layout limits: ${graph_layout_error}")
endif()
foreach(limit MAX_WIRE_ROUTES MAX_WIRE_REROUTE_POINTS MAX_COORDINATE MAX_COMMENT_FRAMES MAX_COMMENT_TITLE_CODEPOINTS MAX_COMMENT_NOTES_CODEPOINTS MIN_COMMENT_WIDTH MIN_COMMENT_HEIGHT)
    string(JSON ${limit} GET "${graph_layout_limits}" "${limit}")
endforeach()
set(graph_layout_directory "${CMAKE_CURRENT_BINARY_DIR}/comment-policy")
configure_file(src/internal/render_graph_layout_policy.h.in
    "${graph_layout_directory}/render_graph_layout_policy.h" @ONLY)
include_directories("${graph_layout_directory}")

# The CLI validator and package builder own the package source limits; the editor document holds
# exactly what they admit, so a package the CLI can build always opens and accepts templates.
set(graph_package_policies
    "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph.py"
    "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_build.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${graph_package_policies})
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import json,sys; sys.path.insert(0,sys.argv[1]); from ludus_tools import render_graph as graph, render_graph_build as build; print(json.dumps({'MAX_ASSETS': graph.MAX_ASSETS, 'MAX_ASSET_BYTES': graph.MAX_ASSET_BYTES, 'MAX_TOTAL_BYTES': graph.MAX_TOTAL_BYTES, 'MAX_FILES': build.MAX_FILES, 'MAX_FILE': build.MAX_FILE, 'MAX_BYTES': build.MAX_BYTES}))"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE graph_package_status OUTPUT_VARIABLE graph_package_limits ERROR_VARIABLE graph_package_error)
if(NOT graph_package_status EQUAL 0)
    message(FATAL_ERROR "Cannot read render-graph package limits: ${graph_package_error}")
endif()
foreach(limit MAX_ASSETS MAX_ASSET_BYTES MAX_TOTAL_BYTES MAX_FILES MAX_FILE MAX_BYTES)
    string(JSON ${limit} GET "${graph_package_limits}" "${limit}")
endforeach()
configure_file(src/internal/render_graph_package_policy.h.in
    "${graph_layout_directory}/render_graph_package_policy.h" @ONLY)

# One colour and spacing token file shared with the init window and the browser editor.
include(DesignTokens.cmake)

set(editor_viewport_sources
    src/internal/viewport_device_description.h
    src/viewport_device_description.cpp
    src/internal/viewport_frame_source.h
    src/viewport_frame_source.cpp
    src/internal/empty_world_view.h
    src/empty_world_view.cpp
    src/internal/viewport_size.h
    src/viewport_size.cpp
    src/internal/viewport_screenshot.h
    src/viewport_screenshot.cpp
    src/internal/ray_path_overlay.h
    src/ray_path_overlay.cpp
    src/preview_viewport.cpp
    src/internal/viewport_pass_bars.h
    src/viewport_pass_bars.cpp
    src/internal/viewport_object_overlay.h
    src/internal/mesh_silhouette.h
    src/mesh_silhouette.cpp
    src/internal/viewport_object_interaction.h
    src/viewport_object_interaction.cpp
    src/internal/viewport_gizmo_readout.h
    src/viewport_gizmo_readout.cpp
    src/viewport_navigation.cpp
    src/viewport_walk_collision.cpp
    src/internal/orientation_widget.h
    src/orientation_widget.cpp
    src/viewport_play_input.cpp
    src/internal/texture_inspection.h
    src/texture_inspection.cpp)
if(APPLE)
    list(APPEND editor_viewport_sources src/internal/viewport_relative_mouse.h src/viewport_relative_mouse_macos.mm)
endif()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick)
    list(APPEND editor_viewport_sources src/internal/viewport_object_overlay_item.h
        src/viewport_object_overlay_item.cpp src/viewport_object_overlay_geometry.cpp)
    # The readback fallback is selected only on Linux, but the shared viewport code references it.
    list(APPEND editor_viewport_sources src/internal/offscreen_frame_reader.h src/offscreen_frame_reader.cpp)
    # RG3b trusted render-graph preview reads exported color back through the same frame reader.
    list(APPEND editor_viewport_sources src/internal/render_graph_preview.h src/render_graph_preview.cpp
        src/internal/viewport_raster_capture.h src/viewport_raster_capture.cpp
        src/internal/viewport_graph_capture.h src/viewport_graph_capture.cpp
        src/internal/graph_comparison_run.h src/graph_comparison_run.cpp
        src/internal/graph_comparison_window.h src/graph_comparison_window.cpp
        src/internal/saved_graph_comparison_view.h src/saved_graph_comparison_view.cpp
        src/internal/render_graph_capture_profile.h src/render_graph_capture_profile.cpp
        src/internal/render_graph_library_capture.h src/render_graph_library_capture.cpp
        src/internal/graph_display_frames.h src/graph_display_frames.cpp
        src/internal/world_graph_view.h src/world_graph_view.cpp
        src/internal/render_graph_worker.h src/render_graph_worker.cpp)
    # Ray path inspection: the diagnostic capture run, its workflow, the inspector panel and its dock.
    list(APPEND editor_viewport_sources
        src/internal/ray_path_capture.h src/ray_path_capture.cpp
        src/internal/ray_path_inspection.h src/ray_path_inspection.cpp
        src/internal/ray_path_inspector.h src/ray_path_inspector.cpp
        src/main_window_ray_paths.cpp)
    # The Renderer tab's texture viewer reads any graph attachment (colour or depth) back the same way.
    list(APPEND editor_viewport_sources
        src/internal/render_graph_targets.h src/render_graph_targets.cpp
        src/internal/render_target_reader.h src/render_target_reader.cpp
        src/internal/texture_viewer.h src/texture_viewer.cpp)
    # Play in the viewport: the game's hosted window and its imported frames as a frame source.
    list(APPEND editor_viewport_sources src/internal/play_viewport_source.h src/play_viewport_source.cpp)
    if(APPLE)
        list(APPEND editor_viewport_sources src/qt_viewport_device_metal.mm)
    else()
        list(APPEND editor_viewport_sources src/qt_viewport_device_vulkan.cpp)
    endif()
endif()

# Live viewport shaders rebuild the built-in renderer recipes from this checkout through the
# headless ludus-shader-worker (no Slang library enters the editor). The generated header names
# the recipe roots owned by LudusRendererShaders.cmake, the worker this build produced (empty
# without LUDUS_BUILD_SHADER_WORKER) and the pinned SPIR-V validator.
function(ludus_editor_enable_shader_reload target)
    ludus_get_renderer_shader_roots(recipe_directory include_directories)
    set(includes "")
    foreach(directory IN LISTS include_directories)
        string(APPEND includes "R\"ludus(${directory})ludus\", ")
    endforeach()
    set(worker "")
    if(TARGET ludus-shader-worker)
        set(worker "$<TARGET_FILE:ludus-shader-worker>")
        add_dependencies(${target} ludus-shader-worker)
    endif()
    set(validator "")
    if(NOT APPLE AND LUDUS_SPIRV_VALIDATOR)
        set(validator "${LUDUS_SPIRV_VALIDATOR}")
    endif()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/shader-reload/${target}/$<CONFIG>")
    file(GENERATE OUTPUT "${directory}/ludus_editor_shader_reload_paths.h" CONTENT
"#pragma once
// Generated by apps/editor/CMakeLists.txt; paths of this build's renderer shader sources and tools.
namespace ludus::editor::shader_reload_paths
{
inline constexpr const char* RECIPE_DIRECTORY = R\"ludus(${recipe_directory})ludus\";
inline constexpr const char* INCLUDE_DIRECTORIES[] = {${includes}};
inline constexpr const char* WORKER_EXECUTABLE = R\"ludus(${worker})ludus\";
inline constexpr const char* SPIRV_VALIDATOR = R\"ludus(${validator})ludus\";
}
")
    # Public: the reload tests read the same paths and shader build interface as the objects they link.
    target_include_directories(${target} PUBLIC "${directory}")
    target_link_libraries(${target} PUBLIC Ludus::ShaderCompilerClient)
endfunction()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    list(APPEND editor_viewport_sources
        src/internal/preview_shader_build.h src/preview_shader_build.cpp
        src/internal/preview_shader_reload.h src/preview_shader_reload.cpp)
endif()

# Viewport reference capture offers native RT1 traversal only with the fixed query kernel, which
# needs the pinned SPIR-V Cross translator; without it the editor offers the CPU oracle alone.
# The GPU-compute RT1 kernels need only compute support and are always compiled.
function(ludus_editor_enable_reference_queries target)
    ludus_compile_renderer_shaders(TARGET ${target} SHADERS renderer_reference renderer_reference_reduce)
    if(LUDUS_SPIRV_CROSS)
        ludus_compile_ray_queries(TARGET ${target} NAME ray_query)
        # Public: the reference capture tests check the same capability as the objects they link.
        target_compile_definitions(${target} PUBLIC LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES=1)
    endif()
endfunction()

# RG3b: the editor host compiles the checked-in RendererStatic offscreen fixture.
# Its generated header supplies the CPU interface identity every previewed runtime
# generation must match, and the same frozen fixture (header and .lurgrt) is the
# preview/live parity input of the editor tests. Profiles follow the host device.
function(ludus_editor_compile_render_graph_host target)
    set(input "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/offscreen")
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/render-graph-host/${target}")
    set(RG2_TEST_PROFILES "[\"spirv\", \"wgsl\"]")
    if(APPLE)
        set(RG2_TEST_PROFILES "[\"metal\", \"wgsl\"]")
    endif()
    ludus_stage_render_graph_fixture("${input}" "${staged}")
    ludus_compile_render_graph(TARGET ${target} NAME editor_renderer_static
        MANIFEST "${staged}/main.lushader.json" RUNTIME_PACKAGE)
    set(editor_renderer_static_RUNTIME_PACKAGE "${editor_renderer_static_RUNTIME_PACKAGE}" PARENT_SCOPE)
    # The engine-owned default graph the Renderer tab shows when a project has none. Its
    # prebuilt generation previews with no project files or tools; its header's interface
    # identity is checked against the host fixture at compile time.
    get_property(default_graph GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_DIR)
    ludus_compile_render_graph(TARGET ${target} NAME editor_default_graph
        MANIFEST "${default_graph}/main.lushader.json" RUNTIME_PACKAGE)
    target_compile_definitions(${target} PRIVATE
        "LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME=\"${editor_default_graph_RUNTIME_PACKAGE}\"")
    # Texture viewer depth readback: packs Depth32Float bits into an RGBA8 attachment.
    ludus_compile_shader(TARGET ${target} NAME editor_texture_depth_pack RASTER
        SOURCE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders/texture_depth_pack.slang"
        VERTEX VertexMain FRAGMENT FragmentMain)
endfunction()

function(ludus_editor_add_shader_api target)
    # Display the same callback declarations the graph compiler includes.
    qt_add_resources(${target} ludus_shader_api
        PREFIX "/shader-api" BASE "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders"
        FILES "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/ray_shader.slang"
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/scene_shader.slang"
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/stochastic_reuse.slang"
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/scene_payload.slang"
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/visibility_buffer.slang")
    qt_add_resources(${target} ludus_resampling_shader_api
        PREFIX "/shader-api" BASE "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
        FILES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders/stochastic_pairwise_mis.slang")
endfunction()

# Lucide icon files (ISC, see icons/LICENSE) parsed by src/editor_icons.cpp.
function(ludus_editor_add_icons target)
    file(GLOB editor_icon_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/icons/*.svg")
    qt_add_resources(${target} ludus_editor_icons
        PREFIX "/icons" BASE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/icons" FILES ${editor_icon_files})
endfunction()

# Default World schema, mesh assets and extraction bindings.
function(ludus_editor_add_builtin_world target)
    qt_add_resources(${target} ludus_builtin_world
        PREFIX "/world" BASE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world"
        FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/world.schema.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/terrain.component.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/aperture.component.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/render.preview.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/cube.mesh.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/plane.mesh.json"
              "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/world/sphere.mesh.json")
endfunction()

# The default graph's authored sources, read-only inside the editor (also in builds
# without the native preview), so the Renderer tab can show and copy them.
function(ludus_editor_add_default_render_graph target)
    set(restir_template "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/restir_di")
    file(GLOB restir_template_shaders CONFIGURE_DEPENDS "${restir_template}/main_shaders/*.slang")
    qt_add_resources(${target} ludus_restir_configuration
        PREFIX "/render-graph/templates/restir-di" BASE "${restir_template}"
        FILES "${restir_template}/library.lurender.json" "${restir_template}/main.lushader.json.in"
              ${restir_template_shaders})
    foreach(template_directory IN ITEMS visibility_buffer visibility_secondary_rays tiled_vbuffer_nee compute_ray_tracing
            simple_compute_ray_tracing veach_guibas_mis line_sampling ris_projected_solid_angle salesin_jarosz_lines
            polygonal_light_sampling slice_sampling_luminaire rough_pair_slice steinberg_plt steinberg_physical_optics wave_tracing restir_gi rao_blackwell_mh guided_restir_fg_plus conditional_restir one_more_vertex_nee light_transformer)
        string(REPLACE "_" "-" template_identity "${template_directory}")
        set(visibility_template "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/${template_directory}")
        file(GLOB template_sources CONFIGURE_DEPENDS "${visibility_template}/main_shaders/*.slang")
        if(template_directory STREQUAL "light_transformer")
            list(APPEND template_sources "${visibility_template}/light_transformer/model.json")
        endif()
        qt_add_resources(${target} "ludus_${template_directory}_configuration"
            PREFIX "/render-graph/templates/${template_identity}" BASE "${visibility_template}"
            FILES "${visibility_template}/library.lurender.json" "${visibility_template}/main.lushader.json.in"
                  ${template_sources})
    endforeach()
    # One catalog lists the built-in graphs, snippets and node types every template picker offers.
    qt_add_resources(${target} ludus_render_graph_templates
        PREFIX "/render-graph" BASE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources"
        FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/resources/render_graph_templates.json")
    get_property(default_graph GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_DIR)
    get_property(default_graph_files GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_FILES)
    list(TRANSFORM default_graph_files PREPEND "${default_graph}/")
    qt_add_resources(${target} ludus_default_render_graph
        PREFIX "/render-graph/default" BASE "${default_graph}" FILES ${default_graph_files})
endfunction()

# Source provenance admission follows the offline compiler's existing expanded-node/pass bounds.
set_source_files_properties(tests/render_graph_execution_plan_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "LUDUS_EDITOR_TEST_PYTHON=\"${Python3_EXECUTABLE}\"")
set(graph_plan_policy "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${graph_plan_policy}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import json,sys; sys.path.insert(0,sys.argv[1]); from ludus_tools import render_graph as p; print(json.dumps({k:getattr(p,k) for k in ('PLAN_VERSION','MAX_NODES','MAX_EXPANDED_NODES','MAX_PASSES','MAX_DEPTH','MAX_CONFIGURATIONS')}))"
    "${PROJECT_SOURCE_DIR}/scripts/python"
    RESULT_VARIABLE graph_plan_status OUTPUT_VARIABLE graph_plan_limits ERROR_VARIABLE graph_plan_error)
if(NOT graph_plan_status EQUAL 0)
    message(FATAL_ERROR "Cannot read execution plan limits: ${graph_plan_error}")
endif()
foreach(limit PLAN_VERSION MAX_NODES MAX_EXPANDED_NODES MAX_PASSES MAX_DEPTH MAX_CONFIGURATIONS)
    string(JSON PLAN_${limit} GET "${graph_plan_limits}" "${limit}")
endforeach()
configure_file(src/internal/render_graph_plan_policy.h.in
    "${graph_layout_directory}/render_graph_plan_policy.h" @ONLY)

# --- Shared editor logic library ---------------------------------------------
# The transition function, project store, tool-process protocol framing and log
# buffer live in a library so offscreen tests can exercise them without creating
# widgets or launching tools. All headers are private (src/internal).
add_library(ludus_editor_core STATIC
    src/browser_player.cpp
    src/internal/tool_process.h
    src/internal/play_process.h
    src/internal/source_watch.h
    src/workspace.cpp
    src/internal/setup_guidance.h
    src/setup_guidance.cpp
    src/project_store.cpp
    src/recent_projects.cpp
    src/tool_operation.cpp
    src/internal/engine_pin.h
    src/engine_pin.cpp
    ${editor_process_sources}
    src/log_buffer.cpp
    src/internal/timing_view.h
    src/timing_view.cpp
)
set_target_properties(ludus_editor_core PROPERTIES AUTOMOC ON)
target_include_directories(ludus_editor_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ludus_editor_core PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
target_link_libraries(ludus_editor_core
    PUBLIC
        Qt6::Core
    PRIVATE
        Ludus::FoundationBase
        Ludus::FoundationLogging
        Ludus::AudioContent
)
ludus_apply_project_defaults(ludus_editor_core)


set(editor_profiling_sources
    src/internal/viewport_trace.h src/viewport_trace.cpp
    src/internal/profiling_controller.h src/profiling_controller.cpp
    src/internal/profiling_panel.h src/profiling_panel.cpp
    src/main_window_profiling.cpp)

# The tree-sitter runtime for the syntax service (ADR 0026), built natively and for the browser.
include(cmake/EditorTreeSitter.cmake)
ludus_editor_add_tree_sitter()

# Reusable private text presentation and async native assistance; no renderer link.
add_library(ludus_editor_code STATIC
    src/internal/code_navigation.h src/code_navigation.cpp src/internal/mouse_history_buttons.h
    src/internal/code_navigation_results.h src/code_navigation_results.cpp
    src/internal/code_editor.h src/code_editor.cpp
    src/internal/code_editor_panel.h src/code_editor_panel.cpp
    src/internal/code_find_overlay.h src/code_find_overlay.cpp
    src/internal/code_language_registry.h src/code_language_registry.cpp
    src/internal/code_latency_probe.h src/code_latency_probe.cpp
    src/internal/code_file_limits.h
    src/internal/code_text_profile.h
    src/internal/code_text_detect.h src/code_text_detect.cpp
    src/internal/code_editor_config.h src/code_editor_config.cpp
    src/internal/code_file_document.h src/code_file_document.cpp
    src/internal/code_edit_set.h src/code_edit_set.cpp
    src/internal/code_conflict_banner.h src/code_conflict_banner.cpp
    src/internal/code_external_change_watch.h src/code_external_change_watch.cpp
    src/internal/code_recovery_journal.h src/code_recovery_journal.cpp
    src/internal/code_recovery_writer.h src/code_recovery_writer.cpp
    src/internal/code_document_registry.h src/code_document_registry.cpp
    src/internal/code_search_service.h src/code_search_service.cpp
    src/internal/search_tool_window.h src/search_tool_window.cpp
    src/internal/code_document_tab.h src/code_document_tab.cpp
    src/internal/code_editor_area.h src/code_editor_area.cpp
    src/internal/code_status_bar.h src/code_status_bar.cpp
    src/internal/script_code_document.h src/script_code_document.cpp
    src/internal/code_syntax_highlighter.h src/code_syntax_highlighter.cpp
    src/internal/code_document.h src/internal/code_edit_scope.h
    src/internal/code_text.h src/code_text.cpp
    src/internal/code_keymap.h src/code_keymap.cpp
    src/internal/editor_commands.h src/editor_commands.cpp
    src/internal/code_multi_cursor.h src/code_multi_cursor.cpp
    src/internal/code_document_controller.h src/code_document_controller.cpp
    src/internal/code_formatter.h src/code_formatter.cpp
    src/internal/lusl_formatting.h src/lusl_formatting.cpp
    src/internal/slang_language_service.h src/slang_language_service.cpp
    src/internal/code_assistance.h src/code_assistance.cpp)
file(READ "${PROJECT_SOURCE_DIR}/config/tool_versions.json" editor_tool_versions)
string(JSON editor_formatter_version GET "${editor_tool_versions}" minimum clang-format)
get_filename_component(editor_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(editor_formatter_program "${editor_compiler_directory}/clang-format${CMAKE_EXECUTABLE_SUFFIX}")
if(EMSCRIPTEN)
    set(editor_formatter_program "")
endif()
configure_file(src/internal/formatter_config.h.in
    generated/ludus_editor_formatter_config.h @ONLY)
target_include_directories(ludus_editor_code PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_target_properties(ludus_editor_code PROPERTIES AUTOMOC ON)
target_include_directories(ludus_editor_code PRIVATE src)
target_compile_definitions(ludus_editor_code PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
target_link_libraries(ludus_editor_code PUBLIC Qt6::Widgets Ludus::FoundationContainers
    PRIVATE Ludus::FoundationBase Ludus::FoundationProfiling Ludus::FoundationStrings)
# LUSL coloring and formatting use the LUSL compiler library in process, natively and in the browser.
target_link_libraries(ludus_editor_code PRIVATE Ludus::ShaderLanguage)
# The syntax service's parsers; tree-sitter types stay inside the editor's syntax worker.
target_link_libraries(ludus_editor_code PRIVATE ludus_editor_tree_sitter)
if(NOT EMSCRIPTEN)
    target_link_libraries(ludus_editor_code PRIVATE Qt6::Concurrent)
endif()
ludus_apply_project_defaults(ludus_editor_code)

# --- Editor application ------------------------------------------------------
option(LUDUS_EDITOR_CRASH_CAPTURE "Capture macOS editor crashes using a bundled external Crashpad handler" ${APPLE})
set(LUDUS_EDITOR_SENTRY_DSN "" CACHE STRING "Default Sentry project DSN for distributed editors (public ingestion key, never an auth token)")
if(LUDUS_EDITOR_CRASH_CAPTURE)
    if(NOT APPLE OR EMSCRIPTEN)
        message(FATAL_ERROR "LUDUS_EDITOR_CRASH_CAPTURE currently supports native macOS only")
    endif()
    include(cmake/EditorCrashReporting.cmake)
    ludus_editor_add_crash_capture()
    # Retain source locations in engine frames as well as app frames in optimized
    # distributions. This build-only interface is never exported to SDK consumers.
    target_compile_options(ludus_project_options INTERFACE "$<$<CONFIG:Release,MinSizeRel>:-g>")
endif()
# --- Shared editor objects ---------------------------------------------------
# The editor and its test executables once listed the same sources, so a shared
# source compiled once per executable (render-graph sources five times). Each
# OBJECT library below compiles its sources once, and every executable that
# names it links the same objects, exactly as if it had compiled them itself.
# A source belongs here only when it preprocesses identically for all of its
# consumers. Sources whose configuration differs between executables stay in
# each of them: main_window.cpp (crash capture), renderer_workspace.cpp,
# renderer_workspace_resources.cpp and render_graph_defaults.cpp in the
# configuration tests (no GPU preview), and the browser scheduler's content jobs.
# Object libraries pass their objects only to targets that link them directly,
# so each executable names every library it uses. The build-graph check
# (tools/check_build_graph.py) rejects a source compiled by several targets.
function(ludus_editor_add_objects target)
    add_library(${target} OBJECT ${ARGN})
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    ludus_apply_project_defaults(${target})
endfunction()

# The render-graph document, templates, node models and live build.
ludus_editor_add_objects(ludus_editor_render_graph_model
    src/internal/render_graph_document.h src/internal/render_graph_source.h src/render_graph_document.cpp
    src/internal/render_graph_node_ports.h src/render_graph_node_ports.cpp
    src/internal/render_graph_configuration_template.h src/render_graph_configuration_template.cpp
    src/render_graph_visibility_template.cpp src/render_graph_compute_template.cpp
    src/render_graph_line_sampling_template.cpp src/render_graph_point_line_template.cpp
    src/render_graph_node_transfer.cpp src/render_graph_configuration_interface.cpp
    src/internal/render_graph_shader_creation.h src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp
    src/render_graph_scene_authoring.cpp src/internal/render_graph_scene_bundle.h src/render_graph_scene_bundle.cpp
    src/render_graph_techniques.cpp
    src/internal/render_graph_pipeline_node.h src/internal/render_graph_upscale_node.h
    src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
    src/internal/render_graph_comment_frame.h src/render_graph_comment_frame.cpp
    src/internal/render_graph_wire_routes.h src/render_graph_wire_routes.cpp
    src/internal/render_graph_live_build.h src/render_graph_live_build.cpp
    src/internal/render_graph_options.h src/render_graph_options.cpp
    src/internal/render_graph_shader_interface.h src/render_graph_shader_interface.cpp)
target_link_libraries(ludus_editor_render_graph_model PUBLIC Qt6::Core Qt6::Widgets
    Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationStrings)

# Editor-private curve/surface authoring adapters (curves and surfaces G5):
# a Qt-free document/undo, async prepare job, transactional preview and cursor
# reload built on the existing owners. Links the cooked-geometry codecs.
ludus_editor_add_objects(ludus_editor_geometry_model
    src/internal/geometry_document.h src/geometry_document.cpp)
target_link_libraries(ludus_editor_geometry_model PUBLIC
    Ludus::GeometryContent Ludus::GeometryFormat Ludus::FoundationGeometry Ludus::Content
    Ludus::FoundationContainers Ludus::FoundationMath)

# The render-graph canvas, its view state, diagnostics and execution plan.
ludus_editor_add_objects(ludus_editor_render_graph_canvas
    src/internal/render_graph_canvas.h
    src/render_graph_canvas.cpp src/render_graph_resources.cpp src/render_graph_canvas_feedback.cpp
    src/internal/theme_preferences.h src/theme_preferences.cpp
    src/internal/render_graph_navigation.h src/render_graph_navigation.cpp
    src/internal/render_graph_minimap.h src/render_graph_minimap.cpp
    src/internal/render_graph_diagnostics.h src/render_graph_diagnostics.cpp
    src/internal/render_graph_execution_plan.h src/render_graph_execution_plan.cpp
    src/internal/render_graph_layout.h src/render_graph_layout.cpp
    src/internal/render_graph_comments.h src/render_graph_comments.cpp
    src/internal/render_graph_reroutes.h src/render_graph_reroutes.cpp
    src/internal/render_graph_resource_inspection.h src/render_graph_resource_inspection.cpp
    src/render_graph_resource_resolution.cpp
    src/internal/render_graph_view_state.h src/internal/render_graph_history.h src/render_graph_history.cpp
    src/internal/render_graph_configuration_editor.h src/render_graph_configuration_editor.cpp
    src/internal/render_graph_pipeline.h src/render_graph_pipeline.cpp
    src/internal/render_graph_job.h src/render_graph_job.cpp)
target_link_libraries(ludus_editor_render_graph_canvas PUBLIC ludus_editor_render_graph_model ludus_editor_presentation
    Ludus::RenderGraphFormat)

# File names and icons shared by the explorers, content browser and Renderer tab.
ludus_editor_add_objects(ludus_editor_presentation
    src/internal/path_text.h src/path_text.cpp
    src/internal/editor_icons.h src/editor_icons.cpp
    src/internal/theme_fonts.h src/theme_fonts.cpp)
target_link_libraries(ludus_editor_presentation PUBLIC Qt6::Widgets Ludus::FoundationBase Ludus::FoundationContainers)

# The Renderer tab's authoring panels that need no viewport device. The
# workspace class itself (renderer_workspace.cpp) compiles with each preview
# configuration and is not part of this library.
ludus_editor_add_objects(ludus_editor_render_graph_authoring
    src/renderer_workspace_selection.cpp src/renderer_workspace_node_details.cpp
    src/internal/render_graph_luminaire_coverage.h src/render_graph_luminaire_coverage.cpp
    src/internal/world_lighting_coverage.h src/world_lighting_coverage.cpp
    src/renderer_workspace_resolution.cpp
    src/internal/render_graph_creation.h src/render_graph_creation.cpp
    src/internal/render_graph_template_registry.h src/render_graph_template_registry.cpp
    src/internal/render_graph_quick_add.h src/render_graph_quick_add.cpp
    src/internal/render_graph_quick_add_graphs.h src/render_graph_quick_add_graphs.cpp
    src/internal/render_graph_settings_editor.h src/render_graph_settings_editor.cpp
    src/internal/render_graph_shader_assistance.h src/render_graph_shader_assistance.cpp
    src/internal/render_graph_source_watch.h src/render_graph_source_watch.cpp
    src/internal/render_graph_code_document.h src/render_graph_code_document.cpp
    src/internal/render_graph_resolution_settings.h src/render_graph_resolution_settings.cpp
    src/internal/project_rendering_settings.h src/project_rendering_settings.cpp
    src/internal/render_graph_node_details.h src/render_graph_node_details.cpp
    src/internal/render_graph_recovery_store.h src/render_graph_recovery_store.cpp
    src/internal/render_graph_recovery.h src/render_graph_recovery.cpp
    src/internal/external_code_editor.h src/external_code_editor.cpp
    src/internal/error_list.h src/error_list.cpp
    src/internal/viewport_frame_pump.h src/viewport_frame_pump.cpp)
# The workspace borrows viewport types, but these sources never call their compiled
# implementations; the configuration tests link them without the renderer.
target_link_libraries(ludus_editor_render_graph_authoring
    PUBLIC
        ludus_editor_render_graph_canvas ludus_editor_presentation ludus_editor_core ludus_editor_code
        Ludus::FoundationProfiling
    PRIVATE
        "$<COMPILE_ONLY:Ludus::GraphicsRenderer>" "$<COMPILE_ONLY:Ludus::GameplayCamera>"
        "$<COMPILE_ONLY:Ludus::Resources>")

# The content browser, its catalog and imports. The job scheduler (content_jobs.cpp)
# differs between the native editor and the browser scheduler test.
ludus_editor_add_objects(ludus_editor_content
    src/internal/content_browser.h src/content_catalog_model.cpp src/content_import.cpp src/content_workspace.cpp
    src/internal/state_view.h src/state_view.cpp
    src/editor_files.cpp src/internal/scene_archive.h src/scene_archive.cpp)
target_link_libraries(ludus_editor_content PUBLIC ludus_editor_presentation Qt6::Core Qt6::Widgets
    Ludus::AudioContent Ludus::FoundationMath ludus_stb_image)

ludus_editor_add_objects(ludus_editor_project_explorer
    src/internal/project_explorer.h src/project_explorer.cpp
    src/internal/project_files.h src/project_files.cpp)
target_link_libraries(ludus_editor_project_explorer PUBLIC ludus_editor_presentation)

ludus_editor_add_objects(ludus_editor_document_well src/internal/document_well.h src/document_well.cpp)
target_link_libraries(ludus_editor_document_well PUBLIC Qt6::Widgets Ludus::FoundationBase Ludus::FoundationContainers)

if(NOT EMSCRIPTEN AND (LUDUS_EDITOR_RENDER_PREVIEW OR LUDUS_BUILD_TESTS))
    ludus_editor_add_objects(ludus_editor_texture_display
        src/internal/texture_display_conversion.h src/texture_display_conversion.cpp)
    target_link_libraries(ludus_editor_texture_display PUBLIC Qt6::Widgets
        Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationProfiling)
endif()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    # Light Transformer encoder asset checks for the preview and their unit test.
    ludus_editor_add_objects(ludus_editor_light_transformer_setup
        src/internal/light_transformer_preview_setup.h src/light_transformer_preview_setup.cpp)
    target_link_libraries(ludus_editor_light_transformer_setup PUBLIC Qt6::Core Ludus::GraphicsRenderer
        Ludus::FoundationMath)
endif()
if(APPLE)
    ludus_editor_add_objects(ludus_editor_accessibility src/qt_accessibility_macos.mm)
    target_link_libraries(ludus_editor_accessibility PUBLIC Qt6::Widgets Ludus::FoundationBase "-framework AppKit")
endif()

# Everything else the editor application and its main test executable share:
# workspaces, viewport, previews and their compiled shaders. The executables
# add only main.cpp or their tests, and main_window.cpp.
ludus_editor_add_objects(ludus_editor_application
    ${editor_profiling_sources}
    src/internal/render_load_job.h
    src/render_load_job.cpp
    src/internal/render_preview.h
    src/internal/render_panel.h
    src/internal/preview_viewport.h
    ${editor_viewport_sources}
    src/render_scene.cpp
    src/render_graph_luminaire_world.cpp
    src/world_lighting_scene.cpp
    src/render_preview.cpp
    src/preview_gpu_generation.cpp
    src/internal/preview_gpu_generation.h
    src/viewport_reference_capture.cpp
    src/internal/viewport_reference_capture.h
    src/reference_comparison.cpp
    src/internal/reference_comparison.h
    src/reference_capture_window.cpp
    src/internal/reference_capture_window.h
    src/captured_image_view.cpp
    src/internal/captured_image_view.h
    src/internal/preview_shader_set.h
    src/preview_shader_set.cpp
    src/internal/preview_shading.h
    src/preview_shading.cpp
    src/internal/viewport_statistics.h
    src/viewport_statistics.cpp
    src/editor_camera_controller.cpp
    src/static_lighting_job.cpp
    src/internal/static_lighting_job.h
    src/preview_area_lights.cpp
    src/internal/preview_area_lights.h
    src/render_panel.cpp
    src/internal/level_workspace.h
    src/level_workspace.cpp
    src/internal/scene_placement.h
    src/scene_placement.cpp
    src/internal/light_switches.h
    src/light_switches.cpp
    src/internal/import_report.h
    src/import_report.cpp
    src/internal/world_workspace.h
    src/internal/world_lights_panel.h
    src/world_lights_panel.cpp
    src/builtin_world.cpp
    src/internal/world_celestial_light.h src/world_celestial_light.cpp
    src/world_document.cpp
    src/world_terrain.cpp
    src/world_terrain_editing.cpp
    src/world_aperture.cpp
    src/world_demo_adapter.cpp
    src/internal/material_inspection.h
    src/material_inspection.cpp
    src/internal/inspector_fields.h
    src/inspector_fields.cpp
    src/internal/world_entity_search.h
    src/world_entity_search.cpp
    src/world_workspace.cpp
    src/world_viewport_editing.cpp
    src/internal/controller.h
    src/internal/configuration_workspace.h
    src/internal/main_window.h
    src/internal/audio_workspace.h
    src/internal/audio_preview.h
    src/internal/startup_sound.h
    src/startup_sound.cpp
    src/content_jobs.cpp
    src/internal/script_workspace.h
    src/script_workspace.cpp
    src/internal/renderer_workspace.h
    src/renderer_workspace.cpp
    src/renderer_workspace_resources.cpp
    src/internal/render_graph_defaults.h
    src/render_graph_defaults.cpp
    src/configuration_workspace.cpp
    src/audio_workspace.cpp
    ${editor_audio_source}
    src/document_commands.cpp
    src/main_window_layout.cpp
    src/main_window_tools.cpp
    src/main_window_project_files.cpp
    src/internal/command_palette.h
    src/command_palette.cpp
    src/internal/options_dialog.h
    src/options_dialog.cpp
    src/internal/render_graph_build_settings.h
    src/internal/timing_panel.h
    src/timing_panel.cpp
    src/project_setup_dialog.cpp
    src/project_creation_dialog.cpp
    src/editor_style.cpp
    src/internal/editor_style.h
    src/workspace_style.cpp
    src/internal/activity_chip.h
    src/activity_chip.cpp
    src/internal/window_size.h
    src/window_size.cpp
    src/internal/application_icon.h
    src/application_icon.cpp
    src/controller.cpp
    src/controller_play.cpp
)
target_link_libraries(ludus_editor_application
    PUBLIC
        ludus_editor_render_graph_authoring
        ludus_editor_content
        ludus_editor_project_explorer
        ludus_editor_document_well
        Ludus::RenderGraphFormat
        ludus_editor_core
        ludus_editor_code
        ludus_world_demo_model
        ludus_world_render_items
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Ludus::FoundationBase
        Ludus::FoundationMemory
        Ludus::FoundationLogging
        Ludus::FoundationProfiling
        Ludus::FoundationTime
        Ludus::AudioContent
        Ludus::FoundationConfigJson
        Ludus::RuntimeConfiguration
        Ludus::FoundationMath
        Ludus::GameplayCamera
        Ludus::GameplayWorld
        Ludus::GraphicsRenderer
        Ludus::Resources
        Ludus::FoundationParsingJson
        Ludus::Platform
        # Scene import, save and assembly run in process (no Python scene backend),
        # including the browser Editor's MEMFS copies.
        Ludus::SceneCook
        # Scene source apertures import as editable World aperture entities.
        Ludus::SceneSource
        Ludus::FoundationThreading
        ludus_stb_image
        # World documents share the version-2 record constants with their owner (object handles stage 3).
        Ludus::ContentIdentity)
if(EMSCRIPTEN)
    target_compile_definitions(ludus_editor_application PUBLIC LUDUS_EDITOR_BROWSER_PREVIEW=1 LUDUS_EDITOR_GPU_PREVIEW=1)
    target_sources(ludus_editor_application PRIVATE src/browser_viewport.cpp src/internal/browser_viewport.h)
    target_link_libraries(ludus_editor_application PUBLIC Ludus::GraphicsRhi)
    ludus_compile_renderer_shaders(TARGET ludus_editor_application SHADERS
        renderer_flat renderer_composite renderer_material renderer_lit renderer_output renderer_shadow
        renderer_grid renderer_lit_shadow renderer_clustered_authored renderer_clustered_authored_shadow
        renderer_lit_debug renderer_lit_debug_shadow
        renderer_clustered_authored_debug renderer_clustered_authored_debug_shadow)
endif()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    target_compile_definitions(ludus_editor_application PUBLIC LUDUS_EDITOR_NATIVE_PREVIEW=1 LUDUS_EDITOR_GPU_PREVIEW=1)
    target_link_libraries(ludus_editor_application PUBLIC ludus_editor_texture_display Ludus::GraphicsRhi Qt6::Quick)
    ludus_compile_renderer_shaders(TARGET ludus_editor_application SHADERS
        renderer_flat renderer_composite renderer_material renderer_lit renderer_output renderer_shadow
        renderer_overlay renderer_grid renderer_selection_mask renderer_selection_outline
        renderer_light_transformer_encoder)
    ludus_compile_renderer_shaders(TARGET ludus_editor_application SHADERS
        renderer_lit_shadow renderer_clustered renderer_clustered_shadow
        renderer_clustered_authored renderer_clustered_authored_shadow
        renderer_lit_debug renderer_lit_debug_shadow
        renderer_clustered_authored_debug renderer_clustered_authored_debug_shadow)
    ludus_editor_compile_render_graph_host(ludus_editor_application)
    ludus_editor_enable_shader_reload(ludus_editor_application)
    ludus_editor_enable_reference_queries(ludus_editor_application)
    # The tests read the same generated shader and graph headers as the objects they link.
    get_target_property(editor_include_directories ludus_editor_application INCLUDE_DIRECTORIES)
    foreach(directory IN LISTS editor_include_directories)
        if(directory MATCHES "/(ludus-shaders|ludus-graphs)/ludus_editor_application/")
            target_include_directories(ludus_editor_application INTERFACE "${directory}")
        endif()
    endforeach()
endif()
# Qt resources compiled once, like the sources above. Linking them as objects keeps
# each resource's registration (a static initializer) in every executable that names it.
function(ludus_editor_add_resource_objects target)
    add_library(${target} OBJECT)
    target_link_libraries(${target} PUBLIC Qt6::Core)
    ludus_apply_project_defaults(${target})
endfunction()
ludus_editor_add_resource_objects(ludus_editor_shader_api_resources)
ludus_editor_add_shader_api(ludus_editor_shader_api_resources)
ludus_editor_add_resource_objects(ludus_editor_render_graph_resources)
ludus_editor_add_default_render_graph(ludus_editor_render_graph_resources)
ludus_editor_add_resource_objects(ludus_editor_icon_resources)
ludus_editor_add_icons(ludus_editor_icon_resources)
ludus_editor_add_resource_objects(ludus_editor_application_resources)
set(editor_brand_assets
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/ludus-icon.png"
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/ludus-mark.png"
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/small/ludus-icon-16.png"
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/small/ludus-icon-24.png"
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/small/ludus-icon-32.png")
qt_add_resources(ludus_editor_application_resources ludus_brand
    PREFIX "/ludus"
    BASE "${PROJECT_SOURCE_DIR}/docs/wiki/assets"
    FILES ${editor_brand_assets})
# The startup theme the native editor plays on launch (docs/development/editor-workspace.md#startup-sound).
qt_add_resources(ludus_editor_application_resources ludus_startup_sound
    PREFIX "/ludus"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/startup-theme.flac")
# Ship editable, self-contained examples with both presentations. Generated
# build outputs and local machine settings are never part of the sample library;
# they are excluded relative to each sample root, not the checkout path.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EditorSampleResources.cmake")
ludus_collect_editor_sample_files(editor_sample_files "${PROJECT_SOURCE_DIR}/examples"
    cornell-box live-edit-game scripted-game editor-sdk-project)
set(world_demo_schema "${PROJECT_SOURCE_DIR}/apps/world_demo/authoring/world.schema.json")
ludus_editor_add_builtin_world(ludus_editor_application_resources)
qt_add_resources(ludus_editor_application_resources world_demo_schema
    PREFIX "/world-demo" BASE "${PROJECT_SOURCE_DIR}/apps/world_demo/authoring" FILES ${world_demo_schema})
qt_add_resources(ludus_editor_application_resources ludus_samples
    PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/examples" FILES ${editor_sample_files})
# The browser editor creates projects without the Python tooling. Render the CLI's
# minimal template once, leaving only the project name and target to substitute
# (docs/development/browser-editor.md#new-projects).
set(session_template_policies)
foreach(policy create templates descriptor lockfile native toolchain)
    list(APPEND session_template_policies "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/${policy}.py")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${session_template_policies})
set(session_template_directory "${CMAKE_CURRENT_BINARY_DIR}/project-templates/minimal")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import sys; sys.path.insert(0, sys.argv[1]); from pathlib import Path; from ludus_tools.create import write_session_template; print(';'.join(write_session_template(Path(sys.argv[2]), sys.argv[3])))"
    "${PROJECT_SOURCE_DIR}/scripts/python" "${session_template_directory}" "${PROJECT_VERSION}"
    RESULT_VARIABLE session_template_status OUTPUT_VARIABLE session_template_files
    ERROR_VARIABLE session_template_error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT session_template_status EQUAL 0)
    message(FATAL_ERROR "Cannot render the editor's new-project template: ${session_template_error}")
endif()
list(TRANSFORM session_template_files PREPEND "${session_template_directory}/")
qt_add_resources(ludus_editor_application_resources ludus_project_templates
    PREFIX "/project-templates/minimal" BASE "${session_template_directory}" FILES ${session_template_files})

# Every object library the editor application and its main test executable link.
set(editor_application_objects
    ludus_editor_application ludus_editor_render_graph_model ludus_editor_render_graph_canvas
    ludus_editor_render_graph_authoring ludus_editor_presentation ludus_editor_content
    ludus_editor_project_explorer ludus_editor_document_well ludus_editor_geometry_model)
if(TARGET ludus_editor_texture_display AND LUDUS_EDITOR_RENDER_PREVIEW)
    list(APPEND editor_application_objects ludus_editor_texture_display)
endif()
if(TARGET ludus_editor_light_transformer_setup)
    list(APPEND editor_application_objects ludus_editor_light_transformer_setup)
endif()
set(editor_application_resources
    ludus_editor_application_resources ludus_editor_render_graph_resources ludus_editor_shader_api_resources
    ludus_editor_icon_resources)

qt_add_executable(ludus_editor main.cpp src/main_window.cpp)
set_target_properties(ludus_editor PROPERTIES AUTOMOC ON)
if(APPLE)
    target_link_libraries(ludus_editor PRIVATE "-framework AppKit" "-framework CoreGraphics")
    target_sources(ludus_editor PRIVATE resources/ludus.icns)
    set_source_files_properties(resources/ludus.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(ludus_editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.ludus.editor"
        MACOSX_BUNDLE_BUNDLE_NAME "Ludus Editor"
        MACOSX_BUNDLE_ICON_FILE "ludus.icns")
endif()
target_include_directories(ludus_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ludus_editor PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
target_link_libraries(ludus_editor PRIVATE ${editor_application_objects} ${editor_application_resources})
ludus_apply_project_defaults(ludus_editor)
if(LUDUS_EDITOR_CRASH_CAPTURE)
    # dSYM is produced from this exact linked executable, before any later stripping/signing.
    find_program(editor_dsymutil NAMES llvm-dsymutil dsymutil REQUIRED)
    foreach(target IN ITEMS ludus_editor ${editor_application_objects})
        target_compile_options(${target} PRIVATE -g)
    endforeach()
    add_custom_command(TARGET ludus_editor POST_BUILD
        COMMAND "${editor_dsymutil}" "$<TARGET_FILE:ludus_editor>" -o "$<TARGET_BUNDLE_DIR:ludus_editor>.dSYM"
        VERBATIM)
    ludus_editor_bundle_crash_capture(ludus_editor)
    target_compile_definitions(ludus_editor PRIVATE LUDUS_EDITOR_CRASH_CAPTURE=1)
    target_link_libraries(ludus_editor PRIVATE Ludus::FoundationContainers)
    set(editor_source_modified false)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" status --porcelain --untracked-files=normal
        OUTPUT_VARIABLE editor_source_status ERROR_QUIET)
    if(editor_source_status)
        set(editor_source_modified true)
    endif()
    if(LUDUS_EDITOR_SENTRY_DSN MATCHES "[\"\n\r]" OR LUDUS_EDITOR_SENTRY_DSN MATCHES "\\)ludus")
        message(FATAL_ERROR "LUDUS_EDITOR_SENTRY_DSN must be a single-line project DSN")
    endif()
    set(editor_crash_config_directory "${CMAKE_CURRENT_BINARY_DIR}/crash-config")
    file(MAKE_DIRECTORY "${editor_crash_config_directory}")
    configure_file(src/internal/crash_config.h.in
        "${editor_crash_config_directory}/ludus_editor_crash_config.h" @ONLY)
    target_include_directories(ludus_editor PRIVATE "${editor_crash_config_directory}")
    # The application and the crash probe link one compiled crash-reporting front end.
    ludus_editor_add_objects(ludus_editor_crash_reporting src/internal/crash_reporting.h src/crash_reporting.cpp)
    target_include_directories(ludus_editor_crash_reporting PRIVATE "${editor_crash_config_directory}")
    target_link_libraries(ludus_editor_crash_reporting PUBLIC Qt6::Widgets Ludus::FoundationContainers
        ludus_editor_crash_capture)
    target_compile_options(ludus_editor_crash_reporting PRIVATE -g)
    target_link_libraries(ludus_editor PRIVATE ludus_editor_crash_reporting)
    # Small production-path crash probe; enabled independently of the full Catch2 test graph.
    option(LUDUS_EDITOR_CRASH_CAPTURE_TESTS "Build the native crash-capture subprocess probe" OFF)
    if(LUDUS_EDITOR_CRASH_CAPTURE_TESTS)
        add_executable(ludus_editor_crash_probe tests/crash_capture_probe.cpp)
        target_include_directories(ludus_editor_crash_probe PRIVATE src "${editor_crash_config_directory}")
        target_compile_definitions(ludus_editor_crash_probe PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_editor_crash_probe PRIVATE ludus_editor_crash_reporting Qt6::Widgets
            Ludus::FoundationContainers)
        ludus_apply_project_defaults(ludus_editor_crash_probe)
        ludus_editor_bundle_crash_capture(ludus_editor_crash_probe)
        add_test(NAME ludus_editor_crash_capture
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_crash_capture.py"
                "$<TARGET_FILE:ludus_editor_crash_probe>" "$<TARGET_FILE:crashpad_handler>")
        set_tests_properties(ludus_editor_crash_capture PROPERTIES LABELS "editor;crash;native" TIMEOUT 180)
    endif()
endif()

# The editor is a developer tool, never part of the installed SDK: no install()
# rule and no LudusTargets export entry here, by design (E01).
if(APPLE)
    target_link_libraries(ludus_editor PRIVATE ludus_editor_accessibility)
endif()

# --- Offscreen logic/widget tests --------------------------------------------
# Offscreen Qt tests verify logic and widget wiring without a GPU/compositor.
# They do NOT prove native Wayland interaction or that a game rendered a frame.
if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_executable(ludus_editor_texture_benchmark EXCLUDE_FROM_ALL
        tests/texture_display_benchmark.cpp src/texture_inspection.cpp)
    target_include_directories(ludus_editor_texture_benchmark PRIVATE src)
    target_compile_definitions(ludus_editor_texture_benchmark PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_editor_texture_benchmark PRIVATE Qt6::Gui
        Ludus::FoundationMath Ludus::FoundationProfiling)
    ludus_apply_project_defaults(ludus_editor_texture_benchmark)
    if(TARGET Qt6::Quick)
        add_executable(ludus_viewport_overlay_benchmark EXCLUDE_FROM_ALL
            tests/viewport_object_overlay_benchmark.cpp src/viewport_object_overlay_geometry.cpp)
        target_include_directories(ludus_viewport_overlay_benchmark PRIVATE src)
        target_compile_definitions(ludus_viewport_overlay_benchmark PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_viewport_overlay_benchmark PRIVATE Qt6::Widgets Qt6::Quick Qt6::Test
            Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationProfiling)
        ludus_apply_project_defaults(ludus_viewport_overlay_benchmark)
    endif()
    find_package(Catch2 3 REQUIRED)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    # One Catch2 session entry point for every editor test executable.
    ludus_editor_add_objects(ludus_editor_test_main tests/main.cpp)
    target_link_libraries(ludus_editor_test_main PUBLIC Qt6::Widgets Catch2::Catch2 Ludus::FoundationBase)
    ludus_enable_test_exceptions(ludus_editor_test_main)
    add_executable(ludus_document_well_tests tests/document_pair_tests.cpp)
    set_target_properties(ludus_document_well_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_document_well_tests PRIVATE src)
    target_compile_definitions(ludus_document_well_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_document_well_tests PRIVATE ludus_editor_document_well Qt6::Widgets Qt6::Test
        Ludus::FoundationBase Catch2::Catch2)
    ludus_apply_project_defaults(ludus_document_well_tests)
    ludus_enable_test_exceptions(ludus_document_well_tests)
    add_test(NAME ludus_document_well_tests COMMAND ludus_document_well_tests)
    set_tests_properties(ludus_document_well_tests PROPERTIES LABELS "unit;editor;layout" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(APPLE)
        add_executable(ludus_editor_accessibility_tests tests/accessibility_tests.mm)
        target_include_directories(ludus_editor_accessibility_tests PRIVATE src)
        target_compile_definitions(ludus_editor_accessibility_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_editor_accessibility_tests PRIVATE ludus_editor_accessibility Qt6::Widgets Catch2::Catch2
            Ludus::FoundationBase "-framework AppKit")
        ludus_apply_project_defaults(ludus_editor_accessibility_tests)
        ludus_enable_test_exceptions(ludus_editor_accessibility_tests)
        add_test(NAME ludus_editor_accessibility_tests COMMAND ludus_editor_accessibility_tests)
        set_tests_properties(ludus_editor_accessibility_tests PROPERTIES LABELS "unit;editor;native;accessibility"
            TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    endif()
    add_executable(ludus_code_editor_tests tests/code_editor_tests.cpp tests/code_document_tests.cpp tests/code_navigation_tests.cpp tests/code_language_registry_tests.cpp tests/code_latency_probe_tests.cpp tests/code_file_document_tests.cpp tests/code_edit_set_tests.cpp tests/code_document_registry_tests.cpp tests/code_recovery_journal_tests.cpp tests/code_editor_area_tests.cpp tests/code_status_bar_tests.cpp tests/script_code_document_tests.cpp tests/code_multi_cursor_tests.cpp tests/code_find_overlay_tests.cpp tests/code_search_tests.cpp tests/code_pairs_tests.cpp tests/code_keymap_tests.cpp tests/code_syntax_runtime_tests.cpp tests/code_editor_benchmark.cpp)
    target_include_directories(ludus_code_editor_tests PRIVATE src)
    target_compile_definitions(ludus_code_editor_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    configure_file(tests/formatter_fixture.py generated/formatter_fixture.py COPYONLY)
    configure_file(tests/slang_server_fixture.py generated/slang_server_fixture.py COPYONLY)
    target_compile_definitions(ludus_code_editor_tests PRIVATE
        "LUDUS_SLANG_FIXTURE=\"${CMAKE_CURRENT_BINARY_DIR}/generated/slang_server_fixture.py\""
        "LUDUS_TEST_PYTHON=\"${Python3_EXECUTABLE}\"")
    target_compile_definitions(ludus_code_editor_tests PRIVATE
        "LUDUS_FORMATTER_FIXTURE=\"${CMAKE_CURRENT_BINARY_DIR}/generated/formatter_fixture.py\"")
    target_link_libraries(ludus_code_editor_tests PRIVATE ludus_editor_code Qt6::Test
        Ludus::FoundationBase Catch2::Catch2 ludus_editor_tree_sitter)
    ludus_apply_project_defaults(ludus_code_editor_tests)
    ludus_enable_test_exceptions(ludus_code_editor_tests)
    add_test(NAME ludus_code_editor_tests COMMAND ludus_code_editor_tests)
    # Linear-work and search cases assert wall-clock ceilings; keep them off a
    # runner shared with parallel CTest GPU cases.
    set_tests_properties(ludus_code_editor_tests PROPERTIES LABELS "unit;editor;text" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RUN_SERIAL TRUE)
    # Replay the content suite through the browser's single-owner scheduler as
    # well as the normal native worker. No Qt/Wasm-private APIs enter this target.
    add_executable(ludus_editor_browser_content_tests tests/content_browser_tests.cpp src/content_jobs.cpp)
    set_target_properties(ludus_editor_browser_content_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_editor_browser_content_tests PRIVATE src
        "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_compile_definitions(ludus_editor_browser_content_tests PRIVATE
        QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS=1)
    target_link_libraries(ludus_editor_browser_content_tests PRIVATE ludus_editor_content ludus_editor_presentation
        ludus_editor_icon_resources Qt6::Core Qt6::Widgets Qt6::Test Ludus::AudioContent Ludus::FoundationMath ludus_stb_image Catch2::Catch2)
    ludus_apply_project_defaults(ludus_editor_browser_content_tests)
    ludus_enable_test_exceptions(ludus_editor_browser_content_tests)
    add_test(NAME ludus_editor_browser_content_tests COMMAND ludus_editor_browser_content_tests)
    set_tests_properties(ludus_editor_browser_content_tests PROPERTIES
        LABELS "unit;editor;browser" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(ludus_project_explorer_tests tests/project_explorer_tests.cpp)
    set_target_properties(ludus_project_explorer_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_project_explorer_tests PRIVATE src)
    target_compile_definitions(ludus_project_explorer_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_project_explorer_tests PRIVATE ludus_editor_project_explorer ludus_editor_presentation
        ludus_editor_icon_resources Qt6::Widgets Qt6::Test Ludus::FoundationBase Catch2::Catch2)
    ludus_apply_project_defaults(ludus_project_explorer_tests)
    ludus_enable_test_exceptions(ludus_project_explorer_tests)
    add_test(NAME ludus_project_explorer_tests COMMAND ludus_project_explorer_tests)
    set_tests_properties(ludus_project_explorer_tests PROPERTIES LABELS "unit;editor" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
        add_executable(ludus_light_transformer_preview_setup_tests tests/light_transformer_preview_setup_tests.cpp)
        target_include_directories(ludus_light_transformer_preview_setup_tests PRIVATE src)
        target_compile_definitions(ludus_light_transformer_preview_setup_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_light_transformer_preview_setup_tests PRIVATE ludus_editor_light_transformer_setup
            Qt6::Core Ludus::GraphicsRenderer Catch2::Catch2WithMain)
        ludus_apply_project_defaults(ludus_light_transformer_preview_setup_tests)
        ludus_enable_test_exceptions(ludus_light_transformer_preview_setup_tests)
        add_test(NAME ludus_light_transformer_preview_setup_tests COMMAND ludus_light_transformer_preview_setup_tests)
        set_tests_properties(ludus_light_transformer_preview_setup_tests PROPERTIES LABELS "unit;editor;light-transformer" TIMEOUT 60)
    endif()
    add_executable(ludus_editor_tests
        tests/profiling_tests.cpp
        tests/viewport_trace_tests.cpp
        tests/import_report_tests.cpp
        tests/world_workspace_tests.cpp
        tests/world_terrain_tests.cpp
        tests/world_aperture_tests.cpp
        tests/material_inspection_tests.cpp
        tests/inspector_fields_tests.cpp
        tests/level_workspace_tests.cpp
        tests/render_authoring_tests.cpp
        tests/world_lighting_switch_tests.cpp
        tests/world_light_switch_tests.cpp
        tests/world_start_view_tests.cpp
        tests/viewport_debug_tests.cpp
        tests/preview_generation_tests.cpp
        tests/viewport_navigation_tests.cpp
        tests/viewport_object_overlay_tests.cpp
        tests/mesh_silhouette_tests.cpp
        tests/orientation_widget_tests.cpp
        tests/design_token_tests.cpp
        tests/editor_style_tests.cpp
        tests/theme_preference_tests.cpp
        tests/viewport_legibility_tests.cpp
        tests/editor_icons_tests.cpp
        tests/editor_visual_consistency_tests.cpp
        tests/path_text_tests.cpp
        tests/state_view_tests.cpp
        tests/setup_guidance_tests.cpp
        tests/activity_chip_tests.cpp
        tests/captured_image_view_tests.cpp
        tests/viewport_frame_pump_tests.cpp
        tests/viewport_frame_source_tests.cpp
        tests/viewport_screenshot_tests.cpp
        tests/world_demo_workspace_tests.cpp
        tests/world_identity_tests.cpp
        tests/browser_import_tests.cpp
        tests/configuration_workspace_tests.cpp
        tests/audio_content_tests.cpp
        tests/startup_sound_tests.cpp
        tests/workspace_tests.cpp
        tests/project_store_tests.cpp
        tests/project_store_v2_tests.cpp
        tests/tool_process_tests.cpp
        tests/log_buffer_tests.cpp
        tests/timing_view_tests.cpp
        tests/controller_tests.cpp
        tests/document_history_tests.cpp
        tests/document_interaction_tests.cpp
        tests/content_import_gate_tests.cpp
        tests/content_browser_tests.cpp
        tests/project_setup_dialog_tests.cpp
        tests/engine_pin_tests.cpp
        tests/project_lifecycle_tests.cpp
        tests/workspace_layout_tests.cpp
        tests/editor_shell_tests.cpp
        tests/command_palette_tests.cpp
        tests/recent_projects_tests.cpp
        tests/source_watch_tests.cpp
        tests/script_workspace_tests.cpp
        tests/renderer_workspace_tests.cpp tests/render_graph_template_tests.cpp
        tests/render_graph_node_menu_tests.cpp
        tests/render_graph_rename_tests.cpp
        tests/render_graph_default_tests.cpp
        tests/render_graph_canvas_tests.cpp tests/render_graph_navigation_tests.cpp tests/render_graph_history_tests.cpp tests/render_graph_layout_tests.cpp tests/render_graph_execution_plan_tests.cpp tests/render_graph_comment_tests.cpp tests/render_graph_reroute_tests.cpp tests/render_graph_resource_inspection_tests.cpp
        tests/render_graph_canvas_feedback_tests.cpp
        tests/render_graph_diagnostic_tests.cpp tests/render_graph_diagnostic_navigation_tests.cpp
        tests/texture_inspection_tests.cpp
        tests/external_code_editor_tests.cpp
        tests/live_journey_tests.cpp
        tests/macos_journey_tests.cpp
        tests/native_session_listener.cpp
        # MainWindow compiles here without the application's crash capture; see ludus_editor_application.
        src/main_window.cpp)
    # Keep the small software rasterizer only as an independent test oracle.
    target_sources(ludus_editor_tests PRIVATE src/software_viewport.cpp)
    set_target_properties(ludus_editor_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_editor_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_compile_definitions(ludus_editor_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        "LUDUS_EDITOR_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "LUDUS_EDITOR_PROJECT_ROOT=\"${PROJECT_SOURCE_DIR}\""
        # The configured spirv-val (empty without one), for tests that build SPIR-V profiles.
        "LUDUS_EDITOR_TEST_SPIRV_VALIDATOR=\"${LUDUS_SPIRV_VALIDATOR}\"")
    target_compile_definitions(ludus_editor_tests PRIVATE
        "LUDUS_EDITOR_SDK_MANIFEST=\"${PROJECT_BINARY_DIR}/cmake/LudusSdkManifest.json\"")
    # Substitute SDKs in the render-graph build tests copy the installed shader-helper layout from here.
    set(editor_test_render_graph_helper "${CMAKE_CURRENT_BINARY_DIR}/render-graph-helper/ludus_editor_tests")
    _ludus_stage_render_graph_helpers("${editor_test_render_graph_helper}")
    target_compile_definitions(ludus_editor_tests PRIVATE
        "LUDUS_EDITOR_TEST_RENDER_GRAPH_HELPER=\"${editor_test_render_graph_helper}\"")
    target_link_libraries(ludus_editor_tests
        PRIVATE
            ${editor_application_objects}
            ${editor_application_resources}
            Ludus::RenderGraphFormat
            ludus_editor_core
            ludus_editor_code
            ludus_world_demo_model
            ludus_world_render_items
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Test
            Ludus::FoundationBase
            Ludus::FoundationMemory
            Ludus::FoundationLogging
            Ludus::FoundationProfiling
            Ludus::FoundationTime
            Ludus::AudioContent
            Ludus::FoundationConfigJson
            Ludus::RuntimeConfiguration
            Ludus::FoundationMath
            Ludus::GameplayCamera
            Ludus::GameplayWorld
            Ludus::GraphicsRenderer
            Ludus::Resources
            Ludus::FoundationParsingJson
            Catch2::Catch2)
    ludus_apply_project_defaults(ludus_editor_tests)
    target_link_libraries(ludus_editor_tests PRIVATE Ludus::Platform)
    target_link_libraries(ludus_editor_tests PRIVATE Ludus::SceneCook Ludus::FoundationThreading ludus_stb_image)
    # The tests also replay committed migration manifests to produce version-2 World fixtures.
    target_link_libraries(ludus_editor_tests PRIVATE Ludus::ContentIdentity)
    if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT APPLE)
        target_link_libraries(ludus_editor_tests PRIVATE Ludus::FoundationTime)
    endif()
    if(LUDUS_EDITOR_RENDER_PREVIEW)
        # The preview definitions, compiled shaders, render-graph host fixture (whose package
        # path editor_renderer_static_RUNTIME_PACKAGE names) and reference queries come with
        # ludus_editor_application.
        target_link_libraries(ludus_editor_tests PRIVATE Ludus::GraphicsRhi Qt6::Quick)
        target_sources(ludus_editor_tests PRIVATE tests/preview_shader_reload_tests.cpp)
        target_sources(ludus_editor_tests PRIVATE tests/viewport_reference_capture_tests.cpp
            tests/reference_comparison_tests.cpp)
        target_sources(ludus_editor_tests PRIVATE tests/viewport_performance_tests.cpp tests/render_graph_preview_tests.cpp tests/restir_benchmark_tests.cpp
            tests/raster_mesh_light_benchmark_tests.cpp tests/render_graph_capture_profile_tests.cpp
            tests/render_graph_library_capture_tests.cpp)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_RENDER_GRAPH_FIXTURE=\"${editor_renderer_static_RUNTIME_PACKAGE}\"")
        set(editor_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/raytracing")
        set(editor_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/render-graph-host/raytracing")
        ludus_stage_render_graph_inputs("${editor_ray_source}" "${editor_ray_staged}")
        # Exercise the canonical editable forward template through the World preview path.
        set(editor_forward_source "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/forward_lit")
        set(editor_forward_staged "${CMAKE_CURRENT_BINARY_DIR}/forward-lit-inputs")
        foreach(file main.lurender.json rasterize.lurender.json main_shaders/geometry.slang main_shaders/color.slang)
            configure_file("${editor_forward_source}/${file}" "${editor_forward_staged}/${file}" COPYONLY)
        endforeach()
        set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"spirv\", \"wgsl\"]")
        if(APPLE)
            set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
        endif()
        configure_file("${editor_forward_source}/main.lushader.json.in"
            "${editor_forward_staged}/main.lushader.json" @ONLY)
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_forward_lit
            MANIFEST "${editor_forward_staged}/main.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_FORWARD_GRAPH_FIXTURE=\"${editor_forward_lit_RUNTIME_PACKAGE}\"")
        # A stochastic ray fixture proves the editor schedules multiple samples of one revision.
        set(progressive_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/scene_ray")
        set(progressive_staged "${CMAKE_CURRENT_BINARY_DIR}/progressive-ray-inputs")
        set(RG2_TEST_PROFILES "[\"spirv\", \"wgsl\"]")
        if(APPLE)
            set(RG2_TEST_PROFILES "[\"metal\", \"wgsl\"]")
        endif()
        configure_file("${editor_ray_source}/main.lushader.json.in"
            "${editor_ray_staged}/main.lushader.json" @ONLY)
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_renderer_ray
            MANIFEST "${editor_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_RAY_GRAPH_FIXTURE=\"${editor_renderer_ray_RUNTIME_PACKAGE}\"")
        # The same sources' diagnostic capture variant, as `render-graph ray-path-capture` cooks it.
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_renderer_ray_capture
            MANIFEST "${editor_ray_staged}/main.lushader.json" RAY_PATH_CAPTURE RAY_PATH_CAPTURE_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_RAY_CAPTURE_FIXTURE=\"${editor_renderer_ray_capture_RAY_PATH_CAPTURE_PACKAGE}\"")
        # A software TriangleLoop graph and its capture variant: capture without native ray queries.
        set(editor_software_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/software-ray-inputs")
        ludus_stage_render_graph_fixture("${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/traversal_parity"
            "${editor_software_ray_staged}")
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_software_ray
            MANIFEST "${editor_software_ray_staged}/main.lushader.json" CONFIGURATION TriangleLoop RUNTIME_PACKAGE)
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_software_ray_capture
            MANIFEST "${editor_software_ray_staged}/main.lushader.json" CONFIGURATION TriangleLoop
            RAY_PATH_CAPTURE RAY_PATH_CAPTURE_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_SOFTWARE_RAY_GRAPH_FIXTURE=\"${editor_software_ray_RUNTIME_PACKAGE}\""
            "LUDUS_EDITOR_SOFTWARE_RAY_CAPTURE_FIXTURE=\"${editor_software_ray_capture_RAY_PATH_CAPTURE_PACKAGE}\"")
        target_sources(ludus_editor_tests PRIVATE tests/ray_path_inspection_tests.cpp)
        ludus_stage_render_graph_fixture("${progressive_source}" "${progressive_staged}")
        configure_file("${progressive_source}/main_shaders/progressive_closest.slang"
            "${progressive_staged}/main_shaders/closest.slang" COPYONLY)
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_progressive_ray
            MANIFEST "${progressive_staged}/main.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE=\"${editor_progressive_ray_RUNTIME_PACKAGE}\"")
        # The polygonal-light template: a scene with more emissive triangles than its interactive budget makes the
        # viewport name the visibility budget as the failure reason.
        set(polygonal_light_source "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/polygonal_light_sampling")
        set(polygonal_light_staged "${CMAKE_CURRENT_BINARY_DIR}/polygonal-light-inputs")
        ludus_stage_render_graph_fixture("${polygonal_light_source}" "${polygonal_light_staged}")
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_polygonal_light
            MANIFEST "${polygonal_light_staged}/main.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_POLYGONAL_LIGHT_FIXTURE=\"${editor_polygonal_light_RUNTIME_PACKAGE}\"")
        # The same sources' frame capture profile, as `render-graph capture-profile` cooks it.
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_progressive_ray_capture
            MANIFEST "${progressive_staged}/main.lushader.json" CAPTURE_PROFILE_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_PROGRESSIVE_CAPTURE_PROFILE_FIXTURE=\"${editor_progressive_ray_capture_CAPTURE_PROFILE_PACKAGE}\"")
        # The point-and-line template captured through a library capture root, as `render-graph capture-library`
        # writes it. Its captured lineSampling Export is not the package's first exported colour.
        set(library_capture_source "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/salesin_jarosz_lines")
        set(library_capture_staged "${CMAKE_CURRENT_BINARY_DIR}/library-capture-inputs")
        ludus_stage_render_graph_fixture("${library_capture_source}" "${library_capture_staged}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/render_graph_library_capture.py")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; sys.path.insert(0, sys.argv[1]); from ludus_tools import render_graph_library_capture as capture; capture.write_capture_sources(sys.argv[2], 'main.lushader.json', 'library.lurender.json', 'PointLineMis', 'lineSampling')"
            "${PROJECT_SOURCE_DIR}/scripts/python" "${library_capture_staged}"
            RESULT_VARIABLE library_capture_status ERROR_VARIABLE library_capture_error)
        if(NOT library_capture_status EQUAL 0)
            message(FATAL_ERROR "Cannot write the library capture fixture root: ${library_capture_error}")
        endif()
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_library_capture
            MANIFEST "${library_capture_staged}/capture.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_LIBRARY_CAPTURE_FIXTURE=\"${editor_library_capture_RUNTIME_PACKAGE}\"")
    endif()
    if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT APPLE)
        target_sources(ludus_editor_tests PRIVATE tests/offscreen_viewport_tests.cpp tests/vulkan_viewport_tests.cpp)
    endif()
    if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
        target_sources(ludus_editor_tests PRIVATE tests/play_viewport_source_tests.cpp)
        if(TARGET ludus_rhi_hosted_producer)
            # A real hosted game: the RHI test producer renders a known colour into its hosted window.
            target_compile_definitions(ludus_editor_tests PRIVATE
                LUDUS_HOSTED_PRODUCER="$<TARGET_FILE:ludus_rhi_hosted_producer>")
            add_dependencies(ludus_editor_tests ludus_rhi_hosted_producer)
            target_include_directories(ludus_editor_tests PRIVATE "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/tests")
        endif()
        if(TARGET ludus_world_demo)
            # The World-demo player: the journey plays it in the World viewport.
            target_compile_definitions(ludus_editor_tests PRIVATE
                LUDUS_WORLD_DEMO_PLAYER="$<TARGET_FILE:ludus_world_demo>")
            add_dependencies(ludus_editor_tests ludus_world_demo)
        endif()
    endif()
    if(APPLE AND LUDUS_EDITOR_RENDER_PREVIEW)
        target_sources(ludus_editor_tests PRIVATE tests/render_native_tests.mm tests/viewport_native_navigation.mm)
        # Test-only GPU pixel oracle; no RHI private header enters editor production or the SDK.
        target_include_directories(ludus_editor_tests PRIVATE "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/src")
        target_link_libraries(ludus_editor_tests PRIVATE "-framework Cocoa")
        # Native viewport acceptance uses the canonical MIS graph at both admitted group shapes.
        set(native_mis_saved_profiles "${LUDUS_DEFAULT_RENDER_GRAPH_PROFILES}")
        # Preserve the editor host's Metal/WGSL CPU interface identity; transport executes on Metal.
        set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
        foreach(shape IN ITEMS LINEAR TILED)
            set(mis_source "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/veach_guibas_mis")
            set(mis_staged "${CMAKE_CURRENT_BINARY_DIR}/native-mis-inputs/${shape}")
            ludus_stage_render_graph_fixture("${mis_source}" "${mis_staged}" library.lurender.json)
            file(READ "${mis_source}/library.lurender.json" mis_library)
            if(shape STREQUAL TILED)
                string(REPLACE "\"state\": {}" "\"state\": {\"group_size\": [8, 8, 1]}" mis_library "${mis_library}")
            else()
                string(REPLACE "\"state\": {}" "\"state\": {\"group_size\": [64, 1, 1]}" mis_library "${mis_library}")
            endif()
            # Qualify native presentation and GPU transport without requiring the
            # hosted Metal device's optional native acceleration-structure capability.
            string(JSON mis_node_count LENGTH "${mis_library}" configurations 0 nodes)
            math(EXPR mis_node_last "${mis_node_count} - 1")
            foreach(node_index RANGE ${mis_node_last})
                string(JSON node_kind GET "${mis_library}" configurations 0 nodes ${node_index} kind)
                if(node_kind STREQUAL "ComputePipeline")
                    string(JSON mis_library SET "${mis_library}" configurations 0 nodes ${node_index}
                        invocation traversal "\"ComputeBvh\"")
                endif()
            endforeach()
            file(WRITE "${mis_staged}.library.json" "${mis_library}")
            file(COPY_FILE "${mis_staged}.library.json" "${mis_staged}/library.lurender.json" ONLY_IF_DIFFERENT)
            ludus_compile_render_graph(TARGET ludus_editor_tests NAME "editor_native_mis_${shape}"
                MANIFEST "${mis_staged}/main.lushader.json" RUNTIME_PACKAGE)
            target_compile_definitions(ludus_editor_tests PRIVATE
                "LUDUS_EDITOR_MIS_${shape}_FIXTURE=\"${editor_native_mis_${shape}_RUNTIME_PACKAGE}\"")
        endforeach()
        set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "${native_mis_saved_profiles}")
    endif()
    if(APPLE)
        target_sources(ludus_editor_tests PRIVATE tests/viewport_relative_mouse_tests.mm)
        target_link_libraries(ludus_editor_tests PRIVATE "-framework AppKit" "-framework CoreGraphics")
    endif()
    # Catch2 reports assertion failures by throwing; test targets (only) re-enable
    # exceptions. Engine/app targets above stay exception-free (-fno-exceptions).
    ludus_enable_test_exceptions(ludus_editor_tests)
    # Thanks to Catch2's authors, "Command line", test sharding:
    # https://github.com/catchorg/Catch2/blob/v3.4.0/docs/command-line.md#test-sharding
    # The 699-case suite takes 434 s locally and exhausts 600 s on macOS ASan.
    # Keep each case exactly once in declaration order, with separate hang deadlines.
    # A resource lock retains serial GPU/worker ownership even when CTest runs in parallel.
    function(ludus_add_editor_test_shards target_name shard_count timeout labels)
        math(EXPR last_shard "${shard_count} - 1")
        foreach(shard_index RANGE 0 ${last_shard})
            set(test_name "${target_name}_${shard_index}")
            add_test(NAME "${test_name}" COMMAND "${target_name}" --order decl
                --shard-count "${shard_count}" --shard-index "${shard_index}" --durations yes)
            set_tests_properties("${test_name}" PROPERTIES LABELS "${labels}" TIMEOUT "${timeout}"
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RESOURCE_LOCK ludus_editor)
        endforeach()
    endfunction()
    ludus_add_editor_test_shards(ludus_editor_tests 4 600 "unit;editor")
    # Transitional, empty: batch CI runs the workflow definitions on main against
    # the develop candidate, and main's ci.yml still builds this target, which
    # 547148cf removed with SceneDocument. Delete it once a promotion carries the
    # current ci.yml to main (continuous-integration.md, batched integration).
    add_custom_target(ludus_editor_scene_tests)
endif()

if(EMSCRIPTEN)
    find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS CorePrivate GuiPrivate)
    # editor_files.cpp compiles in this object library, before the final editor's usage requirements apply.
    target_link_libraries(ludus_editor_content PRIVATE Qt6::CorePrivate)
    target_link_libraries(ludus_editor PRIVATE Qt6::GuiPrivate)
    set_property(TARGET ludus_editor APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/browser/files.js")
    set_target_properties(ludus_editor PROPERTIES QT_WASM_INITIAL_MEMORY 33554432 QT_WASM_MAXIMUM_MEMORY 2147483648)
    target_link_options(ludus_editor PRIVATE "-sASYNCIFY_STACK_SIZE=131072"
        "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/browser/files.js")
endif()

if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    # Keep CPU conversion ownership/cancellation testable without the renderer or shader build.
    add_executable(ludus_texture_display_conversion_tests
        tests/texture_display_conversion_tests.cpp tests/texture_inspection_tests.cpp
        tests/viewport_frame_pump_tests.cpp
        src/internal/texture_inspection.h src/texture_inspection.cpp
        src/internal/viewport_frame_pump.h src/viewport_frame_pump.cpp)
    set_target_properties(ludus_texture_display_conversion_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_texture_display_conversion_tests PRIVATE src)
    target_compile_definitions(ludus_texture_display_conversion_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_texture_display_conversion_tests PRIVATE ludus_editor_texture_display Qt6::Widgets Qt6::Test
        Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationProfiling Catch2::Catch2)
    ludus_apply_project_defaults(ludus_texture_display_conversion_tests)
    ludus_enable_test_exceptions(ludus_texture_display_conversion_tests)
    add_test(NAME ludus_texture_display_conversion_tests COMMAND ludus_texture_display_conversion_tests)
    set_tests_properties(ludus_texture_display_conversion_tests PROPERTIES LABELS "unit;editor" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ludus_render_graph_pipeline_tests tests/render_graph_pipeline_tests.cpp
        tests/render_graph_shader_cleanup_tests.cpp)
    set_target_properties(ludus_render_graph_pipeline_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_pipeline_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_pipeline_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
    target_link_libraries(ludus_render_graph_pipeline_tests PRIVATE
        ludus_editor_render_graph_model ludus_editor_render_graph_canvas ludus_editor_presentation
        ludus_editor_shader_api_resources
        Ludus::RenderGraphFormat Qt6::Core Qt6::Widgets Qt6::Test
        Ludus::FoundationContainers Ludus::FoundationMath Catch2::Catch2)
    ludus_apply_project_defaults(ludus_render_graph_pipeline_tests)
    ludus_enable_test_exceptions(ludus_render_graph_pipeline_tests)
    add_test(NAME ludus_render_graph_pipeline_tests COMMAND ludus_render_graph_pipeline_tests)
    set_tests_properties(ludus_render_graph_pipeline_tests PROPERTIES LABELS "editor;render-graph" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ludus_shader_interface_tests tests/render_graph_shader_interface_tests.cpp)
    target_include_directories(ludus_shader_interface_tests PRIVATE src)
    target_compile_definitions(ludus_shader_interface_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
    target_link_libraries(ludus_shader_interface_tests PRIVATE ludus_editor_render_graph_model ludus_editor_shader_api_resources Qt6::Widgets Catch2::Catch2 Ludus::FoundationBase)
    ludus_apply_project_defaults(ludus_shader_interface_tests)
    ludus_enable_test_exceptions(ludus_shader_interface_tests)
    add_test(NAME ludus_shader_interface_tests COMMAND ludus_shader_interface_tests)
    set_tests_properties(ludus_shader_interface_tests PROPERTIES LABELS "editor;render-graph" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Authoring uses the real workspace/document without requiring a viewport device.
    add_executable(ludus_render_graph_configuration_tests
        tests/render_graph_upscale_tests.cpp tests/render_graph_configuration_tests.cpp tests/render_graph_template_tests.cpp tests/render_graph_luminaire_coverage_tests.cpp tests/world_lighting_coverage_tests.cpp tests/render_graph_template_registry_tests.cpp tests/render_graph_quick_add_tests.cpp tests/render_graph_rename_tests.cpp tests/render_graph_selection_tests.cpp tests/render_graph_canvas_tests.cpp tests/render_graph_navigation_tests.cpp tests/render_graph_history_tests.cpp tests/render_graph_layout_tests.cpp tests/render_graph_minimap_tests.cpp tests/render_graph_execution_plan_tests.cpp tests/render_graph_comment_tests.cpp tests/render_graph_reroute_tests.cpp tests/render_graph_resource_inspection_tests.cpp tests/render_graph_canvas_feedback_tests.cpp tests/render_graph_diagnostic_tests.cpp tests/render_graph_diagnostic_navigation_tests.cpp
        # The workspace class and default graph compile here without the GPU preview; the
        # shared render-graph objects below are the same objects the editor links.
        src/internal/renderer_workspace.h src/renderer_workspace.cpp
        src/internal/render_graph_defaults.h src/render_graph_defaults.cpp
        tests/render_graph_shader_assistance_tests.cpp tests/render_graph_source_view_tests.cpp)
    set_target_properties(ludus_render_graph_configuration_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_configuration_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_configuration_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
        "LUDUS_TEST_PYTHON=\"${Python3_EXECUTABLE}\"")
    # The workspace borrows viewport types, but this test never creates a device or
    # calls their compiled implementations. Keep those dependencies at the header boundary.
    # Thanks to Kitware, "cmake-generator-expressions(7)", COMPILE_ONLY, for the
    # compile-usage-only dependency mechanism:
    # https://cmake.org/cmake/help/v3.29/manual/cmake-generator-expressions.7.html#genex:COMPILE_ONLY
    target_link_libraries(ludus_render_graph_configuration_tests PRIVATE
        ludus_editor_render_graph_model ludus_editor_render_graph_canvas ludus_editor_render_graph_authoring
        ludus_editor_presentation ludus_editor_render_graph_resources ludus_editor_shader_api_resources
        ludus_editor_icon_resources Ludus::RenderGraphFormat ludus_editor_core ludus_editor_code
        Qt6::Widgets Qt6::Test Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationProfiling
        Catch2::Catch2
        "$<COMPILE_ONLY:Ludus::GraphicsRenderer>" "$<COMPILE_ONLY:Ludus::GameplayCamera>"
        "$<COMPILE_ONLY:Ludus::Resources>")
    ludus_apply_project_defaults(ludus_render_graph_configuration_tests)
    ludus_enable_test_exceptions(ludus_render_graph_configuration_tests)
    # The combined canvas suite exceeds 180 s on macOS, including ASan; keep every case
    # under its existing hang deadline without accumulating all cases into that deadline.
    ludus_add_editor_test_shards(ludus_render_graph_configuration_tests 2 180 "editor;render-graph")

    add_executable(ludus_render_graph_live_build_tests tests/render_graph_live_build_tests.cpp)
    set_target_properties(ludus_render_graph_live_build_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_live_build_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_live_build_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_render_graph_live_build_tests PRIVATE ludus_editor_render_graph_model Qt6::Core Qt6::Widgets Qt6::Test
        Ludus::FoundationContainers Ludus::FoundationMath Catch2::Catch2)
    ludus_apply_project_defaults(ludus_render_graph_live_build_tests)
    ludus_enable_test_exceptions(ludus_render_graph_live_build_tests)
    add_test(NAME ludus_render_graph_live_build_tests COMMAND ludus_render_graph_live_build_tests)
    set_tests_properties(ludus_render_graph_live_build_tests PROPERTIES LABELS "editor;shader" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Editor-private curve/surface adapter tests (curves and surfaces G5). The
    # adapters are Qt-free logic, so these run headless without a GPU/compositor.
    add_executable(ludus_editor_geometry_tests
        tests/geometry_document_tests.cpp tests/geometry_preview_tests.cpp)
    target_include_directories(ludus_editor_geometry_tests PRIVATE src)
    target_link_libraries(ludus_editor_geometry_tests PRIVATE
        ludus_editor_geometry_model Ludus::GeometryContent Ludus::GeometryFormat Ludus::FoundationGeometry
        Ludus::Content Ludus::FoundationContainers Ludus::FoundationMath Catch2::Catch2WithMain)
    target_compile_options(ludus_editor_geometry_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_editor_geometry_tests)
    ludus_enable_test_exceptions(ludus_editor_geometry_tests)
    add_test(NAME ludus_editor_geometry_tests COMMAND ludus_editor_geometry_tests)
    set_tests_properties(ludus_editor_geometry_tests PROPERTIES LABELS "editor;geometry" TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Keep the established editor test build entry point complete as suites move into
# separate executables. Trusted batch workflows build this target before running
# the editor CTest label, including during the promotion of newer workflow code.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EditorTestDependencies.cmake")
ludus_complete_editor_test_target(ludus_editor_tests)

if(TARGET ludus_render_graph_configuration_tests)
    target_sources(ludus_render_graph_configuration_tests PRIVATE tests/render_graph_resolution_tests.cpp
        tests/render_graph_options_tests.cpp tests/render_graph_recovery_tests.cpp)
    # Resource inspection bridges metadata to the existing viewer; authoring tests need no device.
    target_sources(ludus_render_graph_configuration_tests PRIVATE src/renderer_workspace_resources.cpp)
endif()

# Precompiled Qt Core and standard headers; see cmake/EditorPrecompiledHeaders.cmake.
# The Qt-free geometry model has no Qt include directories, so it compiles
# without the Qt Core precompiled header.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EditorPrecompiledHeaders.cmake")
set(editor_precompiled_header_objects ${editor_application_objects})
list(REMOVE_ITEM editor_precompiled_header_objects ludus_editor_geometry_model)
ludus_editor_apply_precompiled_headers(
    ludus_editor_core
    ludus_editor_code
    ${editor_precompiled_header_objects}
    ludus_editor_texture_display
    ludus_editor_crash_reporting
    ludus_editor_test_main
    ludus_editor
    ludus_code_editor_tests
    ludus_project_explorer_tests
    ludus_editor_browser_content_tests
    ludus_editor_tests
    ludus_render_graph_pipeline_tests
    ludus_shader_interface_tests
    ludus_render_graph_configuration_tests
    ludus_render_graph_live_build_tests
    ludus_texture_display_conversion_tests
)

# Every editor test executable links the one compiled Catch2 entry point.
foreach(target IN ITEMS ludus_document_well_tests ludus_editor_accessibility_tests ludus_code_editor_tests
        ludus_editor_browser_content_tests ludus_project_explorer_tests ludus_editor_tests
        ludus_texture_display_conversion_tests ludus_render_graph_pipeline_tests ludus_shader_interface_tests
        ludus_render_graph_configuration_tests ludus_render_graph_live_build_tests)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE ludus_editor_test_main)
    endif()
endforeach()
