# Private raw-accumulation access; no public SDK API and omitted when tests are off.
if(LUDUS_BUILD_TESTS)
    set_source_files_properties(src/renderer.cpp src/gpu_preview_ray.cpp PROPERTIES COMPILE_DEFINITIONS LUDUS_RENDERER_TEST_READBACK=1)
endif()
add_library(ludus_graphics_renderer STATIC src/gpu_denoiser.cpp src/render_graph.cpp src/render_graph_forward.cpp src/render_graph_scene_resources.cpp src/visibility_buffer.cpp src/render_graph_ray.cpp src/render_graph_ray_path.cpp src/render_graph_upscale.cpp src/render_graph_metalfx.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_camera.cpp src/gpu_preview_graph.cpp src/render_graph_storage.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_scene.cpp src/reference_path.cpp src/reference_compute.cpp)
add_library(Ludus::GraphicsRenderer ALIAS ludus_graphics_renderer)
include(EngineDenoiserOptions)
if(LUDUS_ENABLE_OIDN)
    target_compile_definitions(ludus_graphics_renderer PRIVATE LUDUS_ENABLE_OIDN=1)
    target_link_libraries(ludus_graphics_renderer PRIVATE OpenImageDenoise)
    # The SDK carries the OIDN runtime beside its own CMake package so consumers and
    # packaged games link and load the same verified release; see LudusConfig.cmake.in.
    cmake_path(GET OpenImageDenoise_DIR PARENT_PATH ludus_oidn_cmake_directory)
    cmake_path(GET ludus_oidn_cmake_directory PARENT_PATH ludus_oidn_library_directory)
    cmake_path(GET ludus_oidn_library_directory PARENT_PATH ludus_oidn_prefix)
    set(LUDUS_OIDN_RUNTIME_LIBRARIES "${LUDUS_OIDN_RUNTIME_LIBRARIES}" PARENT_SCOPE)
    # Install-name link chains are preserved; the package's IMPORTED_LOCATION is the versioned file.
    install(DIRECTORY "${ludus_oidn_library_directory}/" DESTINATION lib USE_SOURCE_PERMISSIONS
        FILES_MATCHING PATTERN "libOpenImageDenoise*.dylib" PATTERN "libtbb*.dylib" PATTERN "cmake" EXCLUDE)
    install(DIRECTORY "${OpenImageDenoise_DIR}/" DESTINATION lib/cmake/OpenImageDenoise-2.5.1)
    list(TRANSFORM LUDUS_OIDN_LICENSE_FILES PREPEND "${ludus_oidn_prefix}/doc/" OUTPUT_VARIABLE ludus_oidn_licenses)
    install(FILES ${ludus_oidn_licenses} DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/licenses/OpenImageDenoise")
    ludus_sdk_register_dependency(OpenImageDenoise "2.5.1" "Apache-2.0" bundled)
endif()
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
    include/ludus/graphics/renderer/light_selection.hpp
    include/ludus/graphics/renderer/lighting.hpp
    include/ludus/graphics/renderer/reference.hpp
    include/ludus/graphics/renderer/gpu_preview.hpp
    include/ludus/graphics/renderer/viewport_frame.hpp
    include/ludus/graphics/renderer/viewport.hpp)
target_link_libraries(ludus_graphics_renderer PUBLIC Ludus::FoundationBase Ludus::FoundationMath Ludus::FoundationThreading Ludus::GraphicsRhi Ludus::RenderGraphFormat
    PRIVATE Ludus::FoundationContainers Ludus::FoundationHash Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources Ludus::SceneFormat)
