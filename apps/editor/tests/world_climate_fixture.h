#pragma once

#include "internal/world_climate.h"

#include <ludus/foundation/base/types.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// The golden weather corpus climate (tests/sdk_consumer/weather_golden_corpus.cpp) spelled as a World "climate"
// member. Each preset uses the corpus formulas, so the cooked bytes must match the corpus climate exactly.
namespace ludus::editor::tests
{
inline QJsonObject MakeGoldenClimatePreset(int id,
                                           const QString& name,
                                           foundation::float64 temperature,
                                           foundation::float64 humidity,
                                           foundation::float64 wind,
                                           foundation::float64 coverage,
                                           foundation::float64 precipitation)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("name"), name},
        {QStringLiteral("temperature"), temperature},
        {QStringLiteral("humidity"), humidity},
        {QStringLiteral("groundWind"), QJsonArray{wind, 0.0, -0.5 * wind}},
        {QStringLiteral("cloudWind"), QJsonArray{2.0 * wind, 0.25, wind}},
        {QStringLiteral("cloudCoverage"), coverage},
        {QStringLiteral("cloudType"), 1.0 - coverage},
        {QStringLiteral("cloudDensity"), 0.5 + coverage},
        {QStringLiteral("fogExtinction"), 0.001 * humidity},
        {QStringLiteral("fogScaleHeight"), 150.0 + 100.0 * coverage},
        {QStringLiteral("precipitation"), precipitation},
    };
}

inline QJsonObject MakeGoldenClimateBounds(foundation::float64 minimumX,
                                           foundation::float64 minimumZ,
                                           foundation::float64 maximumX,
                                           foundation::float64 maximumZ)
{
    return {
        {QStringLiteral("minimumX"), minimumX},
        {QStringLiteral("minimumZ"), minimumZ},
        {QStringLiteral("maximumX"), maximumX},
        {QStringLiteral("maximumZ"), maximumZ},
    };
}

inline QJsonObject MakeGoldenClimateEntry(int preset, int hold, int blend)
{
    return {{QStringLiteral("preset"), preset}, {QStringLiteral("hold"), hold}, {QStringLiteral("blend"), blend}};
}

/// The corpus climate: four presets, a default schedule, a feathered valley and an overlapping ridge.
inline QJsonObject MakeGoldenWorldClimate()
{
    const QJsonArray presets{
        MakeGoldenClimatePreset(1, QStringLiteral("Clear"), 22.5, 0.35, 2.0, 0.1, 0.0),
        MakeGoldenClimatePreset(2, QStringLiteral("Overcast"), 16.0, 0.7, -4.0, 0.85, 0.0),
        MakeGoldenClimatePreset(3, QStringLiteral("Rain"), 12.25, 0.95, -7.5, 1.0, 6.0),
        MakeGoldenClimatePreset(4, QStringLiteral("Storm"), 9.0, 1.0, 18.0, 1.0, 40.0),
    };
    const QJsonArray schedules{
        QJsonObject{
            {QStringLiteral("id"), 10},
            {QStringLiteral("name"), QStringLiteral("Lowlands")},
            {QStringLiteral("loopStart"), 0},
            {QStringLiteral("entries"),
             QJsonArray{MakeGoldenClimateEntry(1, 40, 20), MakeGoldenClimateEntry(2, 30, 10)}},
        },
        QJsonObject{
            {QStringLiteral("id"), 20},
            {QStringLiteral("name"), QStringLiteral("Valley")},
            {QStringLiteral("loopStart"), 1},
            {QStringLiteral("entries"),
             QJsonArray{MakeGoldenClimateEntry(2, 20, 10),
                        MakeGoldenClimateEntry(3, 30, 15),
                        MakeGoldenClimateEntry(1, 25, 5)}},
        },
        QJsonObject{
            {QStringLiteral("id"), 30},
            {QStringLiteral("name"), QStringLiteral("Ridge")},
            {QStringLiteral("loopStart"), 0},
            {QStringLiteral("entries"),
             QJsonArray{MakeGoldenClimateEntry(2, 15, 15), MakeGoldenClimateEntry(1, 0, 15)}},
        },
    };
    const QJsonArray regions{
        QJsonObject{
            {QStringLiteral("id"), 7},
            {QStringLiteral("name"), QStringLiteral("Valley")},
            {QStringLiteral("priority"), 1},
            {QStringLiteral("schedule"), 20},
            {QStringLiteral("bounds"), MakeGoldenClimateBounds(-800.0, -500.0, 600.0, 700.0)},
            {QStringLiteral("feather"), 200.0},
        },
        QJsonObject{
            {QStringLiteral("id"), 3},
            {QStringLiteral("name"), QStringLiteral("Ridge")},
            {QStringLiteral("priority"), 5},
            {QStringLiteral("schedule"), 30},
            {QStringLiteral("bounds"), MakeGoldenClimateBounds(200.0, -300.0, 900.0, 300.0)},
            {QStringLiteral("feather"), 0.0},
        },
    };
    return {
        {QStringLiteral("version"), WORLD_CLIMATE_VERSION},
        {QStringLiteral("id"), 1},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("domain"), MakeGoldenClimateBounds(-2000.0, -2000.0, 2000.0, 2000.0)},
        {QStringLiteral("defaultSchedule"), 10},
        {QStringLiteral("presets"), presets},
        {QStringLiteral("schedules"), schedules},
        {QStringLiteral("regions"), regions},
    };
}
} // namespace ludus::editor::tests
