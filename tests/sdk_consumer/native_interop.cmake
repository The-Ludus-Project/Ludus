# Public-only native sharing fixture, reused by source and installed-SDK builds.
if(APPLE AND NOT CMAKE_OBJCXX_COMPILER_LOADED)
    if(NOT CMAKE_OBJCXX_COMPILER)
        set(CMAKE_OBJCXX_COMPILER "${CMAKE_CXX_COMPILER}")
    endif()
    if(NOT CMAKE_OBJCXX_FLAGS)
        set(CMAKE_OBJCXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif()
    enable_language(OBJCXX)
endif()

function(ludus_add_native_interop_fixture target fixture_root)
    add_executable(${target} "${fixture_root}/native_interop.cpp")
    target_link_libraries(${target} PRIVATE Ludus::GraphicsRhi Ludus::FoundationLogging)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    if(APPLE)
        target_sources(${target} PRIVATE "${fixture_root}/native_interop_metal.mm")
        set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;-fno-objc-exceptions>)
        target_link_libraries(${target} PRIVATE "-framework Metal")
    else()
        find_package(volk CONFIG REQUIRED)
        target_sources(${target} PRIVATE "${fixture_root}/native_interop_vulkan.cpp")
        # Vulkan create-info records intentionally zero omitted native fields;
        # match the RHI's private designated-initializer policy at this boundary.
        set_source_files_properties("${fixture_root}/native_interop_vulkan.cpp" PROPERTIES
            COMPILE_OPTIONS "-Wno-missing-field-initializers")
        target_link_libraries(${target} PRIVATE volk::volk_headers ${CMAKE_DL_LIBS})
    endif()
    ludus_compile_shader(TARGET ${target} NAME native_source RASTER
        SOURCE "${fixture_root}/shaders/native_interop.lusl" VERTEX VertexMain FRAGMENT SourceMain)
    ludus_compile_shader(TARGET ${target} NAME native_sample RASTER
        SOURCE "${fixture_root}/shaders/native_interop.lusl" VERTEX VertexMain FRAGMENT SampleMain)
endfunction()
