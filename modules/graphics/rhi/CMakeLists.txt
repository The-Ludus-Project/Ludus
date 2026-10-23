if(NOT EMSCRIPTEN AND NOT APPLE)
    find_package(volk CONFIG REQUIRED)
endif()

add_library(ludus_graphics_rhi STATIC)
add_library(Ludus::GraphicsRhi ALIAS ludus_graphics_rhi)

set_target_properties(ludus_graphics_rhi PROPERTIES
    OUTPUT_NAME ludus_graphics_rhi
    EXPORT_NAME GraphicsRhi
)

target_sources(ludus_graphics_rhi
    PRIVATE
        src/rhi.cpp
        src/device.cpp
        src/gpu_scope.cpp
        src/gpu_pass_timing_buffer.cpp
        src/profile.cpp
        src/raster.cpp
        src/hosted_consumer.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES include/ludus/graphics/rhi/native_compute.h include/ludus/graphics/rhi/gpu_scope.h include/ludus/graphics/rhi/native_interop.h include/ludus/graphics/rhi/hosted_surface.h include/ludus/graphics/rhi/measurement.h include/ludus/graphics/rhi/ray.h include/ludus/graphics/rhi/profile.h include/ludus/graphics/rhi/compute.h include/ludus/graphics/rhi/graph.h include/ludus/graphics/rhi/lifetime.h include/ludus/graphics/rhi/rhi.h include/ludus/graphics/rhi/render.h include/ludus/graphics/rhi/device.h include/ludus/graphics/rhi/raster.h
)

target_link_libraries(ludus_graphics_rhi
    PRIVATE Ludus::FoundationStrings Ludus::FoundationMath Ludus::FoundationTime
    PUBLIC
        Ludus::FoundationBase
        Ludus::FoundationContainers
        Ludus::Platform
    PRIVATE
        Ludus::FoundationLogging
)
if(NOT EMSCRIPTEN)
    target_link_libraries(ludus_graphics_rhi PRIVATE Ludus::FoundationThreading)
endif()
if(EMSCRIPTEN)
    # Browser Auto build: both web sub-backends plus the dispatcher are linked
    # into one artifact, each sub-backend in its own leaf namespace, and the
    # dispatcher provides backend:: with the WebGPU->WebGL 2 fallback policy.
    target_sources(ludus_graphics_rhi PRIVATE src/rhi_web.cpp src/rhi_webgpu.cpp src/rhi_webgl.cpp src/ray_unsupported.cpp src/native_interop_unsupported.cpp src/hosted_consumer_unsupported.cpp)
    set_source_files_properties(src/rhi_webgpu.cpp PROPERTIES
        COMPILE_DEFINITIONS "LUDUS_RHI_WEBGPU_NAMESPACE=webgpu")
    set_source_files_properties(src/rhi_webgl.cpp PROPERTIES
        COMPILE_DEFINITIONS "LUDUS_RHI_WEBGL_NAMESPACE=webgl")
    target_compile_options(ludus_graphics_rhi PRIVATE "--use-port=emdawnwebgpu:cpp_bindings=false")
    # Propagate both browser backends' link requirements to installed consumers:
    # the emdawnwebgpu port (WebGPU) and the WebGL 2 / full GLES3 settings. An
    # Auto build therefore carries both browser paths.
    target_link_options(ludus_graphics_rhi INTERFACE
        "--use-port=emdawnwebgpu:cpp_bindings=false"
        "-sMAX_WEBGL_VERSION=2"
        "-sMIN_WEBGL_VERSION=2"
        "-sFULL_ES3=1")
elseif(APPLE)
    target_sources(ludus_graphics_rhi PRIVATE src/rhi_metal.mm src/hosted_metal.mm src/hosted_consumer_metal.mm)
    target_link_libraries(ludus_graphics_rhi PRIVATE
        "-framework Metal" "-framework QuartzCore" "-framework AppKit" "-framework IOSurface" "-framework CoreFoundation")
    ludus_configure_cocoa_target(ludus_graphics_rhi)
