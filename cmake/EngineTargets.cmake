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

# Private Windows SDK configuration for a target that includes <windows.h>: the
# Windows 10 API surface (per-monitor DPI), UTF-16 "W" entry points, and no
# min/max macros or rarely used headers leaking into Ludus code.
function(ludus_configure_win32_target target_name)
    target_compile_definitions(${target_name} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endfunction()
