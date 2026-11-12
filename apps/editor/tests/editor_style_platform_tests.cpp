#include "widget_fixture.h"

#include "design_tokens.h"
#include "internal/editor_style.h"
#include "internal/theme_fonts.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QCommonStyle>
#include <QDir>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

using namespace ludus::editor;
using ludus::foundation::int32;

namespace
{
/// The fill a native platform style paints for a bevel, tool button, check box or group card. No token has it, so
/// finding it in a render means the platform drew the control instead of the editor.
constexpr QRgb NATIVE_FILL_RGB = 0x5a5f66;
/// The margin macOS keeps around a push button's bevel for its shadow and focus glow, in logical pixels.
constexpr int NATIVE_BEVEL_MARGIN_VERTICAL = 4;
constexpr int NATIVE_BEVEL_MARGIN_HORIZONTAL = 6;
/// How far a native group card insets its contents, in logical pixels.
constexpr int NATIVE_GROUP_CONTENTS_MARGIN = 10;

/// A stand-in for QMacStyle, which the box running these tests does not have. It does what macOS does to the controls
/// the editor draws itself: insets push-button bevels and layout items, paints tool buttons, check boxes and group
/// boxes natively without asking for the editor's primitives, and centres tabs.
class MacLikeStyle final : public QCommonStyle
{
public:
    void drawControl(ControlElement element,
                     const QStyleOption* option,
                     QPainter* painter,
                     const QWidget* widgetOrNull) const override
    {
        if (element == CE_PushButton || element == CE_PushButtonBevel || element == CE_CheckBox)
        {
            painter->fillRect(subElementRect(SE_PushButtonBevel, option, widgetOrNull), NativeFill());
            return;
        }
        QCommonStyle::drawControl(element, option, painter, widgetOrNull);
    }

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widgetOrNull) const override
    {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck)
        {
            painter->fillRect(option->rect, NativeFill());
            return;
        }
        QCommonStyle::drawPrimitive(element, option, painter, widgetOrNull);
    }

    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex* option,
                            QPainter* painter,
                            const QWidget* widgetOrNull) const override
    {
        if (control == CC_ToolButton || control == CC_GroupBox)
        {
            painter->fillRect(option->rect, NativeFill());
            return;
        }
        QCommonStyle::drawComplexControl(control, option, painter, widgetOrNull);
    }

    [[nodiscard]] QRect
    subElementRect(SubElement element, const QStyleOption* option, const QWidget* widgetOrNull) const override
    {
        switch (element)
        {
            case SE_PushButtonBevel:
            case SE_PushButtonContents:
            case SE_PushButtonLayoutItem:
            case SE_CheckBoxLayoutItem:
            case SE_ToolButtonLayoutItem:
                return option->rect.adjusted(NATIVE_BEVEL_MARGIN_HORIZONTAL,
                                             NATIVE_BEVEL_MARGIN_VERTICAL,
                                             -NATIVE_BEVEL_MARGIN_HORIZONTAL,
                                             -NATIVE_BEVEL_MARGIN_VERTICAL);
            default:
                break;
        }
        return QCommonStyle::subElementRect(element, option, widgetOrNull);
    }

    [[nodiscard]] QRect subControlRect(ComplexControl control,
                                       const QStyleOptionComplex* option,
                                       SubControl subControl,
                                       const QWidget* widgetOrNull) const override
    {
        if (control == CC_GroupBox && subControl == SC_GroupBoxContents)
        {
            return option->rect.adjusted(NATIVE_GROUP_CONTENTS_MARGIN,
                                         NATIVE_GROUP_CONTENTS_MARGIN,
                                         -NATIVE_GROUP_CONTENTS_MARGIN,
                                         -NATIVE_GROUP_CONTENTS_MARGIN);
        }
        return QCommonStyle::subControlRect(control, option, subControl, widgetOrNull);
    }

    [[nodiscard]] int styleHint(StyleHint hint,
                                const QStyleOption* optionOrNull,
                                const QWidget* widgetOrNull,
                                QStyleHintReturn* outReturnDataOrNull) const override
    {
        if (hint == SH_TabBar_Alignment)
        {
            return Qt::AlignCenter;
        }
        return QCommonStyle::styleHint(hint, optionOrNull, widgetOrNull, outReturnDataOrNull);
    }

    [[nodiscard]] static QColor NativeFill()
    {
        return QColor::fromRgb(NATIVE_FILL_RGB);
    }
};

/// An editor style over the macOS stand-in, refreshed from the workspace palette.
struct MacLikeEditorStyle final
{
    MacLikeEditorStyle()
    {
        Style.Refresh();
    }

    /// Give widget and every widget inside it this style; a widget's own style does not pass to its children.
    void Apply(QWidget& inoutWidget)
    {
        inoutWidget.setStyle(&Style);
        for (QWidget* child : inoutWidget.findChildren<QWidget*>())
        {
            child->setStyle(&Style);
        }
    }

