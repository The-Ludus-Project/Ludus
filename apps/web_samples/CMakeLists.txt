# CI builds genuine sample sources into static players; the browser needs no SDK.
include(LudusReflection)
ludus_require_python("Web sample cooking")
ludus_generate_reflection(TARGET sample_game_reflection
    SCHEMA "${PROJECT_SOURCE_DIR}/examples/live-edit-game/body.schema.json"
    BASELINE "${PROJECT_SOURCE_DIR}/examples/live-edit-game/body.schema.baseline.json")
add_executable(ludus_web_live_edit game_player.cpp "${PROJECT_SOURCE_DIR}/examples/live-edit-game/src/game.cpp")
target_link_libraries(ludus_web_live_edit PRIVATE sample_game_reflection Ludus::GamePropertyBinding)

set(scripted "${PROJECT_SOURCE_DIR}/examples/scripted-game/behaviors")
set(cooked "${CMAKE_CURRENT_BINARY_DIR}/encounter")
add_custom_target(ludus_web_encounter_cook
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/behavior_cook.py"
        --contract "${scripted}/contract.json" --package "${scripted}/package.json" --output "${cooked}"
        --profile "${PROJECT_BINARY_DIR}/modules/runtime/behavior/profile.json"
        --compiler "${PROJECT_SOURCE_DIR}/out/luau-probe/host-compiler/luau-compile"
        --analyzer "${PROJECT_SOURCE_DIR}/out/luau-probe/host-compiler/luau-analyze"
    BYPRODUCTS "${cooked}/contract.h" "${cooked}/package.h" "${cooked}/debug_maps.h"
    VERBATIM)
add_executable(ludus_web_scripted game_player.cpp "${PROJECT_SOURCE_DIR}/examples/scripted-game/src/game.cpp")
add_dependencies(ludus_web_scripted ludus_web_encounter_cook)
target_include_directories(ludus_web_scripted PRIVATE "${cooked}")
target_link_libraries(ludus_web_scripted PRIVATE Ludus::Behavior)

option(LUDUS_BUILD_WEB_CORNELL "Build the shader-tool-dependent Cornell web player" ON)
if(LUDUS_BUILD_WEB_CORNELL)
    add_executable(ludus_web_cornell "${PROJECT_SOURCE_DIR}/examples/cornell-box/src/main.cpp"
        "${PROJECT_SOURCE_DIR}/examples/cornell-box/src/camera.cpp")
    target_include_directories(ludus_web_cornell PRIVATE "${PROJECT_SOURCE_DIR}/examples/cornell-box/src")
    target_link_libraries(ludus_web_cornell PRIVATE Ludus::GameplayCamera)
    ludus_compile_shader(TARGET ludus_web_cornell NAME cornell
        SOURCE "${PROJECT_SOURCE_DIR}/examples/cornell-box/shaders/cornell.lusl" VERTEX VertexMain FRAGMENT FragmentMain)
    target_link_options(ludus_web_cornell PRIVATE -sASYNCIFY=1)
endif()

foreach(target ludus_web_live_edit ludus_web_scripted ludus_web_cornell)
    if(NOT TARGET ${target})
        continue()
    endif()
    ludus_apply_project_defaults(${target})
    target_link_libraries(${target} PRIVATE Ludus::FoundationBase Ludus::FoundationLogging
        Ludus::Platform Ludus::GraphicsRhi Ludus::GameApi)
    target_compile_definitions(${target} PRIVATE LUDUS_GAME_STATIC_DISPATCH=1 LUDUS_EXAMPLE_IDENTITY="web-sample")
    target_link_options(${target} PRIVATE -g0 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1
        -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=268435456)
    set_target_properties(${target} PROPERTIES SUFFIX ".js")
endforeach()
foreach(pair "ludus_web_live_edit;live-edit-game" "ludus_web_scripted;scripted-game" "ludus_web_cornell;cornell-box")
    list(GET pair 0 target)
    list(GET pair 1 sample)
    if(NOT TARGET ${target})
        continue()
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME game
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${sample}")
endforeach()
