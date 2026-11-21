# The portable RHI does not expose comparison samplers. Exercise them on Metal with the
# same pinned Slang and SPIRV-Cross tools as the native raster binding-array cooker.
add_executable(ludus_lusl_texture_readback_tests metal_readback.mm)
set_target_properties(ludus_lusl_texture_readback_tests PROPERTIES OBJCXX_STANDARD 23
    OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
target_link_libraries(ludus_lusl_texture_readback_tests PRIVATE
    Ludus::FoundationContainers Catch2::Catch2WithMain "-framework Foundation" "-framework Metal")
target_compile_options(ludus_lusl_texture_readback_tests PRIVATE -fobjc-arc)
ludus_apply_project_defaults(ludus_lusl_texture_readback_tests)
ludus_enable_test_exceptions(ludus_lusl_texture_readback_tests)

set(lowered "${CMAKE_CURRENT_BINARY_DIR}/parity.lusl.slang")
add_custom_command(OUTPUT "${lowered}"
    COMMAND $<TARGET_FILE:luslc> lower --root "${CMAKE_CURRENT_SOURCE_DIR}" --output "${lowered}" parity.lusl
    DEPENDS luslc parity.lusl
    VERBATIM)
foreach(language IN ITEMS lusl slang)
    if(language STREQUAL "lusl")
        set(source "${lowered}")
    else()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/parity.slang")
    endif()
    set(artifact "${CMAKE_CURRENT_BINARY_DIR}/parity.${language}.metal")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/parity.${language}.spv")
    add_custom_command(OUTPUT "${artifact}" BYPRODUCTS "${spirv}"
        COMMAND "${LUDUS_SLANG_COMPILER}" "${source}" -entry Main -target spirv -o "${spirv}"
            -I "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/shaders"
        COMMAND "${LUDUS_SPIRV_CROSS}" "${spirv}" --msl --msl-version 24000 --msl-decoration-binding --output "${artifact}"
        DEPENDS "${source}" "${LUDUS_SLANG_COMPILER}" "${LUDUS_SPIRV_CROSS}"
            "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/shaders/raster_bindings.slang"
        VERBATIM)
    target_sources(ludus_lusl_texture_readback_tests PRIVATE "${artifact}")
    set_source_files_properties("${artifact}" PROPERTIES HEADER_FILE_ONLY TRUE)
    string(TOUPPER "${language}" language_upper)
    target_compile_definitions(ludus_lusl_texture_readback_tests PRIVATE
        LUDUS_TEXTURE_PARITY_${language_upper}="${artifact}")
endforeach()

add_test(NAME ludus_lusl_texture_readback_tests COMMAND ludus_lusl_texture_readback_tests)
set_tests_properties(ludus_lusl_texture_readback_tests PROPERTIES LABELS "gpu;shader" TIMEOUT 60 SKIP_RETURN_CODE 4)
