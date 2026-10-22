add_library(ludus_graphics_renderer STATIC src/render_graph.cpp src/render_graph_ray.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_graph.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_scene.cpp src/reference_path.cpp)
add_library(Ludus::GraphicsRenderer ALIAS ludus_graphics_renderer)
set_target_properties(ludus_graphics_renderer PROPERTIES EXPORT_NAME GraphicsRenderer)
target_sources(ludus_graphics_renderer PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/graphics/renderer/scene_shader_layout.h
    include/ludus/graphics/renderer/scene_shader.hpp
    include/ludus/graphics/renderer/render_graph.hpp
    include/ludus/graphics/renderer/render_graph_activation.hpp
    include/ludus/graphics/renderer/renderer.hpp
    include/ludus/graphics/renderer/world_grid.hpp
    include/ludus/graphics/renderer/views.hpp
    include/ludus/graphics/renderer/overlays.hpp
    include/ludus/graphics/renderer/materials.hpp
    include/ludus/graphics/renderer/environment.hpp
    include/ludus/graphics/renderer/static_lighting.hpp
    include/ludus/graphics/renderer/lighting_layout.h
    include/ludus/graphics/renderer/lighting.hpp
    include/ludus/graphics/renderer/reference.hpp
    include/ludus/graphics/renderer/gpu_preview.hpp
    include/ludus/graphics/renderer/viewport.hpp)
target_link_libraries(ludus_graphics_renderer PUBLIC Ludus::FoundationBase Ludus::FoundationMath Ludus::FoundationThreading Ludus::GraphicsRhi Ludus::RenderGraphFormat
    PRIVATE Ludus::FoundationContainers Ludus::FoundationHash Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources Ludus::SceneFormat)
target_compile_options(ludus_graphics_renderer PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_graphics_renderer)
install(TARGETS ludus_graphics_renderer EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Source is cooked by the existing installed shader helper; no runtime compiler dependency.
install(FILES "${PROJECT_SOURCE_DIR}/modules/content/render_graph_format/include/ludus/content/render_graph_format/scene_data.h" include/ludus/graphics/renderer/scene_shader_layout.h include/ludus/graphics/renderer/lighting_layout.h shaders/recipes.json shaders/renderer_gpu_preview_layout.h shaders/scene_shader.slang shaders/ray_shader.slang shaders/renderer_flat.slang shaders/renderer_composite.slang shaders/renderer_overlay.slang shaders/renderer_material.slang shaders/renderer_lit.slang shaders/renderer_clustered.slang shaders/renderer_shadow.slang shaders/renderer_output.slang shaders/renderer_gpu_preview.slang shaders/renderer_gpu_preview_image.slang shaders/renderer_surface.slang shaders/renderer_clustered_surface.slang shaders/renderer_surface_functions.slang shaders/renderer_authored.slang shaders/renderer_clustered_authored.slang shaders/renderer_lit_debug.slang shaders/renderer_clustered_authored_debug.slang shaders/renderer_environment_layout.h DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/shaders")
# Engine-owned default render graph: the RendererStatic static opaque profile the editor
# shows when a project has no graph of its own, and the read-only starting point a project
# copies when it customizes the renderer. Profiles follow the host device family, as the
# editor's cooked host fixture does. The configured package is published through a global
# property for the editor and installed read-only with the SDK.
# See docs/development/render-graph-authoring.md#start-customizing-the-renderer.
set(default_render_graph_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/default")
set(default_render_graph "${CMAKE_CURRENT_BINARY_DIR}/render_graphs/default")
set(default_render_graph_files
    main.lurender.json
    rasterize.lurender.json
    main.lushader.json
    main_shaders/color.slang
    main_shaders/depth.slang
    main_shaders/geometry.slang)
if(EMSCRIPTEN)
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"wgsl\"]")
elseif(APPLE)
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"metal\", \"wgsl\"]")
else()
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "[\"spirv\", \"wgsl\"]")
endif()
foreach(file IN LISTS default_render_graph_files)
    if(file STREQUAL "main.lushader.json")
        configure_file("${default_render_graph_source}/${file}.in" "${default_render_graph}/${file}" @ONLY)
    else()
        configure_file("${default_render_graph_source}/${file}" "${default_render_graph}/${file}" COPYONLY)
    endif()
