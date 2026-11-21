# Ludus::SequenceFormat: header-only LUSEQ cooked sequence contract shared by
# the sequence cooker and the validating decoder (owned by neither side).
add_library(ludus_sequence_format INTERFACE)
add_library(Ludus::SequenceFormat ALIAS ludus_sequence_format)
set_target_properties(ludus_sequence_format PROPERTIES EXPORT_NAME SequenceFormat)
target_sources(ludus_sequence_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/sequence_format/luseq.h)
target_link_libraries(ludus_sequence_format INTERFACE Ludus::FoundationBase)
install(TARGETS ludus_sequence_format EXPORT LudusTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_sequence_format_tests tests/sequence_format_tests.cpp)
    target_link_libraries(ludus_sequence_format_tests PRIVATE Ludus::SequenceFormat Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_sequence_format_tests)
    ludus_enable_test_exceptions(ludus_sequence_format_tests)
    add_test(NAME ludus_sequence_format_tests COMMAND ludus_sequence_format_tests)
    set_tests_properties(ludus_sequence_format_tests PROPERTIES LABELS "unit;content;sequencer")
endif()
