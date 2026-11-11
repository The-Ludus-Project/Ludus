option(LUDUS_BUILD_MEMORY_BENCHMARKS "Build the standalone memory baseline benchmark" OFF)
if(LUDUS_BUILD_MEMORY_BENCHMARKS)
    add_executable(ludus_memory_bench main.cpp)
    target_link_libraries(
        ludus_memory_bench
        PRIVATE Ludus::FoundationThreading
            Ludus::FoundationBase
            Ludus::FoundationContainers
            Ludus::FoundationMemory
            Ludus::FoundationProfiling
            Ludus::FoundationTime
    )
    ludus_apply_project_defaults(ludus_memory_bench)
endif()
