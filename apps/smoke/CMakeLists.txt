# The lifecycle journal and the fixed-step simulation compile once and are linked
# by the application and by the tests that cover them (tools/check_build_graph.py).
# Object libraries pass their objects only to targets that link them directly.
add_library(ludus_smoke_lifecycle OBJECT lifecycle.cpp)
target_link_libraries(ludus_smoke_lifecycle PUBLIC Ludus::FoundationBase)
ludus_apply_project_defaults(ludus_smoke_lifecycle)
add_library(ludus_smoke_simulation OBJECT simulation.cpp)
target_link_libraries(ludus_smoke_simulation PUBLIC Ludus::FoundationBase Ludus::FoundationTime Ludus::Platform)
ludus_apply_project_defaults(ludus_smoke_simulation)

add_executable(ludus_smoke application.cpp)
target_link_libraries(ludus_smoke PRIVATE ludus_smoke_lifecycle ludus_smoke_simulation Ludus::FoundationBase Ludus::FoundationLogging
    Ludus::FoundationProfiling Ludus::FoundationTime Ludus::Platform Ludus::GraphicsRhi)
ludus_apply_project_defaults(ludus_smoke)
if(EMSCRIPTEN)
    target_sources(ludus_smoke PRIVATE main_web.cpp renderer_web.cpp)
    # The demo draws through the public rendering API with the SDK-generated
    # generic shader; no RHI-private headers or backend handles. WebGL 2 / full
    # GLES3 and the emdawnwebgpu port arrive via the Ludus::GraphicsRhi INTERFACE.
    target_link_options(ludus_smoke PRIVATE -fno-exceptions -g0 $<$<NOT:$<CONFIG:Release>>:-g2>
        -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
        -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=65536 -sASSERTIONS=1)
    set_target_properties(ludus_smoke PROPERTIES OUTPUT_NAME index SUFFIX ".js")
    configure_file(shell.html index.html COPYONLY)
    ludus_compile_shader(TARGET ludus_smoke NAME smoke SOURCE shaders/smoke.slang
        VERTEX vertexMain FRAGMENT fragmentMain INCLUDES shaders)

    # Controlled providers are linked into a separate node test executable only.
    add_executable(ludus_web_smoke_test application.cpp main_web.cpp renderer_web.cpp)
    target_link_libraries(ludus_web_smoke_test PRIVATE ludus_smoke_lifecycle ludus_smoke_simulation Ludus::GraphicsRhi Ludus::FoundationProfiling Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_web_smoke_test)
    target_link_options(ludus_web_smoke_test PRIVATE -fno-exceptions -g0 -g2 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/tests/node_gpu_fixture.js")
    set_target_properties(ludus_web_smoke_test PROPERTIES OUTPUT_NAME smoke-test SUFFIX ".js")
    set_property(TARGET ludus_web_smoke_test APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/node_gpu_fixture.js")
    ludus_compile_shader(TARGET ludus_web_smoke_test NAME smoke SOURCE shaders/smoke.slang
        VERTEX vertexMain FRAGMENT fragmentMain INCLUDES shaders)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ludus_web_smoke_semantics COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_semantics.py" "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_web_smoke_test>")
    set_tests_properties(ludus_web_smoke_semantics PROPERTIES LABELS "web;smoke" TIMEOUT 120)
else()
    target_sources(ludus_smoke PRIVATE main.cpp renderer_native.cpp text_demo.cpp)
    # Ludus::Text is the CPU font API (shaping + rasterization). It is built on
    # the native target only; the Emscripten/web build does not link it yet (the
    # FreeType/HarfBuzz web source-bootstrap is pending), so the text example is
    # native-only. The example reads a bundled OFL font; point it at the fixture.
    target_link_libraries(ludus_smoke PRIVATE Ludus::DiagnosticsIntegration Ludus::Text)
    target_compile_definitions(ludus_smoke PRIVATE
        "LUDUS_SMOKE_FONT_PATH=\"${PROJECT_SOURCE_DIR}/modules/text/tests/fixtures/NotoSans-Regular.ttf\"")
    if(LUDUS_BUILD_TESTS)
        find_package(Catch2 3 REQUIRED)
        add_executable(ludus_smoke_lifecycle_tests tests/lifecycle_tests.cpp)
        target_link_libraries(ludus_smoke_lifecycle_tests PRIVATE ludus_smoke_lifecycle Ludus::FoundationBase Catch2::Catch2WithMain)
        ludus_apply_project_defaults(ludus_smoke_lifecycle_tests)
        ludus_enable_test_exceptions(ludus_smoke_lifecycle_tests)
        add_test(NAME ludus_smoke_lifecycle_tests COMMAND ludus_smoke_lifecycle_tests)
        set_tests_properties(ludus_smoke_lifecycle_tests PROPERTIES LABELS "unit;smoke;lifecycle")
        add_executable(ludus_smoke_simulation_tests tests/simulation_tests.cpp)
        target_link_libraries(ludus_smoke_simulation_tests PRIVATE ludus_smoke_simulation Ludus::Platform Ludus::FoundationTime Catch2::Catch2WithMain)
        ludus_apply_project_defaults(ludus_smoke_simulation_tests)
        ludus_enable_test_exceptions(ludus_smoke_simulation_tests)
        add_test(NAME ludus_smoke_simulation_tests COMMAND ludus_smoke_simulation_tests)
        set_tests_properties(ludus_smoke_simulation_tests PROPERTIES LABELS "unit;smoke")
    endif()
endif()
