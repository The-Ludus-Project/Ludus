#include "internal/world_earth_location.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/gameplay/world/celestial_transform.hpp>

#include <QByteArray>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointF>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#if defined(LUDUS_EDITOR_EARTH_POSITIONING)
#    include <QCoreApplication>
#    include <QGeoCoordinate>
#    include <QGeoPositionInfo>
#    include <QGeoPositionInfoSource>
#    if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#        include <QLocationPermission>
#    endif
#endif

#if defined(__EMSCRIPTEN__)
#    include <QHash>
#    include <emscripten.h>
#endif

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::int64;
using foundation::uint32;

constexpr int32 MAXIMUM_PLACES = 8;
constexpr int32 MAXIMUM_QUERY_CHARACTERS = 200;
constexpr int32 LOCATION_TIMEOUT_MILLISECONDS = 15'000;
constexpr int64 MAXIMUM_RESPONSE_BYTES = 128 * 1024;
constexpr int32 COORDINATE_DISPLAY_DIGITS = 10;

bool AreEarthCoordinatesValid(float64 latitude, float64 longitude)
{
    return foundation::math::Abs(latitude) <= gameplay::world::MAX_CELESTIAL_LATITUDE_DEGREES &&
           foundation::math::Abs(longitude) <= gameplay::world::MAX_CELESTIAL_LONGITUDE_DEGREES;
}

QString FormatCoordinate(float64 coordinate)
{
    return QString::number(coordinate, 'g', COORDINATE_DISPLAY_DIGITS);
}

QString FormatPlaceLabel(const QJsonObject& properties)
{
    const QString street = properties.value(QStringLiteral("street")).toString();
    const QString number = properties.value(QStringLiteral("housenumber")).toString();
    const QStringList parts{properties.value(QStringLiteral("name")).toString(),
                            (street + QLatin1Char(' ') + number).trimmed(),
                            properties.value(QStringLiteral("district")).toString(),
                            properties.value(QStringLiteral("city")).toString(),
                            properties.value(QStringLiteral("state")).toString(),
                            properties.value(QStringLiteral("postcode")).toString(),
                            properties.value(QStringLiteral("country")).toString()};
    QStringList label;
    for (const QString& part : parts)
    {
        if (part.isEmpty() == false && label.contains(part) == false)
        {
            label.append(part);
        }
    }
    return label.join(QStringLiteral(", "));
}

#if defined(__EMSCRIPTEN__)
// Thanks to W3C, "Geolocation", https://www.w3.org/TR/geolocation/.
// Request a single position with the browser's permission UI and return only latitude and longitude.
enum class BrowserPositionStatus : int32
{
    Success = 0,
    Denied = 1,
    Unavailable = 2,
};

// clang-format off
EM_JS(void, RequestBrowserEarthPosition, (uint32 request, int32 timeout), {
    if (globalThis.isSecureContext === false || navigator.geolocation === undefined) {
        Module._LudusReceiveEarthPosition(request, 2, 0, 0);
        return;
    }
    navigator.geolocation.getCurrentPosition(
        (position) => Module._LudusReceiveEarthPosition(request, 0, position.coords.latitude, position.coords.longitude),
        (error) => Module._LudusReceiveEarthPosition(request, error.code, 0, 0),
        { timeout, maximumAge: 0, enableHighAccuracy: false });
});
// clang-format on
class EarthLocationDialog;

QHash<uint32, EarthLocationDialog*>& GetBrowserEarthRequests()
{
    static QHash<uint32, EarthLocationDialog*> requests;
    return requests;
}
#endif

