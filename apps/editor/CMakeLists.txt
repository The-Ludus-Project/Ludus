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
    set(editor_audio_source src/audio_preview.cpp)
endif()

# --- Qt discovery (local to the enabled editor only) -------------------------
# Keep UI bounds and document admission tied to the owning build-service policy.
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
    PROPERTIES COMPILE_DEFINITIONS "${graph_settings_definitions}")

# Use APIs available in Qt 6.4; require at least 6.4. Core/Gui/Widgets only, as
# native system libraries or the pinned source-built WebAssembly Qt.
# Qt is installed only by opted-in editor setup;
# it is never added to the engine Conan recipe or SDK.
find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Widgets)
if(NOT Qt6_FOUND)
    ludus_dependency_error("Editor workspace" "Qt 6.4+ Core, Gui and Widgets"
        "Qt development packages are missing or incompatible. Qt6_DIR='${Qt6_DIR}'."
        "For native Linux setup, run ./init.sh --cli --with-editor from the repository root. For browser setup, follow docs/development/browser-editor.md. For an existing Qt installation set Qt6_DIR; or disable the editor with -DLUDUS_BUILD_EDITOR=OFF.")
endif()

# QT_NO_KEYWORDS forbids the unprefixed signals/slots/emit macros so Qt's macro
# vocabulary cannot collide with engine/standard identifiers; use Q_SIGNALS/
# Q_SLOTS/Q_EMIT and the typed (pointer-to-member) connect overloads instead.
# AUTOMOC is enabled per target below, not globally.

set(editor_viewport_sources
    src/internal/viewport_frame_pump.h
    src/viewport_frame_pump.cpp
    src/internal/viewport_frame_source.h
    src/viewport_frame_source.cpp
    src/internal/render_graph_source_watch.h
    src/render_graph_source_watch.cpp
    src/internal/empty_world_view.h
    src/empty_world_view.cpp
    src/preview_viewport.cpp
    src/viewport_navigation.cpp
    src/viewport_play_input.cpp
    src/internal/texture_inspection.h
    src/texture_inspection.cpp)
if(APPLE)
    list(APPEND editor_viewport_sources src/internal/viewport_relative_mouse.h src/viewport_relative_mouse_macos.mm)
endif()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick)
    # The readback fallback is selected only on Linux, but the shared viewport code references it.
    list(APPEND editor_viewport_sources src/internal/offscreen_frame_reader.h src/offscreen_frame_reader.cpp)
    # RG3b trusted render-graph preview reads exported color back through the same frame reader.
    list(APPEND editor_viewport_sources src/internal/render_graph_preview.h src/render_graph_preview.cpp
        src/internal/graph_display_frames.h src/graph_display_frames.cpp
        src/internal/world_graph_view.h src/world_graph_view.cpp
        src/internal/render_graph_worker.h src/render_graph_worker.cpp)
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
    target_include_directories(${target} PRIVATE "${directory}")
    target_link_libraries(${target} PRIVATE Ludus::ShaderCompilerClient)
endfunction()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    list(APPEND editor_viewport_sources
        src/internal/preview_shader_build.h src/preview_shader_build.cpp
        src/internal/preview_shader_reload.h src/preview_shader_reload.cpp)
endif()

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
    file(COPY "${input}/" DESTINATION "${staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${input}/main.lushader.json.in" "${staged}/main.lushader.json" @ONLY)
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
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/scene_payload.slang"
              "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/visibility_buffer.slang")
endfunction()

# The default graph's authored sources, read-only inside the editor (also in builds
# without the native preview), so the Renderer tab can show and copy them.
function(ludus_editor_add_default_render_graph target)
    ludus_editor_add_shader_api(${target})
    set(restir_template "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/restir_di")
    file(GLOB restir_template_shaders CONFIGURE_DEPENDS "${restir_template}/main_shaders/*.slang")
    qt_add_resources(${target} ludus_restir_configuration
        PREFIX "/render-graph/templates/restir-di" BASE "${restir_template}"
        FILES "${restir_template}/library.lurender.json" "${restir_template}/main.lushader.json.in"
              ${restir_template_shaders})
    foreach(template_directory IN ITEMS visibility_buffer visibility_secondary_rays)
        string(REPLACE "_" "-" template_identity "${template_directory}")
        set(visibility_template "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/${template_directory}")
        file(GLOB visibility_template_shaders CONFIGURE_DEPENDS "${visibility_template}/main_shaders/*.slang")
        qt_add_resources(${target} "ludus_${template_directory}_configuration"
            PREFIX "/render-graph/templates/${template_identity}" BASE "${visibility_template}"
            FILES "${visibility_template}/library.lurender.json" "${visibility_template}/main.lushader.json.in"
                  ${visibility_template_shaders})
    endforeach()
    get_property(default_graph GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_DIR)
    get_property(default_graph_files GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_FILES)
    list(TRANSFORM default_graph_files PREPEND "${default_graph}/")
    qt_add_resources(${target} ludus_default_render_graph
        PREFIX "/render-graph/default" BASE "${default_graph}" FILES ${default_graph_files})
