# D0 admits the x64 Win32/RHI build with the toolset used in the Windows lane.
if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
   OR NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^19[.]44[.]")
    message(FATAL_ERROR "The Windows MSVC build requires the x64 MSVC 14.44 toolset. Select it in the VS 2022 developer environment or with the Visual Studio generator toolset option.")
endif()

# Use the C++23-specific mode verified by the D0 primitive probe.
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CMake initializes these defaults. The Ludus target policies select warning
# level and exception handling once, so cl does not diagnose option overrides.
string(REGEX REPLACE "(^| )[/-](EHsc|W[0-4])( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "(^| )[/-](EHsc|W[0-4])( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

if(LUDUS_VISUAL_STUDIO_MULTI_CONFIG)
    # Development and Profile share RelWithDebInfo build settings and dependencies.
    foreach(configuration IN ITEMS DEVELOPMENT PROFILE)
        set(CMAKE_MAP_IMPORTED_CONFIG_${configuration} RelWithDebInfo)
        foreach(flags IN ITEMS CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
            set(CMAKE_${flags}_FLAGS_${configuration} "${CMAKE_${flags}_FLAGS_RELWITHDEBINFO}"
                CACHE STRING "MSVC ${flags} flags for ${configuration}")
        endforeach()
    endforeach()
    if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        # Keep debug information in SDK objects so consumers can link and debug
        # a relocated static archive without a producer compile PDB.
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,Development,Profile>:Embedded>")
    endif()
endif()

if(LUDUS_ENABLE_ASAN)
    # Thanks to Microsoft, "AddressSanitizer", Build integration and compatibility,
    # for the /RTC, edit-and-continue and incremental-link restrictions.
    # https://learn.microsoft.com/cpp/sanitizers/asan
    string(REGEX REPLACE "(^| )[/-]RTC[1su]+( |$)" " " CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
endif()
