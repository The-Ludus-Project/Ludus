# Release install rules for the executable owned by a game or tool.
# SDK libraries retain CodeView data in their .lib objects through /Z7; the
# final link produces the matching PDB. Keep it in a separate install component.
function(ludus_install_windows_application target_name)
    cmake_parse_arguments(PARSE_ARGV 1 release "" "DESTINATION;COMPONENT;SYMBOL_COMPONENT" "")
    get_target_property(target_type "${target_name}" TYPE)
    if(NOT WIN32 OR NOT MSVC OR NOT target_type STREQUAL "EXECUTABLE" OR
       release_UNPARSED_ARGUMENTS OR NOT release_DESTINATION OR
       NOT release_COMPONENT OR NOT release_SYMBOL_COMPONENT)
        message(FATAL_ERROR "ludus_install_windows_application requires a Windows MSVC executable, DESTINATION, COMPONENT and SYMBOL_COMPONENT")
    endif()
    # Thanks to Microsoft, /Z7, /DEBUG and /PDBALTPATH linker documentation:
    # https://learn.microsoft.com/cpp/build/reference/z7-zi-zi-debug-information-format
    # https://learn.microsoft.com/cpp/build/reference/debug-generate-debug-info
    # https://learn.microsoft.com/cpp/build/reference/pdbaltpath-use-alternate-pdb-path
    # Debug information is independent of Release optimization; keep its path
    # package-relative without rewriting the executable after Authenticode.
    target_compile_options("${target_name}" PRIVATE "$<$<CONFIG:Release>:/Z7>")
    target_link_options("${target_name}" PRIVATE
        "$<$<CONFIG:Release>:/DEBUG:FULL>"
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>"
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
        "$<$<CONFIG:Release>:/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${target_name}>>")
    install(TARGETS "${target_name}" RUNTIME DESTINATION "${release_DESTINATION}"
        CONFIGURATIONS Release COMPONENT "${release_COMPONENT}")
    install(FILES "$<TARGET_PDB_FILE:${target_name}>" DESTINATION "${release_DESTINATION}"
        CONFIGURATIONS Release COMPONENT "${release_SYMBOL_COMPONENT}")
endfunction()