endfunction()

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

# Reusable private text presentation; no renderer or shader-tool dependency.
add_library(ludus_editor_code STATIC
    src/internal/code_editor.h src/code_editor.cpp
    src/internal/code_editor_panel.h src/code_editor_panel.cpp
    src/internal/code_syntax_highlighter.h src/code_syntax_highlighter.cpp
    src/internal/code_document.h src/internal/code_edit_scope.h
    src/internal/code_text.h src/code_text.cpp
    src/internal/code_document_controller.h src/code_document_controller.cpp
    src/internal/code_formatter.h src/code_formatter.cpp)
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
    PRIVATE Ludus::FoundationBase)
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
qt_add_executable(ludus_editor
    ${editor_profiling_sources}
    src/internal/render_load_job.h
    src/render_load_job.cpp
    src/internal/render_preview.h
    src/internal/render_panel.h
    src/internal/preview_viewport.h
    ${editor_viewport_sources}
    src/render_scene.cpp
    src/render_preview.cpp
    src/preview_gpu_generation.cpp
    src/internal/preview_gpu_generation.h
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
    src/internal/import_report.h
    src/import_report.cpp
    src/internal/world_workspace.h
    src/world_document.cpp
    src/world_demo_adapter.cpp
    src/world_workspace.cpp
    src/internal/controller.h
    src/internal/configuration_workspace.h
    src/internal/main_window.h
    src/internal/audio_workspace.h
    src/internal/audio_preview.h
    src/internal/content_browser.h
    src/content_catalog_model.cpp
    src/content_import.cpp
    src/content_jobs.cpp
    src/content_workspace.cpp
    src/internal/script_workspace.h
    src/script_workspace.cpp
    src/internal/render_graph_document.h
    src/internal/render_graph_source.h
    src/render_graph_document.cpp src/render_graph_configuration_template.cpp src/render_graph_visibility_template.cpp src/render_graph_node_transfer.cpp
    src/internal/render_graph_shader_creation.h
    src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp src/render_graph_scene_authoring.cpp src/render_graph_scene_bundle.cpp src/render_graph_techniques.cpp
    src/internal/render_graph_pipeline.h
    src/render_graph_pipeline.cpp
    src/internal/render_graph_pipeline_node.h
    src/internal/render_graph_upscale_node.h
    src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
    src/internal/render_graph_job.h
    src/render_graph_job.cpp
    src/internal/render_graph_creation.h
    src/render_graph_creation.cpp
    src/internal/render_graph_settings_editor.h
    src/render_graph_settings_editor.cpp
    src/internal/render_graph_configuration_editor.h
    src/render_graph_configuration_editor.cpp
    src/render_graph_configuration_interface.cpp
    src/internal/render_graph_canvas.h
    src/render_graph_canvas.cpp src/render_graph_resources.cpp src/render_graph_canvas_feedback.cpp
    src/internal/external_code_editor.h
    src/external_code_editor.cpp
    src/internal/renderer_workspace.h
    src/renderer_workspace.cpp src/renderer_workspace_selection.cpp
    src/internal/render_graph_shader_interface.h
    src/render_graph_shader_interface.cpp
    src/internal/render_graph_defaults.h
    src/render_graph_defaults.cpp
    src/internal/render_graph_live_build.h
    src/render_graph_live_build.cpp
    src/configuration_workspace.cpp
    src/editor_files.cpp
    src/internal/scene_archive.h
    src/scene_archive.cpp
    src/audio_workspace.cpp
    ${editor_audio_source}
    main.cpp
    src/main_window.cpp
    src/document_commands.cpp
    src/main_window_layout.cpp
    src/main_window_tools.cpp
    src/internal/document_well.h
    src/document_well.cpp
    src/internal/editor_commands.h
    src/editor_commands.cpp
    src/internal/options_dialog.h
    src/options_dialog.cpp
    src/internal/render_graph_build_settings.h
    src/internal/error_list.h
    src/error_list.cpp
    src/internal/project_explorer.h
    src/project_explorer.cpp
    src/internal/timing_panel.h
    src/timing_panel.cpp
    src/project_setup_dialog.cpp
    src/project_creation_dialog.cpp
    src/workspace_style.cpp
    src/controller.cpp
    src/controller_play.cpp
)
set_target_properties(ludus_editor PROPERTIES AUTOMOC ON)
set(editor_brand_assets
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/ludus-icon.png"
    "${PROJECT_SOURCE_DIR}/docs/wiki/assets/ludus-mark.png")
qt_add_resources(ludus_editor ludus_brand
    PREFIX "/ludus"
    BASE "${PROJECT_SOURCE_DIR}/docs/wiki/assets"
    FILES ${editor_brand_assets})
