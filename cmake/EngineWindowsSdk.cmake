# Register this guard before any module install rule can write to the prefix.
function(ludus_install_windows_sdk_guard)
    install(CODE "
        if(NOT CMAKE_INSTALL_CONFIG_NAME MATCHES \"^(Debug|Development|Profile|Release)$\")
            message(FATAL_ERROR \"Select a Windows Ludus SDK with --config Debug, Development, Profile or Release.\")
        endif()
        include(\"${PROJECT_BINARY_DIR}/cmake/windows-sdk/\${CMAKE_INSTALL_CONFIG_NAME}/CheckSdkVariant.cmake\")
    " ALL_COMPONENTS)
endfunction()

function(ludus_install_windows_sdk)
    set(LUDUS_WINDOWS_SDK_COMPONENTS
        FoundationBase FoundationMemory FoundationContainers FoundationHash FoundationStrings FoundationTime
        FoundationProfiling FoundationThreading FoundationThreadingProfiling FoundationIdentity FoundationMath
        FoundationLogging FoundationFilesystem Input Platform PlatformEntropy PlatformPacing GraphicsRhi)
    add_custom_target(ludus_windows_sdk)
    foreach(component IN LISTS LUDUS_WINDOWS_SDK_COMPONENTS)
        add_dependencies(ludus_windows_sdk "Ludus::${component}")
        ludus_sdk_register_component("${component}")
        ludus_sdk_register_feature("${component}")
    endforeach()
    ludus_sdk_register_dependency(Threads "system" "system" system)
    ludus_sdk_register_dependency(Microsoft.Direct3D.D3D12 "${LUDUS_D3D12_AGILITY_PACKAGE_VERSION}"
        "Microsoft DirectX Software License Terms" bundled)
    ludus_sdk_register_dependency(xxHash "0.8.3" "BSD-2-Clause" bundled)
    ludus_sdk_register_system_prerequisite("x64 Microsoft STL and dynamic CRT matching the SDK configuration")
    ludus_sdk_register_system_prerequisite("Windows SDK for Win32 and Direct3D 12 system libraries")
    ludus_sdk_register_system_prerequisite("Pinned Slang and DXC for offline DXIL compilation")
    ludus_sdk_finalize_identity()

    set(package_directory "${CMAKE_INSTALL_LIBDIR}/cmake/${LUDUS_ENGINE_PACKAGE_NAME}")
    set(data_directory "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}")
    # Behavior, Unicode and OIDN are outside the installed Windows module closure.
    set(LUDUS_BUILD_BEHAVIOR OFF)
    set(LUDUS_BUILD_UNICODE OFF)
    set(LUDUS_ENABLE_OIDN OFF)
    foreach(sanitizer IN ITEMS ASAN UBSAN TSAN)
        if(LUDUS_ENABLE_${sanitizer})
            set(LUDUS_ENABLE_${sanitizer}_JSON 1)
        else()
            set(LUDUS_ENABLE_${sanitizer}_JSON 0)
        endif()
    endforeach()

    foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
        set(directory "${PROJECT_BINARY_DIR}/cmake/windows-sdk/${configuration}")
        set(CMAKE_BUILD_TYPE "${configuration}")
        set(LUDUS_BUILD_FLAVOR "${configuration}")
        ludus_get_build_flavor_id("${configuration}" LUDUS_BUILD_FLAVOR_ID)
        set(LUDUS_ENABLE_ASSERTS 0)
        if(LUDUS_BUILD_FLAVOR_ID LESS 3)
            set(LUDUS_ENABLE_ASSERTS 1)
        endif()
        set(LUDUS_BREAK_ON_CHECK "${LUDUS_ENABLE_ASSERTS}")
        ludus_format_sdk_variant(LUDUS_SDK_VARIANT)
        set(LUDUS_GAME_COMPATIBILITY_IDENTITY
            "${LUDUS_GIT_REVISION}|${LUDUS_SDK_VARIANT}|${LUDUS_CXX_RUNTIME_ABI}|${LUDUS_COMPILER_ID} ${LUDUS_COMPILER_VERSION}|${LUDUS_TARGET_TRIPLE}|abi-1.1")

        configure_file("${PROJECT_SOURCE_DIR}/cmake/CheckSdkVariant.cmake.in" "${directory}/CheckSdkVariant.cmake" @ONLY)
        configure_file("${PROJECT_SOURCE_DIR}/cmake/LudusSdkManifest.json.in" "${directory}/LudusSdkManifest.json" @ONLY)
        configure_file("${PROJECT_SOURCE_DIR}/cmake/LudusWindowsSdk.cmake.in" "${directory}/LudusWindowsSdk.cmake" @ONLY)
        configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/LudusConfig.cmake.in" "${directory}/LudusConfig.cmake"
            INSTALL_DESTINATION "${package_directory}")
        install(FILES "${directory}/LudusConfig.cmake" "${directory}/LudusWindowsSdk.cmake"
            DESTINATION "${package_directory}" CONFIGURATIONS "${configuration}")
    endforeach()

    write_basic_package_version_file("${PROJECT_BINARY_DIR}/cmake/LudusConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
    install(EXPORT LudusTargets NAMESPACE Ludus:: DESTINATION "${package_directory}")
    install(FILES "${PROJECT_BINARY_DIR}/cmake/LudusConfigVersion.cmake"
        "${PROJECT_SOURCE_DIR}/cmake/LudusShaders.cmake"
        "${PROJECT_SOURCE_DIR}/cmake/LudusDependencies.cmake"
        DESTINATION "${package_directory}")
    install(FILES "${PROJECT_SOURCE_DIR}/modules/graphics/rhi/shaders/raster_bindings.slang"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/compile_shader.py"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/compile_raster.py"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/compile_graph_stage.py"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/native_worker.py"
        "${PROJECT_SOURCE_DIR}/cmake/shaders/cook_texture.py"
        "${PROJECT_SOURCE_DIR}/config/shader_toolchain.json"
        DESTINATION "${package_directory}/shaders")
    install(FILES "${PROJECT_SOURCE_DIR}/cmake/LudusD3D12Agility.cmake"
        "${PROJECT_SOURCE_DIR}/cmake/D3D12AgilityExports.cpp"
        "${PROJECT_SOURCE_DIR}/cmake/StageD3D12Agility.cmake"
        DESTINATION "${package_directory}")
    install(FILES "${LUDUS_D3D12_AGILITY_BINARIES}/D3D12Core.dll"
        "${LUDUS_D3D12_AGILITY_BINARIES}/d3d12SDKLayers.dll"
        "${LUDUS_D3D12_AGILITY_LICENSE}"
        DESTINATION "${package_directory}/agility")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/cmake/sdk/THIRD_PARTY_NOTICES.md"
        DESTINATION "${data_directory}/licenses")
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/xxhash/LICENSE"
        DESTINATION "${data_directory}/licenses" RENAME xxhash-LICENSE)
    # Publish the manifest after the libraries, exports and shader tools.
    install(FILES "${PROJECT_BINARY_DIR}/cmake/windows-sdk/$<CONFIG>/LudusSdkManifest.json"
        DESTINATION "${data_directory}")
endfunction()