class EarthLocationDialog final : public QDialog
{
public:
    explicit EarthLocationDialog(QWidget& inoutParent)
        : QDialog(&inoutParent), mQuery(new QLineEdit(this)), mSearch(new QPushButton(QStringLiteral("Search"), this)),
          mLocate(new QPushButton(QStringLiteral("Use my location"), this)), mResults(new QListWidget(this)),
          mStatus(new QLabel(this)), mChoose(new QPushButton(QStringLiteral("Use Coordinates"), this)), mNetwork(this)
    {
        setWindowTitle(QStringLiteral("Choose Earth Location"));
        setObjectName(QStringLiteral("worldEarthLocationDialog"));
        setAttribute(Qt::WA_DeleteOnClose);
        QVBoxLayout* layout = new QVBoxLayout(this);
        QLabel* help = new QLabel(QStringLiteral("Search for a city, landmark, or address. Your search text is sent to "
                                                 "Photon. Device location is requested only when you choose it."),
                                  this);
        help->setWordWrap(true);
        layout->addWidget(help);

        QHBoxLayout* searchRow = new QHBoxLayout;
        mQuery->setObjectName(QStringLiteral("worldEarthPlaceQuery"));
        mQuery->setAccessibleName(QStringLiteral("City, landmark, or address"));
        mQuery->setPlaceholderText(QStringLiteral("City, landmark, or address"));
        mQuery->setMaxLength(MAXIMUM_QUERY_CHARACTERS);
        mSearch->setObjectName(QStringLiteral("worldEarthSearchPlace"));
        searchRow->addWidget(mQuery);
        searchRow->addWidget(mSearch);
        layout->addLayout(searchRow);

        mLocate->setObjectName(QStringLiteral("worldEarthUseLocation"));
        layout->addWidget(mLocate);
        mStatus->setTextFormat(Qt::PlainText);
        mStatus->setWordWrap(true);
        mStatus->setObjectName(QStringLiteral("worldEarthLocationStatus"));
        layout->addWidget(mStatus);

        mResults->setObjectName(QStringLiteral("worldEarthLocationResults"));
        mResults->setAccessibleName(QStringLiteral("Location results"));
        mResults->setWordWrap(true);
        layout->addWidget(mResults);

        QLabel* attribution = new QLabel(QStringLiteral("Search by <a href=\"https://photon.komoot.io/\">Photon</a>"
                                                        " · © <a href=\"https://www.openstreetmap.org/copyright\">"
                                                        "OpenStreetMap contributors</a>"),
                                         this);
        attribution->setOpenExternalLinks(true);
        layout->addWidget(attribution);
        mChoose->setObjectName(QStringLiteral("worldEarthUseCoordinates"));
        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        buttons->addButton(mChoose, QDialogButtonBox::AcceptRole);
        mSearch->setDefault(true);
        layout->addWidget(buttons);

        connect(mSearch, &QPushButton::clicked, this, [this]() { search(); });
        connect(mQuery, &QLineEdit::returnPressed, this, [this]() { search(); });
        connect(mLocate, &QPushButton::clicked, this, [this]() { locate(); });
        connect(mQuery, &QLineEdit::textChanged, this, [this]() {
            mResults->clear();
            mStatus->clear();
            updateButtons();
        });
        connect(mResults, &QListWidget::currentItemChanged, this, [this]() { updateButtons(); });
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        updateButtons();
    }

    ~EarthLocationDialog() override
    {
        if (mReplyOrNull != nullptr)
        {
            mReplyOrNull->disconnect(this);
            mReplyOrNull->abort();
        }
#if defined(__EMSCRIPTEN__)
        GetBrowserEarthRequests().remove(mBrowserRequest);
#endif
    }

#if defined(__EMSCRIPTEN__)
    void ReceiveBrowserPosition(BrowserPositionStatus status, float64 latitude, float64 longitude)
    {
        mBrowserRequest = 0;
        if (status == BrowserPositionStatus::Success)
        {
            showPosition(latitude, longitude);
        }
        else if (status == BrowserPositionStatus::Denied)
        {
            finish(QStringLiteral("Location permission was denied. Enable it in the browser or search for a place."));
        }
        else
        {
            finish(QStringLiteral(
                "Device location is unavailable. Check browser location settings or search for a place."));
        }
    }
#endif

    [[nodiscard]] bool TryGetCoordinates(QPointF& outCoordinates) const
    {
        const QListWidgetItem* itemOrNull = mResults->currentItem();
        if (mOperation != Operation::Idle || itemOrNull == nullptr)
        {
            return false;
        }
        outCoordinates = itemOrNull->data(Qt::UserRole).toPointF();
        return true;
    }

private:
    enum class Operation : foundation::uint8
    {
        Idle,
        Search,
        Position,
    };

    void updateButtons()
    {
        const bool isIdle = mOperation == Operation::Idle;
        mQuery->setEnabled(isIdle);
        mSearch->setEnabled(isIdle == true && mQuery->text().trimmed().isEmpty() == false);
        mLocate->setEnabled(isIdle);
        mChoose->setEnabled(isIdle == true && mResults->currentItem() != nullptr);
    }

    void begin(Operation operation, const QString& message)
    {
        mOperation = operation;
        mResults->clear();
        mStatus->setText(message);
        updateButtons();
    }

    void finish(const QString& message)
    {
        mOperation = Operation::Idle;
        mStatus->setText(message);
        updateButtons();
    }