# Ship editable, self-contained examples with both presentations. Generated
# build outputs and local machine settings are never part of the sample library;
# they are excluded relative to each sample root, not the checkout path.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EditorSampleResources.cmake")
ludus_collect_editor_sample_files(editor_sample_files "${PROJECT_SOURCE_DIR}/examples"
    cornell-box live-edit-game scripted-game editor-sdk-project)
set(world_demo_schema "${PROJECT_SOURCE_DIR}/apps/world_demo/authoring/world.schema.json")
qt_add_resources(ludus_editor world_demo_schema PREFIX "/world-demo" BASE "${PROJECT_SOURCE_DIR}/apps/world_demo/authoring" FILES ${world_demo_schema})
qt_add_resources(ludus_editor ludus_samples
    PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/examples" FILES ${editor_sample_files})
ludus_editor_add_default_render_graph(ludus_editor)
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
target_link_libraries(ludus_editor
    PRIVATE
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
        Ludus::GraphicsRenderer
        Ludus::Resources
        Ludus::FoundationParsingJson
)
ludus_apply_project_defaults(ludus_editor)
if(LUDUS_EDITOR_CRASH_CAPTURE)
    # dSYM is produced from this exact linked executable, before any later stripping/signing.
    find_program(editor_dsymutil NAMES llvm-dsymutil dsymutil REQUIRED)
    target_compile_options(ludus_editor PRIVATE -g)
    add_custom_command(TARGET ludus_editor POST_BUILD
        COMMAND "${editor_dsymutil}" "$<TARGET_FILE:ludus_editor>" -o "$<TARGET_BUNDLE_DIR:ludus_editor>.dSYM"
        VERBATIM)
    ludus_editor_bundle_crash_capture(ludus_editor)
    target_sources(ludus_editor PRIVATE src/crash_reporting.cpp src/internal/crash_reporting.h)
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
    # Small production-path crash probe; enabled independently of the full Catch2 test graph.
    option(LUDUS_EDITOR_CRASH_CAPTURE_TESTS "Build the native crash-capture subprocess probe" OFF)
    if(LUDUS_EDITOR_CRASH_CAPTURE_TESTS)
        add_executable(ludus_editor_crash_probe tests/crash_capture_probe.cpp src/crash_reporting.cpp)
        target_include_directories(ludus_editor_crash_probe PRIVATE src "${editor_crash_config_directory}")
        target_compile_definitions(ludus_editor_crash_probe PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_editor_crash_probe PRIVATE Qt6::Widgets Ludus::FoundationContainers)
        ludus_apply_project_defaults(ludus_editor_crash_probe)
        ludus_editor_bundle_crash_capture(ludus_editor_crash_probe)
        add_test(NAME ludus_editor_crash_capture
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_crash_capture.py"
                "$<TARGET_FILE:ludus_editor_crash_probe>" "$<TARGET_FILE:crashpad_handler>")
        set_tests_properties(ludus_editor_crash_capture PROPERTIES LABELS "editor;crash;native" TIMEOUT 180)
    endif()
endif()
target_link_libraries(ludus_editor PRIVATE Ludus::Platform)
# Scene import, save and assembly run in process (no Python scene backend),
# including the browser Editor's MEMFS copies.
target_link_libraries(ludus_editor PRIVATE Ludus::SceneCook Ludus::FoundationThreading ludus_stb_image)
# World documents share the version-2 record constants with their owner (object handles stage 3).
target_link_libraries(ludus_editor PRIVATE Ludus::ContentIdentity)
if(EMSCRIPTEN)
    target_compile_definitions(ludus_editor PRIVATE LUDUS_EDITOR_BROWSER_PREVIEW=1 LUDUS_EDITOR_GPU_PREVIEW=1)
    target_sources(ludus_editor PRIVATE src/browser_viewport.cpp src/internal/browser_viewport.h)
    target_link_libraries(ludus_editor PRIVATE Ludus::GraphicsRhi)
    ludus_compile_renderer_shaders(TARGET ludus_editor SHADERS
        renderer_flat renderer_composite renderer_material renderer_lit renderer_output renderer_shadow
        renderer_lit_shadow renderer_clustered_authored renderer_clustered_authored_shadow
        renderer_lit_debug renderer_lit_debug_shadow
        renderer_clustered_authored_debug renderer_clustered_authored_debug_shadow)
endif()
if(LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    target_compile_definitions(ludus_editor PRIVATE LUDUS_EDITOR_NATIVE_PREVIEW=1 LUDUS_EDITOR_GPU_PREVIEW=1)
    target_link_libraries(ludus_editor PRIVATE Ludus::GraphicsRhi Qt6::Quick)
    ludus_compile_renderer_shaders(TARGET ludus_editor SHADERS
        renderer_flat renderer_composite renderer_material renderer_lit renderer_output renderer_shadow)
    ludus_compile_renderer_shaders(TARGET ludus_editor SHADERS
        renderer_lit_shadow renderer_clustered renderer_clustered_shadow
        renderer_clustered_authored renderer_clustered_authored_shadow
        renderer_lit_debug renderer_lit_debug_shadow
        renderer_clustered_authored_debug renderer_clustered_authored_debug_shadow)
    ludus_editor_compile_render_graph_host(ludus_editor)
    ludus_editor_enable_shader_reload(ludus_editor)
endif()

# The editor is a developer tool, never part of the installed SDK: no install()
# rule and no LudusTargets export entry here, by design (E01).
if(APPLE)
    target_sources(ludus_editor PRIVATE src/qt_accessibility_macos.mm)
    target_link_libraries(ludus_editor PRIVATE "-framework AppKit")
endif()

# --- Offscreen logic/widget tests --------------------------------------------
# Offscreen Qt tests verify logic and widget wiring without a GPU/compositor.
# They do NOT prove native Wayland interaction or that a game rendered a frame.
if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    find_package(Catch2 3 REQUIRED)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    if(APPLE)
        add_executable(ludus_editor_accessibility_tests tests/main.cpp tests/accessibility_tests.mm
            src/qt_accessibility_macos.mm)
        target_include_directories(ludus_editor_accessibility_tests PRIVATE src)
        target_compile_definitions(ludus_editor_accessibility_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
        target_link_libraries(ludus_editor_accessibility_tests PRIVATE Qt6::Widgets Catch2::Catch2
            Ludus::FoundationBase "-framework AppKit")
        ludus_apply_project_defaults(ludus_editor_accessibility_tests)
        ludus_enable_test_exceptions(ludus_editor_accessibility_tests)
        add_test(NAME ludus_editor_accessibility_tests COMMAND ludus_editor_accessibility_tests)
        set_tests_properties(ludus_editor_accessibility_tests PROPERTIES LABELS "unit;editor;native;accessibility"
            TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    endif()
    add_executable(ludus_code_editor_tests tests/main.cpp tests/code_editor_tests.cpp tests/code_document_tests.cpp)
    target_include_directories(ludus_code_editor_tests PRIVATE src)
    target_compile_definitions(ludus_code_editor_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    configure_file(tests/formatter_fixture.py generated/formatter_fixture.py COPYONLY)
    target_compile_definitions(ludus_code_editor_tests PRIVATE
        "LUDUS_FORMATTER_FIXTURE=\"${CMAKE_CURRENT_BINARY_DIR}/generated/formatter_fixture.py\"")
    target_link_libraries(ludus_code_editor_tests PRIVATE ludus_editor_code Qt6::Test
        Ludus::FoundationBase Catch2::Catch2)
    ludus_apply_project_defaults(ludus_code_editor_tests)
    ludus_enable_test_exceptions(ludus_code_editor_tests)
    add_test(NAME ludus_code_editor_tests COMMAND ludus_code_editor_tests)
    set_tests_properties(ludus_code_editor_tests PROPERTIES LABELS "unit;editor;text" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Replay the content suite through the browser's single-owner scheduler as
    # well as the normal native worker. No Qt/Wasm-private APIs enter this target.
    add_executable(ludus_editor_browser_content_tests
        tests/main.cpp tests/content_browser_tests.cpp
        src/internal/content_browser.h src/content_catalog_model.cpp
        src/content_import.cpp src/content_jobs.cpp src/content_workspace.cpp
        src/editor_files.cpp src/scene_archive.cpp)
    set_target_properties(ludus_editor_browser_content_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_editor_browser_content_tests PRIVATE src
        "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_compile_definitions(ludus_editor_browser_content_tests PRIVATE
        QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS=1)
    target_link_libraries(ludus_editor_browser_content_tests PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test Ludus::AudioContent Ludus::FoundationMath ludus_stb_image Catch2::Catch2)
    ludus_apply_project_defaults(ludus_editor_browser_content_tests)
    ludus_enable_test_exceptions(ludus_editor_browser_content_tests)
    add_test(NAME ludus_editor_browser_content_tests COMMAND ludus_editor_browser_content_tests)
    set_tests_properties(ludus_editor_browser_content_tests PROPERTIES
        LABELS "unit;editor;browser" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(ludus_editor_tests
        ${editor_profiling_sources}
        tests/profiling_tests.cpp
        tests/viewport_trace_tests.cpp
            src/internal/render_load_job.h
        src/render_load_job.cpp
        src/internal/render_preview.h
        src/internal/render_panel.h
        src/internal/preview_viewport.h
        ${editor_viewport_sources}
        src/render_scene.cpp
        src/render_preview.cpp
        src/preview_gpu_generation.cpp
        src/internal/preview_gpu_generation.h
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
        src/internal/import_report.h
        src/import_report.cpp
        tests/import_report_tests.cpp
        src/internal/world_workspace.h
        src/world_document.cpp
        src/world_demo_adapter.cpp
        src/world_workspace.cpp
            tests/world_workspace_tests.cpp
        tests/level_workspace_tests.cpp
        tests/render_authoring_tests.cpp
        tests/world_lighting_switch_tests.cpp
        tests/viewport_debug_tests.cpp
        tests/preview_generation_tests.cpp
        tests/viewport_navigation_tests.cpp
        tests/viewport_frame_pump_tests.cpp
        tests/viewport_frame_source_tests.cpp
        tests/world_demo_workspace_tests.cpp
        tests/world_identity_tests.cpp
        src/internal/controller.h
        src/internal/configuration_workspace.h
        src/internal/main_window.h
        src/internal/audio_workspace.h
        src/internal/audio_preview.h
        src/internal/content_browser.h
        src/content_catalog_model.cpp
        src/content_import.cpp
        src/content_jobs.cpp
        src/content_workspace.cpp
        src/internal/script_workspace.h
        src/script_workspace.cpp
        src/internal/render_graph_document.h
        src/internal/render_graph_source.h
        src/render_graph_document.cpp src/render_graph_configuration_template.cpp src/render_graph_visibility_template.cpp src/render_graph_node_transfer.cpp
        src/internal/render_graph_shader_creation.h
        src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp src/render_graph_scene_authoring.cpp src/render_graph_scene_bundle.cpp src/render_graph_techniques.cpp
        src/internal/render_graph_pipeline.h
        src/render_graph_pipeline.cpp
        src/internal/render_graph_pipeline_node.h
        src/internal/render_graph_upscale_node.h
        src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
        src/internal/render_graph_job.h
        src/render_graph_job.cpp
        src/internal/render_graph_creation.h
        src/render_graph_creation.cpp
        src/internal/render_graph_settings_editor.h src/render_graph_settings_editor.cpp
        src/internal/render_graph_configuration_editor.h
        src/render_graph_configuration_editor.cpp
        src/render_graph_configuration_interface.cpp
        src/internal/render_graph_canvas.h
        src/render_graph_canvas.cpp src/render_graph_resources.cpp src/render_graph_canvas_feedback.cpp
        src/internal/external_code_editor.h
        src/external_code_editor.cpp
        src/internal/renderer_workspace.h
        src/renderer_workspace.cpp src/renderer_workspace_selection.cpp
        src/internal/render_graph_shader_interface.h
        src/render_graph_shader_interface.cpp
        src/internal/render_graph_defaults.h
        src/render_graph_defaults.cpp
    src/internal/render_graph_live_build.h
    src/render_graph_live_build.cpp
        src/configuration_workspace.cpp
        src/editor_files.cpp
        src/internal/scene_archive.h
        src/scene_archive.cpp
        tests/browser_import_tests.cpp
        src/audio_workspace.cpp
        ${editor_audio_source}
        tests/main.cpp
        tests/configuration_workspace_tests.cpp
        tests/audio_content_tests.cpp
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
        tests/recent_projects_tests.cpp
        tests/source_watch_tests.cpp
        tests/script_workspace_tests.cpp
        tests/renderer_workspace_tests.cpp tests/render_graph_template_tests.cpp
        tests/render_graph_node_menu_tests.cpp
        tests/render_graph_rename_tests.cpp
        tests/render_graph_default_tests.cpp
        tests/render_graph_canvas_tests.cpp
        tests/render_graph_canvas_feedback_tests.cpp
        tests/render_graph_diagnostic_tests.cpp
        tests/texture_inspection_tests.cpp
        tests/external_code_editor_tests.cpp
        tests/live_journey_tests.cpp
        tests/macos_journey_tests.cpp
    )
    # Keep the small software rasterizer only as an independent test oracle.
    target_sources(ludus_editor_tests PRIVATE src/software_viewport.cpp)
    set_target_properties(ludus_editor_tests PROPERTIES AUTOMOC ON)
    qt_add_resources(ludus_editor_tests ludus_brand
        PREFIX "/ludus"
        BASE "${PROJECT_SOURCE_DIR}/docs/wiki/assets"
        FILES ${editor_brand_assets})
    qt_add_resources(ludus_editor_tests world_demo_schema PREFIX "/world-demo" BASE "${PROJECT_SOURCE_DIR}/apps/world_demo/authoring" FILES ${world_demo_schema})
    qt_add_resources(ludus_editor_tests ludus_samples
        PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/examples" FILES ${editor_sample_files})
    ludus_editor_add_default_render_graph(ludus_editor_tests)
    target_include_directories(ludus_editor_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_compile_definitions(ludus_editor_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        "LUDUS_EDITOR_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "LUDUS_EDITOR_PROJECT_ROOT=\"${PROJECT_SOURCE_DIR}\""
        # The configured spirv-val (empty without one), for tests that build SPIR-V profiles.
        "LUDUS_EDITOR_TEST_SPIRV_VALIDATOR=\"${LUDUS_SPIRV_VALIDATOR}\"")
    target_compile_definitions(ludus_editor_tests PRIVATE
        "LUDUS_EDITOR_SDK_MANIFEST=\"${PROJECT_BINARY_DIR}/cmake/LudusSdkManifest.json\"")
    # Controller/MainWindow sources are compiled directly into the test binary so
    # tests can drive the controller and (offscreen) widgets.
    target_sources(ludus_editor_tests PRIVATE
        src/controller.cpp
        src/controller_play.cpp
        src/main_window.cpp
        src/document_commands.cpp
        src/main_window_layout.cpp
        src/main_window_tools.cpp
        src/internal/document_well.h
        src/document_well.cpp
        src/internal/editor_commands.h
        src/editor_commands.cpp
        src/internal/options_dialog.h
        src/options_dialog.cpp
        src/internal/render_graph_build_settings.h
        src/internal/error_list.h
        src/error_list.cpp
        src/internal/project_explorer.h
        src/project_explorer.cpp
        src/internal/timing_panel.h
        src/timing_panel.cpp
        src/project_setup_dialog.cpp
        src/project_creation_dialog.cpp
        src/workspace_style.cpp)
    target_link_libraries(ludus_editor_tests
        PRIVATE
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
        target_compile_definitions(ludus_editor_tests PRIVATE LUDUS_EDITOR_NATIVE_PREVIEW=1 LUDUS_EDITOR_GPU_PREVIEW=1)
        target_link_libraries(ludus_editor_tests PRIVATE Ludus::GraphicsRhi Qt6::Quick)
        ludus_compile_renderer_shaders(TARGET ludus_editor_tests SHADERS
            renderer_flat renderer_composite renderer_material renderer_lit renderer_output renderer_shadow)
        ludus_compile_renderer_shaders(TARGET ludus_editor_tests SHADERS
            renderer_lit_shadow renderer_clustered renderer_clustered_shadow
            renderer_clustered_authored renderer_clustered_authored_shadow
            renderer_lit_debug renderer_lit_debug_shadow
            renderer_clustered_authored_debug renderer_clustered_authored_debug_shadow)
        ludus_editor_compile_render_graph_host(ludus_editor_tests)
        ludus_editor_enable_shader_reload(ludus_editor_tests)
        target_sources(ludus_editor_tests PRIVATE tests/preview_shader_reload_tests.cpp)
        target_sources(ludus_editor_tests PRIVATE tests/render_graph_preview_tests.cpp)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_RENDER_GRAPH_FIXTURE=\"${editor_renderer_static_RUNTIME_PACKAGE}\"")
        set(editor_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/raytracing")
        set(editor_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/render-graph-host/raytracing")
        file(COPY "${editor_ray_source}/" DESTINATION "${editor_ray_staged}" PATTERN "*.in" EXCLUDE)
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
        file(COPY "${progressive_source}/" DESTINATION "${progressive_staged}" PATTERN "*.in" EXCLUDE)
        configure_file("${progressive_source}/main.lushader.json.in" "${progressive_staged}/main.lushader.json" @ONLY)
        configure_file("${progressive_source}/main_shaders/progressive_closest.slang"
            "${progressive_staged}/main_shaders/closest.slang" COPYONLY)
        ludus_compile_render_graph(TARGET ludus_editor_tests NAME editor_progressive_ray
            MANIFEST "${progressive_staged}/main.lushader.json" RUNTIME_PACKAGE)
        target_compile_definitions(ludus_editor_tests PRIVATE
            "LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE=\"${editor_progressive_ray_RUNTIME_PACKAGE}\"")
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
    endif()
    if(APPLE)
        target_sources(ludus_editor_tests PRIVATE tests/viewport_relative_mouse_tests.mm)
        target_link_libraries(ludus_editor_tests PRIVATE "-framework AppKit" "-framework CoreGraphics")
    endif()
    # Catch2 reports assertion failures by throwing; test targets (only) re-enable
    # exceptions. Engine/app targets above stay exception-free (-fno-exceptions).
    ludus_enable_test_exceptions(ludus_editor_tests)
    # Print each completed case's duration to diagnose a stopped aggregate suite.
    add_test(NAME ludus_editor_tests COMMAND ludus_editor_tests --durations yes)
    # Run the offscreen Qt platform plugin so these logic/widget tests need no
    # display. This is for display-free tests only, never native acceptance.
    #
    # The suite's wall time follows host contention, not the change under test:
    # 221 cases (MainWindow show/close cycles, Python adapter processes running
    # CMake preset inspection, a 100,000-row content projection) need about
    # 17 s of CPU but 42 s on the Linux CI runner and, on a shared 10-core macOS
    # host, 53 s at load 33-45, 66 s at load 26-31, 156 s at load 46-48 and
    # 203 s at load 33-63 (the former 120 s TIMEOUT failed there). This TIMEOUT
    # only catches a hung case; --durations yes names the completed ones.
    set_tests_properties(ludus_editor_tests PROPERTIES
        LABELS "unit;editor"
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Transitional, empty: batch CI runs the workflow definitions on main against
    # the develop candidate, and main's ci.yml still builds this target, which
    # 547148cf removed with SceneDocument. Delete it once a promotion carries the
    # current ci.yml to main (continuous-integration.md, batched integration).
    add_custom_target(ludus_editor_scene_tests)
endif()

if(EMSCRIPTEN)
    find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS GuiPrivate)
    target_link_libraries(ludus_editor PRIVATE Qt6::GuiPrivate)
    set_property(TARGET ludus_editor APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/browser/files.js")
    set_target_properties(ludus_editor PROPERTIES QT_WASM_INITIAL_MEMORY 33554432 QT_WASM_MAXIMUM_MEMORY 2147483648)
    target_link_options(ludus_editor PRIVATE "-sASYNCIFY_STACK_SIZE=131072"
        "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/browser/files.js")
endif()

if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_executable(ludus_render_graph_pipeline_tests tests/main.cpp tests/render_graph_pipeline_tests.cpp tests/render_graph_shader_cleanup_tests.cpp
        src/internal/render_graph_pipeline.h src/render_graph_pipeline.cpp
        src/render_graph_document.cpp src/render_graph_configuration_template.cpp src/render_graph_visibility_template.cpp src/render_graph_node_transfer.cpp src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp src/render_graph_scene_authoring.cpp src/render_graph_scene_bundle.cpp src/render_graph_techniques.cpp src/internal/render_graph_pipeline_node.h src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
        src/internal/render_graph_canvas.h src/render_graph_canvas.cpp src/render_graph_resources.cpp src/render_graph_canvas_feedback.cpp
        src/internal/render_graph_configuration_editor.h src/render_graph_configuration_editor.cpp
        src/render_graph_configuration_interface.cpp src/render_graph_shader_interface.cpp)
    set_target_properties(ludus_render_graph_pipeline_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_pipeline_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_pipeline_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
    target_link_libraries(ludus_render_graph_pipeline_tests PRIVATE Ludus::RenderGraphFormat Qt6::Core Qt6::Widgets Qt6::Test
        Ludus::FoundationContainers Ludus::FoundationMath Catch2::Catch2)
    ludus_editor_add_shader_api(ludus_render_graph_pipeline_tests)
    ludus_apply_project_defaults(ludus_render_graph_pipeline_tests)
    ludus_enable_test_exceptions(ludus_render_graph_pipeline_tests)
    add_test(NAME ludus_render_graph_pipeline_tests COMMAND ludus_render_graph_pipeline_tests)
    set_tests_properties(ludus_render_graph_pipeline_tests PROPERTIES LABELS "editor;render-graph" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ludus_shader_interface_tests tests/main.cpp tests/render_graph_shader_interface_tests.cpp
        src/render_graph_shader_interface.cpp)
    target_include_directories(ludus_shader_interface_tests PRIVATE src)
    target_compile_definitions(ludus_shader_interface_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
    target_link_libraries(ludus_shader_interface_tests PRIVATE Qt6::Widgets Catch2::Catch2 Ludus::FoundationBase)
    ludus_editor_add_shader_api(ludus_shader_interface_tests)
    ludus_apply_project_defaults(ludus_shader_interface_tests)
    ludus_enable_test_exceptions(ludus_shader_interface_tests)
    add_test(NAME ludus_shader_interface_tests COMMAND ludus_shader_interface_tests)
    set_tests_properties(ludus_shader_interface_tests PROPERTIES LABELS "editor;render-graph" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Authoring uses the real workspace/document without requiring a viewport device.
    add_executable(ludus_render_graph_configuration_tests
        tests/main.cpp tests/render_graph_upscale_tests.cpp tests/render_graph_configuration_tests.cpp tests/render_graph_template_tests.cpp tests/render_graph_rename_tests.cpp tests/render_graph_selection_tests.cpp tests/render_graph_canvas_tests.cpp tests/render_graph_canvas_feedback_tests.cpp tests/render_graph_diagnostic_tests.cpp
        src/internal/renderer_workspace.h src/renderer_workspace.cpp src/renderer_workspace_selection.cpp
        src/internal/viewport_frame_pump.h src/viewport_frame_pump.cpp
        src/render_graph_document.cpp src/render_graph_configuration_template.cpp src/render_graph_visibility_template.cpp src/render_graph_node_transfer.cpp src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp src/render_graph_scene_authoring.cpp src/render_graph_scene_bundle.cpp src/render_graph_techniques.cpp src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
        src/internal/render_graph_configuration_editor.h src/render_graph_configuration_editor.cpp
        src/render_graph_configuration_interface.cpp
        src/internal/render_graph_canvas.h src/render_graph_canvas.cpp src/render_graph_resources.cpp src/render_graph_canvas_feedback.cpp
        src/internal/render_graph_pipeline.h src/render_graph_pipeline.cpp
        src/internal/render_graph_job.h src/render_graph_job.cpp
        src/internal/error_list.h src/error_list.cpp
        src/internal/render_graph_creation.h src/render_graph_creation.cpp
        src/internal/render_graph_settings_editor.h src/render_graph_settings_editor.cpp
        src/internal/render_graph_live_build.h src/render_graph_live_build.cpp
        src/internal/render_graph_source_watch.h src/render_graph_source_watch.cpp
        src/internal/viewport_frame_pump.h src/viewport_frame_pump.cpp
        src/render_graph_defaults.cpp src/render_graph_shader_interface.cpp src/external_code_editor.cpp)
    set_target_properties(ludus_render_graph_configuration_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_configuration_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_configuration_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        LUDUS_EDITOR_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
    # The workspace borrows viewport types, but this test never creates a device or
    # calls their compiled implementations. Keep those dependencies at the header boundary.
    # Thanks to Kitware, "cmake-generator-expressions(7)", COMPILE_ONLY, for the
    # compile-usage-only dependency mechanism:
    # https://cmake.org/cmake/help/v3.29/manual/cmake-generator-expressions.7.html#genex:COMPILE_ONLY
    target_link_libraries(ludus_render_graph_configuration_tests PRIVATE Ludus::RenderGraphFormat ludus_editor_core
        ludus_editor_code
        Qt6::Widgets Qt6::Test Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationProfiling
        Catch2::Catch2
        "$<COMPILE_ONLY:Ludus::GraphicsRenderer>" "$<COMPILE_ONLY:Ludus::GameplayCamera>"
        "$<COMPILE_ONLY:Ludus::Resources>")
    ludus_editor_add_default_render_graph(ludus_render_graph_configuration_tests)
    ludus_apply_project_defaults(ludus_render_graph_configuration_tests)
    ludus_enable_test_exceptions(ludus_render_graph_configuration_tests)
    add_test(NAME ludus_render_graph_configuration_tests COMMAND ludus_render_graph_configuration_tests)
    set_tests_properties(ludus_render_graph_configuration_tests PROPERTIES LABELS "editor;render-graph" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ludus_render_graph_live_build_tests tests/main.cpp tests/render_graph_live_build_tests.cpp
        src/render_graph_document.cpp src/render_graph_configuration_template.cpp src/render_graph_visibility_template.cpp src/render_graph_node_transfer.cpp
        src/internal/render_graph_shader_creation.h
        src/render_graph_shader_cleanup.cpp src/render_graph_shader_creation.cpp src/render_graph_scene_authoring.cpp src/render_graph_scene_bundle.cpp src/render_graph_techniques.cpp src/render_graph_pipeline_node.cpp src/render_graph_upscale_node.cpp
        src/render_graph_live_build.cpp src/internal/render_graph_live_build.h)
    set_target_properties(ludus_render_graph_live_build_tests PROPERTIES AUTOMOC ON)
    target_include_directories(ludus_render_graph_live_build_tests PRIVATE src)
    target_compile_definitions(ludus_render_graph_live_build_tests PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII)
    target_link_libraries(ludus_render_graph_live_build_tests PRIVATE Qt6::Core Qt6::Widgets Qt6::Test
        Ludus::FoundationContainers Catch2::Catch2)
    ludus_apply_project_defaults(ludus_render_graph_live_build_tests)
    ludus_enable_test_exceptions(ludus_render_graph_live_build_tests)
    add_test(NAME ludus_render_graph_live_build_tests COMMAND ludus_render_graph_live_build_tests)
    set_tests_properties(ludus_render_graph_live_build_tests PROPERTIES LABELS "editor;shader" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Source bindings share the package document; formatter/input code stays generic.
foreach(target IN ITEMS ludus_editor ludus_editor_tests ludus_render_graph_configuration_tests)
    if(TARGET ${target})
        target_sources(${target} PRIVATE src/internal/render_graph_code_document.h src/render_graph_code_document.cpp)
    endif()
endforeach()
foreach(target IN ITEMS ludus_editor ludus_editor_tests ludus_render_graph_configuration_tests
                        ludus_render_graph_pipeline_tests ludus_render_graph_live_build_tests)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE Ludus::FoundationStrings)
    endif()
endforeach()

# Project Settings proxies the graph document rather than storing a second resolution model.
foreach(target IN ITEMS ludus_editor ludus_editor_tests ludus_render_graph_configuration_tests)
    if(TARGET ${target})
        target_sources(${target} PRIVATE
            src/internal/render_graph_resolution_settings.h src/render_graph_resolution_settings.cpp
            src/internal/project_rendering_settings.h src/project_rendering_settings.cpp
            src/renderer_workspace_resolution.cpp)
    endif()
endforeach()

if(TARGET ludus_render_graph_configuration_tests)
    target_sources(ludus_render_graph_configuration_tests PRIVATE tests/render_graph_resolution_tests.cpp)
endif()

# Precompiled Qt Core and standard headers; see cmake/EditorPrecompiledHeaders.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EditorPrecompiledHeaders.cmake")
ludus_editor_apply_precompiled_headers(
    ludus_editor_core
    ludus_editor_code
    ludus_editor
    ludus_code_editor_tests
    ludus_editor_browser_content_tests
    ludus_editor_tests
    ludus_render_graph_pipeline_tests
    ludus_shader_interface_tests
    ludus_render_graph_configuration_tests
    ludus_render_graph_live_build_tests
)
