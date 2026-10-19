# Resolve editor dependencies before the tooling modules are configured. Keep
# this independent of onboarding so direct CMake and IDE builds behave alike.
set(editor_render_preview_default ON)
if(EMSCRIPTEN)
    set(editor_render_preview_default OFF)
endif()
option(LUDUS_EDITOR_RENDER_PREVIEW "Build the trusted native authoring surface (requires pinned shader tools)" ${editor_render_preview_default})

if(LUDUS_BUILD_EDITOR AND LUDUS_EDITOR_RENDER_PREVIEW AND NOT EMSCRIPTEN)
    # Live shader compilation is part of the rendered editor. Override an old
    # OFF cache value without persisting ON: a later lightweight/engine-only
    # configuration should retain the user's standalone worker selection.
    set(LUDUS_BUILD_SHADER_WORKER ON)
endif()
