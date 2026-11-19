cmake_minimum_required(VERSION 3.29)
project(WaylandWorldDemo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Ludus CONFIG REQUIRED COMPONENTS Platform GraphicsRenderer GameplayWorld)

# Build the same game sources and shader recipes as the checkout against imported SDK targets.
function(ludus_apply_project_defaults target)
    ludus_apply_app_policy(${target})
endfunction()
set(LUDUS_BUILD_WORLD_DEMO ON)
add_subdirectory(world_demo)
