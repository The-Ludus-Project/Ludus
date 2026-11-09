#include "internal/theme_preferences.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

using namespace ludus::editor;

namespace
{
constexpr QSize GALLERY_SIZE(900, 640);
constexpr int GALLERY_ROW_COUNT = 6;

/// One window with each control the workspace stylesheet styled, in the states a still image can show.
QMainWindow* MakeStyleGallery()
{
    QMainWindow* window = new QMainWindow;
    window->resize(GALLERY_SIZE);
    QToolBar* toolBar = window->addToolBar(QStringLiteral("Tools"));
    QToolButton* plain = new QToolButton;
    plain->setText(QStringLiteral("Plain"));
    QToolButton* checked = new QToolButton;
    checked->setText(QStringLiteral("Checked"));
    checked->setCheckable(true);
    checked->setChecked(true);
    QToolButton* disabled = new QToolButton;
    disabled->setText(QStringLiteral("Disabled"));
    disabled->setEnabled(false);
    toolBar->addWidget(plain);
    toolBar->addWidget(checked);
    toolBar->addWidget(disabled);
    QTabWidget* tabs = new QTabWidget;
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* normal = new QPushButton(QStringLiteral("Normal"));
    QPushButton* defaultButton = new QPushButton(QStringLiteral("Default"));
    defaultButton->setDefault(true);
    QPushButton* primary = new QPushButton(QStringLiteral("Primary"));
    primary->setProperty("visualRole", QStringLiteral("primary"));
    QPushButton* compact = new QPushButton(QStringLiteral("Compact"));
    compact->setProperty("density", QStringLiteral("compact"));
    QPushButton* toggled = new QPushButton(QStringLiteral("Checked"));
    toggled->setCheckable(true);
    toggled->setChecked(true);
    QPushButton* off = new QPushButton(QStringLiteral("Disabled"));
    off->setEnabled(false);
    for (QPushButton* button : {normal, defaultButton, primary, compact, toggled, off})
    {
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    QHBoxLayout* fields = new QHBoxLayout;
    QLineEdit* line = new QLineEdit(QStringLiteral("Line edit"));
    QLineEdit* focused = new QLineEdit(QStringLiteral("Focused"));
    QDoubleSpinBox* spin = new QDoubleSpinBox;
    spin->setValue(2.5);
    QComboBox* combo = new QComboBox;
    combo->addItems({QStringLiteral("Combo box"), QStringLiteral("Second")});
    fields->addWidget(line);
    fields->addWidget(focused);
    fields->addWidget(spin);
    fields->addWidget(combo);
    layout->addLayout(fields);
    QHBoxLayout* toggles = new QHBoxLayout;
    QCheckBox* check = new QCheckBox(QStringLiteral("Check box"));
    check->setChecked(true);
    QRadioButton* radio = new QRadioButton(QStringLiteral("Radio"));
    radio->setChecked(true);
    QCheckBox* disabledCheck = new QCheckBox(QStringLiteral("Disabled check"));
    disabledCheck->setEnabled(false);
    QLabel* secondary = new QLabel(QStringLiteral("Secondary label"));
    secondary->setProperty("visualRole", QStringLiteral("secondary"));
    toggles->addWidget(check);
    toggles->addWidget(radio);
    toggles->addWidget(disabledCheck);
    toggles->addWidget(secondary);
    layout->addLayout(toggles);
    QHBoxLayout* views = new QHBoxLayout;
    QListWidget* list = new QListWidget;
    for (int row = 0; row < GALLERY_ROW_COUNT; ++row)
    {
        list->addItem(QStringLiteral("List item %1").arg(row));
    }
    list->setCurrentRow(1);
    QTableWidget* table = new QTableWidget(GALLERY_ROW_COUNT, 3);
    table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("Size")});
    for (int row = 0; row < GALLERY_ROW_COUNT; ++row)
    {
        table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("Row %1").arg(row)));
    }
    QPlainTextEdit* text = new QPlainTextEdit(QStringLiteral("Plain text edit\nsecond line"));
    views->addWidget(list);
    views->addWidget(table);
    views->addWidget(text);
    layout->addLayout(views, 1);
    QGroupBox* group = new QGroupBox(QStringLiteral("Group box"));
    QHBoxLayout* groupLayout = new QHBoxLayout(group);
    groupLayout->addWidget(new QLabel(QStringLiteral("Label inside a group")));
    layout->addWidget(group);
    tabs->addTab(page, QStringLiteral("First"));
    tabs->addTab(new QWidget, QStringLiteral("Second"));
    tabs->addTab(new QWidget, QStringLiteral("Third"));
    window->setCentralWidget(tabs);
    QDockWidget* dock = new QDockWidget(QStringLiteral("Dock title"));
    dock->setWidget(new QLabel(QStringLiteral("Dock content")));
    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    window->statusBar()->showMessage(QStringLiteral("Status bar message"));
    focused->setFocus();
    return window;
}
} // namespace

