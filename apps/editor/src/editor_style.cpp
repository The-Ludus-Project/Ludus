#include "internal/editor_style.h"
#include "design_tokens.h"
#include "internal/theme_preferences.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>

namespace ludus::editor
{
namespace
{
/// Control edge and focus ring widths, in logical pixels; high contrast thickens both.
constexpr int CONTROL_EDGE_WIDTH = 1;
constexpr int HIGH_CONTRAST_CONTROL_EDGE_WIDTH = 2;
constexpr int FOCUS_RING_WIDTH = 2;
constexpr int HIGH_CONTRAST_FOCUS_RING_WIDTH = 3;
/// Padding between a control's edge and its content, in logical pixels.
constexpr int BUTTON_PADDING_VERTICAL = 5;
constexpr int BUTTON_PADDING_HORIZONTAL = 12;
constexpr int COMPACT_BUTTON_PADDING_VERTICAL = 2;
constexpr int COMPACT_BUTTON_PADDING_HORIZONTAL = 8;
constexpr int INPUT_PADDING = 4;
constexpr int COMBO_ARROW_PADDING = 20;
constexpr int TAB_PADDING_VERTICAL = 7;
constexpr int TAB_PADDING_HORIZONTAL = 12;
constexpr int TAB_UNDERLINE_WIDTH = 2;
constexpr int DOCK_TITLE_PADDING = 6;
constexpr int TOOL_BUTTON_PADDING = 4;
constexpr int HEADER_PADDING = 4;
constexpr int TOOL_BAR_SPACING = 4;
constexpr int SEPARATOR_EXTENT = 4;
constexpr qreal HALF = 0.5;
const char* const STYLE_OBJECT_NAME = "LudusEditorStyle";
const char* const VISUAL_ROLE_PROPERTY = "visualRole";
const char* const DENSITY_PROPERTY = "density";

QColor Token(design::ThemeColour role, bool isDark, bool isHighContrast)
{
    return QColor::fromRgb(design::GetThemeArgb(role, isDark, isHighContrast));
}

bool IsDarkPalette(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

bool HasProperty(const QWidget* widgetOrNull, const char* name, const char* value)
{
    return widgetOrNull != nullptr && widgetOrNull->property(name).toString() == QLatin1String(value);
}

/// Rounded control body with an edge drawn inside rect.
void DrawControlBody(QPainter* painter, const QRect& rect, const QColor& fill, const QColor& edge, int edgeWidth)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(edge, edgeWidth));
    painter->setBrush(fill);
    const qreal inset = static_cast<qreal>(edgeWidth) * HALF;
    painter->drawRoundedRect(QRectF(rect).adjusted(inset, inset, -inset, -inset),
                             design::RADIUS_CONTROL,
                             design::RADIUS_CONTROL);
    painter->restore();
}
} // namespace

QPalette MakeEditorPalette(const QPalette& platform, bool isDark, bool isHighContrast)
{
    const auto token = [isDark, isHighContrast](design::ThemeColour role) {
        return Token(role, isDark, isHighContrast);
    };
    const QColor workspace = token(design::ThemeColour::SurfaceWorkspace);
    const QColor panel = token(design::ThemeColour::SurfacePanel);
    const QColor raised = token(design::ThemeColour::SurfaceRaised);
    const QColor text = token(design::ThemeColour::TextPrimary);
    const QColor secondary = token(design::ThemeColour::TextSecondary);
    const QColor border = token(design::ThemeColour::BorderControl);
    const QColor subtleBorder = token(design::ThemeColour::BorderSubtle);
    const QColor accent = token(design::ThemeColour::AccentPrimary);
    const QColor accentText = token(design::ThemeColour::AccentOnPrimary);
    const QColor selected = token(design::ThemeColour::AccentSurface);
    QPalette palette = platform;
    palette.setColor(QPalette::Window, workspace);
    palette.setColor(QPalette::Base, panel);
    palette.setColor(QPalette::AlternateBase, raised);
    palette.setColor(QPalette::Button, raised);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, accentText);
    palette.setColor(QPalette::PlaceholderText, secondary);
    palette.setColor(QPalette::Mid, border);
    palette.setColor(QPalette::Midlight, subtleBorder);
    palette.setColor(QPalette::Light, raised);
    palette.setColor(QPalette::Dark, border);
    palette.setColor(QPalette::Shadow, subtleBorder);
    palette.setColor(QPalette::ToolTipBase, raised);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::Link, accent);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    {
        palette.setColor(QPalette::Disabled, role, secondary);
    }
    return palette;
}

