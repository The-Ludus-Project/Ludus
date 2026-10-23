# ccache settings shared by every target that compiles with a precompiled
# header: the editor's Qt headers (apps/editor/cmake/EditorPrecompiledHeaders.cmake)
# and the optional foundational header (cmake/EnginePch.cmake).
#
# ccache reuses objects compiled against a precompiled header only with these
# sloppiness settings; Clang must also omit the header's timestamp. Thanks to
# the ccache manual, "Precompiled headers":
# https://ccache.dev/manual/latest.html#_precompiled_headers
set(LUDUS_PRECOMPILED_HEADER_CCACHE_SLOPPINESS "sloppiness=pch_defines,time_macros")
# Configuration settings on the ccache command line need ccache 4.8 or newer.
set(LUDUS_PRECOMPILED_HEADER_CCACHE_MINIMUM_VERSION "4.8")

# Returns the C++ compiler launcher for a target with a precompiled header: the
# project's ccache launcher plus the settings above, or the unchanged launcher
# when ccache is not in use or too old to accept settings on its command line.
# The ccache version is read once per configure.
function(ludus_precompiled_header_launcher out_launcher)
    set(launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")
    if(NOT LUDUS_CCACHE_PROGRAM OR NOT launcher)
        set(${out_launcher} "${launcher}" PARENT_SCOPE)
        return()
    endif()
    list(FIND launcher "${LUDUS_CCACHE_PROGRAM}" ccache_index)
    if(ccache_index LESS 0)
        set(${out_launcher} "${launcher}" PARENT_SCOPE)
        return()
    endif()
    get_property(is_ccache_checked GLOBAL PROPERTY LUDUS_PRECOMPILED_HEADER_CCACHE_CHECKED SET)
    if(NOT is_ccache_checked)
        execute_process(COMMAND "${LUDUS_CCACHE_PROGRAM}" --version
            OUTPUT_VARIABLE ccache_version_text ERROR_QUIET)
        string(REGEX MATCH "version ([0-9]+\\.[0-9]+)" ccache_version_match "${ccache_version_text}")
        set(is_ccache_supported TRUE)
        if("${CMAKE_MATCH_1}" VERSION_LESS LUDUS_PRECOMPILED_HEADER_CCACHE_MINIMUM_VERSION)
            message(STATUS "ccache '${ccache_version_match}' cannot take settings on its command line; objects "
                "compiled with precompiled headers will not be cached (needs "
                "${LUDUS_PRECOMPILED_HEADER_CCACHE_MINIMUM_VERSION} or newer)")
            set(is_ccache_supported FALSE)
        endif()
        set_property(GLOBAL PROPERTY LUDUS_PRECOMPILED_HEADER_CCACHE_CHECKED "${is_ccache_supported}")
    endif()
    get_property(is_ccache_supported GLOBAL PROPERTY LUDUS_PRECOMPILED_HEADER_CCACHE_CHECKED)
    if(is_ccache_supported)
        list(APPEND launcher "${LUDUS_PRECOMPILED_HEADER_CCACHE_SLOPPINESS}")
    endif()
    set(${out_launcher} "${launcher}" PARENT_SCOPE)
endfunction()

# ludus_precompiled_header_ccache(<target>)
#   Lets ccache reuse <target>'s objects compiled against its precompiled
#   header. Call it on targets that have one; it does not add a header itself.
function(ludus_precompiled_header_ccache target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Without the timestamp, a rebuilt but identical header keeps its
        # dependants' ccache entries valid.
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-Xclang -fno-pch-timestamp>")
    endif()
    ludus_precompiled_header_launcher(launcher)
    if(launcher)
        set_target_properties(${target} PROPERTIES CXX_COMPILER_LAUNCHER "${launcher}")
    endif()
endfunction()
