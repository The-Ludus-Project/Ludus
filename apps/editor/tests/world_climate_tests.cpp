// World climate authoring: the "climate" root member cooks with the engine cooker to the same bytes as the golden
// weather corpus, reports failures by record with a repair, and changes through one undoable World transaction.
#include "internal/builtin_world.h"
#include "internal/world_climate.h"
#include "internal/world_document.h"
#include "weather_golden_corpus.h"
#include "world_climate_fixture.h"

#include <ludus/gameplay/weather/climate.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

namespace ludus::editor::tests
{
namespace
{
using namespace Qt::StringLiterals;
namespace weather_testing = gameplay::weather::testing;
using foundation::Array;
using foundation::uint8;
using foundation::usize;

bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right)
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

QString CookError(const QJsonObject& climate)
{
    Array<uint8> bytes;
    QString error;
    CHECK(TryCookWorldClimate(climate, bytes, error) == false);
    CHECK(bytes.IsEmpty() == true);
    return error;
}

QJsonObject EditArrayMember(QJsonObject climate,
                            QLatin1StringView list,
                            qsizetype index,
                            QLatin1StringView key,
                            const QJsonValue& value)
{
    QJsonArray records = climate.value(list).toArray();
    QJsonObject record = records[index].toObject();
    record.insert(key, value);
    records[index] = record;
    climate.insert(list, records);
    return climate;
}

WorldDocument MakeBuiltInWorld()
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    return world;
}
} // namespace

TEST_CASE("A World climate cooks to the golden weather corpus climate", "[world][climate]")
{
    Array<uint8> authored;
    QString error;
    REQUIRE(TryCookWorldClimate(MakeGoldenWorldClimate(), authored, error) == true);
    CHECK(error.isEmpty() == true);
    Array<uint8> corpus;
    REQUIRE(weather_testing::TryCookGoldenClimate(false, 1, corpus) == true);
    CHECK(AreBytesEqual(authored, corpus) == true);

    gameplay::weather::Climate climate;
    REQUIRE(gameplay::weather::TryLoadClimate({authored.GetData(), authored.GetSize()}, climate) ==
            content::weather_format::WeatherFormatStatus::Success);
    CHECK(climate.GetHeader().TicksPerSecond == content::scene_format::PLAYABLE_WORLD_TICKS_PER_SECOND);
    CHECK(climate.GetRegions().GetSize() == 2);
}

TEST_CASE("World climate failures name the record and its repair", "[world][climate]")
{
    const QJsonObject golden = MakeGoldenWorldClimate();
    SECTION("A channel outside its range names the preset and the range")
    {
        const QString error = CookError(EditArrayMember(golden, "presets"_L1, 3, "humidity"_L1, 1.5));
        CHECK(error.contains(u"Preset “Storm”"_s) == true);
        CHECK(error.contains(u"humidity must be 0 to 1"_s) == true);
    }
    SECTION("A missing preset names the schedule entry")
    {
        QJsonObject climate = golden;
        QJsonArray schedules = climate.value("schedules"_L1).toArray();
        QJsonObject valley = schedules[1].toObject();
        QJsonArray entries = valley.value("entries"_L1).toArray();
        entries[1] = MakeGoldenClimateEntry(99, 30, 15);
        valley.insert("entries"_L1, entries);
        schedules[1] = valley;
        climate.insert("schedules"_L1, schedules);
        const QString error = CookError(climate);
        CHECK(error.contains(u"Entry 2 of schedule “Valley”"_s) == true);
        CHECK(error.contains(u"Choose one of the climate's presets"_s) == true);
    }
    SECTION("A region outside the domain names the region and the fix")
    {
        const QString error = CookError(
            EditArrayMember(golden, "regions"_L1, 1, "bounds"_L1, MakeGoldenClimateBounds(200, -300, 2500, 300)));
        CHECK(error.contains(u"Region “Ridge”"_s) == true);
        CHECK(error.contains(u"enlarge the domain"_s) == true);
    }
    SECTION("A malformed member names the expected fields")
    {
        QJsonObject climate = golden;
        climate.remove("domain"_L1);
        CHECK(CookError(climate).contains(u"domain"_s) == true);
        CHECK(CookError(EditArrayMember(golden, "presets"_L1, 0, "groundWind"_L1, QJsonArray{1, 2}))
                  .contains(u"three-component winds"_s) == true);
        CHECK(CookError(EditArrayMember(golden, "regions"_L1, 0, "name"_L1, u""_s)).contains(u"name of 1 to"_s) ==
              true);
    }
    SECTION("A newer layout asks for the Editor that saved it")
    {
        QJsonObject climate = golden;
        climate.insert("version"_L1, WORLD_CLIMATE_VERSION + 1);
        CHECK(CookError(climate).contains(u"Editor version that saved it"_s) == true);
    }
}