EditorStyle::EditorStyle() : QProxyStyle(QStyleFactory::create(QApplication::style()->name()))
{
    setObjectName(QString::fromLatin1(STYLE_OBJECT_NAME));
}

void EditorStyle::Refresh()
{
    const bool isDark = IsDarkPalette(QApplication::palette());
    mIsHighContrast = GetThemePreferences().IsHighContrast;
    const bool isHighContrast = mIsHighContrast;
    mColours.Workspace = Token(design::ThemeColour::SurfaceWorkspace, isDark, isHighContrast);
    mColours.Panel = Token(design::ThemeColour::SurfacePanel, isDark, isHighContrast);
    mColours.Raised = Token(design::ThemeColour::SurfaceRaised, isDark, isHighContrast);
    mColours.Text = Token(design::ThemeColour::TextPrimary, isDark, isHighContrast);
    mColours.Secondary = Token(design::ThemeColour::TextSecondary, isDark, isHighContrast);
    mColours.Border = Token(design::ThemeColour::BorderControl, isDark, isHighContrast);
    mColours.SubtleBorder = Token(design::ThemeColour::BorderSubtle, isDark, isHighContrast);
    mColours.Accent = Token(design::ThemeColour::AccentPrimary, isDark, isHighContrast);
    mColours.AccentText = Token(design::ThemeColour::AccentOnPrimary, isDark, isHighContrast);
    mColours.Selected = Token(design::ThemeColour::AccentSurface, isDark, isHighContrast);
}

int EditorStyle::GetEdgeWidth() const noexcept
{
    return mIsHighContrast == true ? HIGH_CONTRAST_CONTROL_EDGE_WIDTH : CONTROL_EDGE_WIDTH;
}

int EditorStyle::GetFocusRingWidth() const noexcept
{
    return mIsHighContrast == true ? HIGH_CONTRAST_FOCUS_RING_WIDTH : FOCUS_RING_WIDTH;
}

void EditorStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    // Hover repaints for the controls drawn here that the platform style does not already track.
    if (qobject_cast<QTabBar*>(widget) != nullptr || qobject_cast<QToolButton*>(widget) != nullptr)
    {
        widget->setAttribute(Qt::WA_Hover, true);
    }
}

void EditorStyle::unpolish(QWidget* widget)
{
    if (qobject_cast<QTabBar*>(widget) != nullptr || qobject_cast<QToolButton*>(widget) != nullptr)
    {
        widget->setAttribute(Qt::WA_Hover, false);
    }
    QProxyStyle::unpolish(widget);
}

int EditorStyle::pixelMetric(PixelMetric metric, const QStyleOption* optionOrNull, const QWidget* widgetOrNull) const
{
    switch (metric)
    {
        case PM_DockWidgetSeparatorExtent:
            return SEPARATOR_EXTENT;
        case PM_DockWidgetTitleMargin:
            return DOCK_TITLE_PADDING;
        case PM_ToolBarItemSpacing:
            return TOOL_BAR_SPACING;
        case PM_ToolBarFrameWidth:
            return TOOL_BAR_SPACING;
        default:
            break;
    }
    return QProxyStyle::pixelMetric(metric, optionOrNull, widgetOrNull);
}

