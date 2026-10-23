# Precompiled headers for the optional Qt editor targets.
#
# The editor's translation units spend most of their compile time parsing the
# same Qt Core and standard library headers again and again; measured on an
# 8-core Linux host, precompiling them halves the CPU time of a clean editor
# build. See docs/development/building.md ("Editor precompiled headers") and the
# narrow exception in AGENTS.md ("Build-time hygiene").
#
# Policy, which keeps this a build accelerator only (ADR 0007 section 10):
#   * Only stable third-party headers: Qt Core and the standard library. No
#     Ludus header is ever listed here, so the engine include graph, the
#     build-time budget and every Ludus header's self-sufficiency are unchanged.
#   * Every editor source still includes what it uses. A build with
#     LUDUS_EDITOR_PRECOMPILED_HEADERS=OFF must compile; batch CI builds the
#     editor that way (see docs/development/continuous-integration.md).
#   * Each target compiles its own precompiled header. The editor targets differ
#     in exception policy (-fexceptions for Catch2 tests), Qt keyword macros and
#     per-target definitions, so REUSE_FROM would hand one target a header
#     compiled under another target's macros.
#   * C++ sources only: Objective-C++ sources on macOS compile without it.
#
# CMake spells the feature "precompile headers" (target_precompile_headers); the
# Ludus option spells the name out instead of using the "PCH" abbreviation.

# The environment variable of the same name sets the default for a new build
# tree (0 turns it off), so CI can choose before ./init.sh configures; an
# existing tree keeps its cached choice until -DLUDUS_EDITOR_PRECOMPILED_HEADERS
# changes it.
if(DEFINED ENV{LUDUS_EDITOR_PRECOMPILED_HEADERS} AND NOT "$ENV{LUDUS_EDITOR_PRECOMPILED_HEADERS}" STREQUAL "")
    if("$ENV{LUDUS_EDITOR_PRECOMPILED_HEADERS}" STREQUAL "0")
        set(ludus_editor_precompiled_headers_default OFF)
    else()
        set(ludus_editor_precompiled_headers_default ON)
    endif()
elseif(EMSCRIPTEN)
    # The browser editor build does not use ccache and has not been measured
    # with precompiled headers; keep its compile unchanged.
    set(ludus_editor_precompiled_headers_default OFF)
else()
    set(ludus_editor_precompiled_headers_default ON)
endif()
option(LUDUS_EDITOR_PRECOMPILED_HEADERS
    "Precompile stable Qt Core and standard headers for the editor targets (build accelerator only)"
    ${ludus_editor_precompiled_headers_default})

# Stable, heavy headers that most editor translation units include. Measured with
# ClangBuildAnalyzer: QJsonArray, QJsonObject, <memory>, <string> and <chrono>
# dominate editor parse time. Keep this list to Qt Core and the standard library
# so every editor target (widgets, tests and the core library) can use it.
set(LUDUS_EDITOR_PRECOMPILED_HEADER_NAMES
    memory
    string
    chrono
    QByteArray
    QDir
    QFile
    QFileInfo
    QJsonArray
    QJsonDocument
    QJsonObject
    QObject
    QString
    QStringList
    QTimer
)

# ccache settings for targets with a precompiled header; see
# cmake/PrecompiledHeaderCcache.cmake.
include(PrecompiledHeaderCcache)

# ludus_editor_apply_precompiled_headers(<target>...)
#   Gives each existing target its own precompiled header of the names above.
#   Does nothing when LUDUS_EDITOR_PRECOMPILED_HEADERS is OFF.
function(ludus_editor_apply_precompiled_headers)
    if(NOT LUDUS_EDITOR_PRECOMPILED_HEADERS)
        return()
    endif()
    set(headers)
    foreach(name IN LISTS LUDUS_EDITOR_PRECOMPILED_HEADER_NAMES)
        # $<ANGLE-R> spells the closing angle bracket inside the generator
        # expression that limits the header to C++ (not Objective-C++) sources.
        list(APPEND headers "$<$<COMPILE_LANGUAGE:CXX>:<${name}$<ANGLE-R>>")
    endforeach()
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()
        target_precompile_headers(${target} PRIVATE ${headers})
        ludus_precompiled_header_ccache(${target})
    endforeach()
endfunction()
