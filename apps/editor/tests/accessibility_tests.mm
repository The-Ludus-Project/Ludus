#include "internal/qt_accessibility.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAccessible>
#include <QApplication>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QString>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>

// Test the real Cocoa bridge, including its placeholder replacement and cache cleanup.
@interface NSObject (LudusAccessibilityTests)
+ (id)elementWithId:(QAccessible::Id)identity;
@end

namespace
{
using namespace ludus::foundation;
constexpr int32 ROW_COUNT = 3;
constexpr int32 COLUMN_COUNT = 3;
constexpr int32 RESET_COUNT = 3;

void CheckNativeSelection(QAbstractItemView& inoutWidget, usize expectedCount)
{
    inoutWidget.show();
    QApplication::processEvents();
    QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(&inoutWidget);
    REQUIRE(interface != nullptr);
    const QAccessible::Id identity = QAccessible::uniqueId(interface);
    const QModelIndexList selectedIndexes = inoutWidget.selectionModel()->selectedIndexes();
    @autoreleasepool
    {
        id native = [NSClassFromString(@"QMacAccessibilityElement") elementWithId:identity];
        REQUIRE(native != nil);
        NSArray* selected = [native accessibilitySelectedChildren];
        REQUIRE(selected.count == expectedCount);
        // Replacing a synthetic placeholder must not delete the table or sibling interfaces.
        CHECK(QAccessible::accessibleInterface(identity) == interface);
        usize index = 0;
        for (id cell in selected)
        {
            CHECK([cell accessibilityRole] != nil);
            const QString expected = selectedIndexes.at(static_cast<qsizetype>(index)).data().toString();
            CHECK(QString::fromNSString([cell accessibilityTitle]) == expected);
            ++index;
        }
        CHECK([native accessibilitySelectedChildren].count == expectedCount);
    }
    CHECK(QAccessible::accessibleInterface(identity) == interface);
}
} // namespace

TEST_CASE("Cocoa accessibility preserves selected cells and borrowed table identities",
          "[editor][accessibility][native]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("The native Cocoa bridge is required");
    }
    REQUIRE(ludus::editor::InstallQtAccessibilityOwnershipFix() == true);
    REQUIRE(ludus::editor::InstallQtAccessibilityOwnershipFix() == true);
    QAccessible::setActive(true);

    SECTION("table selection survives native placeholder replacement, deselection and model resets")
    {
        QTableWidget table(0, COLUMN_COUNT);
        for (int32 reset = 0; reset < RESET_COUNT; ++reset)
        {
            table.setRowCount(ROW_COUNT);
            for (int32 row = 0; row < ROW_COUNT; ++row)
            {
                for (int32 column = 0; column < COLUMN_COUNT; ++column)
                {
                    table.setItem(row, column, new QTableWidgetItem(QString::number(row * COLUMN_COUNT + column)));
                }
            }
            table.selectAll();
            CheckNativeSelection(table, ROW_COUNT * COLUMN_COUNT);
            table.clearSelection();
            CheckNativeSelection(table, 0);
            table.selectRow(1);
            CheckNativeSelection(table, COLUMN_COUNT);
            table.setRowCount(0);
            CheckNativeSelection(table, 0);
        }
    }
    SECTION("list multiple selection remains accessible")
    {
        QListWidget list;
        list.setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (int32 row = 0; row < ROW_COUNT; ++row)
        {
            list.addItem(QString::number(row));
        }
        list.selectAll();
        CheckNativeSelection(list, ROW_COUNT);
    }
    SECTION("tree selection remains accessible across expansion and removal")
    {
        QTreeWidget tree;
        tree.setColumnCount(COLUMN_COUNT);
        tree.setSelectionMode(QAbstractItemView::ExtendedSelection);
        QTreeWidgetItem* parent = new QTreeWidgetItem(&tree, {QStringLiteral("parent")});
        for (int32 row = 0; row < ROW_COUNT; ++row)
        {
            new QTreeWidgetItem(parent, {QString::number(row)});
        }
        tree.expandAll();
        tree.setCurrentItem(parent, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        CheckNativeSelection(tree, COLUMN_COUNT);
        tree.clear();
        CheckNativeSelection(tree, 0);
    }
}
