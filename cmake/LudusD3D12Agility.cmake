# Shared by the source build and the installed Windows SDK.
function(ludus_attach_d3d12_agility target_name sdk_version binaries license)
    get_target_property(target_type ${target_name} TYPE)
    if(NOT target_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "Attach the Direct3D 12 Agility SDK to the executable that hosts ${target_name}.")
    endif()
    if(TARGET "${target_name}_d3d12_agility")
        return()
    endif()

    set(runtime_subdirectory D3D12)
    if(NOT TARGET ludus_d3d12_agility_exports)
        add_library(ludus_d3d12_agility_exports OBJECT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/D3D12AgilityExports.cpp")
        target_compile_definitions(ludus_d3d12_agility_exports PRIVATE
            WIN32_LEAN_AND_MEAN
            "LUDUS_D3D12_SDK_VERSION=${sdk_version}"
            "LUDUS_D3D12_SDK_DIRECTORY=\"${runtime_subdirectory}\"")
        if(COMMAND ludus_apply_project_defaults)
            ludus_apply_project_defaults(ludus_d3d12_agility_exports)
        endif()
    endif()
    target_sources(${target_name} PRIVATE "$<TARGET_OBJECTS:ludus_d3d12_agility_exports>")

    # Run for incremental builds too so a deleted DLL is restored without relinking.
    add_custom_target("${target_name}_d3d12_agility"
        COMMAND "${CMAKE_COMMAND}"
            "-Dbinaries=${binaries}"
            "-Dlicense=${license}"
            "-Ddestination=$<TARGET_FILE_DIR:${target_name}>/${runtime_subdirectory}"
            "-Dlock_file=${CMAKE_BINARY_DIR}/CMakeFiles/LudusD3D12Agility.lock"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageD3D12Agility.cmake"
        VERBATIM)
    add_dependencies(${target_name} "${target_name}_d3d12_agility")
endfunction()
