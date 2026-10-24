# Local onboarding choices also apply to direct preset and IDE builds. The
# committed developer presets remain unchanged when no local selection exists.
option(LUDUS_USE_INIT_OPTIONS "Apply saved local initialization choices" ON)
if(LUDUS_USE_INIT_OPTIONS AND EXISTS "${PROJECT_SOURCE_DIR}/out/init/options.json")
    file(READ "${PROJECT_SOURCE_DIR}/out/init/options.json" ludus_init_json)
    string(JSON ludus_init_type ERROR_VARIABLE ludus_init_error TYPE "${ludus_init_json}")
    if(ludus_init_error OR NOT ludus_init_type STREQUAL "OBJECT")
        message(FATAL_ERROR "Invalid saved setup choices in out/init/options.json")
    endif()
    get_filename_component(ludus_init_preset "${CMAKE_BINARY_DIR}" NAME)
    foreach(ludus_init_option IN ITEMS LUDUS_BUILD_TESTS LUDUS_BUILD_SMOKE_APP LUDUS_BUILD_WEB_PROBES LUDUS_BUILD_EDITOR LUDUS_EDITOR_RENDER_PREVIEW LUDUS_BUILD_SHADER_PROBE LUDUS_ENABLE_OIDN)
        string(JSON ludus_init_value ERROR_VARIABLE ludus_init_error GET
            "${ludus_init_json}" "${ludus_init_preset}" "${ludus_init_option}")
        if(NOT ludus_init_error)
            set(${ludus_init_option} "${ludus_init_value}" CACHE BOOL "Selected during local initialization" FORCE)
        endif()
    endforeach()
endif()
