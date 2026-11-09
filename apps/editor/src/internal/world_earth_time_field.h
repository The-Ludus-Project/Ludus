#pragma once

#include <ludus/foundation/base/types.h>

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

Q_SIGNALS:
    void Edited();

private:
    QDateTimeEdit* mDateTime;
    foundation::float64 mOriginalDays;
};
} // namespace ludus::editor
