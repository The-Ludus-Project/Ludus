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
#include <QToolButton>

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

TEST_CASE("a preview advisory is one warning row beside the preview's errors", "[editor][render-graph][diagnostics]")
{
    ErrorList list;
    const QString file = QStringLiteral("graphs/Main/main.lurender.json");
    const QString warning = QStringLiteral("The slice-sampling luminaire panel cannot light this World.");
    const QList<ErrorListEntry> advisory = MakeRenderPreviewWarningEntries(warning, file);
    REQUIRE(advisory.size() == 1);
    CHECK(advisory.first().Severity == ErrorListSeverity::Warning);
    CHECK(advisory.first().Producer == ErrorListProducer::RenderPreview);
    CHECK(advisory.first().Code == QStringLiteral("RenderGraphPreviewWarning"));
    CHECK(advisory.first().Description == warning);
    CHECK(MakeRenderPreviewWarningEntries({}, file).isEmpty() == true);
    list.ReplaceEntries(ErrorListProducer::RenderPreview,
                        MakeRenderPreviewErrorEntries(QStringLiteral("Activation failed."), file) + advisory);
    CHECK(list.GetCount(ErrorListSeverity::Error) == 1);
    CHECK(list.GetCount(ErrorListSeverity::Warning) == 1);
    // Clearing the advisory keeps the preview's error.
    list.ReplaceEntries(ErrorListProducer::RenderPreview,
                        MakeRenderPreviewErrorEntries(QStringLiteral("Activation failed."), file));
    CHECK(list.GetCount(ErrorListSeverity::Warning) == 0);
    CHECK(list.GetCount(ErrorListSeverity::Error) == 1);
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
    // The repair hint reads in the row itself; the full text waits behind Details.
    QToolButton* toggle = list.findChild<QToolButton*>(QStringLiteral("errorListDetailsToggle"));
    REQUIRE(toggle != nullptr);
    CHECK(details->isVisible() == false);
    CHECK(toggle->isEnabled() == true);
    toggle->click();
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == true);
    CHECK(details->height() > 0);
    CHECK(details->isReadOnly() == true);
    CHECK(details->toPlainText() == entries.first().Description);
    QTableWidget* table = list.findChild<QTableWidget*>();
    REQUIRE(table != nullptr);
    CHECK(table->item(0, 2)->text().contains(QStringLiteral("repair: Connect a depth target.")) == true);
    CHECK(table->item(0, 2)->text().contains(QLatin1Char('\n')) == false);
    CHECK(table->viewport()->height() >= 2 * table->verticalHeader()->defaultSectionSize());
    // A one-line row has nothing more to show.
    table->setCurrentCell(1, 0);
    CHECK(details->toPlainText().isEmpty() == true);
    CHECK(toggle->isEnabled() == false);
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

TEST_CASE("Error List keeps details collapsed until Details is opened for a row that has some", "[editor][diagnostics]")
{
    ErrorList list;
    list.resize(800, 200);
    list.show();
    const QPlainTextEdit* details = list.findChild<QPlainTextEdit*>(QStringLiteral("errorListDetails"));
    QToolButton* toggle = list.findChild<QToolButton*>(QStringLiteral("errorListDetailsToggle"));
    QTableWidget* table = list.findChild<QTableWidget*>();
    REQUIRE(details != nullptr);
    REQUIRE(toggle != nullptr);
    REQUIRE(table != nullptr);
    const ErrorListEntry plain
    {
        .Severity = ErrorListSeverity::Message,
        .Producer = ErrorListProducer::RenderGraph,
        .Code = {},
        .Description = QStringLiteral("build succeeded for draft revision 2."),
        .File = {},
        .Location = {},
        .Origin = QStringLiteral("Render graph"),
        .SourceIndex = 0,
        .Details = {},
        .Backends = {},
        .Count = 1,
    };
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {plain});
    QCoreApplication::processEvents();
    CHECK(toggle->isEnabled() == false);
    CHECK(details->isVisible() == false);
    // Raw compiler output backs a row without being a row: it stays under it, collapsed.
    ErrorListEntry compiled = plain;
    compiled.Severity = ErrorListSeverity::Error;
    compiled.Description = QStringLiteral("Shader: #error: \"Implement Me!\"");
    compiled.Details = QStringLiteral("shader helper: Ludus shader build:\nshader.slang(75): error 15900: #error");
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {compiled});
    QCoreApplication::processEvents();
    CHECK(table->rowCount() == 1);
    CHECK(details->isVisible() == false);
    REQUIRE(toggle->isEnabled() == true);
    toggle->click();
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == true);
    CHECK(details->toPlainText().contains(QStringLiteral("shader.slang(75): error 15900")) == true);
    toggle->click();
    QCoreApplication::processEvents();
    CHECK(details->isVisible() == false);
    // A long single line is elided in its row; its tooltip keeps all of it.
    ErrorListEntry wide = plain;
    wide.Severity = ErrorListSeverity::Warning;
    wide.Description = QString(400, QLatin1Char('w'));
    list.ReplaceEntries(ErrorListProducer::RenderGraph, {wide});
    QCoreApplication::processEvents();
    CHECK(table->item(0, 2)->toolTip() == wide.Description);
}