    /// Hand widget and its children back to the application style before this one is destroyed.
    static void Release(QWidget& inoutWidget)
    {
        for (QWidget* child : inoutWidget.findChildren<QWidget*>())
        {
            child->setStyle(nullptr);
        }
        inoutWidget.setStyle(nullptr);
    }

    EditorStyle Style{new MacLikeStyle};
};

/// Pixels of exactly colour in image, which is at logical size.
int32 CountColour(const QImage& image, const QColor& colour)
{
    int32 count = 0;
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            count += image.pixelColor(x, y).rgb() == colour.rgb() ? 1 : 0;
        }
    }
    return count;
}
} // namespace

TEST_CASE("Compact push buttons fill their whole height where the platform insets bevels", "[editor][style][macos]")
{
    // On macOS, Solo, Enable All, Apply Changes and Reset Fields were clipped: the body and label were drawn inside
    // the native bevel rectangle, several pixels shorter than the height the editor budgets for a compact button.
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QHBoxLayout* layout = new QHBoxLayout(&root);
    QPushButton* compact = new QPushButton(QStringLiteral("Solo"), &root);
    compact->setProperty("density", QStringLiteral("compact"));
    compact->setFocusPolicy(Qt::NoFocus);
    QPushButton* regular = new QPushButton(QStringLiteral("Apply Changes"), &root);
    regular->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(compact);
    layout->addWidget(regular);
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.show();
    QApplication::processEvents();
    const QColor body = root.palette().color(QPalette::Button);
    for (QPushButton* button : {compact, regular})
    {
        INFO(button->text().toStdString());
        QStyleOptionButton option;
        option.initFrom(button);
        option.features = QStyleOptionButton::None;
        CHECK(mac.Style.subElementRect(QStyle::SE_PushButtonBevel, &option, button) == button->rect());
        // Layouts place the button edge to edge; the native shadow margin would let neighbours overlap its edge.
        CHECK(mac.Style.subElementRect(QStyle::SE_PushButtonLayoutItem, &option, button) == button->rect());
        const QImage image = RenderWidgetAtLogicalSize(*button);
        // The body reaches from just inside the top edge to just inside the bottom edge, left of the label.
        constexpr int32 INSIDE = 2;
        CHECK(image.pixelColor(INSIDE + 1, INSIDE) == body);
        CHECK(image.pixelColor(INSIDE + 1, image.height() / 2) == body);
        CHECK(image.pixelColor(INSIDE + 1, image.height() - 1 - INSIDE) == body);
        CHECK(CountColour(image, MacLikeStyle::NativeFill()) == 0);
    }
    // The compact height is the label plus the extra-small step and both edges, on every platform.
    const int32 edge = mac.Style.GetEdgeWidth();
    CHECK(compact->sizeHint().height() == compact->fontMetrics().height() + design::SPACING_EXTRA_SMALL + 2 * edge);
    MacLikeEditorStyle::Release(root);
}

TEST_CASE("A switch that is on fills with the accent where the platform draws tool buttons natively",
          "[editor][style][macos]")
{
    // On macOS, Force raster and Stats showed only a faint native fill when on.
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QHBoxLayout* layout = new QHBoxLayout(&root);
    QToolButton* on = new QToolButton(&root);
    on->setText(QStringLiteral("Force raster"));
    on->setProperty("visualRole", QStringLiteral("toggle"));
    on->setCheckable(true);
    on->setChecked(true);
    on->setAutoRaise(true);
    on->setFocusPolicy(Qt::NoFocus);
    QToolButton* off = new QToolButton(&root);
    off->setText(QStringLiteral("Stats"));
    off->setProperty("visualRole", QStringLiteral("toggle"));
    off->setCheckable(true);
    off->setAutoRaise(true);
    off->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(on);
    layout->addWidget(off);
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.show();
    QApplication::processEvents();
    const QColor accent = root.palette().color(QPalette::Link);
    constexpr int32 BODY_SAMPLE_X = 3;
    const QImage onImage = RenderWidgetAtLogicalSize(*on);
    const QImage offImage = RenderWidgetAtLogicalSize(*off);
    CHECK(onImage.pixelColor(BODY_SAMPLE_X, onImage.height() / 2) == accent);
    CHECK(offImage.pixelColor(BODY_SAMPLE_X, offImage.height() / 2) != accent);
    CHECK(CountColour(onImage, MacLikeStyle::NativeFill()) == 0);
    CHECK(CountColour(offImage, MacLikeStyle::NativeFill()) == 0);
    MacLikeEditorStyle::Release(root);
}

