# Precompiled-header support for Ludus.
#
# POLICY (docs/architecture/foundational-headers.md Section 10, ADR 0007):
# a PCH is a BUILD ACCELERATOR ONLY. It must never change what a translation
# unit is allowed to name, and the codebase MUST compile identically with the
# PCH disabled. To keep that guarantee real:
#
#   * The PCH mirrors ONLY the foundational header set (Band 0 + Band 1 =
#     <ludus/foundation/base/core.h>). It is NOT a dumping ground: no
#     containers, strings, logging, profiling, or heavy STL are added here.
#     Anything a TU needs beyond core.h it still includes explicitly.
#   * It is opt-in per target via ludus_apply_pch(<target>), controlled by the
#     LUDUS_ENABLE_PCH option (default OFF). It is enabled only after a
#     profile-build measurement shows a win (ADR 0005 discipline).
#   * A dedicated CI configuration builds with LUDUS_ENABLE_PCH=OFF (the
#     default) so no file can come to rely on a symbol only the PCH provided.
#     The "no-pch" guarantee is simply the default build.
#
# core.h is intentionally cheap (only <cstdint>/<cstddef>/<type_traits>/
# <utility> plus the assertion policy header), so precompiling it removes the
# repeated parse of the ubiquitous foundational vocabulary without coupling any
# TU to a heavy facility.

include(PrecompiledHeaderCcache)

option(LUDUS_ENABLE_PCH "Precompile the foundational header (build accelerator; see cmake/EnginePch.cmake)" OFF)

# Apply the foundational PCH to a target. No-op unless LUDUS_ENABLE_PCH is ON.
# Safe to call on every engine library/application target: CMake generates a
# per-target PCH compiled with that target's own flags, so this composes with
# the -fno-exceptions / test-exception policy without conflict.
function(ludus_apply_pch target_name)
    if(NOT LUDUS_ENABLE_PCH)
        return()
    endif()

    # The PCH is exactly the foundational header. Referenced through the Base
    # target's public include interface so the path resolves identically to a
    # normal #include <ludus/foundation/base/core.h> in any consumer.
    target_precompile_headers(${target_name} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:ludus/foundation/base/core.h>")
    # Keep these objects cacheable by ccache, as for the editor's headers.
    ludus_precompiled_header_ccache(${target_name})

    # Ensure the include search path and the generated Band 0 headers
    # (assert_config.hpp) are visible to the target that precompiles core.h.
    if(NOT target_name STREQUAL "ludus_foundation_base")
        target_link_libraries(${target_name} PRIVATE Ludus::FoundationBase)
    endif()
endfunction()
