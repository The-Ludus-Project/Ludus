add_library(ludus_runtime_configuration STATIC)
add_library(Ludus::RuntimeConfiguration ALIAS ludus_runtime_configuration)
set_target_properties(ludus_runtime_configuration PROPERTIES EXPORT_NAME RuntimeConfiguration)
target_sources(ludus_runtime_configuration PRIVATE src/host_options.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/runtime/configuration/host_options.hpp)
target_link_libraries(ludus_runtime_configuration PUBLIC Ludus::FoundationConfig)
ludus_apply_project_defaults(ludus_runtime_configuration)
install(TARGETS ludus_runtime_configuration EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The fixed table of engine schemas that ludus-config and the Editor select by
# identity. Separate from RuntimeConfiguration so GameHost does not link Audio.
add_library(ludus_runtime_configuration_schemas STATIC)
add_library(Ludus::RuntimeConfigurationSchemas ALIAS ludus_runtime_configuration_schemas)
set_target_properties(ludus_runtime_configuration_schemas PROPERTIES EXPORT_NAME RuntimeConfigurationSchemas)
target_sources(ludus_runtime_configuration_schemas PRIVATE src/schema_catalog.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/runtime/configuration/schema_catalog.hpp)
target_link_libraries(ludus_runtime_configuration_schemas PUBLIC Ludus::FoundationConfig
    PRIVATE Ludus::RuntimeConfiguration Ludus::AudioConfiguration Ludus::FoundationContainers)
ludus_apply_project_defaults(ludus_runtime_configuration_schemas)
install(TARGETS ludus_runtime_configuration_schemas EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
