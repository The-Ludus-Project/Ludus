# Offline fitter for the renderer's linearly transformed cosine table
# (docs/development/ltc-table.md). Its output,
# modules/graphics/renderer/shaders/ltc_table_data.inc, is checked in; the full
# fit is run by hand. Test builds compile the fitter and run its low-resolution
# --quick mode so the tool keeps working. WriteDirectory publishes files on
# Linux and macOS only.
option(LUDUS_BUILD_LTC_FITTER "Build the offline LTC table fitter" OFF)
if((LUDUS_BUILD_LTC_FITTER OR LUDUS_BUILD_TESTS) AND NOT EMSCRIPTEN AND NOT WIN32)
    add_executable(ludus_ltc_fit main.cpp ltc_fit.cpp)
    target_link_libraries(
        ludus_ltc_fit
        PRIVATE
            Ludus::FoundationBase
            Ludus::FoundationContainers
            Ludus::FoundationFilesystem
            Ludus::FoundationMath
    )
    ludus_apply_project_defaults(ludus_ltc_fit)
    if(LUDUS_BUILD_TESTS)
        set(quick_output "${CMAKE_CURRENT_BINARY_DIR}/quick")
        file(MAKE_DIRECTORY "${quick_output}")
        add_test(NAME ludus_ltc_fit_quick COMMAND ludus_ltc_fit "${quick_output}" --quick)
        set_tests_properties(ludus_ltc_fit_quick PROPERTIES LABELS "unit;renderer;tools")
    endif()
endif()
