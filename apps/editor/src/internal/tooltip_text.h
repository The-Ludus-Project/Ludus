#pragma once

// Tooltips that stay short enough to leave the picture visible. Qt never wraps a plain-text tooltip, so one
// long sentence becomes a single line as wide as the sentence and can reach across the whole viewport. The
// editor breaks plain-text tooltips into lines no wider than design::SIZE_TOOLTIP_MAXIMUM_WIDTH; Qt keeps
// placing the tip beside the pointer, so it stays near the control that asked for it. GUI thread only.

#include "design_tokens.h"

#include <QFont>
#include <QObject>
#include <QString>

namespace ludus::editor
{
/// text broken at word boundaries into lines no wider than maximumWidth logical pixels in font. Existing line
/// breaks stay. A single word wider than maximumWidth keeps its own line. Rich text is returned unchanged,
/// because Qt lays that out itself.
[[nodiscard]] QString WrapTooltipText(const QString& text, const QFont& font, int maximumWidth);

/// Show text at positionGlobal as a tooltip for widgetOrNull, wrapped to the editor's maximum tooltip width.
void ShowEditorToolTip(const QPoint& positionGlobal, const QString& text, QWidget* widgetOrNull);

/// Application-wide filter: a widget (or a menu action when the menu shows tooltips) whose plain-text tooltip
/// is wider than design::SIZE_TOOLTIP_MAXIMUM_WIDTH shows it wrapped. Every other tooltip event goes to Qt.
class TooltipWidthFilter final : public QObject
{
public:
    explicit TooltipWidthFilter(QObject* parentOrNull);

    bool eventFilter(QObject* watched, QEvent* event) override;
};
} // namespace ludus::editor
