# Ludus::Unicode: private ICU4C 78.3 integration, built only with
# LUDUS_BUILD_UNICODE=ON after `scripts/icu-probe bootstrap`. Its public header
# names no ICU type; the SDK receives ICU as a raw static link closure.
include("${PROJECT_SOURCE_DIR}/cmake/IcuProfile.cmake")
# Directory scope: the embed command below needs Python3_EXECUTABLE here.
ludus_require_python("ICU4C Unicode services")
ludus_prepare_icu()

set(unicode_data_dat "${PROJECT_SOURCE_DIR}/out/icu-probe/data/icudt78l.dat")
set(unicode_data_source "${CMAKE_CURRENT_BINARY_DIR}/generated/icu_data.cpp")
add_custom_command(OUTPUT "${unicode_data_source}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/icu-probe" embed --output "${unicode_data_source}"
    DEPENDS "${unicode_data_dat}" "${PROJECT_SOURCE_DIR}/out/icu-probe/data/manifest.json"
        "${PROJECT_SOURCE_DIR}/scripts/icu-probe" "${PROJECT_SOURCE_DIR}/config/icu_toolchain.json"
    VERBATIM)

add_library(ludus_foundation_unicode STATIC src/unicode.cpp "${unicode_data_source}")
add_library(Ludus::Unicode ALIAS ludus_foundation_unicode)
set_target_properties(ludus_foundation_unicode PROPERTIES OUTPUT_NAME ludus_foundation_unicode EXPORT_NAME Unicode)
target_sources(ludus_foundation_unicode PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/foundation/unicode/unicode.hpp)
target_include_directories(ludus_foundation_unicode PRIVATE src)
target_compile_definitions(ludus_foundation_unicode PRIVATE "${LUDUS_ICU_CONSUMER_DEFINITION}"
    "LUDUS_UNICODE_PINNED_LABEL=\"${LUDUS_ICU_LINGUISTIC_LABEL}\"")
target_link_libraries(ludus_foundation_unicode PUBLIC Ludus::FoundationBase Ludus::FoundationMemory
    PRIVATE "$<BUILD_INTERFACE:ludus_icu_i18n>" "$<INSTALL_INTERFACE:LudusUnicodeIcu>")
ludus_apply_project_defaults(ludus_foundation_unicode)
set_target_properties(ludus_foundation_unicode PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# Build-tree only: engine owners compiled here (Localization's formatter backend)
# use ICU through src/internal/icu_host.hpp. Never exported or installed.
add_library(ludus_unicode_internal INTERFACE)
target_include_directories(ludus_unicode_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ludus_unicode_internal INTERFACE "${LUDUS_ICU_CONSUMER_DEFINITION}")
target_link_libraries(ludus_unicode_internal INTERFACE ludus_foundation_unicode ludus_icu_i18n)

install(TARGETS ludus_foundation_unicode EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Raw private link closure: no ICU target, header or include directory enters the SDK.
install(FILES "$<TARGET_FILE:ludus_icu_i18n>" "$<TARGET_FILE:ludus_icu_common>"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/Ludus/unicode")
install(FILES "${LUDUS_ICU_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/Ludus/licenses/ICU")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_foundation_unicode_tests tests/unicode_tests.cpp)
    target_link_libraries(ludus_foundation_unicode_tests PRIVATE Ludus::Unicode ludus_unicode_internal
        Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_foundation_unicode_tests)
    ludus_enable_test_exceptions(ludus_foundation_unicode_tests)
    add_test(NAME ludus_foundation_unicode_tests COMMAND ludus_foundation_unicode_tests)
    set_tests_properties(ludus_foundation_unicode_tests PROPERTIES LABELS "unit;localization")
endif()
