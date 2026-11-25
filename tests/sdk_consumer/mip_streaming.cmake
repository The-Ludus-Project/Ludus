function(ludus_add_mip_streaming_consumer target)
    set(fixture "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_executable(${target} "${fixture}/mip_streaming.cpp")
    target_link_libraries(${target} PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime Ludus::FoundationThreading)
    ludus_compile_shader(TARGET ${target} NAME mip_streaming RASTER
        SOURCE "${fixture}/shaders/mip_streaming.slang" VERTEX VertexMain FRAGMENT FragmentMain)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 60)
endfunction()
