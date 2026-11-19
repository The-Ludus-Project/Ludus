# Generated native release component. These files belong to the game.
# Declare all runtime assets/libraries explicitly; never install the SDK itself.
if(NOT APPLE)
    message(FATAL_ERROR "This release component requires macOS")
endif()
set_target_properties(lighting_room PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.ludus.game.lighting_room"
    MACOSX_BUNDLE_BUNDLE_NAME "lighting_room"
    INSTALL_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
install(TARGETS lighting_room BUNDLE DESTINATION . COMPONENT GameRelease)
# The SDK's OIDN denoiser runtime, only when this game links it (Ludus::GraphicsRenderer).
if(COMMAND ludus_install_denoiser_runtime)
    ludus_install_denoiser_runtime(TARGET lighting_room DESTINATION lighting_room.app/Contents/Frameworks COMPONENT GameRelease)
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt" DESTINATION . COMPONENT GameRelease)
if(NOT DEFINED Ludus_SDK_MANIFEST)
    message(FATAL_ERROR "Release packaging requires an SDK with Ludus_SDK_MANIFEST")
endif()
get_filename_component(_game_ludus_share "${Ludus_SDK_MANIFEST}" DIRECTORY)
install(DIRECTORY "${_game_ludus_share}/licenses/" DESTINATION licenses/Ludus COMPONENT GameRelease)
# Add your own assets and their license notices here using COMPONENT GameRelease.
# Runtime dependency policy: only the documented macOS 14.0 system-library baseline is external.
