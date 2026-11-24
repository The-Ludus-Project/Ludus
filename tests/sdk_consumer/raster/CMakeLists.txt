cmake_minimum_required(VERSION 3.29)
project(LudusPortableRasterConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi FoundationTime FoundationMath)
# The fixtures' shaders are LUSL. A cross build such as Emscripten lowers them with a host luslc
# (LUDUS_LUSLC). This project lives in the Ludus checkout, so without the setting it builds that
# compiler from the checkout, as ./scripts/bootstrap-luslc does; the last line it prints is the path.
if(CMAKE_CROSSCOMPILING AND NOT LUDUS_LUSLC)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(ludus_checkout "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${ludus_checkout}/scripts/bootstrap-luslc"
        WORKING_DIRECTORY "${ludus_checkout}" RESULT_VARIABLE bootstrap_result OUTPUT_VARIABLE bootstrap_output)
    string(STRIP "${bootstrap_output}" bootstrap_output)
    string(REGEX REPLACE "^.*\n" "" host_luslc "${bootstrap_output}")
    if(NOT bootstrap_result EQUAL 0 OR NOT EXISTS "${host_luslc}")
        message(FATAL_ERROR "The host luslc bootstrap failed; run ./scripts/bootstrap-luslc from ${ludus_checkout} "
            "and pass -DLUDUS_LUSLC=<the path it prints>.")
    endif()
    set(LUDUS_LUSLC "${host_luslc}" CACHE FILEPATH "Host LUSL compiler for cross builds")
endif()
add_executable(ludus_sdk_raster_consumer ../raster.cpp)
target_link_libraries(ludus_sdk_raster_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_raster_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_raster_consumer NAME cull SOURCE ../shaders/cull.lusl COMPUTE ComputeMain)
ludus_compile_shader(TARGET ludus_sdk_raster_consumer NAME raster RASTER SOURCE ../shaders/raster.lusl
    VERTEX VertexMain FRAGMENT FragmentMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_raster_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_raster_consumer PROPERTIES OUTPUT_NAME raster SUFFIX ".js")
    configure_file(../raster.html raster.html COPYONLY)
endif()

add_executable(ludus_sdk_r6_consumer ../r6.cpp)
target_link_libraries(ludus_sdk_r6_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_r6_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_r6_consumer NAME cull SOURCE ../shaders/cull.lusl COMPUTE ComputeMain)
ludus_compile_shader(TARGET ludus_sdk_r6_consumer NAME raster RASTER SOURCE ../shaders/raster.lusl VERTEX VertexMain FRAGMENT FragmentMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_r6_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r6_consumer PROPERTIES OUTPUT_NAME r6 SUFFIX ".js")
    configure_file(../r6.html r6.html COPYONLY)
endif()

add_executable(ludus_sdk_r8_consumer ../r8.cpp)
target_link_libraries(ludus_sdk_r8_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_r8_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_r8_consumer NAME raster RASTER SOURCE ../shaders/raster.lusl VERTEX VertexMain FRAGMENT FragmentMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_r8_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r8_consumer PROPERTIES OUTPUT_NAME r8 SUFFIX ".js")
    configure_file(../r8.html r8.html COPYONLY)
endif()

add_executable(ludus_sdk_r9_consumer ../r9.cpp)
target_link_libraries(ludus_sdk_r9_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_r9_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_r9_consumer NAME r9 RASTER SOURCE ../shaders/r9.lusl VERTEX VertexMain FRAGMENT FragmentMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_r9_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r9_consumer PROPERTIES OUTPUT_NAME r9 SUFFIX ".js")
    configure_file(../r9.html r9.html COPYONLY)
endif()

add_executable(ludus_sdk_r10_consumer ../r10.cpp)
target_link_libraries(ludus_sdk_r10_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_r10_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
# One program per r10.lusl fragment entry point: r10write uses WriteMain, and so on.
foreach(entry IN ITEMS Write Mask Sample Depth Stencil Coverage)
    string(TOLOWER "${entry}" name)
    ludus_compile_shader(TARGET ludus_sdk_r10_consumer NAME r10${name} RASTER SOURCE ../shaders/r10.lusl
        VERTEX VertexMain FRAGMENT ${entry}Main)
endforeach()
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_r10_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r10_consumer PROPERTIES OUTPUT_NAME r10 SUFFIX ".js")
    configure_file(../r10.html r10.html COPYONLY)