else()
    target_sources(ludus_graphics_rhi PRIVATE src/rhi_vulkan.cpp src/volk_impl.cpp src/vma.cpp src/hosted_vulkan.cpp src/hosted_consumer_vulkan.cpp)
    target_link_libraries(ludus_graphics_rhi PRIVATE volk::volk_headers)
    file(READ "${PROJECT_SOURCE_DIR}/config/vulkan_memory_allocator.json" vma_pin)
    string(JSON vma_header_sha GET "${vma_pin}" sha256 "vk_mem_alloc.h")
    file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/vma/vk_mem_alloc.h" vma_actual_sha)
    if(NOT vma_actual_sha STREQUAL vma_header_sha)
        message(FATAL_ERROR "Vendored Vulkan Memory Allocator differs from its reviewed pin")
    endif()
    string(JSON vma_license_sha GET "${vma_pin}" sha256 "LICENSE.txt")
    file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/vma/LICENSE.txt" vma_license_actual)
    if(NOT vma_license_actual STREQUAL vma_license_sha)
        message(FATAL_ERROR "Vendored Vulkan Memory Allocator license differs from its reviewed pin")
    endif()
    target_include_directories(ludus_graphics_rhi SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/vma")
    target_compile_definitions(ludus_graphics_rhi PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1 VMA_VULKAN_VERSION=1002000)
    target_compile_definitions(ludus_graphics_rhi PRIVATE VK_NO_PROTOTYPES)
    if(LUDUS_PLATFORM_ENABLE_WAYLAND)
        target_compile_definitions(ludus_graphics_rhi PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
    endif()
endif()
ludus_apply_project_defaults(ludus_graphics_rhi)

# Vulkan structs intentionally use partial designated initializers; omitted output
# fields are zero-initialized. Keep this exception private to the RHI target.
set_target_properties(ludus_graphics_rhi PROPERTIES LUDUS_ALLOW_PARTIAL_INITIALIZERS ON)

install(TARGETS ludus_graphics_rhi
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS AND NOT APPLE)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_graphics_rhi_tests tests/rhi_tests.cpp)
    target_include_directories(ludus_graphics_rhi_tests PRIVATE src)
    target_link_libraries(ludus_graphics_rhi_tests PRIVATE
        Ludus::GraphicsRhi Ludus::FoundationLogging volk::volk_headers Catch2::Catch2WithMain)
    if(LUDUS_PLATFORM_ENABLE_WAYLAND)
        target_compile_definitions(ludus_graphics_rhi_tests PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
    endif()
    ludus_apply_project_defaults(ludus_graphics_rhi_tests)
    ludus_enable_test_exceptions(ludus_graphics_rhi_tests)
    add_test(NAME ludus_graphics_rhi_tests COMMAND ludus_graphics_rhi_tests)
    set_tests_properties(ludus_graphics_rhi_tests PROPERTIES TIMEOUT 60)
endif()

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_rhi_lifecycle_tests tests/lifecycle_tests.cpp tests/gpu_scope_tests.cpp tests/device_tests.cpp tests/profile_tests.cpp tests/raster_tests.cpp tests/binding_tests.cpp tests/native_interop_tests.cpp tests/compute_tests.cpp tests/hosted_ring_tests.cpp tests/hosted_adapter_tests.cpp tests/reference_raster.cpp src/rhi.cpp src/device.cpp src/gpu_scope.cpp src/gpu_pass_timing_buffer.cpp src/profile.cpp src/raster.cpp src/ray_unsupported.cpp)
    target_compile_definitions(ludus_rhi_lifecycle_tests PRIVATE LUDUS_RHI_TEST_STORAGE=1)
    target_include_directories(ludus_rhi_lifecycle_tests PRIVATE include src)
    target_link_libraries(ludus_rhi_lifecycle_tests PRIVATE Ludus::Platform Ludus::FoundationContainers Ludus::FoundationStrings Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_rhi_lifecycle_tests)
    ludus_enable_test_exceptions(ludus_rhi_lifecycle_tests)
    add_test(NAME ludus_rhi_lifecycle_tests COMMAND ludus_rhi_lifecycle_tests)
    set_tests_properties(ludus_rhi_lifecycle_tests PROPERTIES LABELS "unit;rhi;build-contract")
endif()

if(LUDUS_BUILD_TESTS AND NOT APPLE AND LUDUS_SLANG_COMPILER AND LUDUS_SPIRV_VALIDATOR)
    target_compile_definitions(ludus_graphics_rhi PRIVATE LUDUS_RHI_TEST_READBACK=1)
    add_executable(ludus_rhi_render_tests tests/render_tests.cpp)
    target_include_directories(ludus_rhi_render_tests PRIVATE src)
    target_link_libraries(ludus_rhi_render_tests PRIVATE Ludus::GraphicsRhi Ludus::FoundationLogging Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_rhi_render_tests)
    ludus_enable_test_exceptions(ludus_rhi_render_tests)
    ludus_compile_shader(TARGET ludus_rhi_render_tests NAME diagnostic
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/diagnostic.slang"
        VERTEX vertexMain FRAGMENT fragmentMain)
    add_test(NAME ludus_rhi_render_pixels COMMAND ludus_rhi_render_tests)
    set_tests_properties(ludus_rhi_render_pixels PROPERTIES LABELS "rhi;gpu" TIMEOUT 60)
endif()

if(LUDUS_BUILD_TESTS AND APPLE)
    target_compile_definitions(ludus_graphics_rhi PRIVATE LUDUS_RHI_TEST_READBACK=1)
    add_executable(ludus_rhi_metal_tests tests/metal_tests.mm)
    target_include_directories(ludus_rhi_metal_tests PRIVATE src)
    target_link_libraries(ludus_rhi_metal_tests PRIVATE Ludus::GraphicsRhi Catch2::Catch2WithMain "-framework Metal" "-framework QuartzCore")
    ludus_apply_project_defaults(ludus_rhi_metal_tests)
    ludus_configure_cocoa_target(ludus_rhi_metal_tests)
    ludus_enable_test_exceptions(ludus_rhi_metal_tests)
    add_test(NAME ludus_rhi_metal_tests COMMAND ludus_rhi_metal_tests)
    set_tests_properties(ludus_rhi_metal_tests PROPERTIES LABELS "unit;rhi;macos" TIMEOUT 60)
    if(LUDUS_SLANG_COMPILER)
        add_executable(ludus_rhi_render_tests tests/render_tests.cpp)
        target_include_directories(ludus_rhi_render_tests PRIVATE src)
        target_compile_definitions(ludus_rhi_render_tests PRIVATE LUDUS_TEST_METAL=1)
        target_link_libraries(ludus_rhi_render_tests PRIVATE Ludus::GraphicsRhi Ludus::FoundationLogging Catch2::Catch2WithMain)
        ludus_apply_project_defaults(ludus_rhi_render_tests)
        ludus_enable_test_exceptions(ludus_rhi_render_tests)
        ludus_compile_shader(TARGET ludus_rhi_render_tests NAME diagnostic
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/diagnostic.slang"
            VERTEX vertexMain FRAGMENT fragmentMain)
        add_test(NAME ludus_rhi_render_pixels COMMAND ludus_rhi_render_tests)
        set_tests_properties(ludus_rhi_render_pixels PROPERTIES LABELS "rhi;gpu;macos" TIMEOUT 60)
    endif()
endif()

if(LUDUS_BUILD_TESTS AND (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND NOT EMSCRIPTEN)
    # Hosted surface: an ordinary RHI application renders into a hosted window
    # whose endpoint and consumer live in the test process.
    add_executable(ludus_rhi_hosted_producer tests/hosted_producer.cpp)
    target_link_libraries(ludus_rhi_hosted_producer PRIVATE Ludus::GraphicsRhi Ludus::Platform Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_hosted_producer)
endif()

if(LUDUS_BUILD_TESTS AND APPLE)
    add_executable(ludus_rhi_hosted_surface_tests tests/hosted_surface_tests.mm)
    target_compile_definitions(ludus_rhi_hosted_surface_tests PRIVATE
        LUDUS_HOSTED_PRODUCER="$<TARGET_FILE:ludus_rhi_hosted_producer>")
    add_dependencies(ludus_rhi_hosted_surface_tests ludus_rhi_hosted_producer)
    target_link_libraries(ludus_rhi_hosted_surface_tests PRIVATE
        Ludus::GraphicsRhi Ludus::Platform Ludus::FoundationStrings Ludus::FoundationTime
        Catch2::Catch2WithMain "-framework Metal")
    ludus_apply_project_defaults(ludus_rhi_hosted_surface_tests)
    ludus_configure_cocoa_target(ludus_rhi_hosted_surface_tests)
    ludus_enable_test_exceptions(ludus_rhi_hosted_surface_tests)
    add_test(NAME ludus_rhi_hosted_surface_tests COMMAND ludus_rhi_hosted_surface_tests)
    set_tests_properties(ludus_rhi_hosted_surface_tests PROPERTIES LABELS "rhi;gpu;macos" TIMEOUT 120)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
    add_executable(ludus_rhi_hosted_surface_tests tests/hosted_surface_vulkan_tests.cpp)
    target_include_directories(ludus_rhi_hosted_surface_tests PRIVATE src)
    target_compile_definitions(ludus_rhi_hosted_surface_tests PRIVATE
        VK_NO_PROTOTYPES LUDUS_HOSTED_PRODUCER="$<TARGET_FILE:ludus_rhi_hosted_producer>")
    add_dependencies(ludus_rhi_hosted_surface_tests ludus_rhi_hosted_producer)
    target_link_libraries(ludus_rhi_hosted_surface_tests PRIVATE
        Ludus::GraphicsRhi Ludus::Platform Ludus::FoundationStrings Ludus::FoundationTime
        volk::volk_headers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_rhi_hosted_surface_tests)
    ludus_enable_test_exceptions(ludus_rhi_hosted_surface_tests)
    add_test(NAME ludus_rhi_hosted_surface_tests COMMAND ludus_rhi_hosted_surface_tests)
    set_tests_properties(ludus_rhi_hosted_surface_tests PROPERTIES LABELS "rhi;gpu;linux" TIMEOUT 120)
endif()

if(TARGET ludus_rhi_render_tests)
    target_sources(ludus_rhi_render_tests PRIVATE tests/raster_render_tests.cpp)
    target_include_directories(ludus_rhi_render_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    ludus_compile_shader(TARGET ludus_rhi_render_tests NAME cull
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/cull.slang" COMPUTE computeMain)
    ludus_compile_shader(TARGET ludus_rhi_render_tests NAME raster RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/raster.slang"
        VERTEX vertexMain FRAGMENT fragmentMain)
endif()

if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN AND LUDUS_SLANG_COMPILER AND (APPLE OR LUDUS_SPIRV_VALIDATOR))
    include("${PROJECT_SOURCE_DIR}/tests/sdk_consumer/native_interop.cmake")
    ludus_add_native_interop_fixture(ludus_rhi_native_interop_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    ludus_apply_project_defaults(ludus_rhi_native_interop_consumer)
    add_test(NAME ludus_rhi_native_interop_consumer COMMAND ludus_rhi_native_interop_consumer)
    set_tests_properties(ludus_rhi_native_interop_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 60)
    add_executable(ludus_rhi_r6_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r6.cpp")
    target_link_libraries(ludus_rhi_r6_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r6_consumer)
    ludus_compile_shader(TARGET ludus_rhi_r6_consumer NAME cull
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/cull.slang" COMPUTE computeMain)
    ludus_compile_shader(TARGET ludus_rhi_r6_consumer NAME raster RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/raster.slang" VERTEX vertexMain FRAGMENT fragmentMain)
    add_executable(ludus_rhi_r8_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r8.cpp")
    target_link_libraries(ludus_rhi_r8_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r8_consumer)
    ludus_compile_shader(TARGET ludus_rhi_r8_consumer NAME raster RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/raster.slang" VERTEX vertexMain FRAGMENT fragmentMain)
    add_executable(ludus_rhi_r11_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r11.cpp")
    target_link_libraries(ludus_rhi_r11_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r11_consumer)
    ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11compute SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11compute.slang" COMPUTE ComputeMain)
    ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11scalar RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11.slang" VERTEX VertexMain FRAGMENT FragmentMain
        INCLUDES "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/shaders")
    # Native binding-array shaders require the explicitly configured translator.
    # Setup/analysis configurations still compile and exercise the dynamic profile;
    # native SDK/GPU CI supplies the pinned translator and qualifies both profiles.
    if(LUDUS_SPIRV_CROSS)
        target_compile_definitions(ludus_rhi_r11_consumer PRIVATE LUDUS_R11_NATIVE_ARRAYS=1)
        foreach(entry IN ITEMS arrays constant)
            if(entry STREQUAL "arrays")
                set(fragment_entry FragmentMain)
            else()
                set(fragment_entry ConstantArrayMain)
            endif()
            ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11${entry} RASTER NATIVE_BINDING_ARRAYS
                SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11.slang" VERTEX VertexMain FRAGMENT ${fragment_entry}
                INCLUDES "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/shaders" DEFINES LUDUS_R11_ARRAYS=1)
        endforeach()
    endif()
    if(LUDUS_SPIRV_CROSS AND NOT EMSCRIPTEN)
        foreach(shape IN ITEMS cube volume)
            string(TOUPPER "${shape}" shape_define)
            ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11${shape} RASTER NATIVE_BINDING_ARRAYS
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11.slang" VERTEX VertexMain FRAGMENT ConstantArrayMain
            DEFINES LUDUS_R11_ARRAYS=1 LUDUS_R11C_WIDER=1 LUDUS_R11C_${shape_define}=1)
        endforeach()
        ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11wider RASTER NATIVE_BINDING_ARRAYS
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11.slang" VERTEX VertexMain FRAGMENT ConstantArrayMain
        DEFINES LUDUS_R11_ARRAYS=1 LUDUS_R11C_WIDER=1)
        if(NOT APPLE)
            target_compile_definitions(ludus_rhi_r11_consumer PRIVATE LUDUS_R11C_RUNTIME=1)
            ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11buffers RASTER NATIVE_BINDING_ARRAYS
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11buffers.slang" VERTEX VertexMain FRAGMENT FragmentMain)
            ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11storage NATIVE_BINDING_ARRAYS
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11storage.slang" COMPUTE ComputeMain)
            ludus_compile_shader(TARGET ludus_rhi_r11_consumer NAME r11runtime RASTER NATIVE_BINDING_ARRAYS RUNTIME_BINDING_ARRAYS
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r11.slang" VERTEX VertexMain FRAGMENT ConstantArrayMain
            DEFINES LUDUS_R11_ARRAYS=1 LUDUS_R11C_RUNTIME=1)
        endif()
    endif()
    add_test(NAME ludus_rhi_r11_consumer COMMAND ludus_rhi_r11_consumer)
    set_tests_properties(ludus_rhi_r11_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
    add_executable(ludus_rhi_r10_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r10.cpp")
    target_link_libraries(ludus_rhi_r10_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r10_consumer)
    foreach(entry write mask sample depth stencil)
        ludus_compile_shader(TARGET ludus_rhi_r10_consumer NAME r10${entry} RASTER
            SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r10.slang" VERTEX vertexMain FRAGMENT ${entry}Main)
    endforeach()
    add_test(NAME ludus_rhi_r10_consumer COMMAND ludus_rhi_r10_consumer)
    set_tests_properties(ludus_rhi_r10_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
    add_executable(ludus_rhi_r18c_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r18c.cpp")
    target_link_libraries(ludus_rhi_r18c_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r18c_consumer)
    ludus_compile_shader(TARGET ludus_rhi_r18c_consumer NAME r18c
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r18c.slang" COMPUTE ComputeMain)
    add_test(NAME ludus_rhi_r18c_consumer COMMAND ludus_rhi_r18c_consumer)
    set_tests_properties(ludus_rhi_r18c_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 60)
    add_executable(ludus_rhi_r9_consumer "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/r9.cpp")
    target_link_libraries(ludus_rhi_r9_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_rhi_r9_consumer)
    ludus_compile_shader(TARGET ludus_rhi_r9_consumer NAME r9 RASTER
        SOURCE "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/shaders/r9.slang" VERTEX vertexMain FRAGMENT fragmentMain)
    add_test(NAME ludus_rhi_r9_consumer COMMAND ludus_rhi_r9_consumer)
    set_tests_properties(ludus_rhi_r9_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
    add_test(NAME ludus_rhi_r8_consumer COMMAND ludus_rhi_r8_consumer)
    set_tests_properties(ludus_rhi_r8_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
    add_test(NAME ludus_rhi_r6_consumer COMMAND ludus_rhi_r6_consumer)
    set_tests_properties(ludus_rhi_r6_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
endif()

if(TARGET ludus_rhi_render_tests AND LUDUS_SPIRV_CROSS)
    add_executable(ludus_rhi_ray_tests tests/ray_tests.cpp)
    target_link_libraries(ludus_rhi_ray_tests PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_rhi_ray_tests)
    ludus_enable_test_exceptions(ludus_rhi_ray_tests)
    if(APPLE)
        target_compile_definitions(ludus_rhi_ray_tests PRIVATE LUDUS_TEST_METAL=1)
    endif()
    ludus_compile_ray_queries(TARGET ludus_rhi_ray_tests NAME ray_query)
    add_test(NAME ludus_rhi_ray_queries COMMAND ludus_rhi_ray_tests)
    set_tests_properties(ludus_rhi_ray_queries PROPERTIES LABELS "rhi;gpu;ray" TIMEOUT 60)
endif()
