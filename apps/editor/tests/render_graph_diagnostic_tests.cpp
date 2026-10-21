// Shared CLI result records and readable Error List details, without a device.
#include "internal/error_list.h"
#include "internal/render_graph_job.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTableWidget>

using namespace ludus::editor;

TEST_CASE("structured validation batches produce one actionable Error List row per issue",
          "[editor][render-graph][diagnostics]")
{
    RenderGraphJobResult result;
    result.Code = QStringLiteral("RenderGraphTypeMismatch");
    result.Message = QStringLiteral("wrong input type");
    result.Diagnostics =
        QJsonArray{QJsonObject{{QStringLiteral("code"), result.Code},
                               {QStringLiteral("severity"), QStringLiteral("error")},
                               {QStringLiteral("message"), result.Message},
                               {QStringLiteral("expected"), QStringLiteral("DepthTexture")},
                               {QStringLiteral("actual"), QStringLiteral("ColorTexture")},
                               {QStringLiteral("repair"), QStringLiteral("Connect a depth target.")},
                               {QStringLiteral("location"),
                                QJsonObject{{QStringLiteral("source"), QStringLiteral("main.lurender.json")},
                                            {QStringLiteral("node"), QStringLiteral("draw")},
                                            {QStringLiteral("port"), QStringLiteral("depth")}}}},
                   QJsonObject{{QStringLiteral("code"), QStringLiteral("RenderGraphUnboundInput")},
                               {QStringLiteral("severity"), QStringLiteral("error")},
                               {QStringLiteral("message"), QStringLiteral("unconnected scene")}}};
    const QJsonObject response{{QStringLiteral("schema"), 1},
                               {QStringLiteral("error"),
                                QJsonObject{{QStringLiteral("code"), result.Code},
                                            {QStringLiteral("message"), result.Message},
                                            {QStringLiteral("diagnostics"), result.Diagnostics}}}};
    const RenderGraphJobResult parsed = RenderGraphJob::ParseOutput({}, QJsonDocument(response).toJson(), 1);
    CHECK(parsed.IsSuccessful == false);
    CHECK(parsed.Diagnostics == result.Diagnostics);
    const QList<ErrorListEntry> entries = MakeRenderGraphErrorEntries(parsed);
    REQUIRE(entries.size() == 2);
    CHECK(result.GetDiagnosticCount() == 2);
    ErrorList list;
    list.ReplaceEntries(ErrorListProducer::RenderGraph, entries);
    const QPlainTextEdit* details = list.findChild<QPlainTextEdit*>(QStringLiteral("errorListDetails"));
    REQUIRE(details != nullptr);
    list.resize(800, 160);
    list.show();
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == true);
    CHECK(details->height() > 0);
    CHECK(details->isReadOnly() == true);
    CHECK(details->toPlainText() == entries.first().Description);
    QTableWidget* table = list.findChild<QTableWidget*>();
    REQUIRE(table != nullptr);
    CHECK(table->viewport()->height() >= 2 * table->verticalHeader()->defaultSectionSize());
    table->setCurrentCell(1, 0);
    CHECK(details->toPlainText() == entries.last().Description);
    CHECK(entries.first().SourceIndex == 0);
    CHECK(entries.last().SourceIndex == 1);
    CHECK(entries.first().Code == result.Code);
    CHECK(entries.first().Description.contains(QStringLiteral("expected: DepthTexture")) == true);
    CHECK(entries.first().Description.contains(QStringLiteral("actual: ColorTexture")) == true);
    CHECK(entries.first().Description.contains(QStringLiteral("repair: Connect a depth target.")) == true);
    CHECK(entries.first().Location.contains(QStringLiteral("port depth")) == true);
    CHECK(result.GetDiagnostic(-1).isEmpty() == true);
    CHECK(result.GetDiagnostic(2).isEmpty() == true);
}