endforeach()
set_property(GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_DIR "${default_render_graph}")
set_property(GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_FILES "${default_render_graph_files}")
install(DIRECTORY "${default_render_graph}/" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/render-graphs/default")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    # Exercise the production renderer/RHI registry with the existing deterministic backend.
    add_executable(ludus_renderer_tests tests/renderer_tests.cpp tests/world_grid_tests.cpp tests/reference_backend.cpp src/render_graph.cpp src/render_graph_ray.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_graph.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_scene.cpp src/reference_path.cpp tests/views_tests.cpp tests/static_lighting_tests.cpp tests/render_graph_tests.cpp tests/render_graph_activation_tests.cpp tests/render_graph_pool_tests.cpp tests/scene_shader_tests.cpp
        ../rhi/src/rhi.cpp ../rhi/src/device.cpp ../rhi/src/gpu_scope.cpp ../rhi/src/gpu_pass_timing_buffer.cpp ../rhi/src/profile.cpp ../rhi/src/raster.cpp ../rhi/tests/reference_raster.cpp ../rhi/src/ray_unsupported.cpp)
    target_include_directories(ludus_renderer_tests PRIVATE include ../rhi/include ../rhi/src ../rhi/tests "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    target_link_libraries(ludus_renderer_tests PRIVATE Ludus::FoundationStrings Ludus::RenderGraphFormat Ludus::Platform Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationThreading Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources Ludus::FoundationHash Catch2::Catch2WithMain)
    target_compile_options(ludus_renderer_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_renderer_tests)
    ludus_enable_test_exceptions(ludus_renderer_tests)
    add_test(NAME ludus_renderer_tests COMMAND ludus_renderer_tests)
    set_tests_properties(ludus_renderer_tests PROPERTIES LABELS "unit;renderer;build-contract")
endif()
# Uses the threading benchmark switch: static-lighting bake timing against the serial executor.
if(LUDUS_BUILD_THREADING_BENCHMARK AND NOT EMSCRIPTEN)
    add_executable(ludus_static_lighting_benchmark tests/static_lighting_benchmark.cpp)
    target_link_libraries(ludus_static_lighting_benchmark PRIVATE
        Ludus::GraphicsRenderer Ludus::FoundationContainers Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources)
    ludus_apply_project_defaults(ludus_static_lighting_benchmark)
endif()
if(TARGET ludus_rhi_render_tests)
    add_executable(ludus_renderer_render_tests tests/render_tests.cpp tests/gpu_preview_tests.cpp tests/gpu_random_tests.cpp tests/authored_tests.cpp tests/render_graph_render_tests.cpp tests/render_graph_pool_render_tests.cpp)
    target_include_directories(ludus_renderer_render_tests PRIVATE ../rhi/src "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    target_link_libraries(ludus_renderer_render_tests PRIVATE Ludus::GraphicsText Ludus::FoundationMath Ludus::FoundationTime Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_renderer_render_tests)
    ludus_enable_test_exceptions(ludus_renderer_render_tests)
    if(APPLE)
        target_compile_definitions(ludus_renderer_render_tests PRIVATE LUDUS_TEST_METAL=1)
    endif()
    # Test-only kernel exercising FoundationMath's shader addressed-random port;
    # shared with the WebGPU browser fixture in tests/sdk_consumer.
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME random_parity
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/random_parity.slang" COMPUTE ComputeMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders")
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_gpu_preview)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_gpu_preview_image)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_flat)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_composite)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_overlay)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_material)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_clustered)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_clustered_shadow)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_lit)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_lit_shadow)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_shadow)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_output)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS
        renderer_surface renderer_surface_shadow renderer_clustered_surface renderer_clustered_surface_shadow)
    ludus_cook_texture(TARGET ludus_renderer_render_tests NAME l4_bake SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/l4-bake.texture.json")
    ludus_cook_texture(TARGET ludus_renderer_render_tests NAME l3_color
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/l3-color.texture.json")
    add_test(NAME ludus_renderer_pixels COMMAND ludus_renderer_render_tests)
    set_tests_properties(ludus_renderer_pixels PROPERTIES LABELS "renderer;gpu" TIMEOUT 60)
    if(APPLE)
        set_property(TEST ludus_renderer_pixels APPEND PROPERTY LABELS macos)
    endif()
endif()

if(TARGET ludus_renderer_render_tests AND LUDUS_SPIRV_CROSS)
    target_compile_definitions(ludus_renderer_render_tests PRIVATE LUDUS_TEST_RAY=1)
    ludus_compile_ray_queries(TARGET ludus_renderer_render_tests NAME ray_query)
endif()

if(TARGET ludus_renderer_render_tests)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_authored renderer_authored_shadow renderer_clustered_authored renderer_clustered_authored_shadow)
endif()

if(TARGET ludus_renderer_render_tests)
    set(RG2_TEST_PROFILES "[\"spirv\", \"wgsl\"]")
    if(APPLE)
        set(RG2_TEST_PROFILES "[\"metal\", \"wgsl\"]")
    endif()
    # bound: per-draw uniform values; masked_color/masked_depth: authored masked coverage parity.
    foreach(kind surface offscreen depth bound masked_color masked_depth independent)
        set(input "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/${kind}")
        set(staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${kind}")
        file(COPY "${input}/" DESTINATION "${staged}" PATTERN "*.in" EXCLUDE)
        configure_file("${input}/main.lushader.json.in" "${staged}/main.lushader.json" @ONLY)
        ludus_compile_render_graph(TARGET ludus_renderer_render_tests NAME rg2_${kind}
            MANIFEST "${staged}/main.lushader.json" RUNTIME_PACKAGE)
    endforeach()
    cmake_path(GET rg2_independent_RUNTIME_PACKAGE PARENT_PATH rg3_independent_directory)
    target_compile_definitions(ludus_renderer_render_tests PRIVATE
        LUDUS_RG3_INDEPENDENT_DIRECTORY="${rg3_independent_directory}")
    # RG3 loads the same cooked surface generation at runtime and compares pixels with its header.
    cmake_path(GET rg2_surface_RUNTIME_PACKAGE PARENT_PATH rg3_surface_directory)
    cmake_path(GET rg2_bound_RUNTIME_PACKAGE PARENT_PATH rg3_bound_directory)
    # RG4 rewrites the offscreen generation into its scratch-transient pooling workload.
    cmake_path(GET rg2_offscreen_RUNTIME_PACKAGE PARENT_PATH rg3_offscreen_directory)
    target_compile_definitions(ludus_renderer_render_tests PRIVATE
        LUDUS_RG3_SURFACE_DIRECTORY="${rg3_surface_directory}" LUDUS_RG3_BOUND_DIRECTORY="${rg3_bound_directory}"
        LUDUS_RG3_OFFSCREEN_DIRECTORY="${rg3_offscreen_directory}")
    target_link_libraries(ludus_renderer_render_tests PRIVATE Ludus::FoundationFilesystem Ludus::ShaderDevelopment)
    if(TARGET ludus-shader-worker)
        add_test(NAME ludus_shader_reload_gpu COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/shader-worker/test_worker.py"
            --worker "$<TARGET_FILE:ludus-shader-worker>" --client "$<TARGET_FILE:ludus_shader_compiler_tests>"
            --renderer "$<TARGET_FILE:ludus_renderer_render_tests>")
        set_tests_properties(ludus_shader_reload_gpu PROPERTIES LABELS "shader;renderer;gpu" TIMEOUT 120)
    endif()
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME rg2_depth_display RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/depth_display.slang" VERTEX VSMain FRAGMENT PSMain)
endif()

if(TARGET ludus_renderer_render_tests)
    add_executable(ludus_render_graph_ray_tests tests/render_graph_ray_tests.cpp)
    target_link_libraries(ludus_render_graph_ray_tests PRIVATE Ludus::GraphicsRenderer
        Ludus::FoundationFilesystem Ludus::FoundationTime Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_render_graph_ray_tests)
    ludus_enable_test_exceptions(ludus_render_graph_ray_tests)
    ludus_compile_renderer_shaders(TARGET ludus_render_graph_ray_tests SHADERS renderer_composite)
    set(ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/raytracing")
    set(ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/raytracing")
    file(COPY "${ray_source}/" DESTINATION "${ray_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${ray_source}/main.lushader.json.in" "${ray_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_raytracing
        MANIFEST "${ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_raytracing_RUNTIME_PACKAGE PARENT_PATH ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_RAYTRACING_DIRECTORY="${ray_package_directory}")
    set(scene_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/scene_ray")
    set(scene_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/scene_ray")
    file(COPY "${scene_ray_source}/" DESTINATION "${scene_ray_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${scene_ray_source}/main.lushader.json.in" "${scene_ray_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_ray
        MANIFEST "${scene_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_ray_RUNTIME_PACKAGE PARENT_PATH scene_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_SCENE_RAY_DIRECTORY="${scene_ray_package_directory}")
    set(ordered_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/ordered_ray")
    set(ordered_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/ordered_ray")
    file(COPY "${ordered_ray_source}/" DESTINATION "${ordered_ray_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${ordered_ray_source}/main.lushader.json.in" "${ordered_ray_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_ordered_ray
        MANIFEST "${ordered_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_ordered_ray_RUNTIME_PACKAGE PARENT_PATH ordered_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_ORDERED_RAY_DIRECTORY="${ordered_ray_package_directory}")
    add_test(NAME ludus_render_graph_ray_pixels COMMAND ludus_render_graph_ray_tests)
    set_tests_properties(ludus_render_graph_ray_pixels PROPERTIES LABELS "renderer;gpu;ray" TIMEOUT 60)
endif()