void EditorStyle::drawPrimitive(PrimitiveElement element,
                                const QStyleOption* option,
                                QPainter* painter,
                                const QWidget* widgetOrNull) const
{
    const bool isEnabled = option->state.testFlag(State_Enabled);
    const bool isFocused = option->state.testFlag(State_HasFocus);
    const bool isHovered = option->state.testFlag(State_MouseOver) && isEnabled == true;
    const int edgeWidth = GetEdgeWidth();
    const int ringWidth = GetFocusRingWidth();
    switch (element)
    {
        case PE_PanelButtonCommand: {
            const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            const bool isPrimary =
                isEnabled == true &&
                (HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "primary") == true ||
                 (button != nullptr && button->features.testFlag(QStyleOptionButton::DefaultButton)));
            QColor fill = mColours.Raised;
            QColor edge = mColours.Border;
            if (isEnabled == false)
            {
                fill = mColours.Workspace;
            }
            else if (isPrimary == true)
            {
                fill = mColours.Accent;
                edge = mColours.Accent;
            }
            else if (option->state.testFlag(State_Sunken) == true || option->state.testFlag(State_On) == true)
            {
                fill = mColours.Selected;
            }
            if (isHovered == true && isPrimary == false)
            {
                edge = mColours.Accent;
            }
            int width = edgeWidth;
            if (isFocused == true && isEnabled == true)
            {
                edge = mColours.Accent;
                width = ringWidth;
            }
            DrawControlBody(painter, option->rect, fill, edge, width);
            return;
        }
        case PE_PanelLineEdit: {
            const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
            if (frame == nullptr || frame->lineWidth <= 0)
            {
                break;
            }
            const bool isRingShown = isFocused == true && isEnabled == true;
            DrawControlBody(painter,
                            option->rect,
                            option->palette.color(QPalette::Base),
                            isRingShown == true ? mColours.Accent : mColours.Border,
                            isRingShown == true ? ringWidth : edgeWidth);
            return;
        }
        case PE_Frame: {
            const bool isFramedView = qobject_cast<const QAbstractItemView*>(widgetOrNull) != nullptr ||
                                      qobject_cast<const QTextEdit*>(widgetOrNull) != nullptr ||
                                      qobject_cast<const QPlainTextEdit*>(widgetOrNull) != nullptr;
            if (isFramedView == false)
            {
                break;
            }
            const bool isRingShown = isFocused == true && isEnabled == true;
            DrawControlBody(painter,
                            option->rect,
                            Qt::transparent,
                            isRingShown == true ? mColours.Accent : mColours.Border,
                            isRingShown == true ? ringWidth : edgeWidth);
            return;
        }
        case PE_FrameTabWidget: {
            painter->fillRect(option->rect, mColours.Panel);
            painter->save();
            painter->setPen(QPen(mColours.Border, edgeWidth));
            painter->setBrush(Qt::NoBrush);
            const qreal inset = static_cast<qreal>(edgeWidth) * HALF;
            painter->drawRect(QRectF(option->rect).adjusted(inset, inset, -inset, -inset));
            painter->restore();
            return;
        }
        case PE_FrameFocusRect:
            // Item views mark the current row by its selection, and the controls draw their own focus ring.
            if (qobject_cast<const QAbstractItemView*>(widgetOrNull) != nullptr)
            {
                return;
            }
            break;
        case PE_PanelButtonTool: {
            if (isHovered == true || option->state.testFlag(State_On) == true)
            {
                DrawControlBody(painter, option->rect, mColours.Raised, Qt::transparent, 0);
            }
            if (isFocused == true && isEnabled == true)
            {
                DrawControlBody(painter, option->rect, Qt::transparent, mColours.Accent, ringWidth);
            }
            return;
        }
        case PE_PanelStatusBar:
            painter->fillRect(option->rect, mColours.Workspace);
            painter->fillRect(QRect(option->rect.topLeft(), QSize(option->rect.width(), CONTROL_EDGE_WIDTH)),
                              mColours.SubtleBorder);
            return;
        case PE_IndicatorDockWidgetResizeHandle:
            painter->fillRect(option->rect, mColours.Border);
            return;
        default:
            break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widgetOrNull);
}

