#include "internal/code_assistance.h"
#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/render_graph_code_document.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_shader_assistance.h"

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

#if !defined(Q_OS_WASM)
TEST_CASE("Shader context preparation sees the unsaved package and reports missing slangd locally",
          "[editor][render-graph][assistance]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    const QString package = temporary.filePath(QStringLiteral("package"));
    const QString sourceFile = QStringLiteral("main_shaders/color.slang");
    const QDir fixture(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {QStringLiteral("main.lurender.json"),
                                QStringLiteral("rasterize.lurender.json"),
                                QStringLiteral("main.lushader.json.in"),
                                QStringLiteral("main_shaders/geometry.slang"),
                                sourceFile,
                                QStringLiteral("main_shaders/depth.slang")})
    {
        QFile input(fixture.filePath(file));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QByteArray data = input.readAll();
        data.replace("@RG2_TEST_PROFILES@", "[\"metal\"]");
        const QString destination =
            QDir(package).filePath(file.endsWith(QStringLiteral(".in")) == true ? file.chopped(3) : file);
        REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
        QFile output(destination);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(data) == data.size());
    }
    const QString helper = temporary.filePath(QStringLiteral("helper"));
    REQUIRE(QDir().mkpath(helper) == true);
    REQUIRE(QFile::copy(
                QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/modules/graphics/renderer/shaders/visibility_buffer.slang"),
                QDir(helper).filePath(QStringLiteral("visibility_buffer.slang"))) == true);
    RenderGraphDocument graph;
    REQUIRE(graph.Load(package, QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    const QByteArray saved = graph.GetDraft(sourceFile);
    const QByteArray draft = saved + "\n// unsaved API request\n";
    REQUIRE(graph.SetText(sourceFile, draft) == RenderGraphDocumentStatus::Ok);
    RenderGraphCodeDocument host(graph);
    host.Bind(sourceFile, 1);
    CodeEditor editor;
    CodeDocumentController controller(editor);
    controller.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    const ToolingPaths tooling{QString::fromUtf8(LUDUS_TEST_PYTHON), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)};
    RenderGraphShaderAssistance assistance(graph, editor, controller, tooling);
    RenderGraphBuildSettings settings;
    settings.ShouldOverrideTools = true;
    settings.Tools.Helper = helper;
    settings.Tools.Compiler = QDir(helper).filePath(QStringLiteral("slangc"));
    assistance.Update(temporary.path(), 1, QStringLiteral("main.lushader.json"), sourceFile, settings);
    Q_EMIT editor.AssistanceRequested(false);
    // Cancel a captured request across a project epoch change before accepting
    // another result from the same unchanged source bytes.
    assistance.Update(temporary.path(), 2, QStringLiteral("main.lushader.json"), sourceFile, settings);
    host.Bind(sourceFile, 2);
    controller.Synchronize();
    RenderGraphJob* jobOrNull = assistance.findChild<RenderGraphJob*>();
    REQUIRE(jobOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([jobOrNull]() { return jobOrNull->IsActive() == false; }, 15000));
    QComboBox* contextsOrNull = assistance.findChild<QComboBox*>(QStringLiteral("shaderProgramContext"));
    REQUIRE(contextsOrNull != nullptr);
    REQUIRE(contextsOrNull->count() == 0);
    Q_EMIT editor.AssistanceRequested(false);
    QLabel* statusOrNull = assistance.findChild<QLabel*>(QStringLiteral("shaderAssistanceStatus"));
    REQUIRE(statusOrNull != nullptr);
    REQUIRE(
        QTest::qWaitFor([statusOrNull]() { return statusOrNull->text().contains(QStringLiteral("slangd")); }, 15000));
    REQUIRE(contextsOrNull->count() > 0);
    REQUIRE(contextsOrNull->currentText().contains(QStringLiteral("Color")));
    REQUIRE(contextsOrNull->toolTip().contains(QStringLiteral("main.lushader.json")));
    REQUIRE(graph.GetDraft(sourceFile) == draft);
    QFile disk(QDir(package).filePath(sourceFile));
    REQUIRE(disk.open(QIODevice::ReadOnly) == true);
    REQUIRE(disk.readAll() == saved);
}
#endif
