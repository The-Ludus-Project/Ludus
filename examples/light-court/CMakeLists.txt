cmake_minimum_required(VERSION 3.29)
project(LightCourt LANGUAGES NONE)

add_custom_target(light_court SOURCES
    art/light-court.luscene.json
    assets/catalog.json
    authoring/light-court.world.json
)
