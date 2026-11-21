cmake_minimum_required(VERSION 3.29)
project(LudusMemoryActivityConsumer LANGUAGES CXX)

find_package(Ludus CONFIG REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_executable(ludus_memory_activity_consumer main.cpp)
target_link_libraries(ludus_memory_activity_consumer PRIVATE
    Ludus::FoundationProfiling Ludus::FoundationThreading)
target_compile_features(ludus_memory_activity_consumer PRIVATE cxx_std_23)
set_target_properties(ludus_memory_activity_consumer PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME ludus_installed_memory_activity COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
    "$<TARGET_FILE:ludus_memory_activity_consumer>" "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set_tests_properties(ludus_installed_memory_activity PROPERTIES TIMEOUT 60)
