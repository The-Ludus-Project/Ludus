#pragma once

// Small widgets that draw what a style sheet used to: a left-aligned flat button, a translucent label over the
// viewport and an outline around a control. They take their colours from the design tokens and the palette, so no
// widget needs a style sheet of its own (docs/architecture/editor-art-direction.md).

#include "design_tokens.h"
#include "internal/theme_colour.h"

#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QWidget>

namespace ludus::editor
{
/// A push button whose text starts at the left edge instead of the middle, as a list row does.
class LeftAlignedButton final : public QPushButton
{
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        static_cast<void>(event);
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        const QString text = option.text;
        option.text.clear();
        painter.drawControl(QStyle::CE_PushButton, option);
        const QRect contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        painter.drawItemText(contents.adjusted(design::SPACING_MEDIUM, 0, 0, 0),
                             Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic,
                             palette(),
                             isEnabled(),
                             text,
                             QPalette::ButtonText);
    }
};

/// A label on a translucent rounded panel with the viewport's HUD text colour, for text drawn over the viewport.
class HudLabel final : public QLabel
{
public:
    static constexpr int BACKGROUND_OPACITY = 190;
    static constexpr int PADDING = design::SPACING_SMALL - design::SPACING_EXTRA_SMALL / 2;

    explicit HudLabel(QWidget* parent) : QLabel(parent)
    {
        setContentsMargins(PADDING, PADDING, PADDING, PADDING);
        QPalette hud = palette();
        hud.setColor(QPalette::WindowText, QColor::fromRgb(GetViewportArgb(design::ThemeColour::TextHud)));
        setPalette(hud);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(
            QColor::fromRgba(WithAlpha(GetViewportArgb(design::ThemeColour::SurfaceViewport), BACKGROUND_OPACITY)));
        painter.drawRoundedRect(rect(), design::RADIUS_CONTROL, design::RADIUS_CONTROL);
        painter.end();
        QLabel::paintEvent(event);
    }
};

/// A frame in the danger colour drawn over a control, to point at the field a diagnostic names. It follows the
/// control's size, ignores the mouse and leaves the control's own painting alone.
class DiagnosticOutline final : public QWidget
{
public:
    static constexpr int WIDTH = 2;

    explicit DiagnosticOutline(QWidget* target) : QWidget(target)
    {
        setObjectName(QStringLiteral("diagnosticOutline"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        target->installEventFilter(this);
        setGeometry(target->rect());
        raise();
        show();
    }

    /// Outline the control once; later calls leave the existing outline.
    static void Attach(QWidget* target)
    {
        // The class has no Q_OBJECT (it is header-only), so find an existing outline by name.
        if (target->findChild<QWidget*>(QStringLiteral("diagnosticOutline"), Qt::FindDirectChildrenOnly) == nullptr)
        {
            new DiagnosticOutline(target);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded))
        {
            setGeometry(parentWidget()->rect());
            raise();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        static_cast<void>(event);
        QPainter painter(this);
        painter.setPen(QPen(GetThemeQColour(design::ThemeColour::StatusDanger), WIDTH));
        painter.setBrush(Qt::NoBrush);
        const qreal inset = static_cast<qreal>(WIDTH) / 2;
        painter.drawRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
    }
};
} // namespace ludus::editor
