#pragma once

#include <ludus/foundation/base/types.h>

#include <QLayout>
#include <QList>

namespace ludus::editor
{
/// Lays its items out left to right at their size hints and wraps onto a new row when the next one would not fit, so
/// a row of buttons in a narrow dock grows taller instead of clipping a label ("Import WAV / FL"). An item wider than
/// the row takes the whole row. Hidden items take no space. GUI thread only.
class FlowLayout final : public QLayout
{
public:
    /// spacingPixels separates neighbouring items across and between rows.
    FlowLayout(QWidget* parentOrNull, foundation::int32 spacingPixels);
    ~FlowLayout() override;
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    [[nodiscard]] QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rectangle) override;
    /// Every item in one row.
    [[nodiscard]] QSize sizeHint() const override;
    /// The widest single item: the narrowest the row can be without clipping one.
    [[nodiscard]] QSize minimumSize() const override;

private:
    /// Places the items in area (when shouldPlace) and returns the height they need.
    foundation::int32 arrange(const QRect& area, bool shouldPlace) const;

    QList<QLayoutItem*> mItems;
};
} // namespace ludus::editor
