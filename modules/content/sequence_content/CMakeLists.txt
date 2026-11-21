# Ludus::SequenceContent: the `.luseq.json` sequence source document, its
# canonical writer, the deterministic LUSEQ cooker and the validating decoder
# (docs/architecture/sequencer-cinematics.md). In-tree only, like
# ContentIdentity, whose version-1 entity-key rule it reuses; not installed
# until an SDK consumer needs it.
add_library(ludus_sequence_content STATIC src/sequence_document.cpp src/sequence_source.cpp src/sequence_cook.cpp)
add_library(Ludus::SequenceContent ALIAS ludus_sequence_content)
target_sources(ludus_sequence_content PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/sequence_content/sequence_content.h)
target_link_libraries(ludus_sequence_content
    PUBLIC Ludus::Content Ludus::ContentIdentity Ludus::FoundationContainers Ludus::FoundationIdentity
        Ludus::FoundationParsing Ludus::GameplaySequencer
    PRIVATE Ludus::FoundationParsingJson Ludus::SequenceFormat)
ludus_apply_project_defaults(ludus_sequence_content)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_sequence_content_tests tests/sequence_content_tests.cpp)
    target_link_libraries(ludus_sequence_content_tests PRIVATE Ludus::SequenceContent Ludus::SequenceFormat
        Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_sequence_content_tests)
    ludus_enable_test_exceptions(ludus_sequence_content_tests)
    add_test(NAME ludus_sequence_content_tests COMMAND ludus_sequence_content_tests)
    set_tests_properties(ludus_sequence_content_tests PROPERTIES LABELS "unit;content;sequencer" TIMEOUT 60)
endif()
