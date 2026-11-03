# Keep the established test entry point complete as optional editor suites are added.
# Directory ownership and executable type exclude imported targets, helper objects and benchmarks.
function(ludus_complete_editor_test_target aggregate_target)
    if(NOT TARGET ${aggregate_target})
        return()
    endif()
    get_property(editor_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(editor_target IN LISTS editor_targets)
        get_target_property(editor_target_type ${editor_target} TYPE)
        if(editor_target MATCHES "_tests$" AND editor_target_type STREQUAL "EXECUTABLE"
                AND NOT editor_target STREQUAL aggregate_target)
            add_dependencies(${aggregate_target} ${editor_target})
        endif()
    endforeach()
endfunction()
