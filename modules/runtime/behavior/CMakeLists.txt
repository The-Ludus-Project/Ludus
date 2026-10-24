# Optional installed provider. Ordinary C++/data SDKs acquire no Luau payload.
ludus_require_python("Behavior provider")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/script-provider" verify-tools
    RESULT_VARIABLE behavior_tools_result OUTPUT_QUIET)
if(NOT behavior_tools_result EQUAL 0)
    message(FATAL_ERROR "Prepare verified paired tools explicitly with scripts/script-provider bootstrap")
endif()
file(SHA256 "${PROJECT_SOURCE_DIR}/config/luau_toolchain.json" behavior_profile)
set(behavior_tools "${PROJECT_SOURCE_DIR}/out/luau-probe/host-compiler")
foreach(tool luau-compile luau-analyze)
    if(NOT EXISTS "${behavior_tools}/${tool}")
        message(FATAL_ERROR "Prepare the paired behavior tools explicitly with scripts/script-provider bootstrap")
    endif()
endforeach()
file(SHA256 "${behavior_tools}/luau-compile" behavior_compiler)
file(SHA256 "${behavior_tools}/luau-analyze" behavior_analyzer)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/profile.json"
    "{\"version\":1,\"profile\":\"${behavior_profile}\",\"compiler\":\"${behavior_compiler}\",\"analyzer\":\"${behavior_analyzer}\"}\n")
add_library(ludus_runtime_behavior STATIC src/behavior.cpp)
add_library(Ludus::Behavior ALIAS ludus_runtime_behavior)
set_target_properties(ludus_runtime_behavior PROPERTIES EXPORT_NAME Behavior)
if(LUDUS_BEHAVIOR_TARGET_PROFILE MATCHES "-wasm32$")
    set(behavior_arch LUDUS_CPU_WASM32)
elseif(LUDUS_BEHAVIOR_TARGET_PROFILE MATCHES "-arm64$")
    set(behavior_arch LUDUS_CPU_ARM64)
else()
    set(behavior_arch LUDUS_CPU_X86_64)
endif()
target_compile_definitions(ludus_runtime_behavior PUBLIC "LUDUS_BEHAVIOR_EXPECTED_ARCH=${behavior_arch}")
target_sources(ludus_runtime_behavior PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/runtime/behavior/behavior.h)
target_compile_definitions(ludus_runtime_behavior PRIVATE "LUDUS_BEHAVIOR_PROFILE=\"${behavior_profile}\"")
target_include_directories(ludus_runtime_behavior PRIVATE "${PROJECT_SOURCE_DIR}/modules/runtime/scripting/src")
target_link_libraries(ludus_runtime_behavior PUBLIC Ludus::FoundationBase PRIVATE
    Ludus::FoundationMath Ludus::FoundationMemory
    "$<BUILD_INTERFACE:ludus_runtime_scripting>" "$<BUILD_INTERFACE:Luau.VM>"
    "$<INSTALL_INTERFACE:LudusBehaviorProfile>")
ludus_apply_project_defaults(ludus_runtime_behavior)
# Installed native game modules link the provider and closed JSON debug bridge
# statically. Own their PIC closure here; private S2 probes are not prerequisites.
# The foundation libraries are position-independent for every native SDK.
if(NOT EMSCRIPTEN)
    foreach(target ludus_runtime_behavior ludus_runtime_scripting Luau.VM Luau.Common)
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endforeach()
endif()
if(EMSCRIPTEN)
    target_compile_options(ludus_runtime_behavior PRIVATE -sSUPPORT_LONGJMP=wasm)
    target_link_options(ludus_runtime_behavior INTERFACE -sSUPPORT_LONGJMP=wasm)
endif()
install(TARGETS ludus_runtime_behavior EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Raw private link closure: no third-party target/include directory enters the SDK.
install(FILES "$<TARGET_FILE:ludus_runtime_scripting>" "$<TARGET_FILE:Luau.VM>" "$<TARGET_FILE:Luau.Common>"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/Ludus/behavior")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/profile.json"
    "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/behavior_cook.py"
    "${PROJECT_SOURCE_DIR}/scripts/python/ludus_tools/behavior_graph.py"
    "${PROJECT_SOURCE_DIR}/out/luau-probe/source/LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/out/luau-probe/source/lua_LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/behavior")
install(PROGRAMS "${behavior_tools}/luau-compile" "${behavior_tools}/luau-analyze"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/behavior/bin")
install(FILES "${PROJECT_SOURCE_DIR}/cmake/LudusBehavior.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/LudusBehaviorProfile.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus")
install(FILES "${PROJECT_SOURCE_DIR}/config/behavior_profiles.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/behavior" RENAME profiles.json)
# Player release helpers already copy share/Ludus/licenses recursively.
install(FILES "${PROJECT_SOURCE_DIR}/out/luau-probe/source/LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/out/luau-probe/source/lua_LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/licenses/Luau")
