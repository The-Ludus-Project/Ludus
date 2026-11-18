#include "internal/world_earth_time_field.h"

#include "internal/world_earth_time_zone.h"

#include <ludus/gameplay/world/celestial_transform.hpp>

#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTime>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QtGlobal>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::int64;

constexpr int32 EPOCH_YEAR = 2000;
constexpr int32 EPOCH_HOUR_UTC = 12;
constexpr int64 MILLISECONDS_PER_DAY = 86'400'000;

QDateTime GetEarthEpoch()
{
    // Thanks to The Qt Company, "QTimeZone Class", lightweight time representations:
    // https://doc.qt.io/qt-6/qtimezone.html#QTimeZone-1
    // Qt 6.5 added the UTC constructor, which also works without timezone data on Wasm.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QDateTime(QDate(EPOCH_YEAR, 1, 1), QTime(EPOCH_HOUR_UTC, 0), QTimeZone(QTimeZone::UTC));
#else
    return QDateTime(QDate(EPOCH_YEAR, 1, 1), QTime(EPOCH_HOUR_UTC, 0), Qt::UTC);
#endif
}

QDateTime ConvertDaysToUtc(float64 daysSinceEpoch)
{
    return GetEarthEpoch().addMSecs(qRound64(daysSinceEpoch * static_cast<float64>(MILLISECONDS_PER_DAY)));
}

QTimeZone GetUtcTimeZone()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QTimeZone(QTimeZone::UTC);
#else
    return QTimeZone::utc();
#endif
}

/// The clock reading of an instant in a zone, held as a UTC date and time so the editor shows it unchanged.
QDateTime ConvertInstantToClock(const QDateTime& instant, const QTimeZone& timeZone)
{
    const QDateTime local = instant.toTimeZone(timeZone);
    return QDateTime(local.date(), local.time(), GetUtcTimeZone());
}

QDateTime ConvertClockToInstant(const QDateTime& clock, const QTimeZone& timeZone)
{
    return QDateTime(clock.date(), clock.time(), timeZone).toUTC();
}

/// The tzdb zone for an IANA id, or an invalid zone when the id is unknown or this Qt build has no zone data.
QTimeZone FindTimeZone(const QString& timeZoneId)
{
    if (timeZoneId == WORLD_EARTH_UTC_TIME_ZONE)
    {
        return GetUtcTimeZone();
    }
    // The browser editor's Qt omits the timezone feature, which leaves only UTC and fixed-offset zones.
#if QT_CONFIG(timezone)
    return QTimeZone(timeZoneId.toLatin1());
#else
    return QTimeZone();
#endif
}

/// The zone's offset name at an instant, such as "UTC+09:00".
QString GetOffsetName(const QTimeZone& timeZone, const QDateTime& instant)
{
#if QT_CONFIG(timezone)
    return timeZone.displayName(instant, QTimeZone::OffsetName);
#else
    return instant.toTimeZone(timeZone).timeZoneAbbreviation();
#endif
}
} // namespace

WorldEarthTimeField::WorldEarthTimeField(float64 daysSinceEpoch, const QString& timeZoneId, QWidget& inoutParent)
    : QWidget(&inoutParent), mDateTime(new QDateTimeEdit(this)), mCaption(new QLabel(this)),
      mOriginalDays(daysSinceEpoch)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mDateTime->setObjectName(QStringLiteral("worldEarthDateTime"));
    mDateTime->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    // The edit holds the zone's clock reading as a UTC value; getInstant converts it to the stored instant.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    mDateTime->setTimeZone(GetUtcTimeZone());
#else
    mDateTime->setTimeSpec(Qt::UTC);
#endif
    mDateTime->setCalendarPopup(true);
    setTimeZone(timeZoneId);
    showInstant(ConvertDaysToUtc(daysSinceEpoch));
    setFocusProxy(mDateTime);
    mCaption->setObjectName(QStringLiteral("worldEarthDateTimeCaption"));
    mCaption->setWordWrap(true);
    mCaption->setBuddy(mDateTime);
    layout->addWidget(mCaption);
    layout->addWidget(mDateTime);
    updateCaption();
    QPushButton* now = new QPushButton(QStringLiteral("Use Now"), this);
    now->setObjectName(QStringLiteral("worldEarthUseNow"));
    now->setToolTip(QStringLiteral("Copy the current time into the preview date and time."));
    layout->addWidget(now);
    connect(now, &QPushButton::clicked, mDateTime, [this]() { showInstant(QDateTime::currentDateTimeUtc()); });
    connect(mDateTime, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        updateCaption();
        Q_EMIT Edited();
    });
}

