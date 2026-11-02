// Library captures of the Reference window (docs/architecture/render-graph-frame-capture.md, phase 5): which
// library outputs a capture can render, one build per generation and target, stale answers ignored and failures
// retried.
#include "internal/render_graph_library_capture.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QMap>
#include <QString>

using ludus::editor::LibraryCaptureState;
using ludus::editor::ListCaptureTargets;
using ludus::editor::RenderGraphCaptureTarget;
using ludus::editor::RenderGraphLibraryCapture;
using ludus::editor::RenderGraphSourceBytes;

TEST_CASE("Library capture targets are every ColorTexture output of every library configuration",
          "[editor][render-graph-capture]")
{
    RenderGraphSourceBytes files;
    files.insert(QStringLiteral("main.lurender.json"),
                 R"({"configurations": [{"id": "Default", "outputs": [{"id": "color", "type": "ColorTexture"}]}]})");
    files.insert(QStringLiteral("PointLineMis.lurender.json"),
                 R"({"configurations": [{"id": "PointLineMis", "outputs": [
                     {"id": "color", "type": "ColorTexture"},
                     {"id": "hits", "type": "Buffer"},
                     {"id": "lineSampling", "type": "ColorTexture"}]}]})");
    files.insert(QStringLiteral("Broken.lurender.json"), "not json");
    files.insert(QStringLiteral("main_shaders/shade.slang"), "// not a graph");
    const QVector<RenderGraphCaptureTarget> targets = ListCaptureTargets(files, QStringLiteral("main.lurender.json"));
    REQUIRE(targets.size() == 2);
    CHECK(targets.at(0) == RenderGraphCaptureTarget{QStringLiteral("PointLineMis.lurender.json"),
                                                    QStringLiteral("PointLineMis"),
                                                    QStringLiteral("color")});
    CHECK(targets.at(1).Output == QStringLiteral("lineSampling"));
    CHECK(targets.at(1).GetLabel() == QStringLiteral("PointLineMis.lurender.json#PointLineMis.lineSampling"));
}

TEST_CASE("A library capture builds once per generation and target and ignores stale answers",
          "[editor][render-graph-capture]")
{
    RenderGraphLibraryCapture capture;
    int requested = 0;
    RenderGraphCaptureTarget requestedTarget;
    QObject::connect(&capture,
                     &RenderGraphLibraryCapture::BuildRequested,
                     [&](const QByteArray&, const RenderGraphCaptureTarget& target) {
                         ++requested;
                         requestedTarget = target;
                     });
    const QByteArray generation("generation");
    const RenderGraphCaptureTarget color{QStringLiteral("PointLineMis.lurender.json"),
                                         QStringLiteral("PointLineMis"),
                                         QStringLiteral("color")};
    RenderGraphCaptureTarget lines = color;
    lines.Output = QStringLiteral("lineSampling");
    CHECK(capture.GetState() == LibraryCaptureState::Idle);
    capture.Request(generation, color);
    CHECK(capture.GetState() == LibraryCaptureState::Building);
    CHECK(requested == 1);
    capture.Request(generation, color);
    CHECK(requested == 1);
    // Another output of the same generation supersedes the build; the first answer is then ignored.
    capture.Request(generation, lines);
    CHECK(requested == 2);
    CHECK(requestedTarget == lines);
    capture.AcceptBuild(generation, color, QStringLiteral("color.lurgrt"), {});
    CHECK(capture.GetState() == LibraryCaptureState::Building);
    capture.AcceptBuild(generation, lines, QStringLiteral("lines.lurgrt"), QStringLiteral("lines.lurgcp"));
    CHECK(capture.GetState() == LibraryCaptureState::Ready);
    CHECK(capture.IsReadyFor(generation, lines) == true);
    CHECK(capture.IsReadyFor(generation, color) == false);
    CHECK(capture.IsReadyFor(QByteArray("newer"), lines) == false);
    CHECK(capture.GetPackagePath() == QStringLiteral("lines.lurgrt"));
    CHECK(capture.GetProfilePath() == QStringLiteral("lines.lurgcp"));
    // A Ready capture of the same pair is reused; a failed one is retried.
    capture.Request(generation, lines);
    CHECK(requested == 2);
    capture.Request(QByteArray("newer"), lines);
    capture.RejectBuild(QByteArray("newer"), lines, QStringLiteral("insert the library on the root canvas"));
    CHECK(capture.GetState() == LibraryCaptureState::Failed);
    CHECK(capture.GetError() == QStringLiteral("insert the library on the root canvas"));
    capture.Request(QByteArray("newer"), lines);
    CHECK(capture.GetState() == LibraryCaptureState::Building);
    CHECK(requested == 4);
    CHECK(capture.GetError().isEmpty() == true);
}
