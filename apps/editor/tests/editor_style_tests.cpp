#include <catch2/catch_test_macros.hpp>

#include "internal/theme_preferences.h"
#include "internal/workspace_style.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
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
