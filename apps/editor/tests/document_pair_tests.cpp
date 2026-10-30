#include "internal/document_well.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QSplitter>
#include <QString>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

using namespace ludus::editor;
using namespace ludus::foundation;

TEST_CASE("Paired documents retain the World parent text history and adjusted split", "[editor][code-viewport]")
{
    DocumentWell well;
    QPlainTextEdit* code = new QPlainTextEdit;
    code->setObjectName(QStringLiteral("code"));
    code->setPlainText(QStringLiteral("float Value;"));
    code->insertPlainText(QStringLiteral("// unsaved\n"));
    QTextCursor cursor = code->textCursor();
    cursor.setPosition(1);
    cursor.setPosition(4, QTextCursor::KeepAnchor);
    code->setTextCursor(cursor);
    const QString text = code->toPlainText();
    QWidget* world = new QWidget;
    world->setObjectName(QStringLiteral("world"));
    well.AddDocument(code, QStringLiteral("Code"), DocumentCloseMode::OwnerCloses);
    well.AddDocument(world, QStringLiteral("World"), DocumentCloseMode::OwnerCloses);
    well.resize(1000, 600);
    well.show();
    REQUIRE(well.OpenDocument(world) == true);
    QWidget* worldParent = world->parentWidget();
    REQUIRE(worldParent != nullptr);
    QSignalSpy closes(&well, &DocumentWell::CloseRequested);
    REQUIRE(well.ShowDocumentsSideBySide(code, world) == true);
    QApplication::processEvents();
    CHECK(well.GetGroupCount() == 2);
    CHECK(well.GetSplitOrientation() == Qt::Horizontal);
    CHECK(well.GetGroupIndex(code) != well.GetGroupIndex(world));
    CHECK(well.IsDocumentCurrent(code) == true);
    CHECK(well.IsDocumentCurrent(world) == true);
    CHECK(well.GetActiveDocumentOrNull() == code);
    CHECK(world->parentWidget() == worldParent);
    CHECK(code->toPlainText() == text);
    CHECK(code->textCursor().position() == cursor.position());
    CHECK(code->textCursor().anchor() == cursor.anchor());
    CHECK(code->document()->isUndoAvailable() == true);
    CHECK(closes.isEmpty() == true);
    QSplitter* splitterOrNull = well.findChild<QSplitter*>(QStringLiteral("documentGroupSplitter"));
    REQUIRE(splitterOrNull != nullptr);
    splitterOrNull->setSizes({350, 650});
    const QList<int32> sizes = splitterOrNull->sizes();
    REQUIRE(well.OpenDocument(world) == true);
    REQUIRE(well.ShowDocumentsSideBySide(code, world) == true);
    QApplication::processEvents();
    CHECK(splitterOrNull->sizes() == sizes);
    CHECK(world->parentWidget() == worldParent);
    CHECK(code->toPlainText() == text);
}

TEST_CASE("A paired layout reuses full tab groups and round trips through existing preferences",
          "[editor][code-viewport]")
{
    DocumentWell well;
    QList<QWidget*> documents;
    for (const QString& name : {QStringLiteral("code"),
                                QStringLiteral("world"),
                                QStringLiteral("otherA"),
                                QStringLiteral("otherB"),
                                QStringLiteral("otherC")})
    {
        QWidget* document = new QWidget;
        document->setObjectName(name);
        well.AddDocument(document, name);
        documents.append(document);
    }
    const QJsonArray groups{
        QJsonObject{
            {QStringLiteral("documents"), QJsonArray{QStringLiteral("code"), QStringLiteral("world")}},
            {QStringLiteral("current"), QStringLiteral("world")},
        },
        QJsonObject{{QStringLiteral("documents"), QJsonArray{QStringLiteral("otherA")}}},
        QJsonObject{{QStringLiteral("documents"), QJsonArray{QStringLiteral("otherB")}}},
        QJsonObject{{QStringLiteral("documents"), QJsonArray{QStringLiteral("otherC")}}},
    };
    const QJsonObject previous{
        {QStringLiteral("version"), 1},
        {QStringLiteral("orientation"), QStringLiteral("vertical")},
        {QStringLiteral("active"), 0},
        {QStringLiteral("groups"), groups},
    };
    REQUIRE(well.RestoreState(previous) == true);
    QWidget* worldParent = documents.at(1)->parentWidget();
    REQUIRE(worldParent != nullptr);
    REQUIRE(well.ShowDocumentsSideBySide(documents.at(0), documents.at(1)) == true);
    CHECK(well.GetGroupCount() == MAX_DOCUMENT_GROUPS);
    CHECK(well.GetSplitOrientation() == Qt::Horizontal);
    CHECK(documents.at(1)->parentWidget() == worldParent);
    for (QWidget* document : documents)
    {
        CHECK(well.IsDocumentOpen(document) == true);
    }
    const QJsonObject saved = well.SaveState();
    well.MergeGroups();
    REQUIRE(well.RestoreState(saved) == true);
    CHECK(well.IsDocumentCurrent(documents.at(0)) == true);
    CHECK(well.IsDocumentCurrent(documents.at(1)) == true);
    CHECK(well.GetActiveDocumentOrNull() == documents.at(0));
    CHECK(well.SaveState().value(QStringLiteral("groups")) == saved.value(QStringLiteral("groups")));
    REQUIRE(well.OpenDocument(documents.at(2)) == true);
    CHECK(well.IsDocumentCurrent(documents.at(0)) == false);
    CHECK(well.IsDocumentCurrent(documents.at(1)) == true);
}

TEST_CASE("Invalid document pair requests preserve the accepted arrangement", "[editor][code-viewport]")
{
    DocumentWell well;
    QWidget* code = new QWidget;
    code->setObjectName(QStringLiteral("code"));
    QWidget* world = new QWidget;
    world->setObjectName(QStringLiteral("world"));
    QWidget unregistered;
    well.AddDocument(code, QStringLiteral("Code"));
    well.AddDocument(world, QStringLiteral("World"));
    REQUIRE(well.OpenDocument(code) == true);
    well.SetDocumentAvailable(world, false);
    const QJsonObject saved = well.SaveState();
    CHECK(well.ShowDocumentsSideBySide(code, world) == false);
    CHECK(well.ShowDocumentsSideBySide(code, &unregistered) == false);
    CHECK(well.ShowDocumentsSideBySide(code, code) == false);
    CHECK(well.SaveState() == saved);
    CHECK(well.GetActiveDocumentOrNull() == code);
}
