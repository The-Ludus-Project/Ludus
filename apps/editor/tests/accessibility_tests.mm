#include "internal/qt_accessibility.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
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
constexpr int32 REFRESH_COUNT = 3;

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
            CheckNativeSelection(table, static_cast<usize>(ROW_COUNT) * static_cast<usize>(COLUMN_COUNT));
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

TEST_CASE("Cocoa accessibility keeps table-owned cells until their table releases them",
          "[editor][accessibility][native]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("The native Cocoa bridge is required");
    }
    REQUIRE(ludus::editor::InstallQtAccessibilityOwnershipFix() == true);
    QAccessible::setActive(true);

    // Editor selectors rebuild their items with clear() and addItem() on refresh. Each
    // step drains its own pool, as the editor's run loop does between events, so the
    // native rows that Cocoa replaces on a model change are really released.
    QComboBox combo;
    QAccessibleInterface* listOrNull = nullptr;
    @autoreleasepool
    {
        for (int32 row = 0; row < ROW_COUNT; ++row)
        {
            combo.addItem(QString::number(row));
        }
        combo.show();
        QApplication::processEvents();
        listOrNull = QAccessible::queryAccessibleInterface(combo.view());
    }
    REQUIRE(listOrNull != nullptr);
    QAccessibleTableInterface* tableOrNull = listOrNull->tableInterface();
    REQUIRE(tableOrNull != nullptr);
    QAccessibleTableInterface& table = *tableOrNull;
    for (int32 refresh = 0; refresh < REFRESH_COUNT; ++refresh)
    {
        INFO("refresh " << refresh);
        QAccessible::Id cellIdentity = 0;
        @autoreleasepool
        {
            QAccessibleInterface* cellOrNull = table.cellAt(0, 0);
            REQUIRE(cellOrNull != nullptr);
            cellIdentity = QAccessible::uniqueId(cellOrNull);
            // A native client reading the item replaces the row's placeholder with a real cell element.
            id native = [NSClassFromString(@"QMacAccessibilityElement") elementWithId:cellIdentity];
            REQUIRE(native != nil);
            // Inserting a row rebuilds the native table model; the old rows are released with the pool.
            combo.addItem(QString::number(ROW_COUNT));
        }
        // The Qt table still caches the cell, so releasing native rows must not delete it.
        QAccessibleInterface* cachedCellOrNull = QAccessible::accessibleInterface(cellIdentity);
        REQUIRE(cachedCellOrNull != nullptr);
        CHECK(cachedCellOrNull == table.cellAt(0, 0));
        @autoreleasepool
        {
            // Qt 6.12.0 dereferences every cached cell while removing rows.
            combo.clear();
            CHECK(QAccessible::accessibleInterface(cellIdentity) == nullptr);
            for (int32 row = 0; row < ROW_COUNT; ++row)
            {
                combo.addItem(QString::number(row));
            }
            QApplication::processEvents();
        }
    }
}