TEST_CASE("Check boxes draw a token box that shows when off, on any platform", "[editor][style][macos]")
{
    // Unchecked boxes were invisible on Linux in the dark palette (Fusion draws only the tick) and dark blobs on
    // macOS. The box has a control edge on the panel, which the contrast pairs hold at 3:1 or more.
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QCheckBox* off = new QCheckBox(QStringLiteral("Stars"), &root);
    QCheckBox* on = new QCheckBox(QStringLiteral("Enabled"), &root);
    on->setChecked(true);
    QCheckBox* partial = new QCheckBox(QStringLiteral("Some"), &root);
    partial->setTristate(true);
    partial->setCheckState(Qt::PartiallyChecked);
    for (QCheckBox* box : {off, on, partial})
    {
        box->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(box);
    }
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.show();
    QApplication::processEvents();
    CHECK(mac.Style.pixelMetric(QStyle::PM_IndicatorWidth, nullptr, off) == design::SIZE_CHECKBOX);
    CHECK(mac.Style.pixelMetric(QStyle::PM_IndicatorHeight, nullptr, off) == design::SIZE_CHECKBOX);
    const QColor edge = root.palette().color(QPalette::Mid);
    const QColor panel = root.palette().color(QPalette::Base);
    const QColor accent = root.palette().color(QPalette::Link);
    const auto indicatorOf = [&mac](QCheckBox* box) {
        QStyleOptionButton option;
        option.initFrom(box);
        option.text = box->text();
        return mac.Style.subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
    };
    // Sample a quarter of the way down the box, clear of the tick and the bar, and the middle of its left edge.
    const auto sampleBody = [](const QImage& image, const QRect& indicator) {
        return image.pixelColor(indicator.center().x(), indicator.top() + indicator.height() / 4);
    };
    const QRect offIndicator = indicatorOf(off);
    const QImage offImage = RenderWidgetAtLogicalSize(*off);
    CHECK(offImage.pixelColor(offIndicator.left(), offIndicator.center().y()) == edge);
    CHECK(sampleBody(offImage, offIndicator) == panel);
    const QImage onImage = RenderWidgetAtLogicalSize(*on);
    CHECK(sampleBody(onImage, indicatorOf(on)) == accent);
    const QImage partialImage = RenderWidgetAtLogicalSize(*partial);
    CHECK(sampleBody(partialImage, indicatorOf(partial)) == accent);
    for (const QImage& image : {offImage, onImage, partialImage})
    {
        CHECK(CountColour(image, MacLikeStyle::NativeFill()) == 0);
    }
    MacLikeEditorStyle::Release(root);
}

TEST_CASE("Group boxes are flat sections with a title, not cards, on any platform", "[editor][style][macos]")
{
    // On macOS every component sat in a rounded inset card under a small plain title.
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QGroupBox* group = new QGroupBox(QStringLiteral("Light"), &root);
    QVBoxLayout* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->addWidget(new QLabel(QStringLiteral("Intensity"), group));
    QGroupBox* checkable = new QGroupBox(QStringLiteral("Shadows"), &root);
    checkable->setCheckable(true);
    checkable->setChecked(false);
    new QVBoxLayout(checkable);
    layout->addWidget(group);
    layout->addWidget(checkable);
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.resize(QSize(320, 240));
    root.show();
    QApplication::processEvents();
    const QFont title = MakeTypeFont(group->font(), TypeRole::Title);
    const int32 edge = 1;
    // The contents run the full width under the title: no card inset at the sides or the bottom.
    const QMargins margins = group->contentsMargins();
    CHECK(margins.left() == 0);
    CHECK(margins.right() == 0);
    CHECK(margins.bottom() == 0);
    CHECK(margins.top() == edge + design::SPACING_MEDIUM + QFontMetrics(title).height() + design::SPACING_SMALL);
    QStyleOptionGroupBox option;
    option.initFrom(group);
    option.text = group->title();
    option.subControls = QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxLabel;
    const QRect label = mac.Style.subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, group);
    CHECK(label.width() >= QFontMetrics(title).horizontalAdvance(group->title()));
    CHECK(label.height() == QFontMetrics(title).height());
    const QImage image = RenderWidgetAtLogicalSize(*group);
    CHECK(CountColour(image, MacLikeStyle::NativeFill()) == 0);
    // A quiet rule along the top separates sections.
    CHECK(image.pixelColor(image.width() / 2, 0) == root.palette().color(QPalette::Midlight));
    // Clicking the title of a checkable section toggles it, as the hit test follows the editor's layout.
    QStyleOptionGroupBox checkOption;
    checkOption.initFrom(checkable);
    checkOption.text = checkable->title();
    checkOption.subControls = QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxLabel | QStyle::SC_GroupBoxCheckBox;
    const QRect box =
        mac.Style.subControlRect(QStyle::CC_GroupBox, &checkOption, QStyle::SC_GroupBoxCheckBox, checkable);
    CHECK(box.size() == QSize(design::SIZE_CHECKBOX, design::SIZE_CHECKBOX));
    CHECK(mac.Style.hitTestComplexControl(QStyle::CC_GroupBox, &checkOption, box.center(), checkable) ==
          QStyle::SC_GroupBoxCheckBox);
    QTest::mouseClick(checkable, Qt::LeftButton, Qt::NoModifier, box.center());
    CHECK(checkable->isChecked() == true);
    MacLikeEditorStyle::Release(root);
}