    void addResult(const EarthPlace& place)
    {
        QListWidgetItem* item = new QListWidgetItem(
            QStringLiteral("%1\nLatitude %2°, longitude %3°")
                .arg(place.Label, FormatCoordinate(place.LatitudeDegrees), FormatCoordinate(place.LongitudeDegrees)),
            mResults);
        item->setData(Qt::UserRole, QPointF(place.LongitudeDegrees, place.LatitudeDegrees));
    }

    void search()
    {
        if (mSearch->isEnabled() == false)
        {
            return;
        }

        begin(Operation::Search, QStringLiteral("Searching Photon…"));
        // Thanks to the Photon contributors for the API v1 search and GeoJSON format:
        // https://github.com/komoot/photon/blob/master/docs/api-v1.md#results-for-search-and-reverse
        QUrl url(QStringLiteral("https://photon.komoot.io/api/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), QString::fromLatin1(QUrl::toPercentEncoding(mQuery->text().trimmed())));
        query.addQueryItem(QStringLiteral("limit"), QString::number(MAXIMUM_PLACES));
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
        QNetworkReply* reply = mNetwork.get(request);
        mReplyOrNull = reply;
        reply->setReadBufferSize(MAXIMUM_RESPONSE_BYTES + 1);
        QTimer::singleShot(LOCATION_TIMEOUT_MILLISECONDS, reply, [reply]() {
            if (reply->isRunning() == true)
            {
                reply->abort();
            }
        });
        connect(reply, &QIODevice::readyRead, reply, [reply]() {
            if (reply->bytesAvailable() > MAXIMUM_RESPONSE_BYTES)
            {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            mReplyOrNull = nullptr;
            reply->deleteLater();
            QList<EarthPlace> places;
            if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > MAXIMUM_RESPONSE_BYTES ||
                TryReadEarthPlaces(reply->readAll(), places) == false)
            {
                finish(
                    QStringLiteral("Search failed. Check your connection, retry, or enter coordinates in Properties."));
                return;
            }
            for (const EarthPlace& place : places)
            {
                addResult(place);
            }
            finish(places.isEmpty() == true
                       ? QStringLiteral("No matches. Try adding a region or country to your search.")
                       : QStringLiteral("Select the matching place to use its coordinates."));
        });
    }

    void locate()
    {
        begin(Operation::Position, QStringLiteral("Waiting for device location…"));
#if defined(__EMSCRIPTEN__)
        static uint32 nextRequest = 0;
        mBrowserRequest = ++nextRequest;
        if (mBrowserRequest == 0)
        {
            mBrowserRequest = ++nextRequest;
        }
        GetBrowserEarthRequests().insert(mBrowserRequest, this);
        QTimer::singleShot(LOCATION_TIMEOUT_MILLISECONDS, this, [this, request = mBrowserRequest]() {
            if (GetBrowserEarthRequests().remove(request) == true)
            {
                ReceiveBrowserPosition(BrowserPositionStatus::Unavailable, 0, 0);
            }
        });
        RequestBrowserEarthPosition(mBrowserRequest, LOCATION_TIMEOUT_MILLISECONDS);
#elif defined(LUDUS_EDITOR_EARTH_POSITIONING)
#    if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        QLocationPermission permission;
        permission.setAccuracy(QLocationPermission::Approximate);
        const Qt::PermissionStatus status = qApp->checkPermission(permission);
        if (status == Qt::PermissionStatus::Undetermined)
        {
            qApp->requestPermission(permission, this, [this](const QPermission& result) {
                if (result.status() == Qt::PermissionStatus::Granted)
                {
                    requestDevicePosition();
                }
                else
                {
                    showPermissionDenied();
                }
            });
            return;
        }
        if (status == Qt::PermissionStatus::Denied)
        {
            showPermissionDenied();
            return;
        }
#    endif
        requestDevicePosition();
#else
        finish(QStringLiteral("Install Qt Positioning and rebuild to use device location, or search for a place "
                              "or enter coordinates in Properties."));
#endif
    }

#if defined(LUDUS_EDITOR_EARTH_POSITIONING)
    void showPermissionDenied()
    {
        finish(QStringLiteral("Location permission was denied. Enable Location Services in system settings or search "
                              "for a place."));
    }

    void requestDevicePosition()
    {
        QGeoPositionInfoSource* sourceOrNull = QGeoPositionInfoSource::createDefaultSource(this);
        if (sourceOrNull == nullptr)
        {
            finish(QStringLiteral(
                "Device location is unavailable. Check system Location Services or search for a place."));
            return;
        }
        connect(sourceOrNull,
                &QGeoPositionInfoSource::positionUpdated,
                this,
                [this, sourceOrNull](const QGeoPositionInfo& position) {
                    sourceOrNull->disconnect(this);
                    sourceOrNull->deleteLater();
                    const QGeoCoordinate coordinate = position.coordinate();
                    showPosition(coordinate.latitude(), coordinate.longitude());
                });
        connect(sourceOrNull,
                &QGeoPositionInfoSource::errorOccurred,
                this,
                [this, sourceOrNull](QGeoPositionInfoSource::Error error) {
                    sourceOrNull->disconnect(this);
                    sourceOrNull->deleteLater();
                    if (error == QGeoPositionInfoSource::AccessError)
                    {
                        showPermissionDenied();
                    }
                    else
                    {
                        finish(QStringLiteral("Could not get device location. Check system Location Services or search "
                                              "for a place."));
                    }
                });
        sourceOrNull->requestUpdate(LOCATION_TIMEOUT_MILLISECONDS);
    }
#endif

#if defined(__EMSCRIPTEN__) || defined(LUDUS_EDITOR_EARTH_POSITIONING)
    void showPosition(float64 latitude, float64 longitude)
    {
        if (AreEarthCoordinatesValid(latitude, longitude) == false)
        {
            finish(QStringLiteral("The device returned invalid coordinates. Search for a place or enter coordinates."));
            return;
        }
        addResult({QStringLiteral("Device location"), latitude, longitude});
        mResults->setCurrentRow(0);
        finish(QStringLiteral("Review the coordinates before copying them to this World."));
    }
#endif

    QLineEdit* mQuery;
    QPushButton* mSearch;
    QPushButton* mLocate;
    QListWidget* mResults;
    QLabel* mStatus;
    QPushButton* mChoose;
    QNetworkAccessManager mNetwork;
    QNetworkReply* mReplyOrNull = nullptr;
    Operation mOperation = Operation::Idle;
#if defined(__EMSCRIPTEN__)
    uint32 mBrowserRequest = 0;
#endif
};
} // namespace

#if defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE void
LudusReceiveEarthPosition(uint32 request, int32 status, float64 latitude, float64 longitude)
{
    EarthLocationDialog* dialogOrNull = GetBrowserEarthRequests().take(request);
    if (dialogOrNull != nullptr)
    {
        dialogOrNull->ReceiveBrowserPosition(static_cast<BrowserPositionStatus>(status), latitude, longitude);
    }
}
#endif

bool TryReadEarthPlaces(const QByteArray& response, QList<EarthPlace>& outPlaces)
{
    const QJsonValue features = QJsonDocument::fromJson(response).object().value(QStringLiteral("features"));
    if (features.isArray() == false)
    {
        return false;
    }
    outPlaces.clear();
    for (const QJsonValue& feature : features.toArray())
    {
        const QJsonObject place = feature.toObject();
        const QJsonObject geometry = place.value(QStringLiteral("geometry")).toObject();
        const QJsonArray coordinates = geometry.value(QStringLiteral("coordinates")).toArray();
        if (geometry.value(QStringLiteral("type")).toString() != QStringLiteral("Point") || coordinates.size() < 2 ||
            coordinates.at(0).isDouble() == false || coordinates.at(1).isDouble() == false)
        {
            continue;
        }
        const float64 longitude = coordinates.at(0).toDouble();
        const float64 latitude = coordinates.at(1).toDouble();
        const QString label = FormatPlaceLabel(place.value(QStringLiteral("properties")).toObject());
        if (AreEarthCoordinatesValid(latitude, longitude) == true && label.isEmpty() == false)
        {
            outPlaces.append({label, latitude, longitude});
            if (outPlaces.size() == MAXIMUM_PLACES)
            {
                break;
            }
        }
    }
    return true;
}

void OpenWorldEarthLocation(QWidget& inoutOwner, QLineEdit& inoutLatitude, QLineEdit& inoutLongitude)
{
    EarthLocationDialog* dialog = new EarthLocationDialog(inoutOwner);
    QObject::connect(dialog, &QDialog::accepted, &inoutOwner, [dialog, &inoutOwner, &inoutLatitude, &inoutLongitude]() {
        QPointF coordinates;
        if (inoutOwner.isHidden() == false && dialog->TryGetCoordinates(coordinates) == true)
        {
            inoutLatitude.setText(FormatCoordinate(coordinates.y()));
            inoutLongitude.setText(FormatCoordinate(coordinates.x()));
            Q_EMIT inoutLongitude.textEdited(inoutLongitude.text());
        }
    });
    dialog->open();
}
} // namespace ludus::editor