endif()

if(NOT EMSCRIPTEN)
    add_executable(ludus_sdk_r11_consumer ../r11.cpp)
    target_link_libraries(ludus_sdk_r11_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    target_compile_options(ludus_sdk_r11_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11compute SOURCE ../shaders/r11compute.lusl COMPUTE ComputeMain)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11scalar RASTER
        SOURCE ../shaders/r11.lusl VERTEX VertexMain FRAGMENT FragmentMain)
    # r11arrays, r11buffers, r11storage and r11runtime stay Slang: LUSL binding arrays hold
    # textures and samplers indexed by constants (config/lusl_migration.json).
    target_compile_definitions(ludus_sdk_r11_consumer PRIVATE LUDUS_R11_NATIVE_ARRAYS=1)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11arrays RASTER NATIVE_BINDING_ARRAYS
        SOURCE ../shaders/r11.slang VERTEX VertexMain FRAGMENT FragmentMain)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11constant RASTER NATIVE_BINDING_ARRAYS
        SOURCE ../shaders/r11_constant.lusl VERTEX VertexMain FRAGMENT ConstantArrayMain)
    foreach(shape IN ITEMS wider cube volume)
        string(TOUPPER "R11_${shape}_IMAGES" shape_define)
        if(shape STREQUAL "wider")
            set(shape_define R11_ARRAY_IMAGES)
        endif()
        ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11${shape} RASTER NATIVE_BINDING_ARRAYS
            SOURCE ../shaders/r11_wider.lusl VERTEX VertexMain FRAGMENT ConstantArrayMain
            DEFINES LUDUS_R11_WIDER_IMAGES=${shape_define})
    endforeach()
    if(NOT APPLE)
        target_compile_definitions(ludus_sdk_r11_consumer PRIVATE LUDUS_R11C_RUNTIME=1)
        ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11buffers RASTER NATIVE_BINDING_ARRAYS
            SOURCE "../shaders/r11buffers.slang" VERTEX VertexMain FRAGMENT FragmentMain)
        ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11storage NATIVE_BINDING_ARRAYS
            SOURCE "../shaders/r11storage.slang" COMPUTE ComputeMain)
        ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11runtime RASTER NATIVE_BINDING_ARRAYS RUNTIME_BINDING_ARRAYS
            SOURCE ../shaders/r11.slang VERTEX VertexMain FRAGMENT ConstantArrayMain
            DEFINES LUDUS_R11C_RUNTIME=1)
    endif()
    add_test(NAME ludus_sdk_r11_consumer COMMAND ludus_sdk_r11_consumer)
    set_tests_properties(ludus_sdk_r11_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 45)
endif()

