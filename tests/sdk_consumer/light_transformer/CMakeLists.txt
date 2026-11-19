cmake_minimum_required(VERSION 3.29)
project(LudusInstalledLightTransformer LANGUAGES CXX)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer FoundationTime)
find_package(Catch2 CONFIG REQUIRED)
enable_testing()
set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(generator "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/graphics/renderer/tests")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${fixtures}/encoder.llte" "${fixtures}/oracle.bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixtures}"
    COMMAND "${Python3_EXECUTABLE}" "${generator}/light_transformer_encoder_fixture.py"
        --encoder "${fixtures}/encoder.llte" --fixture "${fixtures}/oracle.bin"
    DEPENDS "${generator}/light_transformer_encoder_fixture.py"
        "${generator}/light_transformer_encoder_golden.json" "${generator}/light_transformer_fixture.py"
        "${generator}/../../../../scripts/python/ludus_tools/light_transformer_encoder.py"
    VERBATIM)
add_executable(ludus_sdk_encoder "${generator}/light_transformer_encoder_tests.cpp" "${fixtures}/oracle.bin")
target_compile_features(ludus_sdk_encoder PRIVATE cxx_std_23)
target_compile_definitions(ludus_sdk_encoder PRIVATE
    "LUDUS_LIGHT_TRANSFORMER_ENCODER_ASSET=\"${fixtures}/encoder.llte\""
    "LUDUS_LIGHT_TRANSFORMER_ENCODER_FIXTURE=\"${fixtures}/oracle.bin\"")
target_link_libraries(ludus_sdk_encoder PRIVATE Ludus::GraphicsRenderer Ludus::FoundationTime Catch2::Catch2WithMain)
ludus_compile_renderer_shaders(TARGET ludus_sdk_encoder SHADERS renderer_light_transformer_encoder)
add_test(NAME installed_encoder COMMAND ludus_sdk_encoder)
