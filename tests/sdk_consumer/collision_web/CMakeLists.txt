cmake_minimum_required(VERSION 3.29)
# The collision corpus of the native SDK consumer, linked against an installed SDK. Under Emscripten it builds a
# Node program whose exit status reports the result; natively it builds an ordinary executable.
project(LudusCollisionSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Ludus CONFIG REQUIRED COMPONENTS PhysicsCollision)
add_executable(ludus_sdk_collision_consumer main.cpp ../collision.cpp)
target_link_libraries(ludus_sdk_collision_consumer PRIVATE Ludus::PhysicsCollision)
target_compile_options(ludus_sdk_collision_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
if(EMSCRIPTEN)
    target_link_options(ludus_sdk_collision_consumer PRIVATE
        -fno-exceptions -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    set_target_properties(ludus_sdk_collision_consumer PROPERTIES OUTPUT_NAME collision SUFFIX ".js")
endif()