if(NOT EMSCRIPTEN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../native_interop.cmake")
    ludus_add_native_interop_fixture(ludus_sdk_native_interop_consumer "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

if(EMSCRIPTEN)
    add_executable(ludus_sdk_r11_consumer ../r11_web.cpp)
    target_link_libraries(ludus_sdk_r11_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    target_compile_options(ludus_sdk_r11_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11compute SOURCE ../shaders/r11compute.lusl COMPUTE ComputeMain)
    ludus_compile_shader(TARGET ludus_sdk_r11_consumer NAME r11scalar RASTER SOURCE ../shaders/r11.lusl VERTEX VertexMain FRAGMENT FragmentMain)
    target_link_options(ludus_sdk_r11_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r11_consumer PROPERTIES OUTPUT_NAME r11 SUFFIX ".js")
    configure_file(../r11.html r11.html COPYONLY)
endif()

# R14c browser device owners: two canvases, each API and one of each. Needs no shaders.
if(EMSCRIPTEN)
    add_executable(ludus_sdk_r14c_consumer ../r14c.cpp)
    target_link_libraries(ludus_sdk_r14c_consumer PRIVATE Ludus::GraphicsRhi)
    target_compile_options(ludus_sdk_r14c_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    target_link_options(ludus_sdk_r14c_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r14c_consumer PROPERTIES OUTPUT_NAME r14c SUFFIX ".js")
    configure_file(../r14c.html r14c.html COPYONLY)
endif()

add_executable(ludus_sdk_r18c_consumer ../r18c.cpp)
target_link_libraries(ludus_sdk_r18c_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_r18c_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_r18c_consumer NAME r18c SOURCE ../shaders/r18c.lusl COMPUTE ComputeMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_r18c_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_r18c_consumer PROPERTIES OUTPUT_NAME r18c SUFFIX ".js")
    configure_file(../r18c.html r18c.html COPYONLY)
else()
    add_test(NAME ludus_sdk_r18c_consumer COMMAND ludus_sdk_r18c_consumer)
    set_tests_properties(ludus_sdk_r18c_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 60)
endif()

# R12a expanded indirect draws and dispatches, or the authored direct variant without
# IndirectRendering (WebGL 2). The shader source is shared with the native RHI conformance case.
add_executable(ludus_sdk_indirect_consumer ../indirect.cpp)
target_link_libraries(ludus_sdk_indirect_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
target_compile_options(ludus_sdk_indirect_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_indirect_consumer NAME indirect_prepare
    SOURCE ../shaders/indirect_commands.lusl COMPUTE PrepareCommands)
ludus_compile_shader(TARGET ludus_sdk_indirect_consumer NAME indirect_prepare_zero
    SOURCE ../shaders/indirect_commands.lusl COMPUTE PrepareZeroFirstInstanceCommands)
ludus_compile_shader(TARGET ludus_sdk_indirect_consumer NAME indirect_mark
    SOURCE ../shaders/indirect_commands.lusl COMPUTE MarkGroups)
ludus_compile_shader(TARGET ludus_sdk_indirect_consumer NAME indirect_draw RASTER
    SOURCE ../shaders/indirect_commands.lusl VERTEX CommandVertex FRAGMENT CommandFragment)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_indirect_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_indirect_consumer PROPERTIES OUTPUT_NAME indirect SUFFIX ".js")
    configure_file(../indirect.html indirect.html COPYONLY)
else()
    add_test(NAME ludus_sdk_indirect_consumer COMMAND ludus_sdk_indirect_consumer)
    add_test(NAME ludus_sdk_indirect_zero_first_instance_consumer
        COMMAND ludus_sdk_indirect_consumer --zero-first-instance)
    set_tests_properties(ludus_sdk_indirect_consumer ludus_sdk_indirect_zero_first_instance_consumer
        PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 60)
endif()

# Addressed-random GPU parity: the corpus shared with the native renderer test,
# checked against the CPU reference compiled into the fixture (WebAssembly in
# the browser). math.slang comes from the installed SDK shader directory.
add_executable(ludus_sdk_random_parity_consumer ../random_parity.cpp)
target_link_libraries(ludus_sdk_random_parity_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationMath Ludus::FoundationTime)
target_compile_options(ludus_sdk_random_parity_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_sdk_random_parity_consumer NAME random_parity SOURCE ../random_parity.slang COMPUTE ComputeMain)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_random_parity_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_random_parity_consumer PROPERTIES OUTPUT_NAME random_parity SUFFIX ".js")
    configure_file(../random_parity.html random_parity.html COPYONLY)
else()
    add_test(NAME ludus_sdk_random_parity_consumer COMMAND ludus_sdk_random_parity_consumer)
    set_tests_properties(ludus_sdk_random_parity_consumer PROPERTIES LABELS "rhi;gpu;build-contract" TIMEOUT 90)
endif()

if(NOT EMSCRIPTEN)
    enable_testing()
    find_package(Ludus CONFIG REQUIRED COMPONENTS FoundationThreading)
    include(../mip_streaming.cmake)
    ludus_add_mip_streaming_consumer(ludus_sdk_mip_streaming_consumer)
    target_compile_options(ludus_sdk_mip_streaming_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
endif()
