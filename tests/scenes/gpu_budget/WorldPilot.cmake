# Copied into a generated game project by package.py. Only game code and its
# room-scatter helper build here. Platform, RHI, Renderer and shaders use the SDK.
set(world "${pilot_root}/apps/world_demo")
set(scatter "${pilot_root}/modules/content/procedural/environment")
file(READ "${world}/levels/first-room.world.json" LUDUS_FIRST_ROOM)
file(READ "${world}/levels/second-room.world.json" LUDUS_SECOND_ROOM)
configure_file("${world}/levels.h.in" "${CMAKE_CURRENT_BINARY_DIR}/levels.h" @ONLY)
target_sources(MetalBudget PRIVATE
    "${world}/level.cpp" "${world}/world_level.cpp" "${world}/world.cpp"
    "${world}/frame_renderer.cpp" "${world}/camera_view.cpp" "${world}/render_items.cpp"
    "${scatter}/src/scatter.cpp")
target_include_directories(MetalBudget PRIVATE "${world}" "${scatter}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(MetalBudget PRIVATE Ludus::GameplayWorld Ludus::GameplayCamera Ludus::FoundationParsingJson
    Ludus::FoundationProfiling Ludus::FoundationThreading Ludus::GraphicsRenderer Ludus::Platform)
target_compile_options(MetalBudget PRIVATE -ffp-contract=off -Wall -Wextra -Wpedantic -Werror)
set(LUDUS_SLANG_COMPILER "${pilot_root}/out/shader-tools/slang/bin/slangc")
ludus_compile_renderer_shaders(TARGET MetalBudget SHADERS renderer_flat renderer_composite renderer_output renderer_shadow)