TEST_CASE("a shader failure says each problem once: the log folds under one row and backends merge",
          "[editor][render-graph][diagnostics]")
{
    const QJsonObject location{{QStringLiteral("source"), QStringLiteral("shaders/closest_hit.slang")},
                               {QStringLiteral("line"), 75},
                               {QStringLiteral("column"), 12}};
    const auto mapped = [&location](const QString& target) {
        return QJsonObject{{QStringLiteral("severity"), QStringLiteral("error")},
                           {QStringLiteral("message"), QStringLiteral("#error: \"Implement Me!\"")},
                           {QStringLiteral("program"), QStringLiteral("GraphRayCompute")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("location"), location}};
    };
    RenderGraphJobResult result;
    result.Request.Action = QStringLiteral("build");
    result.Code = QStringLiteral("RenderGraphBuildShader");
    result.Message = QStringLiteral("shader helper: Ludus shader build:\nshader.slang(75): error 15900: #error");
    result.Location = location;
    result.Diagnostics = QJsonArray{mapped(QStringLiteral("metal")), mapped(QStringLiteral("spirv"))};
    REQUIRE(result.GetDiagnosticCount() == 3);
    const QList<ErrorListEntry> entries = MakeRenderGraphErrorEntries(result);
    REQUIRE(entries.size() == 1);
    CHECK(entries.first().Count == 2);
    CHECK(entries.first().Backends == QStringList{QStringLiteral("metal"), QStringLiteral("spirv")});
    CHECK(entries.first().Details == result.Message);
    CHECK(entries.first().Location == QStringLiteral("75:12"));
    CHECK(entries.first().Description == QStringLiteral("GraphRayCompute: #error: \"Implement Me!\""));
    // The first mapped diagnostic is the row ActivateDiagnostic maps back to the source.
    CHECK(entries.first().SourceIndex == 1);
    ErrorList list;
    list.ReplaceEntries(ErrorListProducer::RenderGraph, entries);
    CHECK(list.GetCount(ErrorListSeverity::Error) == 1);
    QTableWidget* table = list.findChild<QTableWidget*>();
    REQUIRE(table != nullptr);
    CHECK(table->rowCount() == 1);
    CHECK(table->item(0, 2)->text().startsWith(QStringLiteral("\u00d72")) == true);
    CHECK(table->item(0, 4)->text() == QStringLiteral("75:12"));
    CHECK(table->item(0, 5)->text() == QStringLiteral("Shader \u00b7 metal, spirv"));
    // Without mapped diagnostics the summary stays as the row, so nothing is lost.
    RenderGraphJobResult unmapped = result;
    unmapped.Diagnostics = {};
    CHECK(MakeRenderGraphErrorEntries(unmapped).size() == 1);
}

TEST_CASE("build diagnostics show a compact line and column", "[editor][diagnostics]")
{
    const QList<ErrorListEntry> entries =
        ParseBuildErrorEntries(QStringLiteral("game.cpp:42:7: error: expected ';'\nother.cpp:9: warning: unused\n"));
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(0).Location == QStringLiteral("42:7"));
    CHECK(entries.at(1).Location == QStringLiteral("9"));
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
        .Details = {},
        .Backends = {},
        .Count = 1,
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

TEST_CASE("A successful build lists its reasonless-failure warnings after its summary",
          "[editor][render-graph][diagnostics][render-graph-failures]")
{
    const QJsonObject warning{{QStringLiteral("severity"), QStringLiteral("warning")},
                              {QStringLiteral("code"), QStringLiteral("RenderGraphBuildReasonlessFailure")},
                              {QStringLiteral("message"), QStringLiteral("sets queries.HasFailed without a reason")},
                              {QStringLiteral("program"), QStringLiteral("Shade")},
                              {QStringLiteral("location"),
                               QJsonObject{{QStringLiteral("source"), QStringLiteral("main_shaders/shade.slang")},
                                           {QStringLiteral("line"), 113},
                                           {QStringLiteral("column"), 15},
                                           {QStringLiteral("length"), 16}}}};
    const QJsonObject response{{QStringLiteral("schema"), 1},
                               {QStringLiteral("build"),
                                QJsonObject{{QStringLiteral("generation"), QStringLiteral("g")},
                                            {QStringLiteral("diagnostics"), QJsonArray{warning}}}}};
    RenderGraphJobRequest request;
    request.Action = QStringLiteral("build");
    const RenderGraphJobResult parsed = RenderGraphJob::ParseOutput(request, QJsonDocument(response).toJson(), 0);
    REQUIRE(parsed.IsSuccessful == true);
    // Row 0 is the success summary; the warning follows it and keeps its authored location.
    REQUIRE(parsed.GetDiagnosticCount() == 2);
    CHECK(parsed.GetDiagnostic(0).isEmpty() == true);
    CHECK(parsed.GetDiagnostic(1) == warning);
    const QList<ErrorListEntry> entries = MakeRenderGraphErrorEntries(parsed);
    REQUIRE(entries.size() == 2);
    CHECK(entries.first().Severity == ErrorListSeverity::Message);
    CHECK(entries.last().Severity == ErrorListSeverity::Warning);
    CHECK(entries.last().Code == QStringLiteral("RenderGraphBuildReasonlessFailure"));
    CHECK(entries.last().File == QStringLiteral("main_shaders/shade.slang"));
    CHECK(entries.last().Location.startsWith(QStringLiteral("113:15")) == true);
    CHECK(entries.last().SourceIndex == 1);
    // A build without warnings keeps its single informational row.
    const RenderGraphJobResult clean = RenderGraphJob::ParseOutput(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("build"), QJsonObject{}}}).toJson(),
        0);
    CHECK(clean.GetDiagnosticCount() == 1);
    CHECK(MakeRenderGraphErrorEntries(clean).size() == 1);
}