TEST_CASE("Style gallery renders for visual comparison", "[editor][style][.gallery]")
{
    const QString directory = qEnvironmentVariable("LUDUS_STYLE_GALLERY_DIR");
    if (directory.isEmpty() == true)
    {
        SKIP("Set LUDUS_STYLE_GALLERY_DIR to write gallery images");
    }
    QDir().mkpath(directory);
    const QPalette original = QApplication::palette();
    for (const bool isHighContrast : {false, true})
    {
        qputenv(HIGH_CONTRAST_VARIABLE, isHighContrast == true ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
        for (const bool isDark : {true, false})
        {
            // A system theme's palette: the style takes its colours from the tokens, the rest from here.
            QPalette base = original;
            const QColor background = isDark == true ? QColor(40, 40, 40) : QColor(240, 240, 240);
            const QColor foreground = isDark == true ? QColor(230, 230, 230) : QColor(20, 20, 20);
            base.setColor(QPalette::Window, background);
            base.setColor(QPalette::Button, background.lighter(120));
            base.setColor(QPalette::Base, background.darker(110));
            base.setColor(QPalette::WindowText, foreground);
            base.setColor(QPalette::Text, foreground);
            base.setColor(QPalette::ButtonText, foreground);
            QApplication::setPalette(base);
            QMainWindow* window = MakeStyleGallery();
            ApplyWorkspaceStyle(window);
            window->show();
            QTest::qWait(150);
            window->activateWindow();
            QTest::qWait(150);
            window->grab().save(QStringLiteral("%1/gallery-%2-%3.png")
                                    .arg(directory,
                                         isDark == true ? QStringLiteral("dark") : QStringLiteral("light"),
                                         isHighContrast == true ? QStringLiteral("high") : QStringLiteral("normal")));
            delete window;
        }
    }
    qputenv(HIGH_CONTRAST_VARIABLE, QByteArrayLiteral("0"));
    qunsetenv(HIGH_CONTRAST_VARIABLE);
    QApplication::setPalette(original);
}

TEST_CASE("Combo boxes keep room for their whole text and draw a small chevron", "[editor][style]")
{
    const bool hasSpacedLetters = GENERATE(false, true);
    CAPTURE(hasSpacedLetters);
    constexpr ludus::foundation::float64 LETTER_SPACING_PIXELS = 4;
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QList<QComboBox*> combos;
    for (const QString& text : {QStringLiteral("Default"), QStringLiteral("(asset default)"), QStringLiteral("Edit")})
    {
        QComboBox* combo = new QComboBox(&root);
        combo->setFocusPolicy(Qt::NoFocus);
        if (hasSpacedLetters == true)
        {
            QFont font = combo->font();
            font.setLetterSpacing(QFont::AbsoluteSpacing, LETTER_SPACING_PIXELS);
            combo->setFont(font);
        }
        combo->addItem(text);
        combo->addItem(text + QStringLiteral(" option"));
        layout->addWidget(combo);
        combos.append(combo);
    }
    root.show();
    QApplication::processEvents();
    for (QComboBox* combo : combos)
    {
        INFO(combo->currentText().toStdString());
        QStyleOptionComboBox option;
        option.initFrom(combo);
        const QRect field =
            combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
        const QRect arrow =
            combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, combo);
        // The sized-to-content combo fits its text and leaves the arrow its own column.
        CHECK(field.width() >= combo->fontMetrics().horizontalAdvance(combo->currentText()));
        for (ludus::foundation::int32 item = 0; item < combo->count(); ++item)
        {
            CHECK(field.width() >= combo->fontMetrics().horizontalAdvance(combo->itemText(item)));
        }
        CHECK(field.right() < arrow.left());
        CHECK(combo->rect().contains(arrow) == true);
        // The chevron stays inside its column and is small: a few pixels either way, not a platform-sized arrow.
        const QImage image = combo->grab().toImage();
        const QColor background = combo->palette().color(QPalette::Base);
        QRect inked;
        // Scan inside the edge so only the chevron can leave a mark.
        const QRect scan = arrow.adjusted(1, 1, -1, -1);
        for (int y = scan.top(); y <= scan.bottom(); ++y)
        {
            for (int x = scan.left(); x <= scan.right(); ++x)
            {
                if (image.pixelColor(x, y) != background)
                {
                    inked = inked.united(QRect(x, y, 1, 1));
                }
            }
        }
        CHECK(inked.isNull() == false);
        CHECK(inked.width() <= arrow.width() / 2 + 2);
        CHECK(inked.height() <= arrow.height() / 3);
    }
}