TEST_CASE("Tabs start at the leading edge where the platform centres them", "[editor][style][macos]")
{
    // Lights and Materials read as small centred segmented tabs on macOS.
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QTabWidget* tabs = new QTabWidget(&root);
    tabs->addTab(new QWidget, QStringLiteral("Lights"));
    tabs->addTab(new QWidget, QStringLiteral("Materials"));
    layout->addWidget(tabs);
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.resize(QSize(600, 300));
    root.show();
    QApplication::processEvents();
    CHECK(mac.Style.styleHint(QStyle::SH_TabBar_Alignment, nullptr, tabs, nullptr) == Qt::AlignLeft);
    const QTabBar* bar = tabs->tabBar();
    CHECK(bar->geometry().left() <= design::SPACING_EXTRA_SMALL);
    MacLikeEditorStyle::Release(root);
}

TEST_CASE("Every widget class takes the body font, whatever the platform theme gives it", "[editor][style][macos]")
{
    // macOS gives group box titles, tabs, headers, tool buttons and check boxes a smaller font than the body.
    const QFont body = QApplication::font();
    QFont small = body;
    small.setPointSizeF(body.pointSizeF() > 1.0 ? body.pointSizeF() - 1.0 : body.pointSizeF());
    for (const char* className : BODY_FONT_CLASS_NAMES)
    {
        QApplication::setFont(small, className);
    }
    ApplyThemeFonts();
    for (const char* className : BODY_FONT_CLASS_NAMES)
    {
        INFO(className);
        CHECK(QApplication::font(className).pointSizeF() == body.pointSizeF());
    }
    QGroupBox group(QStringLiteral("Section"));
    QTabBar bar;
    QCheckBox check(QStringLiteral("Enabled"));
    CHECK(group.font().pointSizeF() == body.pointSizeF());
    CHECK(bar.font().pointSizeF() == body.pointSizeF());
    CHECK(check.font().pointSizeF() == body.pointSizeF());
}

TEST_CASE("Style gallery over a macOS-like platform style renders for visual comparison",
          "[editor][style][macos][.gallery]")
{
    const QString directory = qEnvironmentVariable("LUDUS_STYLE_GALLERY_DIR");
    if (directory.isEmpty() == true)
    {
        SKIP("Set LUDUS_STYLE_GALLERY_DIR to write gallery images");
    }
    QDir().mkpath(directory);
    QWidget root;
    ApplyWorkspaceStyle(&root);
    root.setAutoFillBackground(true);
    root.setBackgroundRole(QPalette::Window);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QHBoxLayout* buttons = new QHBoxLayout;
    for (const QString& text : {QStringLiteral("Solo"), QStringLiteral("Enable All"), QStringLiteral("Apply Changes")})
    {
        QPushButton* button = new QPushButton(text);
        button->setProperty("density", QStringLiteral("compact"));
        buttons->addWidget(button);
    }
    QPushButton* primary = new QPushButton(QStringLiteral("Repair project"));
    primary->setProperty("visualRole", QStringLiteral("primary"));
    buttons->addWidget(primary);
    QToolButton* toggle = new QToolButton;
    toggle->setText(QStringLiteral("Force raster"));
    toggle->setProperty("visualRole", QStringLiteral("toggle"));
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setAutoRaise(true);
    buttons->addWidget(toggle);
    layout->addLayout(buttons);
    QGroupBox* group = new QGroupBox(QStringLiteral("Light"));
    QVBoxLayout* groupLayout = new QVBoxLayout(group);
    QCheckBox* enabled = new QCheckBox(QStringLiteral("Enabled"));
    enabled->setChecked(true);
    groupLayout->addWidget(enabled);
    groupLayout->addWidget(new QCheckBox(QStringLiteral("Stars")));
    layout->addWidget(group);
    QTabWidget* tabs = new QTabWidget;
    tabs->addTab(new QLabel(QStringLiteral("Light list")), QStringLiteral("Lights"));
    tabs->addTab(new QWidget, QStringLiteral("Materials"));
    layout->addWidget(tabs, 1);
    MacLikeEditorStyle mac;
    mac.Apply(root);
    root.resize(QSize(560, 360));
    root.show();
    QTest::qWait(150);
    root.grab().save(QStringLiteral("%1/gallery-macos-stand-in.png").arg(directory));
    MacLikeEditorStyle::Release(root);
}
