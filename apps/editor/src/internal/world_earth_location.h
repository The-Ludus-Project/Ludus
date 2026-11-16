#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QByteArray;
class QComboBox;
class QLineEdit;
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
struct EarthPlace final
{
    QString Label;
    foundation::float64 LatitudeDegrees;
    foundation::float64 LongitudeDegrees;
    /// ISO 3166-1 alpha-2 country code, such as "KR", or empty when the source does not name a country.
    QString CountryCode;
};

/// Decode a Photon search response. Skip unusable places and preserve outPlaces on a malformed response.
[[nodiscard]] bool TryReadEarthPlaces(const QByteArray& response, QList<EarthPlace>& outPlaces);

/// Open a modal picker owned by the inspector row. Choosing a result fills both coordinate fields
/// and emits one draft edit. Closing the picker or destroying its owner discards pending results.
/// @param timeZoneOrNull Time zone field made by MakeEarthTimeZoneField. When present, choosing a result also
/// selects the place's time zone (FindEarthTimeZone with the place's country).
void OpenWorldEarthLocation(QWidget& inoutOwner,
                            QLineEdit& inoutLatitude,
                            QLineEdit& inoutLongitude,
                            QComboBox* timeZoneOrNull);
} // namespace ludus::editor
