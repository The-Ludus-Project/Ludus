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
