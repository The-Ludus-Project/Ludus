# Opt-in tools target only. No install/export rules and no runtime compiler link.
if(EMSCRIPTEN)
    message(FATAL_ERROR "The shader probe uses a standalone browser harness; native target requires Vulkan")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(volk CONFIG REQUIRED)
set(shader_output "${PROJECT_BINARY_DIR}/tools/shader-probe/generated")
add_custom_command(
    OUTPUT "${shader_output}/shaders.h"
    BYPRODUCTS "${shader_output}/probe.wgsl" "${shader_output}/probe.vertex.spv"
               "${shader_output}/probe.fragment.spv" "${shader_output}/contract.json"
               "${shader_output}/vertex.reflection.json" "${shader_output}/fragment.reflection.json"
               "${shader_output}/wgsl.reflection.json" "${shader_output}/probe.vertex.spvasm"
               "${shader_output}/probe.fragment.spvasm" "${shader_output}/build-evidence.json"
               "${shader_output}/index.html" "${shader_output}/browser.js"
               "${shader_output}/probe.lusl.slang" "${shader_output}/probe_slots.h" "${shader_output}/lusl.d"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/shader-probe"
            compile --output "${shader_output}" --luslc "$<TARGET_FILE:luslc>"
    DEPENDS probe.lusl luslc "${PROJECT_SOURCE_DIR}/scripts/shader-probe"
            "${PROJECT_SOURCE_DIR}/cmake/shaders/lower_lusl.py"
            "${PROJECT_SOURCE_DIR}/config/shader_toolchain.json" browser.js index.html
    VERBATIM
)
add_executable(ludus_shader_probe native.cpp "${shader_output}/shaders.h")
target_include_directories(ludus_shader_probe PRIVATE "${shader_output}")
target_link_libraries(ludus_shader_probe PRIVATE Ludus::GraphicsRhi Ludus::FoundationLogging volk::volk_headers)
ludus_apply_project_defaults(ludus_shader_probe)
add_test(NAME ludus_shader_probe_pixels COMMAND ludus_shader_probe)
set_tests_properties(ludus_shader_probe_pixels PROPERTIES LABELS "probe;gpu" TIMEOUT 60)
add_test(NAME ludus_shader_probe_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_contract.py" "${shader_output}")
set_tests_properties(ludus_shader_probe_contract PROPERTIES LABELS "probe;build-contract" TIMEOUT 30)
