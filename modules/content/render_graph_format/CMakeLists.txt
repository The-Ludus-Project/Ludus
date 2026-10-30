# Authored render plan records are owned independently of tools and Renderer.
add_library(ludus_render_graph_format INTERFACE)
add_library(Ludus::RenderGraphFormat ALIAS ludus_render_graph_format)
set_target_properties(ludus_render_graph_format PROPERTIES EXPORT_NAME RenderGraphFormat)
target_sources(ludus_render_graph_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/render_graph_format/capture.h include/ludus/content/render_graph_format/plan.h
        include/ludus/content/render_graph_format/scene_data.h include/ludus/content/render_graph_format/runtime_package.h)
target_link_libraries(ludus_render_graph_format INTERFACE Ludus::FoundationBase)
install(TARGETS ludus_render_graph_format EXPORT LudusTargets FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
