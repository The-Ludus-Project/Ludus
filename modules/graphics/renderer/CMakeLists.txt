# Private raw-accumulation access; no public SDK API and omitted when tests are off.
if(LUDUS_BUILD_TESTS)
    set_source_files_properties(src/renderer.cpp src/gpu_preview_ray.cpp src/render_graph.cpp PROPERTIES COMPILE_DEFINITIONS LUDUS_RENDERER_TEST_READBACK=1)
endif()
# The LTC table (docs/development/ltc-table.md) is fitted offline by tools/ltc-fit into data/ltc_table.bin with its
# layout in shaders/ltc_table_layout.h. The build embeds the binary as C++ for the CPU lookup and the GPU upload.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(ludus_ltc_table_source "${CMAKE_CURRENT_BINARY_DIR}/generated/ltc_table_data.cpp")
add_custom_command(
    OUTPUT "${ludus_ltc_table_source}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/shaders/generate_ltc_table.py"
            --table "${CMAKE_CURRENT_SOURCE_DIR}/data/ltc_table.bin"
            --layout "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h"
            --output "${ludus_ltc_table_source}"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/shaders/generate_ltc_table.py" "${CMAKE_CURRENT_SOURCE_DIR}/data/ltc_table.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h"
    COMMENT "Embedding the LTC table"
    VERBATIM)
set_source_files_properties("${ludus_ltc_table_source}" PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(ludus_graphics_renderer STATIC src/gpu_denoiser.cpp src/render_graph.cpp src/render_graph_forward.cpp src/render_graph_deferred.cpp src/raster_mesh_lights.cpp src/raster_mesh_light_polygons.cpp src/raster_mesh_light_grid.cpp src/render_graph_scene_resources.cpp src/visibility_buffer.cpp src/render_graph_ray.cpp src/render_graph_ray_path.cpp src/render_graph_capture.cpp src/render_graph_failures.cpp src/render_graph_upscale.cpp src/render_graph_metalfx.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_camera.cpp src/gpu_preview_graph.cpp src/render_graph_storage.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_coating.cpp src/ltc_table.cpp "${ludus_ltc_table_source}" src/reference_scene.cpp src/reference_path.cpp src/reference_light_sampling.cpp src/reference_compute.cpp)
add_library(Ludus::GraphicsRenderer ALIAS ludus_graphics_renderer)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/pbrt/LICENSE.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/licenses" RENAME pbrt-LICENSE)
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
    include/ludus/graphics/renderer/render_graph_capture.hpp
    include/ludus/graphics/renderer/renderer.hpp
    include/ludus/graphics/renderer/world_grid.hpp
    include/ludus/graphics/renderer/views.hpp
    include/ludus/graphics/renderer/overlays.hpp
    include/ludus/graphics/renderer/materials.hpp
    include/ludus/graphics/renderer/environment.hpp
    include/ludus/graphics/renderer/static_lighting.hpp
    include/ludus/graphics/renderer/deferred_layout.h
    include/ludus/graphics/renderer/lighting_layout.h
    include/ludus/graphics/renderer/light_selection.hpp
    include/ludus/graphics/renderer/lighting.hpp
    include/ludus/graphics/renderer/tone_mapping.hpp
    include/ludus/graphics/renderer/reference.hpp
    include/ludus/graphics/renderer/gpu_preview.hpp
    include/ludus/graphics/renderer/viewport_frame.hpp
    include/ludus/graphics/renderer/viewport.hpp)
target_link_libraries(ludus_graphics_renderer PUBLIC Ludus::FoundationBase Ludus::FoundationMath Ludus::FoundationThreading Ludus::GraphicsRhi Ludus::RenderGraphFormat Ludus::SceneFormat
    PRIVATE Ludus::FoundationContainers Ludus::FoundationHash Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources)
target_compile_options(ludus_graphics_renderer PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_graphics_renderer)
if(EMSCRIPTEN)
    # Activation runs beneath the host's frame on the browser's default 64 KiB stack; catch
    # tens-of-KiB RenderGraphPrograms temporaries during compilation, as Resources does.
    set_source_files_properties(src/render_graph_activation.cpp PROPERTIES COMPILE_OPTIONS -Wframe-larger-than=16384)
