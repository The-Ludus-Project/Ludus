#include "internal/world_earth_time_field.h"

#include <ludus/gameplay/world/celestial_transform.hpp>

#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QLabel>
#include <QPushButton>
#include <QTime>
#include <QVBoxLayout>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QTimeZone>
#endif

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
} // namespace

WorldEarthTimeField::WorldEarthTimeField(float64 daysSinceEpoch, QWidget& inoutParent)
    : QWidget(&inoutParent), mDateTime(new QDateTimeEdit(this)), mOriginalDays(daysSinceEpoch)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mDateTime->setObjectName(QStringLiteral("worldEarthDateTime"));
    mDateTime->setAccessibleName(QStringLiteral("Date and time (UTC)"));
    mDateTime->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    mDateTime->setTimeZone(QTimeZone(QTimeZone::UTC));
#else
    mDateTime->setTimeSpec(Qt::UTC);
#endif
    mDateTime->setDateTimeRange(ConvertDaysToUtc(-gameplay::world::MAX_EARTH_CELESTIAL_DAYS),
                                ConvertDaysToUtc(gameplay::world::MAX_EARTH_CELESTIAL_DAYS));
    mDateTime->setCalendarPopup(true);
    mDateTime->setDateTime(ConvertDaysToUtc(daysSinceEpoch));
    setFocusProxy(mDateTime);
    QLabel* caption = new QLabel(QStringLiteral("Date && Time (UTC)"), this);
    caption->setBuddy(mDateTime);
    layout->addWidget(caption);
    layout->addWidget(mDateTime);
    QPushButton* now = new QPushButton(QStringLiteral("Use Now"), this);
    now->setObjectName(QStringLiteral("worldEarthUseNow"));
    now->setToolTip(QStringLiteral("Copy the current time into the preview date and time."));
    layout->addWidget(now);
    connect(now, &QPushButton::clicked, mDateTime, [this]() {
        mDateTime->setDateTime(QDateTime::currentDateTimeUtc());
    });
    connect(mDateTime, &QDateTimeEdit::dateTimeChanged, this, &WorldEarthTimeField::Edited);
}

float64 WorldEarthTimeField::GetDaysSinceEpoch() const
{
    const QDateTime current = mDateTime->dateTime();
    if (current == ConvertDaysToUtc(mOriginalDays))
    {
        return mOriginalDays;
    }
    return static_cast<float64>(GetEarthEpoch().msecsTo(current)) / static_cast<float64>(MILLISECONDS_PER_DAY);
}
} // namespace ludus::editor