void EditorStyle::drawControl(ControlElement element,
                              const QStyleOption* option,
                              QPainter* painter,
                              const QWidget* widgetOrNull) const
{
    const bool isEnabled = option->state.testFlag(State_Enabled);
    switch (element)
    {
        case CE_PushButtonLabel: {
            const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            const bool isPrimary = isEnabled == true && button != nullptr &&
                                   (HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "primary") == true ||
                                    button->features.testFlag(QStyleOptionButton::DefaultButton));
            if (isPrimary == true)
            {
                QStyleOptionButton primary = *button;
                primary.palette.setColor(QPalette::ButtonText, mColours.AccentText);
                QProxyStyle::drawControl(element, &primary, painter, widgetOrNull);
                return;
            }
            break;
        }
        case CE_TabBarTabShape: {
            const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            if (tab == nullptr || (tab->shape != QTabBar::RoundedNorth && tab->shape != QTabBar::TriangularNorth))
            {
                break;
            }
            const bool isSelected = tab->state.testFlag(State_Selected);
            const bool isHovered = tab->state.testFlag(State_MouseOver) && isEnabled == true;
            QColor fill = mColours.Workspace;
            if (isHovered == true)
            {
                fill = mColours.Raised;
            }
            else if (isSelected == true)
            {
                fill = mColours.Panel;
            }
            painter->fillRect(tab->rect, fill);
            if (isSelected == true)
            {
                painter->fillRect(QRect(tab->rect.left(),
                                        tab->rect.bottom() + 1 - TAB_UNDERLINE_WIDTH,
                                        tab->rect.width(),
                                        TAB_UNDERLINE_WIDTH),
                                  mColours.Accent);
                if (tab->state.testFlag(State_HasFocus) == true)
                {
                    DrawControlBody(painter, tab->rect, Qt::transparent, mColours.Accent, GetFocusRingWidth());
                }
            }
            return;
        }
        case CE_DockWidgetTitle: {
            const auto* dock = qstyleoption_cast<const QStyleOptionDockWidget*>(option);
            if (dock == nullptr || dock->verticalTitleBar == true)
            {
                break;
            }
            painter->fillRect(dock->rect, mColours.Raised);
            painter->fillRect(QRect(dock->rect.left(),
                                    dock->rect.bottom() + 1 - CONTROL_EDGE_WIDTH,
                                    dock->rect.width(),
                                    CONTROL_EDGE_WIDTH),
                              mColours.SubtleBorder);
            const QRect titleRect = proxy()->subElementRect(SE_DockWidgetTitleBarText, dock, widgetOrNull);
            const QString title = dock->fontMetrics.elidedText(dock->title, Qt::ElideRight, titleRect.width());
            painter->setPen(mColours.Text);
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);
            return;
        }
        case CE_HeaderSection: {
            painter->fillRect(option->rect, mColours.Raised);
            painter->fillRect(QRect(option->rect.left(),
                                    option->rect.bottom() + 1 - CONTROL_EDGE_WIDTH,
                                    option->rect.width(),
                                    CONTROL_EDGE_WIDTH),
                              mColours.Border);
            return;
        }
        case CE_ToolBar: {
            painter->fillRect(option->rect, mColours.Workspace);
            if (option->state.testFlag(State_Horizontal) == true)
            {
                painter->fillRect(QRect(option->rect.left(),
                                        option->rect.bottom() + 1 - CONTROL_EDGE_WIDTH,
                                        option->rect.width(),
                                        CONTROL_EDGE_WIDTH),
                                  mColours.SubtleBorder);
            }
            return;
        }
        default:
            break;
    }
    QProxyStyle::drawControl(element, option, painter, widgetOrNull);
}

