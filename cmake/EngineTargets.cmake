function(ludus_apply_project_defaults target_name)
    target_link_libraries(
        ${target_name}
        PRIVATE
            $<BUILD_INTERFACE:ludus_project_options>
            $<BUILD_INTERFACE:ludus_project_warnings>
    )

    # Apply the foundational precompiled header when LUDUS_ENABLE_PCH is ON.
    # This is a no-op by default (build accelerator only; see cmake/EnginePch.cmake
    # and docs/architecture/foundational-headers.md Section 10). ludus_apply_pch is
    # defined in EnginePch.cmake, included after this module at the top level.
    if(COMMAND ludus_apply_pch)
        ludus_apply_pch(${target_name})
    endif()
endfunction()

# Re-enable C++ exceptions for a single target. Engine code is compiled with
# -fno-exceptions (see EngineOptions.cmake); test executables need exceptions
# because Catch2 reports assertion failures by throwing. Apply this ONLY to test
# targets, never to engine libraries or applications.
#
# Setting LUDUS_TEST_EXCEPTIONS suppresses the interface `-fno-exceptions`
# (see EngineOptions.cmake) so this target compiles with `-fexceptions` only,
# with no conflicting `-fno-exceptions` on the same command line. This is
# order-independent, unlike appending `-fexceptions` after an inherited
# `-fno-exceptions` (which CMake ordered so that -fno-exceptions won, silently
# disabling exceptions in tests). Verified with the compile database.
function(ludus_enable_test_exceptions target_name)
    set_target_properties(${target_name} PROPERTIES LUDUS_TEST_EXCEPTIONS ON)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        target_compile_options(${target_name} PRIVATE -fexceptions)
    elseif(MSVC)
        target_compile_options(${target_name} PRIVATE /EHsc)
    endif()
endfunction()

# Stages the files under source into destination for a render graph cook. Each copy is a configure
# input, so an edit re-stages it on the next build without a manual reconfigure. Unlike file(COPY),
# which keeps whole-second source timestamps, a copy is rewritten with a fresh timestamp exactly
# when its content changes: edits always recook, and reconfiguring alone never does. *.in templates
# and the file names given as further arguments are skipped; the caller writes those itself.
function(ludus_stage_render_graph_inputs source destination)
    file(GLOB_RECURSE relative_paths CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE "${source}" "${source}/*")
    foreach(relative_path IN LISTS relative_paths)
        get_filename_component(name "${relative_path}" NAME)
        if(NOT name MATCHES "\\.in$" AND NOT name IN_LIST ARGN)
            configure_file("${source}/${relative_path}" "${destination}/${relative_path}" COPYONLY)
        endif()
    endforeach()
endfunction()

# Stages an authored render graph fixture and configures its main.lushader.json.in manifest.
function(ludus_stage_render_graph_fixture source staged)
    ludus_stage_render_graph_inputs("${source}" "${staged}" ${ARGN})
    configure_file("${source}/main.lushader.json.in" "${staged}/main.lushader.json" @ONLY)
endfunction()

# Private Windows SDK configuration for a target that includes <windows.h>: the
# Windows 10 API surface (per-monitor DPI), UTF-16 "W" entry points, and no
# min/max macros or rarely used headers leaking into Ludus code.
function(ludus_configure_win32_target target_name)
    target_compile_definitions(${target_name} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endfunction()
