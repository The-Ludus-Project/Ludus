# Pinned ICU4C 78.3 build shared by Ludus::Unicode, the L0 probe and the L1
# profile checker (docs/architecture/localization-l0.md#icu-backend-configuration).
# Every upstream common/i18n translation unit compiles with the Ludus
# no-exception, sanitizer and flavor options and the pinned UCONFIG switches.
# Sources come only from an explicit `scripts/icu-probe bootstrap`; configure
# verifies them and never downloads.
include_guard(GLOBAL)
function(ludus_prepare_icu)
    if(TARGET ludus_icu_i18n)
        return()
    endif()
    ludus_require_python("ICU4C Unicode services")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/icu-probe" verify-sources
        RESULT_VARIABLE verified OUTPUT_VARIABLE verification_output ERROR_VARIABLE verification_error TIMEOUT 120)
    if(NOT verified EQUAL 0)
        ludus_dependency_error("ICU4C Unicode services" "the pinned ICU4C sources and sliced data"
            "Source verification failed: ${verification_output}${verification_error}"
            "From the Ludus source repository root: ./scripts/icu-probe bootstrap")
    endif()

    set(icu_source "${PROJECT_SOURCE_DIR}/out/icu-probe/source")
    file(READ "${PROJECT_SOURCE_DIR}/config/icu_toolchain.json" icu_pin)
    string(JSON icu_configuration_count LENGTH "${icu_pin}" configuration)
    math(EXPR icu_configuration_last "${icu_configuration_count} - 1")
    set(icu_configuration)
    foreach(index RANGE ${icu_configuration_last})
        string(JSON definition GET "${icu_pin}" configuration ${index})
        list(APPEND icu_configuration "${definition}")
    endforeach()

    add_custom_target(ludus_icu_source_verify
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/icu-probe" verify-sources)
    file(GLOB icu_common_sources CONFIGURE_DEPENDS "${icu_source}/common/*.cpp")
    file(GLOB icu_i18n_sources CONFIGURE_DEPENDS "${icu_source}/i18n/*.cpp")
    add_library(ludus_icu_common STATIC EXCLUDE_FROM_ALL ${icu_common_sources} "${icu_source}/stubdata/stubdata.cpp")
    target_include_directories(ludus_icu_common PRIVATE "${icu_source}/common")
    target_include_directories(ludus_icu_common SYSTEM INTERFACE "$<BUILD_INTERFACE:${icu_source}/common>")
    target_compile_definitions(ludus_icu_common PRIVATE U_COMMON_IMPLEMENTATION=1 PUBLIC ${icu_configuration})
    add_library(ludus_icu_i18n STATIC EXCLUDE_FROM_ALL ${icu_i18n_sources})
    target_include_directories(ludus_icu_i18n PRIVATE "${icu_source}/i18n")
    target_include_directories(ludus_icu_i18n SYSTEM INTERFACE "$<BUILD_INTERFACE:${icu_source}/i18n>")
    target_compile_definitions(ludus_icu_i18n PRIVATE U_I18N_IMPLEMENTATION=1)
    target_link_libraries(ludus_icu_i18n PUBLIC ludus_icu_common)
    foreach(target ludus_icu_common ludus_icu_i18n)
        add_dependencies(${target} ludus_icu_source_verify)
        target_link_libraries(${target} PRIVATE ludus_project_options)
        # Upstream warnings are not Ludus diagnostics; ICU sources stay unmodified.
        target_compile_options(${target} PRIVATE -w)
        set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    endforeach()

    # Ludus code compiles with the consumer configuration: upstream builds its own
    # deprecated API, Ludus must not depend on it.
    string(JSON icu_consumer_configuration GET "${icu_pin}" consumer_configuration 0)
    string(JSON icu_version GET "${icu_pin}" version)
    string(JSON icu_cldr GET "${icu_pin}" cldr)
    string(JSON icu_unicode GET "${icu_pin}" unicode)
    set(LUDUS_ICU_CONSUMER_DEFINITION "${icu_consumer_configuration}" CACHE INTERNAL "")
    # Same spelling as localization_release.linguistic_label and Catalog::GetLinguisticData.
    set(LUDUS_ICU_LINGUISTIC_LABEL "icu4c-${icu_version}/cldr-${icu_cldr}/unicode-${icu_unicode}" CACHE INTERNAL "")
    set(LUDUS_ICU_SOURCE_DIR "${icu_source}" CACHE INTERNAL "")
endfunction()