target_compile_options(ludus_graphics_renderer PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_graphics_renderer)
install(TARGETS ludus_graphics_renderer EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Source is cooked by the existing installed shader helper; no runtime compiler dependency.
install(FILES "${PROJECT_SOURCE_DIR}/modules/content/render_graph_format/include/ludus/content/render_graph_format/scene_data.h" include/ludus/graphics/renderer/scene_shader_layout.h include/ludus/graphics/renderer/lighting_layout.h shaders/recipes.json shaders/renderer_gpu_preview_layout.h shaders/scene_shader.slang shaders/scene_payload.slang shaders/visibility_buffer.slang shaders/ray_shader.slang shaders/ray_triangle_loop.slang shaders/renderer_flat.slang shaders/renderer_composite.slang shaders/renderer_overlay.slang shaders/renderer_material.slang shaders/renderer_lit.slang shaders/renderer_clustered.slang shaders/renderer_shadow.slang shaders/renderer_output.slang shaders/renderer_gpu_preview.slang shaders/renderer_gpu_preview_visibility_full.slang shaders/renderer_gpu_preview_image.slang shaders/renderer_reference.slang shaders/renderer_reference_layout.h shaders/renderer_surface.slang shaders/renderer_clustered_surface.slang shaders/renderer_surface_functions.slang shaders/renderer_authored.slang shaders/renderer_clustered_authored.slang shaders/renderer_lit_debug.slang shaders/renderer_clustered_authored_debug.slang shaders/renderer_environment_layout.h DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/shaders")
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
    add_executable(ludus_renderer_tests tests/renderer_tests.cpp tests/world_grid_tests.cpp tests/reference_backend.cpp src/gpu_denoiser.cpp src/render_graph.cpp src/render_graph_forward.cpp src/render_graph_scene_resources.cpp src/visibility_buffer.cpp src/render_graph_ray.cpp src/render_graph_ray_path.cpp src/render_graph_upscale.cpp src/render_graph_metalfx.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_camera.cpp src/gpu_preview_graph.cpp src/render_graph_storage.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_scene.cpp src/reference_path.cpp src/reference_compute.cpp tests/views_tests.cpp tests/static_lighting_tests.cpp tests/render_graph_tests.cpp tests/render_graph_ray_path_decode_tests.cpp tests/render_graph_activation_tests.cpp tests/render_graph_pool_tests.cpp tests/scene_shader_tests.cpp tests/reference_bvh_tests.cpp
        ../rhi/src/rhi.cpp ../rhi/src/device.cpp ../rhi/src/dxil_container.cpp ../rhi/src/gpu_scope.cpp ../rhi/src/gpu_pass_timing_buffer.cpp ../rhi/src/profile.cpp ../rhi/src/raster.cpp ../rhi/tests/reference_raster.cpp ../rhi/src/ray_unsupported.cpp)
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
    add_executable(ludus_renderer_render_tests tests/render_tests.cpp tests/denoising_tests.cpp tests/gpu_preview_tests.cpp tests/ray_opacity_tests.cpp tests/visibility_tests.cpp tests/reference_compute_tests.cpp tests/gpu_random_tests.cpp tests/restir_selection_tests.cpp tests/authored_tests.cpp tests/render_graph_render_tests.cpp tests/render_graph_pool_render_tests.cpp)
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
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME restir_selection
        SOURCE "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/tests/restir_selection.slang" COMPUTE ComputeMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
                 "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/restir_di/main_shaders")
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_gpu_preview renderer_gpu_preview_visibility_full)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_gpu_preview_image)
    # Manual timing tool for reference captures on a cooked scene; not a CTest case.
    add_executable(ludus_reference_benchmark tests/reference_benchmark.cpp)
    target_link_libraries(ludus_reference_benchmark PRIVATE
        Ludus::GraphicsRenderer Ludus::Resources Ludus::Content Ludus::FoundationContainers Ludus::FoundationFilesystem
        Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_reference_benchmark)
    ludus_compile_renderer_shaders(TARGET ludus_reference_benchmark SHADERS
        renderer_flat renderer_composite renderer_lit renderer_lit_shadow renderer_shadow renderer_output
        renderer_reference renderer_reference_reduce)
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_reference renderer_reference_reduce)
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
    set(forward_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/forward_lit")
    set(forward_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/forward_lit")
    foreach(file main.lurender.json rasterize.lurender.json main_shaders/geometry.slang main_shaders/color.slang)
        configure_file("${forward_source}/${file}" "${forward_staged}/${file}" COPYONLY)
    endforeach()
    set(LUDUS_DEFAULT_RENDER_GRAPH_PROFILES "${RG2_TEST_PROFILES}")
    configure_file("${forward_source}/main.lushader.json.in" "${forward_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_renderer_render_tests NAME rg2_forward_lit
        MANIFEST "${forward_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_forward_lit_RUNTIME_PACKAGE PARENT_PATH forward_package_directory)
    target_compile_definitions(ludus_renderer_render_tests PRIVATE
        LUDUS_RG3_FORWARD_DIRECTORY="${forward_package_directory}")
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
    set(visibility_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/visibility")
    set(visibility_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/visibility")
    file(COPY "${visibility_source}/" DESTINATION "${visibility_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${visibility_source}/main.lushader.json.in" "${visibility_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_visibility
        MANIFEST "${visibility_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_visibility_RUNTIME_PACKAGE PARENT_PATH visibility_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_VISIBILITY_DIRECTORY="${visibility_package_directory}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_visibility_ordinary
        MANIFEST "${visibility_staged}/main.lushader.json" CONFIGURATION Ordinary RUNTIME_PACKAGE)
    cmake_path(GET rg2_visibility_ordinary_RUNTIME_PACKAGE PARENT_PATH visibility_ordinary_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_VISIBILITY_ORDINARY_DIRECTORY="${visibility_ordinary_package_directory}")
    foreach(template_directory IN ITEMS visibility_buffer visibility_secondary_rays tiled_vbuffer_nee compute_ray_tracing simple_compute_ray_tracing veach_guibas_mis)
        set(template_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/${template_directory}")
        set(template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${template_directory}_template")
        file(COPY "${template_source}/" DESTINATION "${template_staged}" PATTERN "*.in" EXCLUDE)
        configure_file("${template_source}/main.lushader.json.in" "${template_staged}/main.lushader.json" @ONLY)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_${template_directory}_template"
            MANIFEST "${template_staged}/main.lushader.json" RUNTIME_PACKAGE)
        set(template_package "rg2_${template_directory}_template_RUNTIME_PACKAGE")
        cmake_path(GET ${template_package} PARENT_PATH template_package_directory)
        string(TOUPPER "${template_directory}" template_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_${template_definition}_TEMPLATE_DIRECTORY=\"${template_package_directory}\"")
    endforeach()
    set(shared_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/shared_ray")
    set(shared_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/shared_ray")
    file(COPY "${shared_ray_source}/" DESTINATION "${shared_ray_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${shared_ray_source}/main.lushader.json.in" "${shared_ray_staged}/main.lushader.json" @ONLY)
    foreach(configuration IN ITEMS Default Native)
        string(TOLOWER "${configuration}" configuration_suffix)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_shared_ray_${configuration_suffix}
            MANIFEST "${shared_ray_staged}/main.lushader.json" CONFIGURATION "${configuration}" RUNTIME_PACKAGE)
        cmake_path(GET rg2_shared_ray_${configuration_suffix}_RUNTIME_PACKAGE PARENT_PATH shared_ray_package_directory)
        string(TOUPPER "${configuration}" configuration_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_SHARED_RAY_${configuration_definition}_DIRECTORY=\"${shared_ray_package_directory}\"")
    endforeach()
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
    set(compute_split_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/compute_split")
    set(compute_split_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/compute_split")
    file(COPY "${compute_split_source}/" DESTINATION "${compute_split_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${compute_split_source}/main.lushader.json.in" "${compute_split_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_compute_split
        MANIFEST "${compute_split_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_compute_split_RUNTIME_PACKAGE PARENT_PATH compute_split_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_COMPUTE_SPLIT_DIRECTORY="${compute_split_package_directory}")
    # Scene compute passes (compute ray-tracing C1/C2): a single compute pass, its raster
    # configuration for renderer selection, and a mixed compute + ray sequence.
    set(scene_compute_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/scene_compute")
    set(scene_compute_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/scene_compute")
    file(COPY "${scene_compute_source}/" DESTINATION "${scene_compute_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${scene_compute_source}/main.lushader.json.in" "${scene_compute_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_compute
        MANIFEST "${scene_compute_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_compute_RUNTIME_PACKAGE PARENT_PATH scene_compute_package_directory)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_compute_raster
        MANIFEST "${scene_compute_staged}/main.lushader.json" CONFIGURATION Raster RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_compute_raster_RUNTIME_PACKAGE PARENT_PATH scene_compute_raster_package_directory)
    set(compute_mixed_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/compute_mixed")
    set(compute_mixed_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/compute_mixed")
    file(COPY "${compute_mixed_source}/" DESTINATION "${compute_mixed_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${compute_mixed_source}/main.lushader.json.in" "${compute_mixed_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_compute_mixed
        MANIFEST "${compute_mixed_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_compute_mixed_RUNTIME_PACKAGE PARENT_PATH compute_mixed_package_directory)
    # Single-pass RayTracingPipeline reference for the compute-ray-tracing template (C4).
    set(direct_lighting_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/direct_lighting_reference")
    set(direct_lighting_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/direct_lighting_reference")
    file(COPY "${direct_lighting_source}/" DESTINATION "${direct_lighting_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${direct_lighting_source}/main.lushader.json.in"
        "${direct_lighting_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_direct_lighting_reference
        MANIFEST "${direct_lighting_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_direct_lighting_reference_RUNTIME_PACKAGE PARENT_PATH direct_lighting_package_directory)
    # Single-pass RayTracingPipeline references for the Veach and Guibas MIS template, one per shade
    # pass: the MIS estimator and each strategy alone (REFERENCE_STRATEGY 0, 1 and 2).
    set(veach_guibas_reference_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/veach_guibas_reference")
    foreach(veach_guibas_variant IN ITEMS "mis;0" "light_sampling;1" "bsdf_sampling;2")
        list(GET veach_guibas_variant 0 veach_guibas_name)
        list(GET veach_guibas_variant 1 LUDUS_VEACH_GUIBAS_REFERENCE_STRATEGY)
        set(veach_guibas_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/veach_guibas_reference_${veach_guibas_name}")
        file(COPY "${veach_guibas_reference_source}/" DESTINATION "${veach_guibas_staged}" PATTERN "*.in" EXCLUDE)
        configure_file("${veach_guibas_reference_source}/main.lushader.json.in"
            "${veach_guibas_staged}/main.lushader.json" @ONLY)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests
            NAME "rg2_veach_guibas_reference_${veach_guibas_name}"
            MANIFEST "${veach_guibas_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_veach_guibas_reference_${veach_guibas_name}_RUNTIME_PACKAGE" PARENT_PATH
            veach_guibas_package_directory)
        string(TOUPPER "${veach_guibas_name}" veach_guibas_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_VEACH_GUIBAS_REFERENCE_${veach_guibas_definition}_DIRECTORY=\"${veach_guibas_package_directory}\"")
    endforeach()
    # The Veach and Guibas template once per other MIS_HEURISTIC value (the staged default is power):
    # the same sources with only that Graph setting's value replaced.
    set(veach_guibas_template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/veach_guibas_mis_template")
    file(READ "${veach_guibas_template_staged}/main.lushader.json" veach_guibas_manifest)
    string(JSON veach_guibas_setting_count LENGTH "${veach_guibas_manifest}" settings)
    set(mis_heuristic_index -1)
    math(EXPR veach_guibas_setting_last "${veach_guibas_setting_count} - 1")
    foreach(setting_index RANGE ${veach_guibas_setting_last})
        string(JSON setting_id GET "${veach_guibas_manifest}" settings ${setting_index} id)
        if(setting_id STREQUAL "MIS_HEURISTIC")
            set(mis_heuristic_index ${setting_index})
        endif()
    endforeach()
    if(mis_heuristic_index LESS 0)
        message(FATAL_ERROR "The Veach and Guibas MIS template manifest has no MIS_HEURISTIC setting.")
    endif()
    foreach(mis_variant IN ITEMS "balance;0" "cutoff;2" "maximum;3")
        list(GET mis_variant 0 mis_name)
        list(GET mis_variant 1 mis_value)
        set(mis_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/veach_guibas_mis_${mis_name}")
        file(COPY "${veach_guibas_template_staged}/" DESTINATION "${mis_staged}" PATTERN "main.lushader.json" EXCLUDE)
        string(JSON mis_manifest SET "${veach_guibas_manifest}" settings ${mis_heuristic_index} value ${mis_value})
        # Rewrite the staged manifest only when it changes, so reconfiguring does not rebuild the package.
        file(WRITE "${mis_staged}.lushader.json" "${mis_manifest}\n")
        file(COPY_FILE "${mis_staged}.lushader.json" "${mis_staged}/main.lushader.json" ONLY_IF_DIFFERENT)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_veach_guibas_mis_${mis_name}"
            MANIFEST "${mis_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_veach_guibas_mis_${mis_name}_RUNTIME_PACKAGE" PARENT_PATH mis_package_directory)
        string(TOUPPER "${mis_name}" mis_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_VEACH_GUIBAS_MIS_${mis_definition}_DIRECTORY=\"${mis_package_directory}\"")
    endforeach()
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_SCENE_COMPUTE_DIRECTORY="${scene_compute_package_directory}"
        LUDUS_RG3_SCENE_COMPUTE_RASTER_DIRECTORY="${scene_compute_raster_package_directory}"
        LUDUS_RG3_COMPUTE_MIXED_DIRECTORY="${compute_mixed_package_directory}"
        LUDUS_RG3_DIRECT_LIGHTING_REFERENCE_DIRECTORY="${direct_lighting_package_directory}")
    set(visibility_sequence_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/visibility_sequence")
    set(visibility_sequence_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/visibility_sequence")
    file(COPY "${visibility_sequence_source}/" DESTINATION "${visibility_sequence_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${visibility_sequence_source}/main.lushader.json.in"
        "${visibility_sequence_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_visibility_sequence
        MANIFEST "${visibility_sequence_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_visibility_sequence_RUNTIME_PACKAGE PARENT_PATH visibility_sequence_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_VISIBILITY_SEQUENCE_DIRECTORY="${visibility_sequence_package_directory}")
    set(restir_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/restir_di")
    set(restir_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/restir_di")
    file(COPY "${restir_source}/" DESTINATION "${restir_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${restir_source}/main.lushader.json.in" "${restir_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_restir_di
        MANIFEST "${restir_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_restir_di_RUNTIME_PACKAGE PARENT_PATH restir_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_RESTIR_DIRECTORY="${restir_package_directory}")
    set(storage_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/storage_ray")
    set(storage_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/storage_ray")
    file(COPY "${scene_ray_source}/" DESTINATION "${storage_ray_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${storage_ray_source}/main.lurender.json" "${storage_ray_staged}/main.lurender.json" COPYONLY)
    configure_file("${storage_ray_source}/reservoir.slang" "${storage_ray_staged}/main_shaders/reservoir.slang" COPYONLY)
    configure_file("${storage_ray_source}/main.lushader.json.in" "${storage_ray_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_storage_ray
        MANIFEST "${storage_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_storage_ray_RUNTIME_PACKAGE PARENT_PATH storage_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_STORAGE_RAY_DIRECTORY="${storage_ray_package_directory}")
    add_test(NAME ludus_render_graph_ray_pixels COMMAND ludus_render_graph_ray_tests)
    set_tests_properties(ludus_render_graph_ray_pixels PROPERTIES LABELS "renderer;gpu;ray" TIMEOUT 60)
    # Ray path capture: each node form's diagnostic LUDUS_RAY_PATH_CAPTURE variant is compiled
    # from the same staged snapshot as its plain header, which the bit-identity checks compare.
    add_executable(ludus_render_graph_ray_path_tests tests/render_graph_ray_path_tests.cpp)
    target_link_libraries(ludus_render_graph_ray_path_tests PRIVATE Ludus::GraphicsRenderer
        Ludus::FoundationTime Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_render_graph_ray_path_tests)
    ludus_enable_test_exceptions(ludus_render_graph_ray_path_tests)
    ludus_compile_renderer_shaders(TARGET ludus_render_graph_ray_path_tests SHADERS renderer_composite)
    set(ray_path_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/ray_path")
    set(ray_path_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/ray_path")
    file(COPY "${ray_path_source}/" DESTINATION "${ray_path_staged}" PATTERN "*.in" EXCLUDE)
    configure_file("${ray_path_source}/main.lushader.json.in" "${ray_path_staged}/main.lushader.json" @ONLY)
    set(raytracing_staged "${ray_staged}")
    foreach(ray_path_form IN ITEMS raytracing scene_ray visibility ordered_ray ray_path)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests NAME rg2_${ray_path_form}_plain
            MANIFEST "${${ray_path_form}_staged}/main.lushader.json")
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests NAME rg2_${ray_path_form}_capture
            MANIFEST "${${ray_path_form}_staged}/main.lushader.json" RAY_PATH_CAPTURE)
    endforeach()
    add_test(NAME ludus_render_graph_ray_path_capture COMMAND ludus_render_graph_ray_path_tests)
    set_tests_properties(ludus_render_graph_ray_path_capture PROPERTIES LABELS "renderer;gpu;ray" TIMEOUT 60)
endif()
