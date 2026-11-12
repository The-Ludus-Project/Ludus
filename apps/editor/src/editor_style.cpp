#include "internal/editor_style.h"
#include "design_tokens.h"
#include "internal/editor_icons.h"
#include "internal/path_text.h"
#include "internal/theme_fonts.h"
#include "internal/theme_preferences.h"

#include <ludus/foundation/base/types.h>

#include <algorithm>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFontMetrics>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
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
/// Half the extra-small step above and below a compact button's label: with the edge, a compact button is the label
/// height plus the extra-small step plus both edges, on every platform.
constexpr int COMPACT_BUTTON_PADDING_VERTICAL = design::SPACING_EXTRA_SMALL / 2;
constexpr int COMPACT_BUTTON_PADDING_HORIZONTAL = 8;
constexpr int INPUT_PADDING = 4;
constexpr int COMBO_ARROW_PADDING = 20;

/// Drop-down and branch chevron: the width of its two strokes and the pen weight, in logical pixels. The platform
/// style would size its own arrow, which comes out oversized on macOS, so every arrow is drawn from these.
constexpr qreal CHEVRON_SPAN = 8.0;
constexpr qreal CHEVRON_STROKE = 1.5;
constexpr int CHEVRON_POINT_COUNT = 3;
/// Quarter turns that rotate a downward chevron to each direction, in degrees.
constexpr qreal CHEVRON_UP_DEGREES = 180.0;
constexpr qreal CHEVRON_LEFT_DEGREES = 90.0;
constexpr qreal CHEVRON_RIGHT_DEGREES = -90.0;
constexpr int TAB_PADDING_VERTICAL = 7;
constexpr int TAB_PADDING_HORIZONTAL = 12;
constexpr int TAB_UNDERLINE_WIDTH = 2;
constexpr int DOCK_TITLE_PADDING = 6;
/// Space between a dock tab's icon and its text, and around an icon-only tab's icon.
constexpr int TAB_ICON_GAP = 6;
constexpr int ICON_ONLY_TAB_PADDING = 8;
constexpr int TOOL_BUTTON_PADDING = 4;
/// Room a menu button reserves at its right edge for the drop-down chevron, in logical pixels: the label is centred in
/// what remains, so the chevron never lands on the last letters.
constexpr int TOOL_BUTTON_MENU_INDICATOR_WIDTH = 16;
constexpr int HEADER_PADDING = 4;
constexpr int TOOL_BAR_SPACING = design::SPACING_EXTRA_SMALL;
constexpr int SEPARATOR_EXTENT = 4;
/// Height of the rule a toolbar separator draws between groups, in logical pixels.
constexpr int TOOL_BAR_SEPARATOR_LENGTH = design::SPACING_LARGE;
constexpr qreal HALF = 0.5;
/// Window lightness (of 255) below which the application palette counts as dark.
constexpr int DARK_LIGHTNESS_LIMIT = 128;
/// Gap between a check box and its label, in logical pixels.
constexpr int CHECK_LABEL_SPACING = design::SPACING_SMALL;
/// The tick of a checked box, as fractions of the box: the short stroke down from the left, then the long one up.
constexpr qreal TICK_START_X = 0.27;
constexpr qreal TICK_START_Y = 0.52;
constexpr qreal TICK_CORNER_X = 0.43;
constexpr qreal TICK_CORNER_Y = 0.68;
constexpr qreal TICK_END_X = 0.74;
constexpr qreal TICK_END_Y = 0.34;
/// The bar of a partially checked box runs between these fractions of the box width.
constexpr qreal PARTIAL_BAR_START = 0.28;
constexpr qreal PARTIAL_BAR_END = 0.72;
/// A group box is a flat section: a rule, then this space above its title and this space below it.
constexpr int GROUP_TITLE_SPACE_ABOVE = design::SPACING_MEDIUM;
constexpr int GROUP_TITLE_SPACE_BELOW = design::SPACING_SMALL;
const char* const STYLE_OBJECT_NAME = "LudusEditorStyle";
const char* const VISUAL_ROLE_PROPERTY = "visualRole";
const char* const SEGMENT_POSITION_PROPERTY = "segmentPosition";
const char* const DENSITY_PROPERTY = "density";
const char* const SECONDARY_LABEL_MARKER = "ludusSecondaryLabel";

foundation::int32 ComputeTextAdvancePaddingPixels(const QFontMetrics& fontMetrics, const QString& text) noexcept
{
    const foundation::int32 inkWidthPixels = fontMetrics.boundingRect(text).width();
    const foundation::int32 advanceWidthPixels = fontMetrics.horizontalAdvance(text);
    return advanceWidthPixels > inkWidthPixels ? advanceWidthPixels - inkWidthPixels : 0;
}

QColor Token(design::ThemeColour role, bool isDark, bool isHighContrast)
{
    return QColor::fromRgb(design::GetThemeArgb(role, isDark, isHighContrast));
}

bool IsDarkPalette(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightness() < DARK_LIGHTNESS_LIMIT;
}

/// Whether the editor paints this push button's bevel and label itself: every framed push button. Flat buttons and
/// command links keep the platform look.
bool IsStyledPushButton(const QStyleOptionButton* buttonOrNull)
{
    return buttonOrNull != nullptr && buttonOrNull->features.testFlag(QStyleOptionButton::Flat) == false &&
           buttonOrNull->features.testFlag(QStyleOptionButton::CommandLinkButton) == false;
}

bool HasProperty(const QWidget* widgetOrNull, const char* name, const char* value)
{
    return widgetOrNull != nullptr && widgetOrNull->property(name).toString() == QLatin1String(value);
}

/// Whether the control speaks the toolbar's "ghost" language (visual role "ghost"): no fill or edge at rest, the
/// raised surface and control edge on hover or while open, so a row of menus and commands reads as one bar.
bool IsGhostControl(const QWidget* widgetOrNull)
{
    return HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "ghost");
}

/// True when a control inside container takes focus with Tab.
bool HasTabFocusableDescendant(const QWidget& container)
{
    const QList<QWidget*> children = container.findChildren<QWidget*>();
    return std::any_of(children.begin(), children.end(), [](const QWidget* child) {
        return (child->focusPolicy() & Qt::TabFocus) != 0;
    });
}

