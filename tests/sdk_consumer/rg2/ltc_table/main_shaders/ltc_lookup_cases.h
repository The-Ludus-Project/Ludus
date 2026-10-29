#pragma once
// Lookup cases of the ltc_table render-graph fixture, shared by its compute kernel (ltc_lookup.slang) and
// modules/graphics/renderer/tests/render_graph_ray_tests.cpp. Target column c evaluates roughness index
// c % ROUGHNESS_COUNT, view index (c / ROUGHNESS_COUNT) % VIEW_COUNT and F0 index c / (ROUGHNESS_COUNT * VIEW_COUNT).
// Roughness and the view cosine are the midpoints (i + 0.5) / count; the gray F0 is i / (FRESNEL_COUNT - 1).
#define LTC_LOOKUP_ROUGHNESS_COUNT 4
#define LTC_LOOKUP_VIEW_COUNT 3
#define LTC_LOOKUP_FRESNEL_COUNT 3
#define LTC_LOOKUP_CASE_COUNT (LTC_LOOKUP_ROUGHNESS_COUNT * LTC_LOOKUP_VIEW_COUNT * LTC_LOOKUP_FRESNEL_COUNT)
