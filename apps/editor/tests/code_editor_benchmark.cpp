// Keypress-to-handling baseline corpus for the code editor (M0).
//
// These [.benchmark] cases record the CURRENT editor's offscreen cost on the
// workloads named in the architecture performance table
// (docs/architecture/code-editor.md): real shaders, a 100,000-line C++ file, an
// 8 MiB JSON file, a single 1 MiB line, typing during diagnostics and a
// 100-cursor edit. Multiple cursors and live diagnostics do not exist before
// M2/M4, so the cursor and diagnostics cases measure the pre-feature baseline
// the later milestones must not regress, and say so in their INFO lines.
//
// Offscreen Qt event handling only: this excludes native input and compositor
// latency, so it is a relative baseline, not the keypress-to-paint budget. That
// budget is qualified by the native latency probe and interactive runs on the
// reference machines (M9). Raw numbers belong in ignored host output; record the
// summary in the task ledger.

#include "internal/code_editor.h"
#include "internal/code_syntax_highlighter.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QSysInfo>
#include <QTextCursor>
#include <QVector>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
/// The number of byte columns in one synthetic long line, about 1 MiB.
constexpr int32 LONG_LINE_BYTES = 1024 * 1024;
/// The target line count for the generated C++ corpus file.
constexpr int32 GENERATED_CPP_LINES = 100000;
/// The target byte size for the generated JSON corpus file, about 8 MiB.
constexpr int32 GENERATED_JSON_BYTES = 8 * 1024 * 1024;
/// The cursor count the architecture's 100-cursor workload names.
constexpr int32 CURSOR_CORPUS_COUNT = 100;
/// Timed samples per workload; p50 and p95 are reported from the sorted set.
constexpr int32 SAMPLE_COUNT = 20;

void FinishEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

void ReportSamples(const char* workload, int32 characters, int32 blocks, QVector<int64>& inoutSamples)
{
    std::sort(inoutSamples.begin(), inoutSamples.end());
    INFO("Workload: " << workload);
    INFO("Offscreen synthetic baseline; includes Qt event processing, excludes native input/compositor latency.");
    INFO("System: " << QSysInfo::prettyProductName().toStdString() << " / "
                     << QSysInfo::currentCpuArchitecture().toStdString() << "; Qt " << qVersion());
    INFO("Characters=" << characters << ", blocks=" << blocks
                       << ", edit nanoseconds p50=" << inoutSamples.at(inoutSamples.size() / 2)
                       << ", p95=" << inoutSamples.at(inoutSamples.size() - 1));
}

QString GenerateCppFile()
{
    QString source;
    const QString line = QStringLiteral("    const int value%1 = %1 * 2 + 1; // generated corpus line\n");
    for (int32 index = 0; index < GENERATED_CPP_LINES; ++index)
    {
        source += line.arg(index);
    }
    return source;
}

QString GenerateJsonFile()
{
    QString source = QStringLiteral("{\n  \"entries\": [\n");
    const QString entry = QStringLiteral("    { \"id\": %1, \"name\": \"entry_%1\", \"value\": %1.5 },\n");
    int32 index = 0;
    while (source.size() < GENERATED_JSON_BYTES)
    {
        source += entry.arg(index);
        ++index;
    }
    source += QStringLiteral("    { \"id\": -1, \"name\": \"tail\", \"value\": 0 }\n  ]\n}\n");
    return source;
}
} // namespace

TEST_CASE("corpus: real-shader typing baseline", "[.benchmark][editor][text][code-editor]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QString source;
    const QString line = QStringLiteral(
        "float3 EvaluateSun(float3 normalWorld, float3 lightWorld, float3 sunColor)\n"
        "{\n"
        "    float3 radiance = sunColor * max(dot(normalWorld, lightWorld), 0.0);\n"
        "    return radiance;\n"
        "}\n");
    for (int32 repeat = 0; repeat < 200; ++repeat)
    {
        source += line;
    }
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.GoToLine(editor.blockCount() / 2);
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    ReportSamples("real-shader typing", source.size(), editor.blockCount(), samples);
    CHECK(editor.toPlainText().compare(source) == 0);
}

TEST_CASE("corpus: 100,000-line C++ typing baseline", "[.benchmark][editor][text][code-editor]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::PlainText);
    const QString source = GenerateCppFile();
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.GoToLine(editor.blockCount() / 2);
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    ReportSamples("100,000-line C++ typing", source.size(), editor.blockCount(), samples);
}

TEST_CASE("corpus: 8 MiB JSON typing baseline", "[.benchmark][editor][text][code-editor]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::PlainText);
    const QString source = GenerateJsonFile();
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.GoToLine(editor.blockCount() / 2);
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    ReportSamples("8 MiB JSON typing", source.size(), editor.blockCount(), samples);
}

TEST_CASE("corpus: single 1 MiB line typing baseline", "[.benchmark][editor][text][code-editor]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::PlainText);
    const QString source(LONG_LINE_BYTES, QLatin1Char('x'));
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    INFO("Long-line layout is the measured Qt bottleneck; this workload is not qualified against the budget.");
    ReportSamples("single 1 MiB line typing", source.size(), editor.blockCount(), samples);
}

TEST_CASE("corpus: typing during diagnostics baseline", "[.benchmark][editor][text][diagnostics][code-editor]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QString source;
    const QString line = QStringLiteral("float4 Shade(float4 value) { return value; }\n");
    for (int32 repeat = 0; repeat < 2000; ++repeat)
    {
        source += line;
    }
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    QList<CodeDiagnostic> diagnostics;
    for (int32 index = 0; index < 200; ++index)
    {
        CodeDiagnostic diagnostic;
        diagnostic.Line = index * 10 + 1;
        diagnostic.Column = 1;
        diagnostic.Length = 6;
        diagnostic.Severity = CodeDiagnosticSeverity::Warning;
        diagnostic.Message = QStringLiteral("corpus analysis note");
        diagnostics.append(diagnostic);
    }
    editor.SetDiagnostics(diagnostics, true);
    editor.GoToLine(editor.blockCount() / 2);
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    ReportSamples("typing during diagnostics (build markers present)", source.size(), editor.blockCount(), samples);
}

TEST_CASE("corpus: multi-cursor pre-feature typing baseline", "[.benchmark][editor][text][code-editor]")
{
    // Multiple cursors arrive in M2. This records the current single-cursor cost
    // of applying N separate inserts at N positions, which the M2 one-edit-set
    // path must not regress below.
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::PlainText);
    QString source;
    const QString line = QStringLiteral("value = value + 1\n");
    for (int32 repeat = 0; repeat < CURSOR_CORPUS_COUNT * 4; ++repeat)
    {
        source += line;
    }
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    FinishEvents();
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        QTextCursor cursor(editor.document());
        cursor.beginEditBlock();
        for (int32 target = 0; target < CURSOR_CORPUS_COUNT; ++target)
        {
            cursor.movePosition(QTextCursor::Start);
            for (int32 step = 0; step < target; ++step)
            {
                cursor.movePosition(QTextCursor::NextBlock);
            }
            cursor.insertText(QStringLiteral("x"));
        }
        cursor.endEditBlock();
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    INFO("Pre-M2 baseline: " << CURSOR_CORPUS_COUNT << " sequential single-cursor inserts, not one edit set.");
    ReportSamples("100-position pre-feature edit", source.size(), editor.blockCount(), samples);
}
