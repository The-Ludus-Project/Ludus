#pragma once

#include <ludus/foundation/base/types.h>

#include <QDateTime>
#include <QString>
#include <QTimeZone>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QDateTimeEdit;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Edit a fixed preview instant on a time zone's clock while retaining the stored fractional UTC-day
/// representation.
class WorldEarthTimeField final : public QWidget
{
    Q_OBJECT
public:
    /// @param daysSinceEpoch Fractional UTC days since 2000-01-01 12:00 UTC.
    /// @param timeZoneId IANA identifier whose clock the field shows. An identifier this Qt build cannot
    /// resolve shows UTC, and the caption says so.
    WorldEarthTimeField(foundation::float64 daysSinceEpoch, const QString& timeZoneId, QWidget& inoutParent);
    /// Preserve the original numeric value if the displayed instant is unchanged.
    [[nodiscard]] foundation::float64 GetDaysSinceEpoch() const;
    /// Show another time zone's clock at the same reading, such as 12:00 in Seoul becoming 12:00 in Toronto.
    /// The instant changes by the difference between the zones' offsets and Edited is emitted when it does.
    /// The reading is clamped to the supported Earth date range.
    void SetTimeZoneKeepingClock(const QString& timeZoneId);
    /// The current UTC instant as fractional days since 2000-01-01 12:00 UTC, clamped to the Earth preset range.
    [[nodiscard]] static foundation::float64 GetCurrentDaysSinceEpoch();
    /// Format a stored instant for display, such as "2026-11-15 21:27:32 UTC".
    /// @param daysSinceEpoch Fractional UTC days since 2000-01-01 12:00 UTC.
    [[nodiscard]] static QString FormatDaysSinceEpoch(foundation::float64 daysSinceEpoch);

Q_SIGNALS:
    void Edited();

private:
    /// The UTC instant that the shown clock reading means in the current zone.
    [[nodiscard]] QDateTime getInstant() const;
    void setTimeZone(const QString& timeZoneId);
    void showInstant(const QDateTime& instant);
    void updateCaption();

    QDateTimeEdit* mDateTime;
    QLabel* mCaption;
    QTimeZone mTimeZone;
    QString mTimeZoneId;
    QDateTime mShownClock;
    QDateTime mShownInstant;
    bool mIsTimeZoneKnown = true;
    foundation::float64 mOriginalDays;
};
} // namespace ludus::editor
