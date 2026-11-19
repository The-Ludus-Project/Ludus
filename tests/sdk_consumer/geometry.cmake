# The installed-geometry exercise (geometry.cpp), reused by the installed-SDK consumer and the
# browser tools/web-math-probe. The source and the libraries its includes name are added together,
# so a dependency added to the exercise reaches every target that compiles it.
function(ludus_add_sdk_geometry_exercise target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/geometry.cpp")
    target_link_libraries(${target} PRIVATE Ludus::Content Ludus::FoundationContainers Ludus::FoundationGeometry
        Ludus::FoundationMath Ludus::GeometryContent Ludus::GeometryFormat)
endfunction()
