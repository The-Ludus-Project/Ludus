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
#include <QSignalSpy>
#include <QTableWidget>

using namespace ludus::editor;

TEST_CASE("preview diagnostics replace independently and reveal a changed activation failure once",
          "[editor][render-graph][diagnostics]")
{
    ErrorList list;
    QSignalSpy problems(&list, &ErrorList::ProblemsReported);
    const QString failure = QStringLiteral("Renderer activation failed (OutOfMemory) for generation 2; "
                                           "the last good scene is kept.");
    const QString file = QStringLiteral("graphs/Main/main.lurender.json");
    const QList<ErrorListEntry> preview = MakeRenderPreviewErrorEntries(failure, file);
    REQUIRE(preview.size() == 1);
    CHECK(preview.first().Severity == ErrorListSeverity::Error);
    CHECK(preview.first().Producer == ErrorListProducer::RenderPreview);
    CHECK(preview.first().Description == failure);
    CHECK(preview.first().File == file);
    CHECK(preview.first().Code == QStringLiteral("RenderGraphPreviewFailed"));
    list.ReplaceEntries(ErrorListProducer::Build, ParseBuildErrorEntries(QStringLiteral("game.cpp:1: error: failure")));
    problems.clear();
    list.ReplaceEntries(ErrorListProducer::RenderPreview, preview);
    CHECK(problems.count() == 1);
    list.ReplaceEntries(ErrorListProducer::RenderPreview, preview);
    CHECK(problems.count() == 1);
    RenderGraphJobResult successful;
    successful.IsSuccessful = true;
    successful.Request.Action = QStringLiteral("build");
    list.ReplaceEntries(ErrorListProducer::RenderGraph, MakeRenderGraphErrorEntries(successful));
    CHECK(list.GetCount(ErrorListSeverity::Error) == 2);
    list.ReplaceEntries(ErrorListProducer::RenderPreview, MakeRenderPreviewErrorEntries({}, file));
    CHECK(list.GetCount(ErrorListSeverity::Error) == 1);
    CHECK(list.GetCount(ErrorListSeverity::Message) == 1);
}

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

TEST_CASE("Error List shows details only for descriptions its row cannot show", "[editor][diagnostics]")
{
    ErrorList list;
    list.resize(800, 200);
    list.show();
    const QPlainTextEdit* details = list.findChild<QPlainTextEdit*>(QStringLiteral("errorListDetails"));
    REQUIRE(details != nullptr);
    list.ReplaceEntries(ErrorListProducer::RenderGraph,
                        {
                        {
                            .Severity = ErrorListSeverity::Message,
                            .Producer = ErrorListProducer::RenderGraph,
                            .Code = {},
                            .Description = QStringLiteral("build succeeded for draft revision 2."),
                            .File = {},
                            .Location = {},
                            .Origin = QStringLiteral("Render graph"),
                            .SourceIndex = 0,
                        }});
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == false);
    list.ReplaceEntries(ErrorListProducer::RenderGraph,
                        {
                        {
                            .Severity = ErrorListSeverity::Error,
                            .Producer = ErrorListProducer::RenderGraph,
                            .Code = QStringLiteral("RenderGraphTypeMismatch"),
                            .Description = QStringLiteral("wrong input type\nrepair: Connect a depth target."),
                            .File = {},
                            .Location = {},
                            .Origin = QStringLiteral("Render graph"),
                            .SourceIndex = 0,
                        }});
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == true);
    // A single line wider than its column is elided in the row, so the pane shows all of it.
    list.ReplaceEntries(ErrorListProducer::RenderGraph,
                        {
                        {
                            .Severity = ErrorListSeverity::Warning,
                            .Producer = ErrorListProducer::RenderGraph,
                            .Code = {},
                            .Description = QString(400, QLatin1Char('w')),
                            .File = {},
                            .Location = {},
                            .Origin = QStringLiteral("Shader"),
                            .SourceIndex = 0,
                        }});
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == true);
}

TEST_CASE("Error List reports new errors and warnings once per changed result", "[editor][diagnostics]")
{
    ErrorList list;
    QSignalSpy reported(&list, &ErrorList::ProblemsReported);
    const ErrorListEntry message
    {
        .Severity = ErrorListSeverity::Message,
        .Producer = ErrorListProducer::RenderGraph,
        .Code = {},
        .Description = QStringLiteral("validate succeeded for draft revision 1."),
        .File = {},
        .Location = {},
        .Origin = QStringLiteral("Render graph"),
        .SourceIndex = 0,
    };
    ErrorListEntry warning = message;
    warning.Severity = ErrorListSeverity::Warning;
    warning.Description = QStringLiteral("implicit truncation");
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {message});
    CHECK(reported.count() == 0);
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {warning});
    CHECK(reported.count() == 1);
    // The same result again (a live preview rebuild) leaves a closed list closed.
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {warning});
    CHECK(reported.count() == 1);
    ErrorListEntry error = warning;
    error.Severity = ErrorListSeverity::Error;
    error.Producer = ErrorListProducer::Build;
    list.ReplaceEntries(ErrorListProducer::Build, {error});
    CHECK(reported.count() == 2);
    list.ReplaceEntries(ErrorListProducer::Build, {});
    CHECK(reported.count() == 2);
    CHECK(list.GetEntries().size() == 1);
}
