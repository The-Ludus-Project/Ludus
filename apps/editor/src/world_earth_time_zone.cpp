#include "internal/world_earth_time_zone.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QByteArray>
#include <QComboBox>
#include <QFile>
#include <QList>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;

constexpr int32 LATITUDE_DEGREE_DIGITS = 2;
constexpr int32 LONGITUDE_DEGREE_DIGITS = 3;
constexpr int32 MINUTE_DIGITS = 2;
constexpr int32 SECOND_DIGITS = 2;
constexpr int32 MINUTES_PER_DEGREE = 60;
constexpr int32 SECONDS_PER_DEGREE = 3'600;
constexpr qsizetype MAX_TIME_ZONE_ID_CHARACTERS = 64;
constexpr qsizetype ZONE_TAB_COLUMNS = 3;
constexpr int32 ZONE_LIST_MINIMUM_CHARACTERS = 18;

struct EarthTimeZoneLocation final
{
    QString CountryCode;
    QString Id;
    float64 X;
    float64 Y;
    float64 Z;
};

/// Read one ISO 6709 angle, `±DDMM` or `±DDMMSS` (latitude) and `±DDDMM` or `±DDDMMSS` (longitude), in degrees.
bool TryReadIso6709Angle(QStringView text, int32 degreeDigits, float64& outDegrees)
{
    const qsizetype digits = text.size() - 1;
    const bool hasSeconds = digits == degreeDigits + MINUTE_DIGITS + SECOND_DIGITS;
    if ((digits != degreeDigits + MINUTE_DIGITS && hasSeconds == false) ||
        (text.front() != u'+' && text.front() != u'-'))
    {
        return false;
    }
    bool isDegreesValid = false;
    bool isMinutesValid = false;
    bool isSecondsValid = true;
    const int32 degrees = text.sliced(1, degreeDigits).toInt(&isDegreesValid);
    const int32 minutes = text.sliced(1 + degreeDigits, MINUTE_DIGITS).toInt(&isMinutesValid);
    const int32 seconds =
        hasSeconds == true ? text.sliced(1 + degreeDigits + MINUTE_DIGITS, SECOND_DIGITS).toInt(&isSecondsValid) : 0;
    if (isDegreesValid == false || isMinutesValid == false || isSecondsValid == false)
    {
        return false;
    }
    const float64 magnitude = static_cast<float64>(degrees) +
                              static_cast<float64>(minutes) / static_cast<float64>(MINUTES_PER_DEGREE) +
                              static_cast<float64>(seconds) / static_cast<float64>(SECONDS_PER_DEGREE);
    outDegrees = text.front() == u'-' ? -magnitude : magnitude;
    return true;
}

void ComputeUnitVector(float64 latitudeDegrees, float64 longitudeDegrees, EarthTimeZoneLocation& outLocation)
{
    using namespace foundation::math;
    const float64 latitude = DegreesToRadians(latitudeDegrees);
    const float64 longitude = DegreesToRadians(longitudeDegrees);
    outLocation.X = Cos(latitude) * Cos(longitude);
    outLocation.Y = Cos(latitude) * Sin(longitude);
    outLocation.Z = Sin(latitude);
}

// Thanks to the tz database contributors, "zone.tab" (public domain), https://www.iana.org/time-zones:
// each row names a country, the ISO 6709 coordinates of a zone's principal location and its identifier.
QList<EarthTimeZoneLocation> ReadEarthTimeZoneLocations()
{
    QList<EarthTimeZoneLocation> locations;
    QFile file(QStringLiteral(":/time-zones/zone.tab"));
    if (file.open(QIODevice::ReadOnly) == false)
    {
        return locations;
    }
    const QString table = QString::fromLatin1(file.readAll());
    for (const QStringView line : QStringView(table).split(u'\n'))
    {
        if (line.isEmpty() == true || line.front() == u'#')
        {
            continue;
        }
        const QList<QStringView> columns = line.split(u'\t');
        if (columns.size() < ZONE_TAB_COLUMNS)
        {
            continue;
        }
        const QStringView coordinates = columns.at(1);
        qsizetype longitudeStart = coordinates.indexOf(u'+', 1);
        if (longitudeStart < 0)
        {
            longitudeStart = coordinates.indexOf(u'-', 1);
        }
        float64 latitude = 0;
        float64 longitude = 0;
        if (longitudeStart < 0 ||
            TryReadIso6709Angle(coordinates.first(longitudeStart), LATITUDE_DEGREE_DIGITS, latitude) == false ||
            TryReadIso6709Angle(coordinates.sliced(longitudeStart), LONGITUDE_DEGREE_DIGITS, longitude) == false ||
            IsEarthTimeZoneIdValid(columns.at(2)) == false)
        {
            continue;
        }
        EarthTimeZoneLocation location{columns.at(0).toString(), columns.at(2).toString(), 0, 0, 0};
        ComputeUnitVector(latitude, longitude, location);
        locations.append(location);
    }
    return locations;
}

