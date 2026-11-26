#include "design_tokens.h"
#include "internal/editor_style.h"
#include "internal/tooltip_text.h"
#include "internal/viewport_hud_text.h"
#include "internal/viewport_statistics.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QMenu>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QToolTip>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
using namespace ludus::editor;

constexpr int SETTLE_MILLISECONDS = 30;
const QString LONG_SENTENCE = QStringLiteral(
    "Create and assign an ordered dielectric coat over a diffuse substrate, keeping the slot's base colour, "
    "roughness, textures and mask, then edit the coat refraction index and thickness in the Material document.");

/// Widest line of text in pixels, one line per newline.
int GetWidestLine(const QString& text, const QFont& font)
{
    const QFontMetrics metrics(font);
    int widest = 0;
    for (const QString& line : text.split(QLatin1Char('\n')))
    {
        widest = std::max(widest, metrics.horizontalAdvance(line));
    }
    return widest;
}

void SendToolTipEvent(QWidget& inoutWidget, const QPoint& local)
{
    QHelpEvent event(QEvent::ToolTip, local, inoutWidget.mapToGlobal(local));
    QApplication::sendEvent(&inoutWidget, &event);
}
} // namespace

TEST_CASE("The tooltip width is a design token", "[editor][tooltip][design]")
{
    CHECK(design::SIZE_TOOLTIP_MAXIMUM_WIDTH > 0);
}

TEST_CASE("Long plain tooltips wrap at the design token width", "[editor][tooltip]")
{
    const QFont font = QToolTip::font();
    const QString wrapped = WrapTooltipText(LONG_SENTENCE, font, design::SIZE_TOOLTIP_MAXIMUM_WIDTH);
    CHECK(wrapped.contains(QLatin1Char('\n')) == true);
    CHECK(GetWidestLine(wrapped, font) <= design::SIZE_TOOLTIP_MAXIMUM_WIDTH);
    // Only the breaks change: every word stays, in order.
    CHECK(QString(wrapped).replace(QLatin1Char('\n'), QLatin1Char(' ')) == LONG_SENTENCE);
}

TEST_CASE("Short tooltips, line breaks, rich text and long words are left alone", "[editor][tooltip]")
{
    const QFont font = QToolTip::font();
    const int width = design::SIZE_TOOLTIP_MAXIMUM_WIDTH;
    CHECK(WrapTooltipText(QStringLiteral("Click to collapse"), font, width) == QStringLiteral("Click to collapse"));
    CHECK(WrapTooltipText(QString(), font, width).isEmpty() == true);
    // Existing breaks stay, and each paragraph wraps by itself.
    CHECK(WrapTooltipText(QStringLiteral("First\nSecond"), font, width) == QStringLiteral("First\nSecond"));
    const QString rich = QStringLiteral("<b>Bold</b> ") + LONG_SENTENCE;
    CHECK(WrapTooltipText(rich, font, width) == rich);
    const QString longWord(200, QLatin1Char('w'));
    CHECK(WrapTooltipText(longWord + QStringLiteral(" tail"), font, width) == longWord + QStringLiteral("\ntail"));
    // A non-positive width disables wrapping instead of breaking every word.
    CHECK(WrapTooltipText(LONG_SENTENCE, font, 0) == LONG_SENTENCE);
}

TEST_CASE("The tooltip filter wraps a widget tooltip that is too wide and leaves a short one to Qt",
          "[editor][tooltip]")
{
    InstallEditorStyle();
    QPushButton button(QStringLiteral("Make Coated Diffuse…"));
    TooltipWidthFilter filter(nullptr);
    button.installEventFilter(&filter);
    button.show();
    QTest::qWait(SETTLE_MILLISECONDS);

    button.setToolTip(LONG_SENTENCE);
    QToolTip::hideText();
    SendToolTipEvent(button, button.rect().center());
    REQUIRE(QToolTip::isVisible() == true);
    CHECK(QToolTip::text().contains(QLatin1Char('\n')) == true);
    CHECK(GetWidestLine(QToolTip::text(), QToolTip::font()) <= design::SIZE_TOOLTIP_MAXIMUM_WIDTH);

    QToolTip::hideText();
    button.setToolTip(QStringLiteral("Add a clear coat."));
    QHelpEvent event(QEvent::ToolTip, button.rect().center(), button.mapToGlobal(button.rect().center()));
    CHECK(filter.eventFilter(&button, &event) == false);

    button.setToolTip(QString());
    CHECK(filter.eventFilter(&button, &event) == false);
    QToolTip::hideText();
}

TEST_CASE("The tooltip filter wraps a menu action tooltip only when the menu shows tooltips", "[editor][tooltip]")
{
    QMenu menu;
    QAction* action = menu.addAction(QStringLiteral("Preset"));
    action->setToolTip(LONG_SENTENCE);
    TooltipWidthFilter filter(nullptr);
    menu.show();
    QTest::qWait(SETTLE_MILLISECONDS);
    const QPoint inside = menu.actionGeometry(action).center();
    QHelpEvent event(QEvent::ToolTip, inside, menu.mapToGlobal(inside));
    menu.setToolTipsVisible(false);
    CHECK(filter.eventFilter(&menu, &event) == false);
    menu.setToolTipsVisible(true);
    QToolTip::hideText();
    CHECK(filter.eventFilter(&menu, &event) == true);
    CHECK(GetWidestLine(QToolTip::text(), QToolTip::font()) <= design::SIZE_TOOLTIP_MAXIMUM_WIDTH);
    QToolTip::hideText();
    menu.close();
}

TEST_CASE("The statistics overlay tooltip is only the click hint", "[editor][tooltip][viewport]")
{
    InstallEditorStyle();
    ViewportStatistics statistics;
    statistics.Width = 1280;
    statistics.Height = 720;
    statistics.Draws = 42;
    statistics.GpuState = ViewportGpuTimeState::Available;
    statistics.GpuMilliseconds.Add(31.01);
    statistics.CpuMilliseconds.Add(50.0);
    HudGridLabel label(nullptr);
    label.SetStatisticsText(FormatCompactViewportStatistics(statistics), FormatViewportStatisticsSummary(statistics));
    CHECK(label.toolTip() == QStringLiteral("Click to collapse"));
    CHECK(label.toolTip().contains(QLatin1Char('\n')) == false);
    CHECK(label.toolTip().contains(QStringLiteral("p95")) == false);
    SetViewportStatisticsOverlayCollapsed(true);
    label.SetStatisticsText(FormatCompactViewportStatistics(statistics), FormatViewportStatisticsSummary(statistics));
    CHECK(label.toolTip() == QStringLiteral("Click to expand"));
    SetViewportStatisticsOverlayCollapsed(false);
}