endif()
install(TARGETS ludus_graphics_renderer EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Source is cooked by the existing installed shader helper; no runtime compiler dependency.
install(FILES include/ludus/graphics/renderer/detail/deferred_binding_slots.inc
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ludus/graphics/renderer/detail")
install(FILES "${PROJECT_SOURCE_DIR}/modules/content/scene_format/include/ludus/content/scene_format/material.h" "${PROJECT_SOURCE_DIR}/modules/content/render_graph_format/include/ludus/content/render_graph_format/scene_data.h" include/ludus/graphics/renderer/scene_shader_layout.h include/ludus/graphics/renderer/lighting_layout.h include/ludus/graphics/renderer/deferred_layout.h include/ludus/graphics/renderer/detail/deferred_binding_slots.inc shaders/renderer_deferred_fullscreen.slang shaders/renderer_deferred_material.slang shaders/renderer_deferred_lighting.slang shaders/renderer_mesh_lights.slang shaders/renderer_deferred_depth.slang shaders/recipes.json shaders/renderer_gpu_preview_layout.h shaders/scene_shader.slang shaders/scene_payload.slang shaders/visibility_buffer.slang shaders/ray_shader.slang shaders/ray_triangle_loop.slang shaders/ray_line_visibility.slang shaders/ray_bvh_nodes.slang shaders/ray_bvh_traversal.slang shaders/renderer_flat.slang shaders/renderer_composite.slang shaders/renderer_overlay.slang shaders/renderer_grid.slang shaders/renderer_material.slang shaders/renderer_lit.slang shaders/renderer_clustered.slang shaders/renderer_shadow.slang shaders/renderer_output.slang shaders/display_tone_mapping.slang shaders/renderer_gpu_preview.slang shaders/renderer_gpu_preview_visibility_full.slang shaders/renderer_gpu_preview_hit_cached.slang shaders/renderer_gpu_preview_group32.slang shaders/renderer_gpu_preview_group64.slang shaders/renderer_gpu_preview_group128.slang shaders/renderer_gpu_preview_group8x8.slang shaders/renderer_gpu_preview_image.slang shaders/renderer_reference.slang shaders/renderer_reference_layout.h shaders/reference_light_sampling.slang shaders/renderer_surface.slang shaders/renderer_clustered_surface.slang shaders/renderer_surface_functions.slang shaders/material_scattering.slang shaders/coated_diffuse.slang shaders/coated_diffuse_layout.h shaders/renderer_authored.slang shaders/renderer_clustered_authored.slang shaders/renderer_lit_debug.slang shaders/renderer_clustered_authored_debug.slang shaders/renderer_environment_layout.h shaders/ltc_table.slang shaders/ltc_table_layout.h DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/shaders")
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
    main_shaders/lighting.slang
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
    add_executable(ludus_renderer_tests tests/renderer_tests.cpp tests/world_grid_tests.cpp tests/tone_mapping_tests.cpp tests/reference_backend.cpp src/gpu_denoiser.cpp src/render_graph.cpp src/render_graph_forward.cpp src/render_graph_deferred.cpp src/raster_mesh_lights.cpp src/raster_mesh_light_polygons.cpp src/raster_mesh_light_grid.cpp src/render_graph_scene_resources.cpp src/visibility_buffer.cpp src/render_graph_ray.cpp src/render_graph_ray_path.cpp src/render_graph_capture.cpp src/render_graph_failures.cpp src/render_graph_upscale.cpp src/render_graph_metalfx.cpp src/render_graph_ray_scene.cpp src/gpu_preview_ray.cpp src/gpu_preview_camera.cpp src/gpu_preview_graph.cpp src/render_graph_storage.cpp src/scene_shader.cpp src/render_graph_activation.cpp src/render_graph_pool.cpp src/renderer.cpp src/world_grid.cpp src/uniform_set_table.cpp src/viewport.cpp src/views.cpp src/overlays.cpp src/lighting.cpp src/static_lighting.cpp src/environment.cpp src/reference_bsdf.cpp src/reference_coating.cpp src/ltc_table.cpp "${ludus_ltc_table_source}" src/reference_scene.cpp src/reference_path.cpp src/reference_light_sampling.cpp src/reference_compute.cpp tests/views_tests.cpp tests/static_lighting_tests.cpp tests/render_graph_tests.cpp tests/render_graph_ray_path_decode_tests.cpp tests/render_graph_activation_tests.cpp tests/render_graph_pool_tests.cpp tests/scene_shader_tests.cpp tests/reference_emitter_selection_tests.cpp tests/ltc_table_tests.cpp tests/reference_light_sampling_tests.cpp tests/raster_mesh_lights_tests.cpp tests/raster_mesh_light_grid_tests.cpp tests/reference_bvh_tests.cpp tests/polygonal_light_sampling_oracle.cpp tests/polygonal_light_sampling_oracle_tests.cpp
        ../rhi/src/rhi.cpp ../rhi/src/device.cpp ../rhi/src/dxil_container.cpp ../rhi/src/gpu_scope.cpp ../rhi/src/gpu_pass_timing_buffer.cpp ../rhi/src/profile.cpp ../rhi/src/raster.cpp ../rhi/tests/reference_raster.cpp ../rhi/src/ray_unsupported.cpp)
    target_include_directories(ludus_renderer_tests PRIVATE include ../rhi/include ../rhi/src ../rhi/tests "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    target_link_libraries(ludus_renderer_tests PRIVATE Ludus::FoundationStrings Ludus::RenderGraphFormat Ludus::Platform Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationThreading Ludus::FoundationThreadingProfiling Ludus::FoundationTime Ludus::Resources Ludus::FoundationHash Catch2::Catch2WithMain)
    target_compile_options(ludus_renderer_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_renderer_tests)
    ludus_enable_test_exceptions(ludus_renderer_tests)
    add_executable(ludus_reference_material_tests tests/reference_material_tests.cpp)
    target_link_libraries(ludus_reference_material_tests PRIVATE Ludus::GraphicsRenderer Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_reference_material_tests)
    ludus_enable_test_exceptions(ludus_reference_material_tests)
    add_test(NAME ludus_reference_material_tests COMMAND ludus_reference_material_tests)
    set_tests_properties(ludus_reference_material_tests PROPERTIES LABELS "unit;renderer;build-contract")
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
    add_executable(ludus_renderer_render_tests tests/render_tests.cpp tests/denoising_tests.cpp tests/gpu_preview_tests.cpp tests/ray_opacity_tests.cpp tests/visibility_tests.cpp tests/reference_compute_tests.cpp tests/reference_light_sampling_benchmark.cpp tests/gpu_random_tests.cpp tests/projected_solid_angle_parity_tests.cpp src/ltc_table.cpp "${ludus_ltc_table_source}" tests/power_cdf_parity_tests.cpp tests/ris_projected_solid_angle_weight_tests.cpp tests/reference_light_sampling_parity_tests.cpp tests/polygonal_light_sampling_oracle.cpp tests/polygonal_light_sampling_parity_tests.cpp tests/slice_sampling_luminaire_estimator_tests.cpp tests/restir_selection_tests.cpp tests/authored_tests.cpp tests/render_graph_render_tests.cpp tests/render_graph_pool_render_tests.cpp)
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
    # Test-only kernel comparing FoundationMath's shader projected solid angle sampling with C++.
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME projected_solid_angle_parity
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/projected_solid_angle_parity.slang" COMPUTE ComputeMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders")
    # Test-only kernel comparing the shader lookup of the generated LTC table with src/ltc_table.cpp.
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME ltc_table_parity
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/ltc_table_parity.slang" COMPUTE ComputeMain
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table.slang" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h")
    # Test-only kernel comparing FoundationMath's shader linearly transformed cosines with C++.
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME linearly_transformed_cosine_parity
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/linearly_transformed_cosine_parity.slang" COMPUTE ComputeMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders")
    # Test-only kernel comparing the public power-CDF scene accessors with the CPU reference.
    ludus_get_renderer_shader_roots(renderer_shader_directory renderer_shader_includes)
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME power_cdf_parity
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/power_cdf_parity.slang" COMPUTE PowerCdfParityMain
        INCLUDES "${renderer_shader_directory}" ${renderer_shader_includes}
        DEPENDS "${renderer_shader_directory}/renderer_gpu_preview.slang" "${renderer_shader_directory}/scene_shader.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/power_cdf_parity_layout.h")
    # Test-only kernel running the ris-projected-solid-angle template's scene-independent MIS weights.
    set(ris_projected_solid_angle_shaders "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/ris_projected_solid_angle/main_shaders")
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME ris_projected_solid_angle_weights
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/ris_projected_solid_angle_weights.slang" COMPUTE ComputeMain
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
                 "${ris_projected_solid_angle_shaders}"
        DEPENDS "${ris_projected_solid_angle_shaders}/projected_solid_angle_light.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ris_projected_solid_angle_weights_layout.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table.slang" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h")
    # The same template functions for receivers viewed from below their shading horizon (Peters 2021, section 6.1).
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME ris_projected_solid_angle_clipped_receivers
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/ris_projected_solid_angle_clipped_receivers.slang"
        COMPUTE ClippedReceiverMain
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
                 "${ris_projected_solid_angle_shaders}"
        DEPENDS "${ris_projected_solid_angle_shaders}/projected_solid_angle_light.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ris_projected_solid_angle_weights_layout.h"
                "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders/shading_normal.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table.slang" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h")
    # Test-only kernel comparing the GPU-compute reference's resampled light pieces with src/reference_light_sampling.cpp.
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME reference_light_sampling_parity
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_light_sampling_parity.slang" COMPUTE ComputeMain
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/reference_light_sampling.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_light_sampling_parity_layout.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/renderer_reference_layout.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table.slang" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ltc_table_layout.h")
    # Test-only kernel running the polygonal-light-sampling template's estimators against the CPU oracle.
    set(polygonal_light_sampling_shaders "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/polygonal_light_sampling/main_shaders")
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME polygonal_light_sampling_estimators
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/polygonal_light_sampling_estimators.slang" COMPUTE PolygonalLightEstimatorsMain
        INCLUDES "${renderer_shader_directory}" ${renderer_shader_includes} "${polygonal_light_sampling_shaders}"
        DEPENDS "${polygonal_light_sampling_shaders}/polygonal_light.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/polygonal_light_sampling_estimators_layout.h"
                "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders/shading_normal.slang"
                "${renderer_shader_directory}/renderer_gpu_preview.slang" "${renderer_shader_directory}/scene_shader.slang"
                "${renderer_shader_directory}/ltc_table.slang" "${renderer_shader_directory}/ltc_table_layout.h")
    # Test-only kernel running the slice-sampling-luminaire template's proposals at fixed receivers.
    set(slice_sampling_luminaire_shaders "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/slice_sampling_luminaire/main_shaders")
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME slice_sampling_luminaire_estimators
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/slice_sampling_luminaire_estimators.slang" COMPUTE SliceSamplingEstimatorsMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders" "${slice_sampling_luminaire_shaders}"
        DEPENDS "${slice_sampling_luminaire_shaders}/slice_sampling.slang" "${slice_sampling_luminaire_shaders}/luminaire.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/slice_sampling_luminaire_estimators_layout.h"
                "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders/math.slang")
    ludus_compile_shader(TARGET ludus_renderer_render_tests NAME restir_selection
        SOURCE "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/tests/restir_selection.slang" COMPUTE ComputeMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders"
                 "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/render_graphs/restir_di/main_shaders"
                 "${CMAKE_CURRENT_SOURCE_DIR}/include/ludus/graphics/renderer"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/restir_di/main_shaders/initial_emitter_sampling.slang"
                "${CMAKE_CURRENT_SOURCE_DIR}/include/ludus/graphics/renderer/scene_shader_layout.h")
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_gpu_preview renderer_gpu_preview_visibility_full renderer_gpu_preview_hit_cached renderer_gpu_preview_group32 renderer_gpu_preview_group64 renderer_gpu_preview_group128 renderer_gpu_preview_group8x8)
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
    ludus_compile_renderer_shaders(TARGET ludus_renderer_render_tests SHADERS renderer_grid)
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
        ludus_stage_render_graph_fixture("${input}" "${staged}")
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
    # The default graph ends in a ToneMap that only a clustered deferred resolve can apply. The parity tests compare
    # linear radiance, so their copies of the default graph disable it, which exports clamped linear colour.
    function(ludus_stage_linear_default_render_graph staged)
        foreach(file IN LISTS default_render_graph_files)
            configure_file("${default_render_graph}/${file}" "${staged}/${file}" COPYONLY)
        endforeach()
        file(READ "${staged}/main.lurender.json" graph)
        string(JSON configuration_count LENGTH "${graph}" configurations)
        math(EXPR last_configuration "${configuration_count} - 1")
        foreach(configuration RANGE ${last_configuration})
            string(JSON node_count LENGTH "${graph}" configurations ${configuration} nodes)
            math(EXPR last_node "${node_count} - 1")
            foreach(node RANGE ${last_node})
                string(JSON kind GET "${graph}" configurations ${configuration} nodes ${node} kind)
                if(kind STREQUAL "ToneMap")
                    string(JSON graph SET "${graph}" configurations ${configuration} nodes ${node} enabled false)
                endif()
            endforeach()
        endforeach()
        file(WRITE "${staged}/main.lurender.json" "${graph}\n")
    endfunction()
    # A cooked clustered-forward control shares the exact default material and geometry callbacks.
    set(clustered_forward_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/clustered_forward")
    ludus_stage_linear_default_render_graph("${clustered_forward_staged}")
    file(READ "${default_render_graph}/main.lushader.json" clustered_forward_manifest)
    string(JSON clustered_forward_manifest SET "${clustered_forward_manifest}" techniques 0 scene_interface profile "\"ClusteredForward\"")
    string(JSON clustered_forward_manifest SET "${clustered_forward_manifest}" programs 1 function "\"ForwardMain\"")
    string(JSON clustered_forward_manifest REMOVE "${clustered_forward_manifest}" programs 3)
    file(WRITE "${clustered_forward_staged}/main.lushader.json" "${clustered_forward_manifest}\n")
    ludus_compile_render_graph(TARGET ludus_renderer_render_tests NAME rg2_clustered_forward_control
        MANIFEST "${clustered_forward_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_clustered_forward_control_RUNTIME_PACKAGE PARENT_PATH clustered_forward_package_directory)
    target_compile_definitions(ludus_renderer_render_tests PRIVATE
        LUDUS_RG3_CLUSTERED_FORWARD_DIRECTORY="${clustered_forward_package_directory}")
    set(clustered_deferred_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/clustered_deferred")
    ludus_stage_linear_default_render_graph("${clustered_deferred_staged}")
    foreach(configuration Default ZPrepass)
        ludus_compile_render_graph(TARGET ludus_renderer_render_tests NAME rg2_clustered_${configuration}
            MANIFEST "${clustered_deferred_staged}/main.lushader.json" CONFIGURATION "${configuration}" RUNTIME_PACKAGE)
        cmake_path(GET rg2_clustered_${configuration}_RUNTIME_PACKAGE PARENT_PATH clustered_package_directory)
        string(TOUPPER "${configuration}" configuration_constant)
        target_compile_definitions(ludus_renderer_render_tests PRIVATE
            LUDUS_RG3_CLUSTERED_${configuration_constant}_DIRECTORY="${clustered_package_directory}")
    endforeach()
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
    ludus_stage_render_graph_fixture("${ray_source}" "${ray_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_raytracing
        MANIFEST "${ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_raytracing_RUNTIME_PACKAGE PARENT_PATH ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_RAYTRACING_DIRECTORY="${ray_package_directory}")
    set(visibility_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/visibility")
    set(visibility_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/visibility")
    ludus_stage_render_graph_fixture("${visibility_source}" "${visibility_staged}")
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
    foreach(template_directory IN ITEMS visibility_buffer visibility_secondary_rays tiled_vbuffer_nee compute_ray_tracing
            simple_compute_ray_tracing veach_guibas_mis line_sampling ris_projected_solid_angle salesin_jarosz_lines
            polygonal_light_sampling slice_sampling_luminaire steinberg_plt)
        set(template_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/${template_directory}")
        set(template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${template_directory}_template")
        ludus_stage_render_graph_fixture("${template_source}" "${template_staged}")
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_${template_directory}_template"
            MANIFEST "${template_staged}/main.lushader.json" RUNTIME_PACKAGE)
        set(template_package "rg2_${template_directory}_template_RUNTIME_PACKAGE")
        cmake_path(GET ${template_package} PARENT_PATH template_package_directory)
        string(TOUPPER "${template_directory}" template_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_${template_definition}_TEMPLATE_DIRECTORY=\"${template_package_directory}\"")
    endforeach()
    # The polygonal-light template's frame capture profile: its ray kernels link the template's
    # scene_interface.capture_visibility_queries, which only RenderGraphPackage::DecodeCaptureProfile admits.
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_polygonal_light_sampling_capture
        MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/polygonal_light_sampling_template/main.lushader.json"
        CAPTURE_PROFILE_PACKAGE)
    cmake_path(GET rg2_polygonal_light_sampling_capture_CAPTURE_PROFILE_PACKAGE PARENT_PATH polygonal_capture_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        "LUDUS_RG3_POLYGONAL_LIGHT_SAMPLING_CAPTURE_DIRECTORY=\"${polygonal_capture_directory}\"")
    set(shared_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/shared_ray")
    set(shared_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/shared_ray")
    ludus_stage_render_graph_fixture("${shared_ray_source}" "${shared_ray_staged}")
    foreach(configuration IN ITEMS Default Native ComputeBvh)
        string(TOLOWER "${configuration}" configuration_suffix)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_shared_ray_${configuration_suffix}
            MANIFEST "${shared_ray_staged}/main.lushader.json" CONFIGURATION "${configuration}" RUNTIME_PACKAGE)
        cmake_path(GET rg2_shared_ray_${configuration_suffix}_RUNTIME_PACKAGE PARENT_PATH shared_ray_package_directory)
        string(TOUPPER "${configuration}" configuration_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_SHARED_RAY_${configuration_definition}_DIRECTORY=\"${shared_ray_package_directory}\"")
    endforeach()
    # One package per traversal strategy over the same callbacks; the GPU test compares their images.
    set(traversal_parity_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/traversal_parity")
    set(traversal_parity_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/traversal_parity")
    ludus_stage_render_graph_fixture("${traversal_parity_source}" "${traversal_parity_staged}")
    foreach(configuration IN ITEMS TriangleLoop Native ComputeBvh)
        string(TOLOWER "${configuration}" configuration_suffix)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_traversal_parity_${configuration_suffix}
            MANIFEST "${traversal_parity_staged}/main.lushader.json" CONFIGURATION "${configuration}" RUNTIME_PACKAGE)
        cmake_path(GET rg2_traversal_parity_${configuration_suffix}_RUNTIME_PACKAGE PARENT_PATH traversal_parity_package_directory)
        string(TOUPPER "${configuration}" configuration_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_TRAVERSAL_PARITY_${configuration_definition}_DIRECTORY=\"${traversal_parity_package_directory}\"")
    endforeach()
    # ComputeBvh rays along the edge opposite each triangle's first corner reload their hits.
    set(bvh_edge_barycentrics_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/bvh_edge_barycentrics")
    ludus_stage_render_graph_fixture("${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/bvh_edge_barycentrics"
        "${bvh_edge_barycentrics_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_bvh_edge_barycentrics
        MANIFEST "${bvh_edge_barycentrics_staged}/main.lushader.json" CONFIGURATION ComputeBvh RUNTIME_PACKAGE)
    cmake_path(GET rg2_bvh_edge_barycentrics_RUNTIME_PACKAGE PARENT_PATH bvh_edge_barycentrics_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        "LUDUS_RG3_BVH_EDGE_BARYCENTRICS_DIRECTORY=\"${bvh_edge_barycentrics_directory}\"")
    # The compute ray-tracing template's extra configuration runs without native ray tracing.
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_compute_ray_tracing_bvh_template
        MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/compute_ray_tracing_template/main.lushader.json"
        CONFIGURATION ComputeBvh RUNTIME_PACKAGE)
    cmake_path(GET rg2_compute_ray_tracing_bvh_template_RUNTIME_PACKAGE PARENT_PATH compute_bvh_template_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        "LUDUS_RG3_COMPUTE_RAY_TRACING_BVH_TEMPLATE_DIRECTORY=\"${compute_bvh_template_directory}\"")
    set(scene_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/scene_ray")
    set(scene_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/scene_ray")
    ludus_stage_render_graph_fixture("${scene_ray_source}" "${scene_ray_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_ray
        MANIFEST "${scene_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_ray_RUNTIME_PACKAGE PARENT_PATH scene_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_SCENE_RAY_DIRECTORY="${scene_ray_package_directory}")
    function(ludus_stage_ray_invocation_fixture original destination mode)
        # Reuse authored callbacks and change only the invocation on each logical RT node.
        # The rewritten graph is written only when it changes, so reconfiguring does not recook it.
        ludus_stage_render_graph_inputs("${original}" "${destination}" main.lurender.json)
        file(READ "${original}/main.lurender.json" document)
        string(JSON node_count LENGTH "${document}" configurations 0 nodes)
        math(EXPR last_node "${node_count} - 1")
        set(ray_index 0)
        foreach(node_index RANGE 0 ${last_node})
            string(JSON node_kind GET "${document}" configurations 0 nodes ${node_index} kind)
            if(node_kind STREQUAL "RayTracingPipeline")
                if(mode STREQUAL "Native" OR ray_index EQUAL 0)
                    string(JSON document SET "${document}" configurations 0 nodes ${node_index} invocation type "\"RayTracing\"")
                else()
                    string(JSON document SET "${document}" configurations 0 nodes ${node_index} invocation type "\"Compute\"")
                    if(ray_index EQUAL 1)
                        set(traversal NativeQuery)
                    else()
                        set(traversal TriangleLoop)
                    endif()
                    string(JSON document SET "${document}" configurations 0 nodes ${node_index} invocation traversal "\"${traversal}\"")
                endif()
                math(EXPR ray_index "${ray_index} + 1")
            endif()
        endforeach()
        file(WRITE "${destination}.lurender.json" "${document}")
        file(COPY_FILE "${destination}.lurender.json" "${destination}/main.lurender.json" ONLY_IF_DIFFERENT)
    endfunction()
    if(APPLE)
        set(native_visibility_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/native_visibility")
        ludus_stage_ray_invocation_fixture("${visibility_staged}" "${native_visibility_staged}" Native)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_native_visibility
            MANIFEST "${native_visibility_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET rg2_native_visibility_RUNTIME_PACKAGE PARENT_PATH native_visibility_package_directory)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            LUDUS_RG3_NATIVE_VISIBILITY_DIRECTORY="${native_visibility_package_directory}")
        set(native_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/native_ray")
        ludus_stage_ray_invocation_fixture("${scene_ray_staged}" "${native_ray_staged}" Native)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_native_ray
            MANIFEST "${native_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET rg2_native_ray_RUNTIME_PACKAGE PARENT_PATH native_ray_package_directory)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            LUDUS_RG3_NATIVE_RAY_DIRECTORY="${native_ray_package_directory}")
    endif()
    set(ordered_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/ordered_ray")
    set(ordered_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/ordered_ray")
    ludus_stage_render_graph_fixture("${ordered_ray_source}" "${ordered_ray_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_ordered_ray
        MANIFEST "${ordered_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_ordered_ray_RUNTIME_PACKAGE PARENT_PATH ordered_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_ORDERED_RAY_DIRECTORY="${ordered_ray_package_directory}")
    if(APPLE)
        set(mixed_invocation_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/mixed_invocation")
        ludus_stage_ray_invocation_fixture("${ordered_ray_staged}" "${mixed_invocation_staged}" Mixed)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_mixed_invocation
            MANIFEST "${mixed_invocation_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET rg2_mixed_invocation_RUNTIME_PACKAGE PARENT_PATH mixed_invocation_package_directory)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            LUDUS_RG3_MIXED_INVOCATION_DIRECTORY="${mixed_invocation_package_directory}")
    endif()
    set(compute_split_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/compute_split")
    set(compute_split_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/compute_split")
    ludus_stage_render_graph_fixture("${compute_split_source}" "${compute_split_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_compute_split
        MANIFEST "${compute_split_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_compute_split_RUNTIME_PACKAGE PARENT_PATH compute_split_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_COMPUTE_SPLIT_DIRECTORY="${compute_split_package_directory}")
    # Scene compute passes (compute ray-tracing C1/C2): a single compute pass, its raster
    # configuration for renderer selection, and a mixed compute + ray sequence.
    set(scene_compute_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/scene_compute")
    set(scene_compute_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/scene_compute")
    ludus_stage_render_graph_fixture("${scene_compute_source}" "${scene_compute_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_compute
        MANIFEST "${scene_compute_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_compute_RUNTIME_PACKAGE PARENT_PATH scene_compute_package_directory)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_scene_compute_raster
        MANIFEST "${scene_compute_staged}/main.lushader.json" CONFIGURATION Raster RUNTIME_PACKAGE)
    cmake_path(GET rg2_scene_compute_raster_RUNTIME_PACKAGE PARENT_PATH scene_compute_raster_package_directory)
    # GetEmitterBounds: a compute pass compares the packed emitter bounds with their triangles.
    set(emitter_bounds_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/emitter_bounds")
    set(emitter_bounds_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/emitter_bounds")
    ludus_stage_render_graph_fixture("${emitter_bounds_source}" "${emitter_bounds_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_emitter_bounds
        MANIFEST "${emitter_bounds_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_emitter_bounds_RUNTIME_PACKAGE PARENT_PATH emitter_bounds_package_directory)
    # scene_interface.polygonal_light_sampling: a compute pass reads the renderer-bound LTC table at slot 13.
    set(ltc_table_fixture_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/ltc_table")
    set(ltc_table_fixture_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/ltc_table")
    ludus_stage_render_graph_fixture("${ltc_table_fixture_source}" "${ltc_table_fixture_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_ltc_table
        MANIFEST "${ltc_table_fixture_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_ltc_table_RUNTIME_PACKAGE PARENT_PATH ltc_table_fixture_package_directory)
    # The test recomputes the kernel's lookup cases from the same header.
    target_include_directories(ludus_render_graph_ray_tests PRIVATE "${ltc_table_fixture_source}/main_shaders")
    set(compute_mixed_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/compute_mixed")
    set(compute_mixed_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/compute_mixed")
    ludus_stage_render_graph_fixture("${compute_mixed_source}" "${compute_mixed_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_compute_mixed
        MANIFEST "${compute_mixed_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_compute_mixed_RUNTIME_PACKAGE PARENT_PATH compute_mixed_package_directory)
    # Single-pass RayTracingPipeline reference for the compute-ray-tracing template (C4).
    set(direct_lighting_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/direct_lighting_reference")
    set(direct_lighting_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/direct_lighting_reference")
    ludus_stage_render_graph_fixture("${direct_lighting_source}" "${direct_lighting_staged}")
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
        ludus_stage_render_graph_fixture("${veach_guibas_reference_source}" "${veach_guibas_staged}")
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
        ludus_stage_render_graph_inputs("${veach_guibas_template_staged}" "${mis_staged}" main.lushader.json)
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
    # The steinberg-plt template with Graph settings replaced, each variant a name followed by setting/value
    # pairs: 20 mm and 200 mm natural point sources, whose spectra the tests compare, and a flat grating with the
    # 20 mm source, whose wave and geometric images must agree. Sources of a few millimetres make sub-pixel glints
    # at the tests' 96 x 64 resolution.
    set(plt_template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/steinberg_plt_template")
    file(READ "${plt_template_staged}/main.lushader.json" plt_manifest)
    string(JSON plt_setting_count LENGTH "${plt_manifest}" settings)
    math(EXPR plt_setting_last "${plt_setting_count} - 1")
    foreach(plt_variant IN ITEMS "small_source;PLT_POINT_SOURCE_RADIUS_MILLIMETERS;20"
            "large_source;PLT_POINT_SOURCE_RADIUS_MILLIMETERS;200"
            "flat;PLT_POINT_SOURCE_RADIUS_MILLIMETERS;20;PLT_GRATING_AMPLITUDE_NANOMETERS;0")
        list(POP_FRONT plt_variant plt_name)
        set(plt_variant_manifest "${plt_manifest}")
        while(plt_variant)
            list(POP_FRONT plt_variant plt_setting plt_value)
            set(plt_setting_index -1)
            foreach(setting_index RANGE ${plt_setting_last})
                string(JSON setting_id GET "${plt_manifest}" settings ${setting_index} id)
                if(setting_id STREQUAL plt_setting)
                    set(plt_setting_index ${setting_index})
                endif()
            endforeach()
            if(plt_setting_index LESS 0)
                message(FATAL_ERROR "The steinberg-plt template manifest has no ${plt_setting} setting.")
            endif()
            string(JSON plt_variant_manifest SET "${plt_variant_manifest}" settings ${plt_setting_index} value
                ${plt_value})
        endwhile()
        set(plt_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/steinberg_plt_${plt_name}")
        ludus_stage_render_graph_inputs("${plt_template_staged}" "${plt_staged}" main.lushader.json)
        # Rewrite the staged manifest only when it changes, so reconfiguring does not rebuild the package.
        file(WRITE "${plt_staged}.lushader.json" "${plt_variant_manifest}\n")
        file(COPY_FILE "${plt_staged}.lushader.json" "${plt_staged}/main.lushader.json" ONLY_IF_DIFFERENT)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_steinberg_plt_${plt_name}"
            MANIFEST "${plt_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_steinberg_plt_${plt_name}_RUNTIME_PACKAGE" PARENT_PATH plt_package_directory)
        string(TOUPPER "${plt_name}" plt_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_STEINBERG_PLT_${plt_definition}_DIRECTORY=\"${plt_package_directory}\"")
    endforeach()
    # The Veach and Guibas template with every scene compute pass on the triangle loop, so the
    # line-sampling comparison with its lightSampling output needs no native ray profile.
    set(veach_guibas_loop_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/veach_guibas_mis_triangle_loop")
    ludus_stage_render_graph_inputs("${veach_guibas_template_staged}" "${veach_guibas_loop_staged}" library.lurender.json)
    file(READ "${veach_guibas_template_staged}/library.lurender.json" veach_guibas_library)
    string(JSON veach_guibas_node_count LENGTH "${veach_guibas_library}" configurations 0 nodes)
    math(EXPR veach_guibas_node_last "${veach_guibas_node_count} - 1")
    foreach(node_index RANGE ${veach_guibas_node_last})
        string(JSON node_kind GET "${veach_guibas_library}" configurations 0 nodes ${node_index} kind)
        if(node_kind STREQUAL "ComputePipeline")
            string(JSON veach_guibas_library SET "${veach_guibas_library}"
                configurations 0 nodes ${node_index} invocation traversal "\"TriangleLoop\"")
        endif()
    endforeach()
    file(WRITE "${veach_guibas_loop_staged}.lurender.json" "${veach_guibas_library}\n")
    file(COPY_FILE "${veach_guibas_loop_staged}.lurender.json" "${veach_guibas_loop_staged}/library.lurender.json"
        ONLY_IF_DIFFERENT)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_veach_guibas_mis_triangle_loop
        MANIFEST "${veach_guibas_loop_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_veach_guibas_mis_triangle_loop_RUNTIME_PACKAGE PARENT_PATH veach_guibas_loop_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_VEACH_GUIBAS_MIS_TRIANGLE_LOOP_DIRECTORY="${veach_guibas_loop_package_directory}")
    # The opt-in [.ris-projected-solid-angle-benchmark] packages: the ris-projected-solid-angle and Veach and Guibas
    # templates with every scene compute pass on the compute BVH, so both trace with one traversal, and with the
    # BSDF-only program replaced by tests/render_graph_query_count.slang, which runs the unchanged MIS program and
    # returns its query counts. The MIS and light-only programs are the templates' own.
    foreach(benchmark_template IN ITEMS ris_projected_solid_angle veach_guibas_mis)
        set(benchmark_source "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${benchmark_template}_template")
        set(benchmark_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${benchmark_template}_benchmark")
        ludus_stage_render_graph_inputs("${benchmark_source}" "${benchmark_staged}"
            main.lushader.json library.lurender.json)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tests/render_graph_query_count.slang"
            "${benchmark_staged}/main_shaders/query_count.slang" COPYONLY)
        file(READ "${benchmark_source}/main.lushader.json" benchmark_manifest)
        string(JSON benchmark_program_count LENGTH "${benchmark_manifest}" programs)
        math(EXPR benchmark_program_last "${benchmark_program_count} - 1")
        set(benchmark_program_found OFF)
        foreach(program_index RANGE ${benchmark_program_last})
            string(JSON program_id GET "${benchmark_manifest}" programs ${program_index} id)
            if(program_id STREQUAL "ShadeBsdfSampling")
                string(JSON benchmark_manifest SET "${benchmark_manifest}"
                    programs ${program_index} source "\"main_shaders/query_count.slang\"")
                string(JSON benchmark_manifest SET "${benchmark_manifest}"
                    programs ${program_index} function "\"CountMultipleImportanceQueries\"")
                set(benchmark_program_found ON)
            endif()
        endforeach()
        if(NOT benchmark_program_found)
            message(FATAL_ERROR "The ${benchmark_template} template manifest has no ShadeBsdfSampling program.")
        endif()
        file(READ "${benchmark_source}/library.lurender.json" benchmark_library)
        string(JSON benchmark_node_count LENGTH "${benchmark_library}" configurations 0 nodes)
        math(EXPR benchmark_node_last "${benchmark_node_count} - 1")
        foreach(node_index RANGE ${benchmark_node_last})
            string(JSON node_kind GET "${benchmark_library}" configurations 0 nodes ${node_index} kind)
            if(node_kind STREQUAL "ComputePipeline")
                string(JSON benchmark_library SET "${benchmark_library}"
                    configurations 0 nodes ${node_index} invocation traversal "\"ComputeBvh\"")
            endif()
        endforeach()
        if(NOT benchmark_template STREQUAL "ris_projected_solid_angle")
            # Rewrite the staged files only when they change, so reconfiguring does not rebuild the package.
            file(WRITE "${benchmark_staged}.lushader.json" "${benchmark_manifest}\n")
            file(COPY_FILE "${benchmark_staged}.lushader.json" "${benchmark_staged}/main.lushader.json"
                ONLY_IF_DIFFERENT)
            file(WRITE "${benchmark_staged}.lurender.json" "${benchmark_library}\n")
            file(COPY_FILE "${benchmark_staged}.lurender.json" "${benchmark_staged}/library.lurender.json"
                ONLY_IF_DIFFERENT)
            ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_${benchmark_template}_benchmark"
                MANIFEST "${benchmark_staged}/main.lushader.json" RUNTIME_PACKAGE)
            cmake_path(GET "rg2_${benchmark_template}_benchmark_RUNTIME_PACKAGE" PARENT_PATH
                benchmark_package_directory)
            string(TOUPPER "${benchmark_template}" benchmark_definition)
            target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
                "LUDUS_RG3_${benchmark_definition}_BENCHMARK_DIRECTORY=\"${benchmark_package_directory}\"")
            continue()
        endif()
        # One RIS package per RIS_TARGET:RIS_CANDIDATE_COUNT:RIS_NORMALIZATION_CANDIDATE_COUNT variant; the benchmark
        # measures each against the same Veach and Guibas reference. The first variants are the template's exact
        # target and its defaults; a longer list measures an M and K sweep without rebuilding the others.
        set(LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS "0:32:8;1:8:4" CACHE STRING
            "RIS_TARGET:RIS_CANDIDATE_COUNT:RIS_NORMALIZATION_CANDIDATE_COUNT variants of the opt-in RIS benchmark")
        foreach(setting_id IN ITEMS RIS_TARGET RIS_CANDIDATE_COUNT RIS_NORMALIZATION_CANDIDATE_COUNT)
            set(benchmark_${setting_id}_index -1)
        endforeach()
        string(JSON benchmark_setting_count LENGTH "${benchmark_manifest}" settings)
        math(EXPR benchmark_setting_last "${benchmark_setting_count} - 1")
        foreach(setting_index RANGE ${benchmark_setting_last})
            string(JSON setting_id GET "${benchmark_manifest}" settings ${setting_index} id)
            set(benchmark_${setting_id}_index ${setting_index})
        endforeach()
        set(benchmark_variant_entries "")
        set(benchmark_variant_count 0)
        foreach(benchmark_variant IN LISTS LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS)
            string(REPLACE ":" ";" benchmark_variant_values "${benchmark_variant}")
            list(LENGTH benchmark_variant_values benchmark_variant_length)
            if(NOT benchmark_variant_length EQUAL 3)
                message(FATAL_ERROR "RIS benchmark variant '${benchmark_variant}' is not TARGET:M:K.")
            endif()
            list(GET benchmark_variant_values 0 variant_target)
            list(GET benchmark_variant_values 1 variant_candidates)
            list(GET benchmark_variant_values 2 variant_normalization)
            set(variant_manifest "${benchmark_manifest}")
            foreach(setting_pair IN ITEMS "RIS_TARGET;${variant_target}" "RIS_CANDIDATE_COUNT;${variant_candidates}"
                    "RIS_NORMALIZATION_CANDIDATE_COUNT;${variant_normalization}")
                list(GET setting_pair 0 setting_id)
                list(GET setting_pair 1 setting_value)
                if(benchmark_${setting_id}_index LESS 0)
                    message(FATAL_ERROR "The ris_projected_solid_angle template manifest has no ${setting_id} setting.")
                endif()
                string(JSON variant_manifest SET "${variant_manifest}"
                    settings ${benchmark_${setting_id}_index} value ${setting_value})
            endforeach()
            set(variant_suffix "t${variant_target}_m${variant_candidates}_k${variant_normalization}")
            set(variant_staged "${benchmark_staged}_${variant_suffix}")
            ludus_stage_render_graph_inputs("${benchmark_source}" "${variant_staged}"
                main.lushader.json library.lurender.json)
            configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tests/render_graph_query_count.slang"
                "${variant_staged}/main_shaders/query_count.slang" COPYONLY)
            # Rewrite the staged files only when they change, so reconfiguring does not rebuild the package.
            file(WRITE "${variant_staged}.lushader.json" "${variant_manifest}\n")
            file(COPY_FILE "${variant_staged}.lushader.json" "${variant_staged}/main.lushader.json" ONLY_IF_DIFFERENT)
            file(WRITE "${variant_staged}.lurender.json" "${benchmark_library}\n")
            file(COPY_FILE "${variant_staged}.lurender.json" "${variant_staged}/library.lurender.json"
                ONLY_IF_DIFFERENT)
            set(variant_name "rg2_ris_projected_solid_angle_benchmark_${variant_suffix}")
            ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "${variant_name}"
                MANIFEST "${variant_staged}/main.lushader.json" RUNTIME_PACKAGE)
            cmake_path(GET "${variant_name}_RUNTIME_PACKAGE" PARENT_PATH variant_package_directory)
            string(APPEND benchmark_variant_entries
                "    {{\"${variant_package_directory}\", \"${variant_name}.lurgrt\"}, "
                "${variant_target}, ${variant_candidates}, ${variant_normalization}}, \\\n")
            math(EXPR benchmark_variant_count "${benchmark_variant_count} + 1")
        endforeach()
        if(benchmark_variant_count EQUAL 0)
            message(FATAL_ERROR "LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS lists no variant.")
        endif()
        set(benchmark_variant_header "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/ris_projected_solid_angle_benchmark_variants")
        file(WRITE "${benchmark_variant_header}/variants.h.staged"
            "// Generated from LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS by modules/graphics/renderer.\n"
            "#define LUDUS_RG3_RIS_BENCHMARK_VARIANT_COUNT ${benchmark_variant_count}\n"
            "#define LUDUS_RG3_RIS_BENCHMARK_VARIANTS \\\n${benchmark_variant_entries}\n")
        file(COPY_FILE "${benchmark_variant_header}/variants.h.staged"
            "${benchmark_variant_header}/ris_projected_solid_angle_benchmark_variants.h" ONLY_IF_DIFFERENT)
        target_include_directories(ludus_render_graph_ray_tests PRIVATE "${benchmark_variant_header}")
    endforeach()
    # The salesin-jarosz-lines template with its POINT_LINE_MIS_HEURISTIC Graph setting set to power (the staged
    # default is balance), and the point_line_densities kernel over the template's own strategy and
    # weight sources under each heuristic (point-line PL2).
    set(point_line_template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/salesin_jarosz_lines_template")
    file(READ "${point_line_template_staged}/main.lushader.json" point_line_manifest)
    string(JSON point_line_setting_count LENGTH "${point_line_manifest}" settings)
    set(point_line_heuristic_index -1)
    math(EXPR point_line_setting_last "${point_line_setting_count} - 1")
    foreach(setting_index RANGE ${point_line_setting_last})
        string(JSON setting_id GET "${point_line_manifest}" settings ${setting_index} id)
        if(setting_id STREQUAL "POINT_LINE_MIS_HEURISTIC")
            set(point_line_heuristic_index ${setting_index})
        endif()
    endforeach()
    if(point_line_heuristic_index LESS 0)
        message(FATAL_ERROR "The salesin-jarosz-lines template manifest has no POINT_LINE_MIS_HEURISTIC setting.")
    endif()
    set(point_line_power_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/salesin_jarosz_lines_power")
    file(COPY "${point_line_template_staged}/" DESTINATION "${point_line_power_staged}"
        PATTERN "main.lushader.json" EXCLUDE)
    string(JSON point_line_power_manifest SET "${point_line_manifest}" settings ${point_line_heuristic_index} value 1)
    # Rewrite the staged manifest only when it changes, so reconfiguring does not rebuild the package.
    file(WRITE "${point_line_power_staged}.lushader.json" "${point_line_power_manifest}\n")
    file(COPY_FILE "${point_line_power_staged}.lushader.json" "${point_line_power_staged}/main.lushader.json"
        ONLY_IF_DIFFERENT)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_salesin_jarosz_lines_power
        MANIFEST "${point_line_power_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_salesin_jarosz_lines_power_RUNTIME_PACKAGE PARENT_PATH point_line_power_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_SALESIN_JAROSZ_LINES_POWER_DIRECTORY="${point_line_power_package_directory}")
    # The polygonal-light-sampling template with PROJECTED_SOLID_ANGLE_MIS_VISIBILITY set to 1, so its color output is
    # Peters' weighted balance heuristic (2021, section 4.2), for the occluded-penumbra comparison with v = 1/2.
    set(polygonal_light_template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/polygonal_light_sampling_template")
    file(READ "${polygonal_light_template_staged}/main.lushader.json" polygonal_light_manifest)
    string(JSON polygonal_light_setting_count LENGTH "${polygonal_light_manifest}" settings)
    set(polygonal_light_visibility_index -1)
    math(EXPR polygonal_light_setting_last "${polygonal_light_setting_count} - 1")
    foreach(setting_index RANGE ${polygonal_light_setting_last})
        string(JSON setting_id GET "${polygonal_light_manifest}" settings ${setting_index} id)
        if(setting_id STREQUAL "PROJECTED_SOLID_ANGLE_MIS_VISIBILITY")
            set(polygonal_light_visibility_index ${setting_index})
        endif()
    endforeach()
    if(polygonal_light_visibility_index LESS 0)
        message(FATAL_ERROR
            "The polygonal-light-sampling template manifest has no PROJECTED_SOLID_ANGLE_MIS_VISIBILITY setting.")
    endif()
    set(polygonal_light_weighted_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/polygonal_light_sampling_weighted")
    file(COPY "${polygonal_light_template_staged}/" DESTINATION "${polygonal_light_weighted_staged}"
        PATTERN "main.lushader.json" EXCLUDE)
    string(JSON polygonal_light_weighted_manifest SET "${polygonal_light_manifest}"
        settings ${polygonal_light_visibility_index} value 1)
    # Rewrite the staged manifest only when it changes, so reconfiguring does not rebuild the package.
    file(WRITE "${polygonal_light_weighted_staged}.lushader.json" "${polygonal_light_weighted_manifest}\n")
    file(COPY_FILE "${polygonal_light_weighted_staged}.lushader.json"
        "${polygonal_light_weighted_staged}/main.lushader.json" ONLY_IF_DIFFERENT)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_polygonal_light_sampling_weighted
        MANIFEST "${polygonal_light_weighted_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_polygonal_light_sampling_weighted_RUNTIME_PACKAGE PARENT_PATH
        polygonal_light_weighted_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_POLYGONAL_LIGHT_SAMPLING_WEIGHTED_DIRECTORY="${polygonal_light_weighted_package_directory}")
    set(point_line_densities_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/point_line_densities")
    set(point_line_densities_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/point_line_densities")
    file(COPY "${point_line_densities_source}/" DESTINATION "${point_line_densities_staged}" PATTERN "*.in" EXCLUDE)
    foreach(point_line_source IN ITEMS line_sampling.slang multiple_importance.slang)
        file(COPY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/salesin_jarosz_lines/main_shaders/${point_line_source}"
            "${point_line_densities_staged}/main_shaders/${point_line_source}" ONLY_IF_DIFFERENT)
    endforeach()
    configure_file("${point_line_densities_source}/main.lushader.json.in"
        "${point_line_densities_staged}/balance.lushader.json" @ONLY)
    file(READ "${point_line_densities_staged}/balance.lushader.json" point_line_densities_manifest)
    string(JSON point_line_densities_heuristic GET "${point_line_densities_manifest}" settings 0 id)
    if(NOT point_line_densities_heuristic STREQUAL "POINT_LINE_MIS_HEURISTIC")
        message(FATAL_ERROR "The point_line_densities manifest must list POINT_LINE_MIS_HEURISTIC first.")
    endif()
    foreach(point_line_variant IN ITEMS "balance;0" "power;1")
        list(GET point_line_variant 0 point_line_name)
        list(GET point_line_variant 1 point_line_value)
        string(JSON point_line_variant_manifest SET "${point_line_densities_manifest}"
            settings 0 value ${point_line_value})
        set(point_line_variant_staged "${point_line_densities_staged}_${point_line_name}")
        file(COPY "${point_line_densities_staged}/" DESTINATION "${point_line_variant_staged}"
            PATTERN "*.lushader.json" EXCLUDE)
        file(WRITE "${point_line_variant_staged}.lushader.json" "${point_line_variant_manifest}\n")
        file(COPY_FILE "${point_line_variant_staged}.lushader.json" "${point_line_variant_staged}/main.lushader.json"
            ONLY_IF_DIFFERENT)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_point_line_densities_${point_line_name}"
            MANIFEST "${point_line_variant_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_point_line_densities_${point_line_name}_RUNTIME_PACKAGE" PARENT_PATH
            point_line_densities_package_directory)
        string(TOUPPER "${point_line_name}" point_line_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_POINT_LINE_DENSITIES_${point_line_definition}_DIRECTORY=\"${point_line_densities_package_directory}\"")
    endforeach()
    # FindVisibleLineIntervals against analytic intervals (point-line PL1), once with a TriangleLoop
    # pass and once with a NativeQuery pass, which must give the same software line traversal.
    set(line_visibility_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/line_visibility")
    set(line_visibility_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/line_visibility")
    ludus_stage_render_graph_fixture("${line_visibility_source}" "${line_visibility_staged}")
    foreach(configuration IN ITEMS Loop Native)
        string(TOLOWER "${configuration}" configuration_suffix)
        string(TOUPPER "${configuration}" configuration_definition)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_line_visibility_${configuration_suffix}
            MANIFEST "${line_visibility_staged}/main.lushader.json" CONFIGURATION "${configuration}" RUNTIME_PACKAGE)
        cmake_path(GET rg2_line_visibility_${configuration_suffix}_RUNTIME_PACKAGE PARENT_PATH
            line_visibility_package_directory)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_LINE_VISIBILITY_${configuration_definition}_DIRECTORY=\"${line_visibility_package_directory}\"")
    endforeach()
    # The point-line PL3 parity oracle: a staged graph cooked again with scene_interface.line_traversal
    # "TriangleLoop", so the GPU tests compare the default BVH line walk with the linear scan.
    function(ludus_stage_line_scan_variant staged destination)
        ludus_stage_render_graph_inputs("${staged}" "${destination}" main.lushader.json)
        file(READ "${staged}/main.lushader.json" manifest)
        string(JSON technique_count LENGTH "${manifest}" techniques)
        math(EXPR technique_last "${technique_count} - 1")
        set(has_scene_interface FALSE)
        foreach(technique_index RANGE ${technique_last})
            string(JSON interface_type ERROR_VARIABLE interface_error
                TYPE "${manifest}" techniques ${technique_index} scene_interface)
            if(interface_error STREQUAL "NOTFOUND" AND interface_type STREQUAL "OBJECT")
                string(JSON manifest SET "${manifest}"
                    techniques ${technique_index} scene_interface line_traversal "\"TriangleLoop\"")
                set(has_scene_interface TRUE)
            endif()
        endforeach()
        if(NOT has_scene_interface)
            message(FATAL_ERROR "${staged}/main.lushader.json declares no scene_interface.")
        endif()
        # Rewrite the staged manifest only when it changes, so reconfiguring does not rebuild the package.
        file(WRITE "${destination}.lushader.json" "${manifest}\n")
        file(COPY_FILE "${destination}.lushader.json" "${destination}/main.lushader.json" ONLY_IF_DIFFERENT)
    endfunction()
    set(line_visibility_scan_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/line_visibility_line_scan")
    ludus_stage_line_scan_variant("${line_visibility_staged}" "${line_visibility_scan_staged}")
    foreach(configuration IN ITEMS Loop Native)
        string(TOLOWER "${configuration}" configuration_suffix)
        string(TOUPPER "${configuration}" configuration_definition)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests
            NAME rg2_line_visibility_${configuration_suffix}_line_scan
            MANIFEST "${line_visibility_scan_staged}/main.lushader.json" CONFIGURATION "${configuration}"
            RUNTIME_PACKAGE)
        cmake_path(GET rg2_line_visibility_${configuration_suffix}_line_scan_RUNTIME_PACKAGE PARENT_PATH
            line_visibility_scan_package_directory)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_LINE_VISIBILITY_${configuration_definition}_LINE_SCAN_DIRECTORY=\"${line_visibility_scan_package_directory}\"")
    endforeach()
    foreach(template_directory IN ITEMS line_sampling salesin_jarosz_lines)
        set(template_scan_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${template_directory}_line_scan")
        ludus_stage_line_scan_variant("${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${template_directory}_template"
            "${template_scan_staged}")
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_${template_directory}_line_scan"
            MANIFEST "${template_scan_staged}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_${template_directory}_line_scan_RUNTIME_PACKAGE" PARENT_PATH template_scan_package_directory)
        string(TOUPPER "${template_directory}" template_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_${template_definition}_LINE_SCAN_DIRECTORY=\"${template_scan_package_directory}\"")
    endforeach()
    # Both line templates with every pass on ComputeBvh rays, for the hidden large-scene measurements: a
    # triangle-loop primary or shadow ray would dominate there. Line sampling is also cooked with the
    # line scan for the line query measurement (point-line PL3); point-and-line MIS is compared with
    # point sampling at equal GPU time (the large-scene measurement after PL3).
    foreach(bvh_template IN ITEMS line_sampling salesin_jarosz_lines)
        set(bvh_template_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${bvh_template}_compute_bvh")
        ludus_stage_render_graph_inputs("${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${bvh_template}_template"
            "${bvh_template_staged}" library.lurender.json)
        file(READ "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/${bvh_template}_template/library.lurender.json"
            bvh_template_library)
        string(JSON bvh_template_node_count LENGTH "${bvh_template_library}" configurations 0 nodes)
        math(EXPR bvh_template_node_last "${bvh_template_node_count} - 1")
        foreach(node_index RANGE ${bvh_template_node_last})
            string(JSON node_kind GET "${bvh_template_library}" configurations 0 nodes ${node_index} kind)
            if(node_kind STREQUAL "ComputePipeline")
                string(JSON bvh_template_library SET "${bvh_template_library}"
                    configurations 0 nodes ${node_index} invocation traversal "\"ComputeBvh\"")
            endif()
        endforeach()
        file(WRITE "${bvh_template_staged}.lurender.json" "${bvh_template_library}\n")
        file(COPY_FILE "${bvh_template_staged}.lurender.json" "${bvh_template_staged}/library.lurender.json"
            ONLY_IF_DIFFERENT)
    endforeach()
    set(line_sampling_bvh_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/line_sampling_compute_bvh")
    set(line_sampling_bvh_scan_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/line_sampling_compute_bvh_line_scan")
    ludus_stage_line_scan_variant("${line_sampling_bvh_staged}" "${line_sampling_bvh_scan_staged}")
    foreach(bvh_variant IN ITEMS "line_sampling_compute_bvh;${line_sampling_bvh_staged}"
            "line_sampling_compute_bvh_line_scan;${line_sampling_bvh_scan_staged}"
            "salesin_jarosz_lines_compute_bvh;${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/salesin_jarosz_lines_compute_bvh")
        list(GET bvh_variant 0 bvh_variant_name)
        list(GET bvh_variant 1 bvh_variant_directory)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME "rg2_${bvh_variant_name}"
            MANIFEST "${bvh_variant_directory}/main.lushader.json" RUNTIME_PACKAGE)
        cmake_path(GET "rg2_${bvh_variant_name}_RUNTIME_PACKAGE" PARENT_PATH bvh_variant_package_directory)
        string(TOUPPER "${bvh_variant_name}" bvh_variant_definition)
        target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
            "LUDUS_RG3_${bvh_variant_definition}_DIRECTORY=\"${bvh_variant_package_directory}\"")
    endforeach()
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_SCENE_COMPUTE_DIRECTORY="${scene_compute_package_directory}"
        LUDUS_RG3_SCENE_COMPUTE_RASTER_DIRECTORY="${scene_compute_raster_package_directory}"
        LUDUS_RG3_EMITTER_BOUNDS_DIRECTORY="${emitter_bounds_package_directory}"
        LUDUS_RG3_LTC_TABLE_DIRECTORY="${ltc_table_fixture_package_directory}"
        LUDUS_RG3_COMPUTE_MIXED_DIRECTORY="${compute_mixed_package_directory}"
        LUDUS_RG3_DIRECT_LIGHTING_REFERENCE_DIRECTORY="${direct_lighting_package_directory}")
    set(visibility_sequence_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/visibility_sequence")
    set(visibility_sequence_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/visibility_sequence")
    ludus_stage_render_graph_fixture("${visibility_sequence_source}" "${visibility_sequence_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_visibility_sequence
        MANIFEST "${visibility_sequence_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_visibility_sequence_RUNTIME_PACKAGE PARENT_PATH visibility_sequence_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_VISIBILITY_SEQUENCE_DIRECTORY="${visibility_sequence_package_directory}")
    set(restir_source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/restir_di")
    set(restir_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/restir_di")
    ludus_stage_render_graph_fixture("${restir_source}" "${restir_staged}")
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_restir_di
        MANIFEST "${restir_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_restir_di_RUNTIME_PACKAGE PARENT_PATH restir_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_RESTIR_DIRECTORY="${restir_package_directory}")
    set(storage_ray_source "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/rg2/storage_ray")
    set(storage_ray_staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/storage_ray")
    ludus_stage_render_graph_inputs("${scene_ray_source}" "${storage_ray_staged}" main.lurender.json)
    configure_file("${storage_ray_source}/main.lurender.json" "${storage_ray_staged}/main.lurender.json" COPYONLY)
    configure_file("${storage_ray_source}/reservoir.slang" "${storage_ray_staged}/main_shaders/reservoir.slang" COPYONLY)
    configure_file("${storage_ray_source}/main.lushader.json.in" "${storage_ray_staged}/main.lushader.json" @ONLY)
    ludus_compile_render_graph(TARGET ludus_render_graph_ray_tests NAME rg2_storage_ray
        MANIFEST "${storage_ray_staged}/main.lushader.json" RUNTIME_PACKAGE)
    cmake_path(GET rg2_storage_ray_RUNTIME_PACKAGE PARENT_PATH storage_ray_package_directory)
    target_compile_definitions(ludus_render_graph_ray_tests PRIVATE
        LUDUS_RG3_STORAGE_RAY_DIRECTORY="${storage_ray_package_directory}")
    add_test(NAME ludus_render_graph_ray_pixels
        COMMAND ludus_render_graph_ray_tests
            "~[ris-projected-solid-angle]~[polygonal-light-sampling]~[slice-sampling-luminaire]")
    set_tests_properties(ludus_render_graph_ray_pixels PROPERTIES LABELS "renderer;gpu;ray" TIMEOUT 60)
    # The ris-projected-solid-angle template's convergence and error checks render about 3100 frames and
    # need no native ray tracing, so they also run on software Vulkan adapters; they get their own timeout.
    add_test(NAME ludus_render_graph_ris_projected_solid_angle_pixels
        COMMAND ludus_render_graph_ray_tests "[ris-projected-solid-angle]")
    set_tests_properties(ludus_render_graph_ris_projected_solid_angle_pixels PROPERTIES
        LABELS "renderer;gpu;ray" TIMEOUT 600)
    # The polygonal-light-sampling template's comparisons render about 2100 compute-BVH frames (20 s on Apple M5)
    # and need no native ray tracing, so they also run on software Vulkan adapters.
    add_test(NAME ludus_render_graph_polygonal_light_sampling_pixels
        COMMAND ludus_render_graph_ray_tests "[polygonal-light-sampling]")
    set_tests_properties(ludus_render_graph_polygonal_light_sampling_pixels PROPERTIES
        LABELS "renderer;gpu;ray" TIMEOUT 600)
    # The slice-sampling-luminaire template's checks render about 1300 compute-BVH frames and need no native ray
    # tracing, so they also run on software Vulkan adapters.
    add_test(NAME ludus_render_graph_slice_sampling_luminaire_pixels
        COMMAND ludus_render_graph_ray_tests "[slice-sampling-luminaire]")
    set_tests_properties(ludus_render_graph_slice_sampling_luminaire_pixels PROPERTIES
        LABELS "renderer;gpu;ray" TIMEOUT 600)
    add_executable(ludus_graph_workgroup_tests tests/graph_workgroup_tests.cpp)
    target_link_libraries(ludus_graph_workgroup_tests PRIVATE Ludus::GraphicsRenderer
        Ludus::FoundationTime Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_graph_workgroup_tests)
    ludus_enable_test_exceptions(ludus_graph_workgroup_tests)
    ludus_compile_renderer_shaders(TARGET ludus_graph_workgroup_tests SHADERS renderer_composite)
    foreach(graph IN ITEMS compute_ray_tracing veach_guibas_mis)
        foreach(shape IN ITEMS linear tiled)
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/render_graphs/${graph}")
            set(staged "${CMAKE_CURRENT_BINARY_DIR}/rg2-inputs/workgroups_${graph}_${shape}")
            ludus_stage_render_graph_fixture("${source}" "${staged}" library.lurender.json)
            file(READ "${source}/library.lurender.json" library)
            if(shape STREQUAL tiled)
                string(REPLACE "\"state\": {}" "\"state\": {\"group_size\": [8, 8, 1]}" library "${library}")
            else()
                string(REPLACE "\"state\": {}" "\"state\": {\"group_size\": [64, 1, 1]}" library "${library}")
            endif()
            file(WRITE "${staged}.library.json" "${library}")
            file(COPY_FILE "${staged}.library.json" "${staged}/library.lurender.json" ONLY_IF_DIFFERENT)
            ludus_compile_render_graph(TARGET ludus_graph_workgroup_tests NAME "workgroups_${graph}_${shape}"
                MANIFEST "${staged}/main.lushader.json" RUNTIME_PACKAGE)
            target_compile_definitions(ludus_graph_workgroup_tests PRIVATE
                LUDUS_WORKGROUP_${graph}_${shape}="${workgroups_${graph}_${shape}_RUNTIME_PACKAGE}")
        endforeach()
    endforeach()
    add_test(NAME ludus_graph_workgroup_pixels COMMAND ludus_graph_workgroup_tests)
    # Both cases skip without native ray queries (lavapipe); Catch2 then returns 4, reported as
    # Skipped like ludus_render_graph_ray_path_capture below.
    set_tests_properties(ludus_graph_workgroup_pixels PROPERTIES
        LABELS "renderer;gpu;ray" TIMEOUT 120 SKIP_RETURN_CODE 4)
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
    ludus_stage_render_graph_fixture("${ray_path_source}" "${ray_path_staged}")
    set(raytracing_staged "${ray_staged}")
    foreach(ray_path_form IN ITEMS raytracing scene_ray visibility ordered_ray ray_path)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests NAME rg2_${ray_path_form}_plain
            MANIFEST "${${ray_path_form}_staged}/main.lushader.json")
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests NAME rg2_${ray_path_form}_capture
            MANIFEST "${${ray_path_form}_staged}/main.lushader.json" RAY_PATH_CAPTURE)
    endforeach()
    # The scene compute fixture's software traversal configurations capture without native rays.
    foreach(configuration IN ITEMS TriangleLoop ComputeBvh)
        string(TOLOWER "${configuration}" configuration_suffix)
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests
            NAME rg2_ray_path_${configuration_suffix}_plain
            MANIFEST "${ray_path_staged}/main.lushader.json" CONFIGURATION "${configuration}")
        ludus_compile_render_graph(TARGET ludus_render_graph_ray_path_tests
            NAME rg2_ray_path_${configuration_suffix}_capture
            MANIFEST "${ray_path_staged}/main.lushader.json" CONFIGURATION "${configuration}" RAY_PATH_CAPTURE)
    endforeach()
    add_test(NAME ludus_render_graph_ray_path_capture COMMAND ludus_render_graph_ray_path_tests)
    # Native-query cases skip on a device without hardware ray queries; the software
    # traversal cases still run there. Catch2 3.4 returns 4 only when every case skipped.
    # Thanks to Catch2, Session::runInternal (v3.4.0 src/catch2/catch_session.cpp), and
    # CMake 3.29 SKIP_RETURN_CODE for reporting that as Skipped rather than Failed:
    # https://cmake.org/cmake/help/v3.29/prop_test/SKIP_RETURN_CODE.html
    set_tests_properties(ludus_render_graph_ray_path_capture PROPERTIES
        LABELS "renderer;gpu;ray" TIMEOUT 60 SKIP_RETURN_CODE 4)
endif()
