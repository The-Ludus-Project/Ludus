cmake_minimum_required(VERSION 3.29)
project(LightCourt LANGUAGES NONE)

add_custom_target(light_court SOURCES
    art/light-court.gltf
    art/light-court.bin
    art/concrete-albedo.png
    art/concrete-normal.png
    art/concrete-surface.png
    art/copper-normal.png
    art/copper-surface.png
    art/enamel-normal.png
    art/enamel-surface.png
    art/graphite-normal.png
    art/graphite-surface.png
    art/LICENSE.txt
    assets/catalog.json
    authoring/light-court.world.json
)