const QList<EarthTimeZoneLocation>& GetEarthTimeZoneLocations()
{
    static const QList<EarthTimeZoneLocation> LOCATIONS = ReadEarthTimeZoneLocations();
    return LOCATIONS;
}

bool IsTimeZoneIdCharacter(QChar character)
{
    return (character >= u'A' && character <= u'Z') || (character >= u'a' && character <= u'z') ||
           (character >= u'0' && character <= u'9') || character == u'_' || character == u'-' || character == u'+';
}
} // namespace

const QStringList& GetEarthTimeZoneIds()
{
    static const QStringList IDS = []() {
        QStringList sorted;
        for (const EarthTimeZoneLocation& location : GetEarthTimeZoneLocations())
        {
            if (sorted.contains(location.Id) == false)
            {
                sorted.append(location.Id);
            }
        }
        sorted.sort();
        return sorted;
    }();
    return IDS;
}

QString FindEarthTimeZone(float64 latitudeDegrees, float64 longitudeDegrees, QStringView countryCode)
{
    EarthTimeZoneLocation place;
    ComputeUnitVector(latitudeDegrees, longitudeDegrees, place);
    const QList<EarthTimeZoneLocation>& locations = GetEarthTimeZoneLocations();
    bool hasCountry = false;
    for (const EarthTimeZoneLocation& location : locations)
    {
        hasCountry = hasCountry == true || location.CountryCode.compare(countryCode, Qt::CaseInsensitive) == 0;
    }
    // The largest dot product of unit vectors is the smallest great-circle distance.
    QString nearest = WORLD_EARTH_UTC_TIME_ZONE;
    bool hasNearest = false;
    float64 nearestDot = 0;
    for (const EarthTimeZoneLocation& location : locations)
    {
        if (hasCountry == true && location.CountryCode.compare(countryCode, Qt::CaseInsensitive) != 0)
        {
            continue;
        }
        const float64 dot = location.X * place.X + location.Y * place.Y + location.Z * place.Z;
        if (hasNearest == false || dot > nearestDot)
        {
            hasNearest = true;
            nearestDot = dot;
            nearest = location.Id;
        }
    }
    return nearest;
}

bool IsEarthTimeZoneIdValid(QStringView id)
{
    if (id == WORLD_EARTH_UTC_TIME_ZONE)
    {
        return true;
    }
    if (id.isEmpty() == true || id.size() > MAX_TIME_ZONE_ID_CHARACTERS)
    {
        return false;
    }
    for (const QStringView component : id.split(u'/'))
    {
        if (component.isEmpty() == true || component.front() == u'-')
        {
            return false;
        }
        for (const QChar character : component)
        {
            if (IsTimeZoneIdCharacter(character) == false)
            {
                return false;
            }
        }
    }
    return true;
}

QComboBox* MakeEarthTimeZoneField(const QString& current, QWidget& inoutParent)
{
    QComboBox* field = new QComboBox(&inoutParent);
    field->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    field->setMinimumContentsLength(ZONE_LIST_MINIMUM_CHARACTERS);
    field->addItem(WORLD_EARTH_UTC_TIME_ZONE, QString(WORLD_EARTH_UTC_TIME_ZONE));
    for (const QString& id : GetEarthTimeZoneIds())
    {
        field->addItem(id, id);
    }
    SelectEarthTimeZone(*field, current);
    return field;
}

void SelectEarthTimeZone(QComboBox& inoutField, const QString& id)
{
    qsizetype index = inoutField.findData(id);
    if (index < 0)
    {
        inoutField.addItem(id, id);
        index = inoutField.count() - 1;
    }
    inoutField.setCurrentIndex(static_cast<int32>(index));
}
} // namespace ludus::editor