void WorldEarthTimeField::SetTimeZoneKeepingClock(const QString& timeZoneId)
{
    if (timeZoneId == mTimeZoneId)
    {
        return;
    }
    const QDateTime before = getInstant();
    const QDateTime clock = mDateTime->dateTime();
    {
        const QSignalBlocker blocker(mDateTime);
        setTimeZone(timeZoneId);
        mDateTime->setDateTime(clock);
        mShownClock = mDateTime->dateTime();
        mShownInstant = ConvertClockToInstant(mShownClock, mTimeZone);
    }
    updateCaption();
    if (getInstant() != before)
    {
        Q_EMIT Edited();
    }
}

QDateTime WorldEarthTimeField::getInstant() const
{
    // An unedited reading keeps its instant, including the second pass through a repeated daylight-saving hour.
    const QDateTime clock = mDateTime->dateTime();
    return clock == mShownClock ? mShownInstant : ConvertClockToInstant(clock, mTimeZone);
}

void WorldEarthTimeField::setTimeZone(const QString& timeZoneId)
{
    mTimeZoneId = timeZoneId;
    const QTimeZone timeZone = FindTimeZone(timeZoneId);
    mIsTimeZoneKnown = timeZone.isValid();
    mTimeZone = mIsTimeZoneKnown == true ? timeZone : GetUtcTimeZone();
    mDateTime->setDateTimeRange(
        ConvertInstantToClock(ConvertDaysToUtc(-gameplay::world::MAX_EARTH_CELESTIAL_DAYS), mTimeZone),
        ConvertInstantToClock(ConvertDaysToUtc(gameplay::world::MAX_EARTH_CELESTIAL_DAYS), mTimeZone));
}

void WorldEarthTimeField::showInstant(const QDateTime& instant)
{
    mShownInstant = instant.toUTC();
    mShownClock = ConvertInstantToClock(mShownInstant, mTimeZone);
    mDateTime->setDateTime(mShownClock);
}

void WorldEarthTimeField::updateCaption()
{
    QString zone = QString(WORLD_EARTH_UTC_TIME_ZONE);
    if (mIsTimeZoneKnown == false)
    {
        zone = QStringLiteral("UTC; %1 time zone data is unavailable in this build").arg(mTimeZoneId);
    }
    else if (mTimeZoneId != WORLD_EARTH_UTC_TIME_ZONE)
    {
        zone = QStringLiteral("%1, %2").arg(mTimeZoneId, GetOffsetName(mTimeZone, getInstant()));
    }
    mCaption->setText(QStringLiteral("Date && Time (%1)").arg(zone));
    mDateTime->setAccessibleName(QStringLiteral("Date and time (%1)").arg(zone));
}

float64 WorldEarthTimeField::GetCurrentDaysSinceEpoch()
{
    const float64 days = static_cast<float64>(GetEarthEpoch().msecsTo(QDateTime::currentDateTimeUtc())) /
                         static_cast<float64>(MILLISECONDS_PER_DAY);
    return qBound(-gameplay::world::MAX_EARTH_CELESTIAL_DAYS, days, gameplay::world::MAX_EARTH_CELESTIAL_DAYS);
}

QString WorldEarthTimeField::FormatDaysSinceEpoch(float64 daysSinceEpoch)
{
    return ConvertDaysToUtc(daysSinceEpoch).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
}

float64 WorldEarthTimeField::GetDaysSinceEpoch() const
{
    const QDateTime current = getInstant();
    if (current == ConvertDaysToUtc(mOriginalDays))
    {
        return mOriginalDays;
    }
    return static_cast<float64>(GetEarthEpoch().msecsTo(current)) / static_cast<float64>(MILLISECONDS_PER_DAY);
}
} // namespace ludus::editor