/// True for the controls whose edge becomes the focus ring (see drawPrimitive and drawComplexControl).
bool HasOwnFocusRing(const QWidget* widgetOrNull)
{
    return qobject_cast<const QPushButton*>(widgetOrNull) != nullptr ||
           qobject_cast<const QToolButton*>(widgetOrNull) != nullptr ||
           qobject_cast<const QComboBox*>(widgetOrNull) != nullptr ||
           qobject_cast<const QAbstractSpinBox*>(widgetOrNull) != nullptr ||
           qobject_cast<const QLineEdit*>(widgetOrNull) != nullptr ||
           qobject_cast<const QTabBar*>(widgetOrNull) != nullptr;
}

struct ControlBodyColours final
{
    QColor Fill;
    QColor Edge;
};

/// Rounded control body with an edge drawn inside rect.
void DrawControlBody(QPainter* painter, const QRect& rect, const ControlBodyColours& colours, int edgeWidth)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(colours.Edge, edgeWidth));
    painter->setBrush(colours.Fill);
    const qreal inset = static_cast<qreal>(edgeWidth) * HALF;
    painter->drawRoundedRect(QRectF(rect).adjusted(inset, inset, -inset, -inset),
                             design::RADIUS_CONTROL,
                             design::RADIUS_CONTROL);
    painter->restore();
}