TEST_CASE("Climate edits are one undoable World transaction that saves and reloads", "[world][climate]")
{
    WorldDocument world = MakeBuiltInWorld();
    const QJsonObject withoutClimate = world.Draft();
    const usize history = world.GetHistory().GetCount();
    QString error;

    REQUIRE(world.SetClimate(MakeGoldenWorldClimate(), error) == true);
    CHECK(world.GetHistory().GetCount() == history + 1);
    const QJsonObject first = world.Draft().value(WORLD_CLIMATE).toObject();
    CHECK(first.value("revision"_L1).toInteger() == 1);

    // An unchanged climate adds no history and keeps every revision.
    REQUIRE(world.SetClimate(first, error) == true);
    CHECK(world.GetHistory().GetCount() == history + 1);

    // Changing one preset raises its revision and the climate's, and leaves the others alone.
    REQUIRE(world.SetClimate(EditArrayMember(first, "presets"_L1, 2, "precipitation"_L1, 9.5), error) == true);
    const QJsonObject second = world.Draft().value(WORLD_CLIMATE).toObject();
    CHECK(second.value("revision"_L1).toInteger() == 2);
    CHECK(second.value("presets"_L1).toArray()[2].toObject().value("revision"_L1).toInteger() == 2);
    CHECK(second.value("presets"_L1).toArray()[0].toObject().value("revision"_L1).toInteger() == 1);

    // A rejected edit names the failure and preserves the draft and history.
    const usize accepted = world.GetHistory().GetCount();
    CHECK(world.SetClimate(EditArrayMember(second, "presets"_L1, 0, "temperature"_L1, 150.0), error) == false);
    CHECK(error.contains(u"Preset “Clear”"_s) == true);
    CHECK(world.Draft().value(WORLD_CLIMATE).toObject() == second);
    CHECK(world.GetHistory().GetCount() == accepted);
    CHECK(world.SetClimate(QJsonValue(3), error) == false);

    // Save and reopen keep the climate exactly.
    WorldDocument reopened = MakeBuiltInWorld();
    REQUIRE(reopened.Load(world.Encode(), error) == true);
    CHECK(reopened.Draft().value(WORLD_CLIMATE).toObject() == second);

    // Undo walks back through the climate edits to the World without one.
    REQUIRE(world.Undo() == true);
    CHECK(world.Draft().value(WORLD_CLIMATE).toObject() == first);
    REQUIRE(world.Undo() == true);
    CHECK(world.Draft() == withoutClimate);
    REQUIRE(world.Redo() == true);
    REQUIRE(world.SetClimate(QJsonValue::Undefined, error) == true);
    CHECK(world.Draft().contains(WORLD_CLIMATE) == false);

    // Loading rejects an invalid climate with the record and its repair.
    QJsonObject invalid = world.Draft();
    invalid.insert(WORLD_CLIMATE, EditArrayMember(second, "regions"_L1, 0, "schedule"_L1, 77));
    WorldDocument rejected = MakeBuiltInWorld();
    CHECK(rejected.Load(QJsonDocument(invalid).toJson(QJsonDocument::Compact), error) == false);
    CHECK(error.contains(u"Region “Valley” uses a schedule that does not exist"_s) == true);
}
} // namespace ludus::editor::tests
