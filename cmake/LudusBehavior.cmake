# Project-owned script assets, explicit paired host tools, no configure-time cook.
include("${CMAKE_CURRENT_LIST_DIR}/LudusDependencies.cmake")
function(ludus_cook_behaviors)
    cmake_parse_arguments(BEHAVIOR "" "NAME;CONTRACT;PACKAGE;PROFILE;COMPILER;ANALYZER" "" ${ARGN})
    if(BEHAVIOR_UNPARSED_ARGUMENTS OR NOT BEHAVIOR_NAME MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
        message(FATAL_ERROR "ludus_cook_behaviors: use a target identifier and declared arguments")
    endif()
    foreach(argument NAME CONTRACT PACKAGE)
        if(NOT BEHAVIOR_${argument})
            message(FATAL_ERROR "ludus_cook_behaviors: ${argument} is required")
        endif()
    endforeach()
    ludus_require_python("Behavior cooking")
    set(resource "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../share/Ludus/behavior")
    # The installed host tools carry the host executable suffix (.exe on Windows).
    set(tool_suffix "")
    if(CMAKE_HOST_WIN32)
        set(tool_suffix ".exe")
    endif()
    foreach(pair "PROFILE;profile.json" "COMPILER;bin/luau-compile${tool_suffix}" "ANALYZER;bin/luau-analyze${tool_suffix}")
        list(GET pair 0 argument)
        list(GET pair 1 filename)
        if(NOT BEHAVIOR_${argument})
            set(BEHAVIOR_${argument} "${resource}/${filename}")
        endif()
    endforeach()
    foreach(argument CONTRACT PACKAGE PROFILE COMPILER ANALYZER)
        get_filename_component(BEHAVIOR_${argument} "${BEHAVIOR_${argument}}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endforeach()
    foreach(argument COMPILER ANALYZER)
        ludus_require_tool(BEHAVIOR_${argument} "Behavior cooking"
            "Restore the matching SDK host tools or explicitly set ${argument} in ludus_cook_behaviors.")
    endforeach()
    foreach(argument CONTRACT PACKAGE PROFILE)
        ludus_require_file("${BEHAVIOR_${argument}}" "Behavior cooking"
            "Restore the input or correct ${argument} in ludus_cook_behaviors; reinstall the SDK if its profile is missing.")
    endforeach()
    ludus_require_file("${resource}/behavior_cook.py" "Behavior cooking"
        "Reinstall the matching Ludus SDK into a clean prefix.")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${BEHAVIOR_NAME}")
    # The cooker validates the bounded source closure/content on every build.
    add_custom_target(${BEHAVIOR_NAME}_cook
        COMMAND "${Python3_EXECUTABLE}" "${resource}/behavior_cook.py"
            --contract "${BEHAVIOR_CONTRACT}" --package "${BEHAVIOR_PACKAGE}" --output "${output}"
            --profile "${BEHAVIOR_PROFILE}" --compiler "${BEHAVIOR_COMPILER}" --analyzer "${BEHAVIOR_ANALYZER}"
        BYPRODUCTS "${output}/contract.h" "${output}/package.h" "${output}/debug_maps.h" "${output}/current.json"
        VERBATIM)
    set(${BEHAVIOR_NAME}_DIRECTORY "${output}" PARENT_SCOPE)
endfunction()
