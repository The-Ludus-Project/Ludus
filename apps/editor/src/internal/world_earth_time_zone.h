#pragma once

#include <ludus/foundation/base/types.h>

#include <QLatin1StringView>
#include <QString>
#include <QStringList>
#include <QStringView>

QT_BEGIN_NAMESPACE
class QComboBox;
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Time zone of a World that names none. The stored Earth time is UTC whichever zone the editor shows.
inline constexpr QLatin1StringView WORLD_EARTH_UTC_TIME_ZONE{"UTC"};

/// IANA time zone identifiers listed in the editor's bundled tzdb `zone.tab`, sorted, without UTC.
/// Empty when the resource cannot be read. Call from the GUI thread.
[[nodiscard]] const QStringList& GetEarthTimeZoneIds();

/// The tzdb zone whose principal location is nearest the given place.
/// @param latitudeDegrees North-positive latitude.
/// @param longitudeDegrees East-positive longitude.
/// @param countryCode ISO 3166-1 alpha-2 code of the place, such as "KR". Only that country's zones are
/// considered when it has any; an empty or unlisted code considers every zone. Near a zone border inside one
/// country the nearest principal location can belong to the neighbouring zone.
/// @return A `zone.tab` identifier, or WORLD_EARTH_UTC_TIME_ZONE when the bundled list is unavailable.
[[nodiscard]] QString
FindEarthTimeZone(foundation::float64 latitudeDegrees, foundation::float64 longitudeDegrees, QStringView countryCode);

/// True for WORLD_EARTH_UTC_TIME_ZONE or a well-formed IANA identifier: slash-separated nonempty components of
/// ASCII letters, digits, '_', '-' and '+'. The identifier need not be in the bundled list.
[[nodiscard]] bool IsEarthTimeZoneIdValid(QStringView id);

/// Make a choice of UTC and the bundled zones whose item data is the identifier, showing `current` even when
/// the bundled list lacks it.
[[nodiscard]] QComboBox* MakeEarthTimeZoneField(const QString& current, QWidget& inoutParent);

/// Select a zone in a field made by MakeEarthTimeZoneField, adding it when the field lacks it.
void SelectEarthTimeZone(QComboBox& inoutField, const QString& id);
} // namespace ludus::editor
