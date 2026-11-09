#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QByteArray;
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
};

/// Decode a Photon search response. Skip unusable places and preserve outPlaces on a malformed response.
[[nodiscard]] bool TryReadEarthPlaces(const QByteArray& response, QList<EarthPlace>& outPlaces);

/// Open a modal picker owned by the inspector row. Choosing a result fills both coordinate fields
/// and emits one draft edit. Closing the picker or destroying its owner discards pending results.
void OpenWorldEarthLocation(QWidget& inoutOwner, QLineEdit& inoutLatitude, QLineEdit& inoutLongitude);
} // namespace ludus::editor
