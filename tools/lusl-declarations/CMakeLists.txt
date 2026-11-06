# lusl-declarations: Slang declarations spelled as LUSL, from which cmake/shaders/generate_luslh.py
# generates the SDK's .luslh headers (docs/modules/tooling/shader-language.md#sdk-declarations).
# It needs the Slang-linked compiler, which only shader-worker builds produce.
add_executable(lusl-declarations main.cpp)
target_link_libraries(lusl-declarations PRIVATE Ludus::ShaderCompiler Ludus::FoundationFilesystem
    Ludus::FoundationStrings)
ludus_apply_project_defaults(lusl-declarations)
# The Slang library loads its SPIR-V optimizer beside itself; name its directory as the worker does.
set_target_properties(lusl-declarations PROPERTIES BUILD_RPATH "${LUDUS_SLANG_ROOT}/lib")
if(APPLE)
    target_compile_options(lusl-declarations PRIVATE -mmacosx-version-min=15.0)
    target_link_options(lusl-declarations PRIVATE -mmacosx-version-min=15.0)
endif()
if(LUDUS_BUILD_TESTS)
    # The SDK's .luslh headers are generated from their templates; a stale header fails here.
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME lusl_declarations_tests COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/generate_luslh.py" --check --tool "$<TARGET_FILE:lusl-declarations>"
        "${PROJECT_SOURCE_DIR}/modules/foundation/math/shaders/math.luslh.in"
        "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/ray_shader.luslh.in"
        "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/scene_shader.luslh.in"
        "${PROJECT_SOURCE_DIR}/modules/graphics/renderer/shaders/scene_payload.luslh.in")
    set_tests_properties(lusl_declarations_tests PROPERTIES LABELS "unit;shader;tooling" TIMEOUT 120)
endif()