void EditorStyle::drawComplexControl(ComplexControl control,
                                     const QStyleOptionComplex* option,
                                     QPainter* painter,
                                     const QWidget* widgetOrNull) const
{
    const bool isEnabled = option->state.testFlag(State_Enabled);
    const bool isRingShown = option->state.testFlag(State_HasFocus) == true && isEnabled == true;
    const auto drawBody = [&]() {
        DrawControlBody(painter,
                        option->rect,
                        option->palette.color(QPalette::Base),
                        isRingShown == true ? mColours.Accent : mColours.Border,
                        isRingShown == true ? GetFocusRingWidth() : GetEdgeWidth());
    };
    if (control == CC_ComboBox)
    {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo != nullptr)
        {
            if (combo->frame == true)
            {
                drawBody();
            }
            QStyleOption arrow;
            arrow.initFrom(widgetOrNull != nullptr ? widgetOrNull : nullptr);
            arrow.rect = proxy()->subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widgetOrNull);
            arrow.state = option->state;
            arrow.palette = option->palette;
            proxy()->drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widgetOrNull);
            return;
        }
    }
    if (control == CC_SpinBox)
    {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (spin != nullptr)
        {
            if (spin->frame == true)
            {
                drawBody();
            }
            for (const SubControl part : {SC_SpinBoxUp, SC_SpinBoxDown})
            {
                QStyleOption arrow;
                arrow.rect = proxy()->subControlRect(CC_SpinBox, spin, part, widgetOrNull);
                arrow.state = option->state;
                arrow.palette = option->palette;
                const bool isStepEnabled = spin->stepEnabled.testFlag(
                    part == SC_SpinBoxUp ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled);
                arrow.state.setFlag(State_Enabled, isEnabled == true && isStepEnabled == true);
                proxy()->drawPrimitive(part == SC_SpinBoxUp ? PE_IndicatorSpinUp : PE_IndicatorSpinDown,
                                       &arrow,
                                       painter,
                                       widgetOrNull);
            }
            return;
        }
    }
    QProxyStyle::drawComplexControl(control, option, painter, widgetOrNull);
}

QSize EditorStyle::sizeFromContents(ContentsType type,
                                    const QStyleOption* option,
                                    const QSize& contentsSize,
                                    const QWidget* widgetOrNull) const
{
    const int edgeWidth = GetEdgeWidth();
    switch (type)
    {
        case CT_PushButton: {
            const bool isCompact = HasProperty(widgetOrNull, DENSITY_PROPERTY, "compact");
            const int vertical = isCompact == true ? COMPACT_BUTTON_PADDING_VERTICAL : BUTTON_PADDING_VERTICAL;
            const int horizontal = isCompact == true ? COMPACT_BUTTON_PADDING_HORIZONTAL : BUTTON_PADDING_HORIZONTAL;
            return contentsSize + QSize(2 * (horizontal + edgeWidth), 2 * (vertical + edgeWidth));
        }
        case CT_ToolButton:
            return contentsSize + QSize(2 * TOOL_BUTTON_PADDING, 2 * TOOL_BUTTON_PADDING);
        case CT_LineEdit:
            return contentsSize + QSize(2 * (INPUT_PADDING + edgeWidth), 2 * (INPUT_PADDING + edgeWidth));
        case CT_ComboBox:
            return contentsSize +
                   QSize(2 * edgeWidth + INPUT_PADDING + COMBO_ARROW_PADDING, 2 * (INPUT_PADDING + edgeWidth));
        case CT_TabBarTab:
            return contentsSize + QSize(2 * TAB_PADDING_HORIZONTAL, 2 * TAB_PADDING_VERTICAL + TAB_UNDERLINE_WIDTH);
        case CT_HeaderSection:
            return contentsSize + QSize(2 * HEADER_PADDING, 2 * HEADER_PADDING + CONTROL_EDGE_WIDTH);
        default:
            break;
    }
    return QProxyStyle::sizeFromContents(type, option, contentsSize, widgetOrNull);
}

void InstallEditorStyle()
{
    EditorStyle* style = nullptr;
    QStyle* current = QApplication::style();
    if (current != nullptr && current->objectName() == QLatin1String(STYLE_OBJECT_NAME))
    {
        style = static_cast<EditorStyle*>(current);
    }
    else
    {
        style = new EditorStyle;
        QApplication::setStyle(style);
    }
    style->Refresh();
}
} // namespace ludus::editor
