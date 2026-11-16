#pragma once

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QDateTimeEdit;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Edit a fixed UTC preview instant while retaining the stored fractional-day representation.
class WorldEarthTimeField final : public QWidget
{
    Q_OBJECT
public:
    WorldEarthTimeField(foundation::float64 daysSinceEpoch, QWidget& inoutParent);
    /// Preserve the original numeric value if the displayed instant is unchanged.
    [[nodiscard]] foundation::float64 GetDaysSinceEpoch() const;
    /// The current UTC instant as fractional days since 2000-01-01 12:00 UTC, clamped to the Earth preset range.
    [[nodiscard]] static foundation::float64 GetCurrentDaysSinceEpoch();
    /// Format a stored instant for display, such as "2026-11-15 21:27:32 UTC".
    /// @param daysSinceEpoch Fractional UTC days since 2000-01-01 12:00 UTC.
    [[nodiscard]] static QString FormatDaysSinceEpoch(foundation::float64 daysSinceEpoch);

Q_SIGNALS:
    void Edited();

private:
    QDateTimeEdit* mDateTime;
    foundation::float64 mOriginalDays;
};
} // namespace ludus::editor
