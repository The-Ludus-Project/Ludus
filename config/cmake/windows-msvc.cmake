# The Visual Studio generator selects the SDK declared by the preset. Ninja
# uses the SDK libraries and headers supplied by the developer environment.
if(CMAKE_GENERATOR STREQUAL "Ninja")
    string(REGEX REPLACE "[/\\\\]+$" "" windows_sdk_version "$ENV{WindowsSDKVersion}")
    if(NOT windows_sdk_version STREQUAL CMAKE_SYSTEM_VERSION)
        message(FATAL_ERROR "Open an x64 VS 2022 developer shell with MSVC 14.44 and Windows SDK ${CMAKE_SYSTEM_VERSION}.")
    endif()
endif()