/// A thin chevron pointing the given way, centred in rect and never larger than it.
void DrawChevron(QPainter* painter, const QRect& rect, QStyle::PrimitiveElement element, const QColor& colour)
{
    const qreal span = std::min(CHEVRON_SPAN, static_cast<qreal>(std::min(rect.width(), rect.height())));
    const qreal half = span * HALF;
    const qreal quarter = half * HALF;
    qreal degrees = 0.0;
    if (element == QStyle::PE_IndicatorArrowUp)
    {
        degrees = CHEVRON_UP_DEGREES;
    }
    else if (element == QStyle::PE_IndicatorArrowLeft)
    {
        degrees = CHEVRON_LEFT_DEGREES;
    }
    else if (element == QStyle::PE_IndicatorArrowRight)
    {
        degrees = CHEVRON_RIGHT_DEGREES;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(colour, CHEVRON_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->translate(QRectF(rect).center());
    painter->rotate(degrees);
    const QPointF points[CHEVRON_POINT_COUNT] = {{-half, -quarter}, {0.0, quarter}, {half, -quarter}};
    painter->drawPolyline(points, CHEVRON_POINT_COUNT);
    painter->restore();
}

/// One button of a segmented control: the rounded outline belongs to the whole row, so only the outer
/// corners of the first and last segments are rounded and neighbours share a divider.
void DrawSegmentBody(QPainter* painter,
                     const QRect& rect,
                     const ControlBodyColours& colours,
                     int edgeWidth,
                     bool isFirst,
                     bool isLast)
{
    painter->save();
    painter->setClipRect(rect);
    const int overhang = static_cast<int>(design::RADIUS_CONTROL) * 2;
    const QRect body = rect.adjusted(isFirst == true ? 0 : -overhang, 0, isLast == true ? 0 : overhang, 0);
    DrawControlBody(painter, body, colours, edgeWidth);
    if (isFirst == false)
    {
        painter->fillRect(QRect(rect.left(), rect.top(), edgeWidth, rect.height()), colours.Edge);
    }
    painter->restore();
}

/// Section titles use the Title type role of the group box's own font, so they follow the system text size.
QFont MakeGroupTitleFont(const QWidget* widgetOrNull)
{
    return MakeTypeFont(widgetOrNull != nullptr ? widgetOrNull->font() : QApplication::font("QGroupBox"),
                        TypeRole::Title);
}

/// Where a flat section puts its parts: a rule along the top edge, the title (after its check box, when it has one)
/// below it, and the contents under the title, full width so a section lines up with the dock around it.
struct GroupBoxLayout final
{
    QRect CheckBox;
    QRect Label;
    QRect Contents;
    QSize Minimum;
};

GroupBoxLayout
ComputeGroupBoxLayout(const QStyleOptionGroupBox& groupBox, const QFont& titleFont, int indicatorSize, int labelSpacing)
{
    const QRect& rect = groupBox.rect;
    // The contents always end at the box's bottom edge, even while the box is shorter than its title: QGroupBox
    // turns this rectangle into its contents margins once, possibly at its default height, and keeps them after
    // later resizes. A height clamped to zero would leave a negative bottom margin; Qt's common style does not clamp.
    // Thanks to The Qt Company, Qt 6.8 QGroupBoxPrivate::calculateFrame and QCommonStyle SC_GroupBoxContents:
    // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/widgets/qgroupbox.cpp
    const bool isCheckable = groupBox.subControls.testFlag(QStyle::SC_GroupBoxCheckBox);
    const int titleTop = rect.top() + CONTROL_EDGE_WIDTH + GROUP_TITLE_SPACE_ABOVE;
    GroupBoxLayout layout;
    if (groupBox.text.isEmpty() == true && isCheckable == false)
    {
        // An untitled section keeps only its rule, with the space a title would leave below it.
        const int contentsTop = rect.top() + CONTROL_EDGE_WIDTH + GROUP_TITLE_SPACE_BELOW;
        layout.Contents = QRect(rect.left(), contentsTop, rect.width(), rect.bottom() + 1 - contentsTop);
        layout.Minimum = QSize(0, contentsTop - rect.top());
        return layout;
    }
    const QFontMetrics metrics(titleFont);
    const int titleHeight = std::max(metrics.height(), isCheckable == true ? indicatorSize : 0);
    int labelLeft = rect.left();
    if (isCheckable == true)
    {
        layout.CheckBox =
            QRect(rect.left(), titleTop + (titleHeight - indicatorSize) / 2, indicatorSize, indicatorSize);
        labelLeft += indicatorSize + labelSpacing;
    }
    const int textWidth = metrics.horizontalAdvance(groupBox.text);
    layout.Label =
        QRect(labelLeft, titleTop, std::max(0, std::min(textWidth, rect.right() + 1 - labelLeft)), titleHeight);
    const int contentsTop = titleTop + titleHeight + GROUP_TITLE_SPACE_BELOW;
    layout.Contents = QRect(rect.left(), contentsTop, rect.width(), rect.bottom() + 1 - contentsTop);
    layout.Minimum = QSize(labelLeft - rect.left() + textWidth, contentsTop - rect.top());
    return layout;
}

/// Colours of a check box indicator.
struct CheckIndicatorColours final
{
    QColor Panel;
    QColor Border;
    QColor Accent;
    QColor Mark;
    QColor Disabled;
};

void DrawCheckIndicator(const QStyleOption& option,
                        const CheckIndicatorColours& colours,
                        int edgeWidth,
                        QPainter& inoutPainter)
{
    // A box of the token size with a control edge on the panel, so an unchecked box is as visible as a checked one;
    // checked and partly checked boxes fill with the accent and carry a tick or a bar in the accent's text colour.
    const int size = std::min({static_cast<int>(design::SIZE_CHECKBOX), option.rect.width(), option.rect.height()});
    QRect box(QPoint(0, 0), QSize(size, size));
    box.moveCenter(option.rect.center());
    const bool isEnabled = option.state.testFlag(QStyle::State_Enabled);
    const bool isOn = option.state.testFlag(QStyle::State_On);
    const bool isPartial = option.state.testFlag(QStyle::State_NoChange);
    const bool isHovered = option.state.testFlag(QStyle::State_MouseOver) && isEnabled == true;
    QColor fill = colours.Panel;
    QColor edge = isHovered == true ? colours.Accent : colours.Border;
    QColor mark = colours.Mark;
    if (isEnabled == false)
    {
        edge = colours.Disabled;
        mark = colours.Disabled;
    }
    else if (isOn == true || isPartial == true)
    {
        fill = colours.Accent;
        edge = colours.Accent;
    }
    DrawControlBody(&inoutPainter, box, { .Fill = fill, .Edge = edge }, edgeWidth);
    if (isOn == false && isPartial == false)
    {
        return;
    }
    const QRectF area(box);
    const auto at = [&area](qreal x, qreal y) {
        return QPointF(area.left() + area.width() * x, area.top() + area.height() * y);
    };
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(QPen(mark, CHEVRON_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    inoutPainter.setBrush(Qt::NoBrush);
    if (isOn == true)
    {
        const QPointF tick[CHEVRON_POINT_COUNT] = {at(TICK_START_X, TICK_START_Y),
                                                   at(TICK_CORNER_X, TICK_CORNER_Y),
                                                   at(TICK_END_X, TICK_END_Y)};
        inoutPainter.drawPolyline(tick, CHEVRON_POINT_COUNT);
    }
    else
    {
        inoutPainter.drawLine(at(PARTIAL_BAR_START, HALF), at(PARTIAL_BAR_END, HALF));
    }
    inoutPainter.restore();
}

/// Colours of a flat section.
struct SectionColours final
{
    QColor Rule;
    QColor Title;
    QColor DisabledTitle;
};

void DrawGroupBox(const QStyle& style,
                  const QStyleOptionGroupBox& groupBox,
                  const SectionColours& colours,
                  QPainter& inoutPainter,
                  const QWidget* widgetOrNull)
{
    // A flat section, as docs/architecture/editor-art-direction.md asks: a quiet rule and a clear title instead of
    // the rounded card macOS draws around every group.
    const QFont titleFont = MakeGroupTitleFont(widgetOrNull);
    const GroupBoxLayout layout =
        ComputeGroupBoxLayout(groupBox,
                              titleFont,
                              style.pixelMetric(QStyle::PM_IndicatorWidth, &groupBox, widgetOrNull),
                              style.pixelMetric(QStyle::PM_CheckBoxLabelSpacing, &groupBox, widgetOrNull));
    inoutPainter.fillRect(QRect(groupBox.rect.left(), groupBox.rect.top(), groupBox.rect.width(), CONTROL_EDGE_WIDTH),
                          colours.Rule);
    const bool isEnabled = groupBox.state.testFlag(QStyle::State_Enabled);
    if (groupBox.subControls.testFlag(QStyle::SC_GroupBoxCheckBox) == true)
    {
        QStyleOption check(groupBox);
        check.rect = layout.CheckBox;
        check.state.setFlag(QStyle::State_HasFocus, false);
        check.state.setFlag(
            QStyle::State_MouseOver,
            groupBox.state.testFlag(QStyle::State_MouseOver) == true &&
                (groupBox.activeSubControls & (QStyle::SC_GroupBoxCheckBox | QStyle::SC_GroupBoxLabel)) != 0);
        style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &check, &inoutPainter, widgetOrNull);
    }
    if (groupBox.text.isEmpty() == false)
    {
        inoutPainter.save();
        inoutPainter.setFont(titleFont);
        inoutPainter.setPen(isEnabled == true ? colours.Title : colours.DisabledTitle);
        const QString title = QFontMetrics(titleFont).elidedText(groupBox.text, Qt::ElideRight, layout.Label.width());
        inoutPainter.drawText(layout.Label, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic, title);
        inoutPainter.restore();
    }
    if (groupBox.state.testFlag(QStyle::State_HasFocus) == true && isEnabled == true)
    {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(groupBox);
        focus.rect = layout.CheckBox.isNull() == true ? layout.Label : layout.CheckBox.united(layout.Label);
        style.drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &inoutPainter, widgetOrNull);
    }
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

EditorStyle::EditorStyle() : EditorStyle(QStyleFactory::create(QApplication::style()->name())) {}

EditorStyle::EditorStyle(QStyle* baseStyle) : QProxyStyle(baseStyle)
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
    mColours.Disabled = Token(design::ThemeColour::TextDisabled, isDark, isHighContrast);
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
    if (QLabel* label = qobject_cast<QLabel*>(widget);
        label != nullptr && HasProperty(label, VISUAL_ROLE_PROPERTY, "secondary") == true)
    {
        // PlaceholderText carries the secondary text colour in MakeEditorPalette, so palette changes follow.
        label->setForegroundRole(QPalette::PlaceholderText);
        label->setProperty(SECONDARY_LABEL_MARKER, true);
    }
    // A scroll area that wraps a panel of controls is not a Tab stop of its own: it would take focus without showing
    // it, and the controls inside already scroll into view as Tab reaches them. One with nothing focusable inside
    // keeps focus so the keyboard can still scroll it.
    if (QScrollArea* scroll = qobject_cast<QScrollArea*>(widget);
        scroll != nullptr && scroll->widget() != nullptr && HasTabFocusableDescendant(*scroll->widget()) == true)
    {
        scroll->setFocusPolicy(Qt::NoFocus);
    }
    // Hover repaints for the controls drawn here that the platform style does not already track.
    if (qobject_cast<QTabBar*>(widget) != nullptr || qobject_cast<QToolButton*>(widget) != nullptr)
    {
        widget->setAttribute(Qt::WA_Hover, true);
    }
}

void EditorStyle::unpolish(QWidget* widget)
{
    if (widget->property(SECONDARY_LABEL_MARKER).toBool() == true)
    {
        widget->setForegroundRole(QPalette::WindowText);
        widget->setProperty(SECONDARY_LABEL_MARKER, QVariant());
    }
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
        case PM_DefaultFrameWidth:
            // Item views and text editors draw their own edge, so reserve its full width.
            if (qobject_cast<const QAbstractItemView*>(widgetOrNull) != nullptr ||
                qobject_cast<const QTextEdit*>(widgetOrNull) != nullptr ||
                qobject_cast<const QPlainTextEdit*>(widgetOrNull) != nullptr)
            {
                return GetEdgeWidth();
            }
            break;
        case PM_TabBarTabHSpace:
            return 2 * TAB_PADDING_HORIZONTAL;
        case PM_TabBarTabVSpace:
            return 2 * TAB_PADDING_VERTICAL + TAB_UNDERLINE_WIDTH;
        case PM_MenuButtonIndicator:
            return TOOL_BUTTON_MENU_INDICATOR_WIDTH;
        case PM_ToolBarItemSpacing:
        case PM_ToolBarFrameWidth:
            return TOOL_BAR_SPACING;
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
            return design::SIZE_CHECKBOX;
        case PM_CheckBoxLabelSpacing:
            return CHECK_LABEL_SPACING;
        default:
            break;
    }
    return QProxyStyle::pixelMetric(metric, optionOrNull, widgetOrNull);
}

int EditorStyle::styleHint(StyleHint hint,
                           const QStyleOption* optionOrNull,
                           const QWidget* widgetOrNull,
                           QStyleHintReturn* outReturnDataOrNull) const
{
    // Tabs start at the leading edge like the document tabs, on every platform; macOS would centre them.
    if (hint == SH_TabBar_Alignment)
    {
        return Qt::AlignLeft;
    }
    return QProxyStyle::styleHint(hint, optionOrNull, widgetOrNull, outReturnDataOrNull);
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
            const QStyleOptionButton* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (IsGhostControl(widgetOrNull) == true)
            {
                const bool isPressed =
                    option->state.testFlag(State_Sunken) == true || option->state.testFlag(State_On) == true;
                if (isFocused == true && isEnabled == true)
                {
                    DrawControlBody(painter,
                                    option->rect,
                                    {
                                        .Fill = isPressed == true ? mColours.Selected : mColours.Raised,
                                        .Edge = mColours.Accent,
                                    },
                                    ringWidth);
                }
                else if (isPressed == true || isHovered == true)
                {
                    DrawControlBody(painter,
                                    option->rect,
                                    {
                                        .Fill = isPressed == true ? mColours.Selected : mColours.Raised,
                                        .Edge = mColours.Border,
                                    },
                                    edgeWidth);
                }
                return;
            }
            if (HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "segment") == true)
            {
                const bool isSelected = option->state.testFlag(State_On);
                const QColor fill = isEnabled == false   ? mColours.Workspace
                                    : isSelected == true ? mColours.Accent
                                    : isHovered == true  ? mColours.Selected
                                                         : mColours.Raised;
                const QColor edge = isFocused == true && isEnabled == true ? mColours.Accent : mColours.Border;
                DrawSegmentBody(painter,
                                option->rect,
                                { .Fill = fill, .Edge = edge },
                                isFocused == true && isEnabled == true ? ringWidth : edgeWidth,
                                HasProperty(widgetOrNull, SEGMENT_POSITION_PROPERTY, "middle") == false &&
                                    HasProperty(widgetOrNull, SEGMENT_POSITION_PROPERTY, "last") == false,
                                HasProperty(widgetOrNull, SEGMENT_POSITION_PROPERTY, "middle") == false &&
                                    HasProperty(widgetOrNull, SEGMENT_POSITION_PROPERTY, "first") == false);
                return;
            }
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
            DrawControlBody(painter, option->rect, { .Fill = fill, .Edge = edge }, width);
            return;
        }
        case PE_PanelLineEdit: {
            const QStyleOptionFrame* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
            if (frame == nullptr || frame->lineWidth <= 0)
            {
                break;
            }
            const bool isRingShown = isFocused == true && isEnabled == true;
            DrawControlBody(painter,
                            option->rect,
                            {
                                .Fill = option->palette.color(QPalette::Base),
                                .Edge = isRingShown == true ? mColours.Accent : mColours.Border,
                            },
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
                            {
                                .Fill = Qt::transparent,
                                .Edge = isRingShown == true ? mColours.Accent : mColours.Border,
                            },
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
        case PE_FrameFocusRect: {
            // Buttons, fields, combo and spin boxes and tabs draw the ring on their own edge, so the platform's
            // inner rectangle would be a second, differently coloured marker.
            if (HasOwnFocusRing(widgetOrNull) == true)
            {
                return;
            }
            // Everything else (check boxes, radio buttons, the current row of a focused list or tree, group box
            // titles) gets the same accent ring, whether or not the row is also selected: selection is a tinted
            // fill, keyboard focus an outline (docs/architecture/editor-art-direction.md).
            if (isEnabled == true)
            {
                DrawControlBody(painter, option->rect, { .Fill = Qt::transparent, .Edge = mColours.Accent }, ringWidth);
            }
            return;
        }
        case PE_PanelButtonTool: {
            // A switch that is on (visual role "toggle", such as Force raster) fills with the accent like a selected
            // segment, so its state reads at a glance. Disclosure buttons (Advanced, Details) keep the edge only:
            // being open is not a mode that changes what the editor does.
            if (isEnabled == true && option->state.testFlag(State_On) == true &&
                HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "toggle") == true)
            {
                DrawControlBody(painter, option->rect, { .Fill = mColours.Accent, .Edge = mColours.Accent }, edgeWidth);
            }
            // A checked toggle (Live Preview) keeps an accent edge so its state reads without hovering.
            else if (option->state.testFlag(State_On) == true)
            {
                DrawControlBody(painter,
                                option->rect,
                                { .Fill = mColours.Raised, .Edge = mColours.Accent },
                                CONTROL_EDGE_WIDTH);
            }
            else if (isHovered == true)
            {
                DrawControlBody(painter, option->rect, { .Fill = mColours.Raised, .Edge = Qt::transparent }, 0);
            }
            if (isFocused == true && isEnabled == true)
            {
                DrawControlBody(painter, option->rect, { .Fill = Qt::transparent, .Edge = mColours.Accent }, ringWidth);
            }
            return;
        }
        case PE_IndicatorToolBarSeparator: {
            // A short quiet rule between groups: visible on the workspace surface without competing with controls.
            const bool isHorizontalBar = option->state.testFlag(State_Horizontal) == true;
            const QRect& area = option->rect;
            const int length =
                std::min(TOOL_BAR_SEPARATOR_LENGTH, isHorizontalBar == true ? area.height() : area.width());
            const QRect rule =
                isHorizontalBar == true
                    ? QRect(area.center().x(), area.center().y() - length / 2, CONTROL_EDGE_WIDTH, length)
                    : QRect(area.center().x() - length / 2, area.center().y(), length, CONTROL_EDGE_WIDTH);
            painter->fillRect(rule, mColours.SubtleBorder);
            return;
        }
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            DrawCheckIndicator(*option,
                               {
                                   .Panel = mColours.Panel,
                                   .Border = mColours.Border,
                                   .Accent = mColours.Accent,
                                   .Mark = mColours.AccentText,
                                   .Disabled = mColours.Disabled,
                               },
                               edgeWidth,
                               *painter);
            return;
        case PE_PanelStatusBar:
            painter->fillRect(option->rect, mColours.Workspace);
            painter->fillRect(QRect(option->rect.topLeft(), QSize(option->rect.width(), CONTROL_EDGE_WIDTH)),
                              mColours.SubtleBorder);
            return;
        case PE_IndicatorDockWidgetResizeHandle:
            painter->fillRect(option->rect, mColours.Border);
            return;
        case PE_IndicatorArrowDown:
        case PE_IndicatorArrowUp:
        case PE_IndicatorArrowLeft:
        case PE_IndicatorArrowRight: {
            // A menu button's chevron sits in the column its label leaves, centred on the button's height; the
            // common style would put it in the bottom corner, level with the last letters.
            const QToolButton* menuButtonOrNull = qobject_cast<const QToolButton*>(widgetOrNull);
            QRect chevronRect = option->rect;
            if (menuButtonOrNull != nullptr && element == PE_IndicatorArrowDown &&
                menuButtonOrNull->popupMode() != QToolButton::MenuButtonPopup)
            {
                chevronRect = QRect(menuButtonOrNull->width() - TOOL_BUTTON_MENU_INDICATOR_WIDTH,
                                    0,
                                    TOOL_BUTTON_MENU_INDICATOR_WIDTH,
                                    menuButtonOrNull->height());
            }
            DrawChevron(
                painter,
                chevronRect,
                element,
                option->palette.color(isEnabled == true ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
        }
            return;
        default:
            break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widgetOrNull);
}

namespace
{
/// A tab bar owned by the main window is a dock tab bar. It shows icons only when its full labels
/// (icon, text and padding for every tab) would not fit its width, and every tab has an icon.
[[nodiscard]] bool IsIconOnlyDockTabBar(const QWidget* widgetOrNull, const QStyle& style)
{
    const QTabBar* tabBar = qobject_cast<const QTabBar*>(widgetOrNull);
    if (tabBar == nullptr || qobject_cast<const QMainWindow*>(tabBar->parentWidget()) == nullptr || tabBar->count() < 2)
    {
        return false;
    }
    const QFontMetrics metrics(tabBar->font());
    const int iconSize = style.pixelMetric(QStyle::PM_TabBarIconSize, nullptr, tabBar);
    const int padding = style.pixelMetric(QStyle::PM_TabBarTabHSpace, nullptr, tabBar);
    int fullWidth = 0;
    for (int index = 0; index < tabBar->count(); ++index)
    {
        if (tabBar->tabIcon(index).isNull() == true)
        {
            return false;
        }
        fullWidth += metrics.horizontalAdvance(tabBar->tabText(index)) + iconSize + TAB_ICON_GAP + padding;
    }
    return fullWidth > tabBar->width();
}
} // namespace

void EditorStyle::drawControl(ControlElement element,
                              const QStyleOption* option,
                              QPainter* painter,
                              const QWidget* widgetOrNull) const
{
    const bool isEnabled = option->state.testFlag(State_Enabled);
    switch (element)
    {
#if defined(Q_OS_WIN)
        case CE_ItemViewItem:
            // Thanks to the Qt Company for QCommonStyle::drawControl (Qt 6.8.3).
            // QWindowsVistaStyle can suppress the current item's focus rectangle.
            // The common path calls our focus primitive for selected and unselected rows.
            // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/styles/qcommonstyle.cpp#L2178-L2244
            // https://github.com/qt/qtbase/blob/v6.8.3/src/plugins/styles/modernwindows/qwindowsvistastyle.cpp#L2934-L2976
            QCommonStyle::drawControl(element, option, painter, widgetOrNull);
            return;
#endif
        case CE_PushButton:
            // The common style lays a push button out from SE_PushButtonContents and draws it through the bevel and
            // label below, which the platform style might otherwise do natively in one piece.
            if (IsStyledPushButton(qstyleoption_cast<const QStyleOptionButton*>(option)) == false)
            {
                break;
            }
            QCommonStyle::drawControl(element, option, painter, widgetOrNull);
            return;
        case CE_CheckBox:
        case CE_CheckBoxLabel:
            // A check box is the indicator drawn below and its label, laid out by the common style; macOS would draw
            // a native box at its own size, a dark blob on the editor's panels.
            QCommonStyle::drawControl(element, option, painter, widgetOrNull);
            return;
        case CE_PushButtonBevel: {
            // Some platform styles, macOS's among them, draw a push button's bevel natively and never ask for
            // PE_PanelButtonCommand, so the accent fill of a primary button was lost while its label still took the
            // text colour meant for the accent: dark text on a grey native bevel, which reads as a disabled button.
            // The common style paints the bevel through PE_PanelButtonCommand, so every platform gets the same
            // body, focus ring and primary fill, over the whole button (see SE_PushButtonBevel in subElementRect).
            const QStyleOptionButton* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (IsStyledPushButton(button) == false)
            {
                break;
            }
            QStyleOptionButton bevel = *button;
            bevel.rect = proxy()->subElementRect(SE_PushButtonBevel, button, widgetOrNull);
            QCommonStyle::drawControl(element, &bevel, painter, widgetOrNull);
            return;
        }
        case CE_PushButtonLabel: {
            const QStyleOptionButton* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (IsStyledPushButton(button) == false)
            {
                break;
            }
            const bool isSelectedSegment = isEnabled == true && option->state.testFlag(State_On) == true &&
                                           HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "segment") == true;
            const bool isPrimary =
                isEnabled == true &&
                (isSelectedSegment == true || HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "primary") == true ||
                 button->features.testFlag(QStyleOptionButton::DefaultButton));
            // The common style draws the label in the palette's button text; a platform style may swap that colour
            // for its own bevel (white on a default macOS button), which would not suit the editor's body.
            QStyleOptionButton label = *button;
            if (isPrimary == true)
            {
                label.palette.setColor(QPalette::ButtonText, mColours.AccentText);
            }
            QCommonStyle::drawControl(element, &label, painter, widgetOrNull);
            return;
        }
        case CE_ToolButtonLabel: {
            const QStyleOptionToolButton* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            if (tool != nullptr && isEnabled == true && option->state.testFlag(State_On) == true &&
                HasProperty(widgetOrNull, VISUAL_ROLE_PROPERTY, "toggle") == true)
            {
                // The label of a switch that is on sits on the accent fill, so it takes the accent's text colour.
                QStyleOptionToolButton onLabel = *tool;
                onLabel.palette.setColor(QPalette::ButtonText, mColours.AccentText);
                QCommonStyle::drawControl(element, &onLabel, painter, widgetOrNull);
                return;
            }
            const bool isInstantMenu = tool != nullptr &&
                                       tool->features.testFlag(QStyleOptionToolButton::HasMenu) == true &&
                                       tool->features.testFlag(QStyleOptionToolButton::MenuButtonPopup) == false;
            if (isInstantMenu == false)
            {
                // The common label, in the palette's text colour and font; macOS would restyle a tool bar's labels.
                QCommonStyle::drawControl(element, option, painter, widgetOrNull);
                return;
            }
            // Centre the label in what the chevron leaves, so a long label ends before the chevron starts.
            QStyleOptionToolButton label = *tool;
            label.rect.setRight(label.rect.right() - proxy()->pixelMetric(PM_MenuButtonIndicator, tool, widgetOrNull));
            QCommonStyle::drawControl(element, &label, painter, widgetOrNull);
            return;
        }
        case CE_TabBarTabLabel: {
            const QStyleOptionTab* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            if (tab == nullptr || tab->icon.isNull() == true || IsIconOnlyDockTabBar(widgetOrNull, *this) == false)
            {
                // The label in the palette's text colour; macOS would swap it for its own on the selected tab.
                QCommonStyle::drawControl(element, option, painter, widgetOrNull);
                return;
            }
            const QIcon::Mode mode = isEnabled == true ? QIcon::Normal : QIcon::Disabled;
            const int iconSize = pixelMetric(PM_TabBarIconSize, tab, widgetOrNull);
            QRect iconRect(QPoint(0, 0), QSize(iconSize, iconSize));
            iconRect.moveCenter(tab->rect.center());
            tab->icon.paint(painter, iconRect, Qt::AlignCenter, mode, QIcon::Off);
            return;
        }
        case CE_ComboBoxLabel: {
            const QStyleOptionComboBox* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
            if (combo == nullptr || combo->editable == true || combo->currentIcon.isNull() == false)
            {
                break;
            }
            // Text the field cannot hold is elided from the middle with a folder dimmed and the file
            // name kept, instead of being cut mid-word.
            const QRect textRect = subControlRect(CC_ComboBox, combo, SC_ComboBoxEditField, widgetOrNull);
            const QPalette::ColorGroup group = isEnabled == true ? QPalette::Active : QPalette::Disabled;
            const QColor nameColour = combo->palette.color(group, QPalette::ButtonText);
            painter->save();
            DrawPathText(*painter, textRect, combo->currentText, nameColour, GetFolderColour(nameColour));
            painter->restore();
            return;
        }
        case CE_TabBarTabShape: {
            const QStyleOptionTab* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            const bool isNorth =
                tab != nullptr && (tab->shape == QTabBar::RoundedNorth || tab->shape == QTabBar::TriangularNorth);
            const bool isSouth =
                tab != nullptr && (tab->shape == QTabBar::RoundedSouth || tab->shape == QTabBar::TriangularSouth);
            if (isNorth == false && isSouth == false)
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
                // The accent rule sits on the edge facing the content: below a north tab, above a south one.
                const int ruleTop = isNorth == true ? tab->rect.bottom() + 1 - TAB_UNDERLINE_WIDTH : tab->rect.top();
                painter->fillRect(QRect(tab->rect.left(), ruleTop, tab->rect.width(), TAB_UNDERLINE_WIDTH),
                                  mColours.Accent);
                if (tab->state.testFlag(State_HasFocus) == true)
                {
                    DrawControlBody(painter,
                                    tab->rect,
                                    { .Fill = Qt::transparent, .Edge = mColours.Accent },
                                    GetFocusRingWidth());
                }
            }
            return;
        }
        case CE_DockWidgetTitle: {
            const QStyleOptionDockWidget* dock = qstyleoption_cast<const QStyleOptionDockWidget*>(option);
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
    if (control == CC_ToolButton)
    {
        // Thanks to the Qt Company, Qt 6.8.3 QCommonStyle::drawComplexControl, QWindowsVistaStyle and QMacStyle
        // (CC_ToolButton). Both native styles paint regular tool buttons themselves and never ask for
        // PE_PanelButtonTool, so a switch that is on (Force raster, Stats) showed only a faint native fill. The common
        // layout paints through the panel primitive above on every platform.
        // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/styles/qcommonstyle.cpp
        QCommonStyle::drawComplexControl(control, option, painter, widgetOrNull);
        // An auto-raise button at rest draws no panel at all, so ring keyboard focus here.
        if (isRingShown == true)
        {
            DrawControlBody(painter,
                            option->rect,
                            { .Fill = Qt::transparent, .Edge = mColours.Accent },
                            GetFocusRingWidth());
        }
        return;
    }
    if (control == CC_GroupBox)
    {
        const QStyleOptionGroupBox* groupBox = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (groupBox != nullptr)
        {
            DrawGroupBox(*proxy(),
                         *groupBox,
                         { .Rule = mColours.SubtleBorder, .Title = mColours.Text, .DisabledTitle = mColours.Secondary },
                         *painter,
                         widgetOrNull);
            return;
        }
    }
    const auto drawBody = [&]() {
        DrawControlBody(painter,
                        option->rect,
                        {
                            .Fill = option->palette.color(QPalette::Base),
                            .Edge = isRingShown == true ? mColours.Accent : mColours.Border,
                        },
                        isRingShown == true ? GetFocusRingWidth() : GetEdgeWidth());
    };
    if (control == CC_ComboBox)
    {
        const QStyleOptionComboBox* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo != nullptr)
        {
            const bool isGhost = IsGhostControl(widgetOrNull);
            const bool isOpenOrHovered =
                option->state.testFlag(State_MouseOver) == true || option->state.testFlag(State_On) == true;
            if (isGhost == true && isEnabled == true && (isRingShown == true || isOpenOrHovered == true))
            {
                DrawControlBody(
                    painter,
                    option->rect,
                    { .Fill = mColours.Raised, .Edge = isRingShown == true ? mColours.Accent : mColours.Border },
                    isRingShown == true ? GetFocusRingWidth() : GetEdgeWidth());
            }
            else if (combo->frame == true && isGhost == false)
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
        const QStyleOptionSpinBox* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
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
    if (control == CC_Slider)
    {
        // The platform marks a focused slider with a faint highlight; draw the same accent ring as every other
        // control around the handle, which is what the arrow keys move.
        QProxyStyle::drawComplexControl(control, option, painter, widgetOrNull);
        if (isRingShown == true)
        {
            const QRect handle = proxy()->subControlRect(CC_Slider, option, SC_SliderHandle, widgetOrNull);
            DrawControlBody(painter, handle, { .Fill = Qt::transparent, .Edge = mColours.Accent }, GetFocusRingWidth());
        }
        return;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widgetOrNull);
}

namespace
{
[[nodiscard]] int GetItemPadding(const QWidget* widgetOrNull)
{
    return widgetOrNull != nullptr ? widgetOrNull->property(ITEM_PADDING_PROPERTY).toInt() : 0;
}
} // namespace

QRect EditorStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widgetOrNull) const
{
    switch (element)
    {
        case SE_PushButtonBevel:
        case SE_PushButtonFocusRect:
        case SE_PushButtonContents:
            // macOS keeps a margin around the bevel for its shadow and focus glow, so the editor's body and label
            // got a rectangle several pixels shorter than the height sizeFromContents budgets, and compact buttons
            // (Solo, Apply Changes) clipped. The editor's body fills the button; the label sits inside its edge.
            if (IsStyledPushButton(qstyleoption_cast<const QStyleOptionButton*>(option)) == true)
            {
                if (element == SE_PushButtonContents)
                {
                    const int edgeWidth = GetEdgeWidth();
                    return option->rect.adjusted(edgeWidth, edgeWidth, -edgeWidth, -edgeWidth);
                }
                return option->rect;
            }
            break;
        case SE_CheckBoxIndicator:
        case SE_CheckBoxContents:
        case SE_CheckBoxFocusRect:
        case SE_CheckBoxClickRect:
        case SE_TabBarTabText:
        case SE_TabBarTabLeftButton:
        case SE_TabBarTabRightButton:
        case SE_TabWidgetTabBar:
        case SE_TabWidgetTabPane:
        case SE_TabWidgetTabContents:
        case SE_TabWidgetLeftCorner:
        case SE_TabWidgetRightCorner:
            // The controls the editor draws itself keep the common geometry on every platform.
            return QCommonStyle::subElementRect(element, option, widgetOrNull);
        case SE_PushButtonLayoutItem:
        case SE_ToolButtonLayoutItem:
        case SE_CheckBoxLayoutItem:
        case SE_ComboBoxLayoutItem:
        case SE_SpinBoxLayoutItem:
        case SE_GroupBoxLayoutItem:
        case SE_TabWidgetLayoutItem:
            // These controls are drawn edge to edge, so layouts must not overlap them by the platform's shadow
            // margins (macOS) or neighbours would cover each other's edges.
            return option->rect;
        default:
            break;
    }
    QRect rect = QProxyStyle::subElementRect(element, option, widgetOrNull);
    const QWidget* viewOrNull = widgetOrNull != nullptr ? widgetOrNull->parentWidget() : nullptr;
    // Item views hand their viewport to the style, so the padding property lives on the view itself.
    const int padding = std::max(GetItemPadding(widgetOrNull), GetItemPadding(viewOrNull));
    if (padding > 0 && (element == SE_ItemViewItemText || element == SE_ItemViewItemDecoration ||
                        element == SE_ItemViewItemCheckIndicator))
    {
        rect.translate(padding, 0);
        if (element == SE_ItemViewItemText)
        {
            rect.setWidth(std::max(0, rect.width() - 2 * padding));
        }
    }
    return rect;
}

