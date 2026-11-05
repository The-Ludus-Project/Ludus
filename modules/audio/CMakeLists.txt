add_library(ludus_audio STATIC)
add_library(Ludus::Audio ALIAS ludus_audio)

set_target_properties(
    ludus_audio
    PROPERTIES
        OUTPUT_NAME ludus_audio
        EXPORT_NAME Audio
)

target_sources(
    ludus_audio
    PRIVATE
        src/status.cpp
        src/device.cpp
        src/source.cpp
        src/prepared.cpp
        src/stream.cpp
        src/audio_system.cpp
        src/control_owner.cpp
        src/facade.cpp
        src/audio_gym.cpp
        src/audio_app_adapter.cpp
        src/spatial.cpp
        src/buses.cpp
        src/internal/decode.cpp
        src/internal/decode_scheduler.cpp
        src/internal/resampler.cpp
        src/internal/voice_kernel.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/audio/audio_source.h
            include/ludus/audio/audio_types.h
            include/ludus/audio/audio_system.h
            include/ludus/audio/audio_debug.h
            include/ludus/audio/audio_gym.h
            include/ludus/audio/audio_app_adapter.h
)

# Private implementation include root (src/internal/*) and the private miniaudio
# header for the decode/resampler translation units. The miniaudio include dir
# comes transitively from the Ludus::Miniaudio target (PRIVATE below), but the
# .cpp files include "miniaudio.h" directly, so expose it here too.
target_include_directories(
    ludus_audio
    PRIVATE
        src
)

# Audio publicly depends on FoundationBase for the fixed-width vocabulary and
# UniquePtr. FoundationContainers and FoundationLogging are private; the private
# miniaudio C dependency supplies decode/resampler primitives. PRIVATE linkage
# does not remove the static dependency closure in the installed SDK (handled by
# installing ludus_miniaudio into the same export set; design section 2).
target_link_libraries(
    ludus_audio
    PUBLIC
        Ludus::FoundationBase
        # audio_gym.h exposes Ludus Array in the public Gym API.
        Ludus::FoundationContainers
    PRIVATE
        Ludus::FoundationMath
        Ludus::FoundationLogging
        Ludus::Miniaudio
)

ludus_apply_project_defaults(ludus_audio)

install(
    TARGETS ludus_audio
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_audio_tests)
    target_sources(
        ludus_audio_tests
        PRIVATE
            tests/device_tests.cpp
            tests/stream_tests.cpp
            tests/frame_conversion_tests.cpp
            tests/spsc_ring_tests.cpp
            tests/handle_tests.cpp
            tests/admission_tests.cpp
            tests/lifecycle_tests.cpp
            tests/gym_tests.cpp
            tests/dsp_tests.cpp
            tests/bus_tests.cpp
            tests/adapter_tests.cpp
            tests/batch_order_tests.cpp
            tests/scheduler_tests.cpp
            tests/descriptor_tests.cpp
            tests/diagnostics_tests.cpp
    )
    # Tests reach into src/internal for the SPSC ring and handle helpers.
    target_include_directories(ludus_audio_tests PRIVATE src include)
    target_link_libraries(
        ludus_audio_tests
        PRIVATE
            Ludus::Audio
            Ludus::FoundationBase
            Catch2::Catch2WithMain
    )
    ludus_apply_project_defaults(ludus_audio_tests)
    ludus_enable_test_exceptions(ludus_audio_tests)
    if(WIN32)
        # The portable offline/resident suite is admitted on Windows. The
        # streaming worker and native playback still require a host backend.
        add_test(NAME ludus_audio_tests COMMAND ludus_audio_tests "[audio]~[native-audio-worker]~[.device]")
    else()
        add_test(NAME ludus_audio_tests COMMAND ludus_audio_tests)
    endif()
    set_tests_properties(ludus_audio_tests PROPERTIES LABELS "unit;audio")
    if(NOT WIN32)
        add_test(NAME ludus_audio_device_tests COMMAND ludus_audio_tests "[.device]")
        # Catch2 3.4 returns 4 when its only case skips for unavailable hardware.
        # Thanks to Catch2, Session::runInternal (v3.4.0 src/catch2/catch_session.cpp),
        # and CMake 3.29 SKIP_RETURN_CODE for preserving skip status through CTest:
        # https://cmake.org/cmake/help/v3.29/prop_test/SKIP_RETURN_CODE.html
        set_tests_properties(ludus_audio_device_tests PROPERTIES LABELS "audio;device" SKIP_RETURN_CODE 4)
    endif()

    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_audio_alloc_tests)
        target_sources(ludus_audio_alloc_tests PRIVATE tests/allocation_tests.cpp)
        target_include_directories(ludus_audio_alloc_tests PRIVATE src include)
        target_link_libraries(
            ludus_audio_alloc_tests
            PRIVATE
                Ludus::Audio
                Catch2::Catch2WithMain
        )
        ludus_apply_project_defaults(ludus_audio_alloc_tests)
        ludus_enable_test_exceptions(ludus_audio_alloc_tests)
        add_test(NAME ludus_audio_alloc_tests COMMAND ludus_audio_alloc_tests)
        set_tests_properties(ludus_audio_alloc_tests PROPERTIES LABELS "unit;audio;alloc")
    endif()
endif()

# These native owners call pthread directly; keep the system dependency explicit.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_package(Threads REQUIRED)
    target_link_libraries(ludus_audio PRIVATE Threads::Threads)
endif()
