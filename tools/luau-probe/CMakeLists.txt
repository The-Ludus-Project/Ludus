# Private S0 experiment. Never install/export these targets or discover Luau in
# ordinary SDK builds. The compiler runs separately on the host, not in the VM.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(LUDUS_COMPILED_LOG_LEVEL GREATER 2)
    message(FATAL_ERROR "S0 verification needs Info diagnostics; configure with -DLUDUS_COMPILED_LOG_LEVEL=2")
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/luau-probe" verify
    RESULT_VARIABLE luau_verified
)
if(NOT luau_verified EQUAL 0)
    message(FATAL_ERROR "Prepare the pinned S0 sources and bytecode with scripts/luau-probe bootstrap, then cook")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/LuauProfile.cmake")
ludus_prepare_luau()
set(luau_source "${PROJECT_SOURCE_DIR}/out/luau-probe/source")
add_custom_target(ludus_luau_verify
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/luau-probe" verify)
add_dependencies(Luau.VM ludus_luau_verify)

add_executable(ludus_luau_probe main.cpp)
target_include_directories(ludus_luau_probe PRIVATE "${PROJECT_SOURCE_DIR}/out/luau-probe/cooked")
target_link_libraries(ludus_luau_probe PRIVATE Ludus::FoundationTime Luau.VM Ludus::FoundationLogging)
ludus_apply_project_defaults(ludus_luau_probe)
configure_file("${luau_source}/LICENSE.txt" Luau-LICENSE.txt COPYONLY)
configure_file("${luau_source}/lua_LICENSE.txt" Lua-LICENSE.txt COPYONLY)

if(EMSCRIPTEN)
    target_link_options(ludus_luau_probe PRIVATE
        -fno-exceptions -g0 -sSUPPORT_LONGJMP=wasm -sENVIRONMENT=web,node
        -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    # Longjmp code generation must match the link profile, including upstream.
    target_compile_options(Luau.VM PRIVATE -sSUPPORT_LONGJMP=wasm)
    target_compile_options(ludus_luau_probe PRIVATE -g0 -sSUPPORT_LONGJMP=wasm)
    set_target_properties(ludus_luau_probe PROPERTIES OUTPUT_NAME index SUFFIX ".js")
    configure_file(shell.html index.html COPYONLY)
endif()

enable_testing()
add_test(NAME ludus_luau_semantics
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
        "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_luau_probe>"
        "${PROJECT_BINARY_DIR}/compile_commands.json")
set_tests_properties(ludus_luau_semantics PROPERTIES LABELS "scripting;build-contract" TIMEOUT 120)