QStyle::SubControl EditorStyle::hitTestComplexControl(ComplexControl control,
                                                      const QStyleOptionComplex* option,
                                                      const QPoint& position,
                                                      const QWidget* widgetOrNull) const
{
    // Hit tests follow the parts as laid out in subControlRect, not as the platform style would place them.
    if (control == CC_ToolButton || control == CC_GroupBox)
    {
        return QCommonStyle::hitTestComplexControl(control, option, position, widgetOrNull);
    }
    return QProxyStyle::hitTestComplexControl(control, option, position, widgetOrNull);
}

QRect EditorStyle::subControlRect(ComplexControl control,
                                  const QStyleOptionComplex* option,
                                  SubControl subControl,
                                  const QWidget* widgetOrNull) const
{
    if (control == CC_ToolButton)
    {
        return QCommonStyle::subControlRect(control, option, subControl, widgetOrNull);
    }
    if (control == CC_GroupBox)
    {
        const QStyleOptionGroupBox* groupBox = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (groupBox != nullptr)
        {
            const GroupBoxLayout layout =
                ComputeGroupBoxLayout(*groupBox,
                                      MakeGroupTitleFont(widgetOrNull),
                                      proxy()->pixelMetric(PM_IndicatorWidth, option, widgetOrNull),
                                      proxy()->pixelMetric(PM_CheckBoxLabelSpacing, option, widgetOrNull));
            switch (subControl)
            {
                case SC_GroupBoxCheckBox:
                    return layout.CheckBox;
                case SC_GroupBoxLabel:
                    return layout.Label;
                case SC_GroupBoxContents:
                    return layout.Contents;
                case SC_GroupBoxFrame:
                    return groupBox->rect;
                default:
                    return {};
            }
        }
    }
    const QStyleOptionComboBox* combo =
        control == CC_ComboBox ? qstyleoption_cast<const QStyleOptionComboBox*>(option) : nullptr;
    if (combo == nullptr ||
        (subControl != SC_ComboBoxArrow && subControl != SC_ComboBoxEditField && subControl != SC_ComboBoxFrame))
    {
        return QProxyStyle::subControlRect(control, option, subControl, widgetOrNull);
    }
    // The field and arrow split the box exactly as sizeFromContents budgets it, whatever the platform style
    // would reserve, so a combo sized to its text always has room to show all of it.
    const int edgeWidth = GetEdgeWidth();
    const QRect inner = combo->rect.adjusted(edgeWidth, edgeWidth, -edgeWidth, -edgeWidth);
    QRect result = combo->rect;
    if (subControl == SC_ComboBoxArrow)
    {
        result = QRect(inner.right() + 1 - COMBO_ARROW_PADDING, inner.top(), COMBO_ARROW_PADDING, inner.height());
    }
    else if (subControl == SC_ComboBoxEditField)
    {
        result = QRect(inner.left() + INPUT_PADDING,
                       inner.top() + edgeWidth,
                       std::max(0, inner.width() - INPUT_PADDING - COMBO_ARROW_PADDING),
                       std::max(0, inner.height() - 2 * edgeWidth));
    }
    return visualRect(combo->direction, combo->rect, result);
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
        case CT_ToolButton: {
            // A menu button also holds its chevron, which the label never overlaps.
            const QStyleOptionToolButton* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            const bool isInstantMenu = tool != nullptr &&
                                       tool->features.testFlag(QStyleOptionToolButton::HasMenu) == true &&
                                       tool->features.testFlag(QStyleOptionToolButton::MenuButtonPopup) == false;
            const int indicator = isInstantMenu == true ? TOOL_BUTTON_MENU_INDICATOR_WIDTH : 0;
            return contentsSize + QSize(2 * TOOL_BUTTON_PADDING + indicator, 2 * TOOL_BUTTON_PADDING);
        }
        case CT_LineEdit:
            // The editor inside a spin box or combo box sits inside the field's own padding.
            if (widgetOrNull != nullptr &&
                (qobject_cast<const QAbstractSpinBox*>(widgetOrNull->parentWidget()) != nullptr ||
                 qobject_cast<const QComboBox*>(widgetOrNull->parentWidget()) != nullptr))
            {
                break;
            }
            return contentsSize + QSize(2 * (INPUT_PADDING + edgeWidth), 2 * (INPUT_PADDING + edgeWidth));
        case CT_SpinBox:
            return QProxyStyle::sizeFromContents(type, option, contentsSize, widgetOrNull) +
                   QSize(0, 2 * INPUT_PADDING);
        case CT_TabBarTab: {
            // The tab bar already adds the padding of PM_TabBarTabHSpace and PM_TabBarTabVSpace; macOS would add
            // its own on top.
            const QSize base = QCommonStyle::sizeFromContents(type, option, contentsSize, widgetOrNull);
            if (IsIconOnlyDockTabBar(widgetOrNull, *this) == true)
            {
                return {pixelMetric(PM_TabBarIconSize, option, widgetOrNull) + 2 * ICON_ONLY_TAB_PADDING,
                        base.height()};
            }
            return base;
        }
        case CT_CheckBox:
            return QCommonStyle::sizeFromContents(type, option, contentsSize, widgetOrNull);
        case CT_GroupBox: {
            const QStyleOptionGroupBox* groupBox = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
            if (groupBox == nullptr)
            {
                break;
            }
            // QGroupBox measures the title in its body font; the title is drawn larger, so measure it here.
            const GroupBoxLayout layout =
                ComputeGroupBoxLayout(*groupBox,
                                      MakeGroupTitleFont(widgetOrNull),
                                      proxy()->pixelMetric(PM_IndicatorWidth, option, widgetOrNull),
                                      proxy()->pixelMetric(PM_CheckBoxLabelSpacing, option, widgetOrNull));
            return layout.Minimum;
        }
        case CT_ComboBox: {
            // Thanks to The Qt Company, Qt 6.8 QComboBoxPrivate::recomputeSizeHint:
            // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/widgets/qcombobox.cpp
            // Qt budgets glyph ink bounds, which can omit trailing advance. Preserve its icon/size-policy
            // budget and add the largest advance deficit before reserving our field and arrow padding.
            const QStyleOptionComboBox* comboOptionOrNull = qstyleoption_cast<const QStyleOptionComboBox*>(option);
            foundation::int32 advancePaddingPixels =
                comboOptionOrNull != nullptr
                    ? ComputeTextAdvancePaddingPixels(comboOptionOrNull->fontMetrics, comboOptionOrNull->currentText)
                    : 0;
            const QComboBox* comboWidgetOrNull = qobject_cast<const QComboBox*>(widgetOrNull);
            if (comboWidgetOrNull != nullptr &&
                comboWidgetOrNull->sizeAdjustPolicy() != QComboBox::AdjustToMinimumContentsLengthWithIcon)
            {
                const QFontMetrics fontMetrics = comboWidgetOrNull->fontMetrics();
                const foundation::int32 itemCount = comboWidgetOrNull->count();
                for (foundation::int32 item = 0; item < itemCount; ++item)
                {
                    const foundation::int32 itemPaddingPixels =
                        ComputeTextAdvancePaddingPixels(fontMetrics, comboWidgetOrNull->itemText(item));
                    if (itemPaddingPixels > advancePaddingPixels)
                    {
                        advancePaddingPixels = itemPaddingPixels;
                    }
                }
                const foundation::int32 placeholderPaddingPixels =
                    ComputeTextAdvancePaddingPixels(fontMetrics, comboWidgetOrNull->placeholderText());
                if (placeholderPaddingPixels > advancePaddingPixels)
                {
                    advancePaddingPixels = placeholderPaddingPixels;
                }
            }
            return contentsSize + QSize(advancePaddingPixels + 2 * edgeWidth + INPUT_PADDING + COMBO_ARROW_PADDING,
                                        2 * (INPUT_PADDING + edgeWidth));
        }
        case CT_ItemViewItem: {
            const int padding =
                std::max(GetItemPadding(widgetOrNull),
                         GetItemPadding(widgetOrNull != nullptr ? widgetOrNull->parentWidget() : nullptr));
            return QProxyStyle::sizeFromContents(type, option, contentsSize, widgetOrNull) +
                   QSize(2 * padding, 2 * padding);
        }
        case CT_HeaderSection:
            return QProxyStyle::sizeFromContents(type, option, contentsSize, widgetOrNull) +
                   QSize(2 * HEADER_PADDING, 2 * HEADER_PADDING + CONTROL_EDGE_WIDTH);
        default:
            break;
    }
    return QProxyStyle::sizeFromContents(type, option, contentsSize, widgetOrNull);
}

EditorStyle* GetEditorStyleOrNull()
{
    QStyle* current = QApplication::style();
    // The class has no Q_OBJECT, so the object name marks it rather than qobject_cast.
    if (current != nullptr && current->objectName() == QLatin1String(STYLE_OBJECT_NAME))
    {
        return static_cast<EditorStyle*>(current);
    }
    return nullptr;
}

void InstallEditorStyle()
{
    EditorStyle* style = GetEditorStyleOrNull();
    if (style == nullptr)
    {
        style = new EditorStyle;
        QApplication::setStyle(style);
    }
    style->Refresh();
}
} // namespace ludus::editor
