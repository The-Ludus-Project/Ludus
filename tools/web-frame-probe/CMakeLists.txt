add_executable(ludus_web_frame_probe main.cpp)
target_include_directories(ludus_web_frame_probe PRIVATE "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/src")
target_compile_options(ludus_web_frame_probe PRIVATE "--use-port=emdawnwebgpu:cpp_bindings=false")
target_link_libraries(ludus_web_frame_probe PRIVATE Ludus::GraphicsRhi)
ludus_apply_project_defaults(ludus_web_frame_probe)
target_link_options(ludus_web_frame_probe PRIVATE -fno-exceptions -g0 -g2 -sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/node_gpu_fixture.js")
set_target_properties(ludus_web_frame_probe PROPERTIES OUTPUT_NAME index SUFFIX ".js")
set_property(TARGET ludus_web_frame_probe APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/node_gpu_fixture.js")
configure_file(shell.html index.html COPYONLY)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME ludus_web_frame_semantics COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_semantics.py" "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_web_frame_probe>")
set_tests_properties(ludus_web_frame_semantics PROPERTIES LABELS "web;rhi" TIMEOUT 120)

# A real-browser fixture proves offscreen preparation leaves the visible canvas
# untouched, while GPU readback proves the offscreen attachment was rendered.
add_executable(ludus_web_offscreen_probe offscreen.cpp)
target_link_libraries(ludus_web_offscreen_probe PRIVATE Ludus::GraphicsRhi Ludus::FoundationContainers)
ludus_apply_project_defaults(ludus_web_offscreen_probe)
target_link_options(ludus_web_offscreen_probe PRIVATE -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
set_target_properties(ludus_web_offscreen_probe PROPERTIES OUTPUT_NAME offscreen SUFFIX ".js")
configure_file(offscreen.html offscreen.html COPYONLY)