TEST_CASE("A menu button keeps its label and its chevron apart", "[editor][style]")
{
    constexpr int MIN_GAP_PIXELS = 2;
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QList<QToolButton*> buttons;
    for (const QString& text : {QStringLiteral("Add World Preset"), QStringLiteral("Add")})
    {
        QToolButton* button = new QToolButton(&root);
        button->setText(text);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setMenu(new QMenu(button));
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
        buttons.append(button);
    }
    root.show();
    QApplication::processEvents();
    for (QToolButton* button : buttons)
    {
        INFO(button->text().toStdString());
        // The button is sized for its whole label and the chevron beside it.
        CHECK(button->sizeHint().width() >=
              button->fontMetrics().horizontalAdvance(button->text()) +
                  button->style()->pixelMetric(QStyle::PM_MenuButtonIndicator, nullptr, button));
        const QImage image = button->grab().toImage();
        const QColor background = image.pixelColor(image.width() / 2, 1);
        // The inked columns inside the edge form the label, then the chevron: two runs with blank columns between.
        QList<bool> columns;
        for (int x = 2; x < image.width() - 2; ++x)
        {
            bool hasInk = false;
            for (int y = 2; y < image.height() - 2; ++y)
            {
                hasInk = hasInk == true || image.pixelColor(x, y) != background;
            }
            columns.append(hasInk);
        }
        int lastInk = -1;
        int chevronStart = -1;
        for (int x = static_cast<int>(columns.size()) - 1; x >= 0; --x)
        {
            if (columns[x] == true && lastInk < 0)
            {
                lastInk = x;
            }
            if (lastInk >= 0 && columns[x] == false)
            {
                chevronStart = x + 1;
                break;
            }
        }
        REQUIRE(lastInk >= 0);
        REQUIRE(chevronStart >= 0);
        int gap = 0;
        for (int x = chevronStart - 1; x >= 0 && columns[x] == false; --x)
        {
            ++gap;
        }
        CHECK(gap >= MIN_GAP_PIXELS);
        CHECK(chevronStart > static_cast<int>(columns.size()) / 2);
    }
}

TEST_CASE("Tabs below their content draw the accent rule on the edge facing it", "[editor][style]")
{
    QWidget root;
    ApplyWorkspaceStyle(&root);
    QVBoxLayout* layout = new QVBoxLayout(&root);
    QTabBar* bar = new QTabBar(&root);
    bar->setShape(QTabBar::RoundedSouth);
    bar->setFocusPolicy(Qt::NoFocus);
    bar->addTab(QStringLiteral("Properties"));
    bar->addTab(QStringLiteral("Live Inspector"));
    bar->setCurrentIndex(0);
    layout->addWidget(bar);
    root.show();
    QApplication::processEvents();
    const QRect tab = bar->tabRect(0);
    const QImage image = bar->grab().toImage();
    const QColor accent = bar->palette().color(QPalette::Link);
    const int centre = tab.center().x();
    // The rule is on the top edge, toward the content above, and not on the outer edge.
    CHECK(image.pixelColor(centre, tab.top()) == accent);
    CHECK(image.pixelColor(centre, tab.bottom()) != accent);
    // The unselected tab carries no rule.
    CHECK(image.pixelColor(bar->tabRect(1).center().x(), bar->tabRect(1).top()) != accent);
}
