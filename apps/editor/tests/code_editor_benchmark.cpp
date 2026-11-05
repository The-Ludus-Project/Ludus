// Keypress baseline corpus for the code editor (M0).
//
// These [.benchmark] cases record the CURRENT editor's offscreen cost on the
// workloads named in the architecture performance table
// (docs/architecture/code-editor.md): real shaders, a 100,000-line C++ file, an
// 8 MiB JSON file, a single 1 MiB line, typing during diagnostics and a
// 100-cursor edit. Multiple cursors and live diagnostics do not exist before
// M2/M4, so the cursor and diagnostics cases measure the pre-feature baseline
// the later milestones must not regress, and say so in their output.
//
// Each sample sends a real key event through CodeEditor::keyPressEvent and then
// drains the event loop, so "handling" covers key handling, the document change,
// relayout and the offscreen paint. The view's CodeLatencyProbe separately
// reports keypress-to-paint from the end of key handling to the paint that shows
// the edit. Offscreen Qt only: this excludes native input and compositor
// latency, so it is a relative baseline, not the reference-machine budget, which
// M9 qualifies with native runs. Run with:
//   QT_QPA_PLATFORM=offscreen ludus_code_editor_tests "[.benchmark]"
// Each case prints one WARN summary; record those summaries in the task ledger.

#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/code_file_document.h"
#include "internal/code_file_limits.h"
#include "internal/code_latency_probe.h"
#include "internal/code_multi_cursor.h"
#include "internal/code_syntax_highlighter.h"
#include "internal/code_text_profile.h"

#include <ludus/foundation/containers/sort.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QString>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
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
/// Timed samples per workload; enough for a nearest-rank p95 that is not the maximum.
constexpr int32 SAMPLE_COUNT = 100;
/// The percentiles each summary reports.
constexpr int32 MEDIAN_PERCENTILE = 50;
constexpr int32 TAIL_PERCENTILE = 95;
constexpr int32 PERCENT_MAX = 100;
/// The editor viewport size used by every workload, in pixels.
constexpr int32 VIEW_WIDTH = 800;
constexpr int32 VIEW_HEIGHT = 600;
/// Nanoseconds per microsecond, for readable summaries.
constexpr int64 NANOSECONDS_PER_MICROSECOND = 1000;

void FinishEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// Nearest-rank percentile of a sorted, nonempty sample set, in the sample unit.
int64 GetNearestRankPercentile(const QVector<int64>& sorted, int32 percentile)
{
    const int32 count = static_cast<int32>(sorted.size());
    int32 rank = (percentile * count + PERCENT_MAX - 1) / PERCENT_MAX;
    if (rank < 1)
    {
        rank = 1;
    }
    return sorted.at(rank - 1);
}

void ReportSamples(const char* workload,
                   const QString& note,
                   int32 characters,
                   int32 blocks,
                   QVector<int64>& inoutHandlingSamples,
                   const CodeLatencyProbe& probe)
{
    Sort(inoutHandlingSamples.data(),
         static_cast<usize>(inoutHandlingSamples.size()),
         [](int64 left, int64 right) noexcept { return left < right; });
    const QString summary =
        QStringLiteral("BASELINE | %1 | chars=%2 blocks=%3 samples=%4 | handling us p50=%5 p95=%6 max=%7 | "
                       "key-to-paint us p50=%8 p95=%9 (paint samples=%10) | %11 / %12; Qt %13%14")
            .arg(QString::fromUtf8(workload))
            .arg(characters)
            .arg(blocks)
            .arg(inoutHandlingSamples.size())
            .arg(GetNearestRankPercentile(inoutHandlingSamples, MEDIAN_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(GetNearestRankPercentile(inoutHandlingSamples, TAIL_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(inoutHandlingSamples.last() / NANOSECONDS_PER_MICROSECOND)
            .arg(probe.GetPercentileNanoseconds(MEDIAN_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(probe.GetPercentileNanoseconds(TAIL_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(probe.GetSampleCount())
            .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QString::fromUtf8(qVersion()))
            .arg(note.isEmpty() == true ? QString() : QStringLiteral(" | ") + note);
    WARN(summary.toStdString());
}

/// Show the editor with the source, put the caret at the end of the middle line and settle layout.
void PrepareEditor(CodeEditor& inoutEditor, CodeLanguage language, const QString& source)
{
    inoutEditor.SetLanguage(language);
    inoutEditor.resize(VIEW_WIDTH, VIEW_HEIGHT);
    inoutEditor.setPlainText(source);
    inoutEditor.show();
    inoutEditor.GoToLine(inoutEditor.blockCount() / 2);
    inoutEditor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
}

/// Type one character per sample through keyPressEvent, undoing it between samples.
void MeasureTyping(CodeEditor& inoutEditor, const char* workload, const QString& note, const QString& source)
{
    CodeLatencyProbe& probe = inoutEditor.GetLatencyProbe();
    probe.Start();
    QVector<int64> samples;
    samples.reserve(SAMPLE_COUNT);
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(&inoutEditor, Qt::Key_X);
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        inoutEditor.undo();
        FinishEvents();
    }
    probe.Stop();
    ReportSamples(workload, note, static_cast<int32>(source.size()), inoutEditor.blockCount(), samples, probe);
    CHECK(inoutEditor.toPlainText().compare(source) == 0);
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
    constexpr int32 SHADER_REPEATS = 200;
    QString source;
    const QString function =
        QStringLiteral("float3 EvaluateSun(float3 normalWorld, float3 lightWorld, float3 sunColor)\n"
                       "{\n"
                       "    float3 radiance = sunColor * max(dot(normalWorld, lightWorld), 0.0);\n"
                       "    return radiance;\n"
                       "}\n");
    for (int32 repeat = 0; repeat < SHADER_REPEATS; ++repeat)
    {
        source += function;
    }
    CodeEditor editor;
    PrepareEditor(editor, CodeLanguage::Slang, source);
    MeasureTyping(editor, "real-shader typing (Slang lexical highlighter)", QString(), source);
}

TEST_CASE("corpus: 100,000-line C++ typing baseline", "[.benchmark][editor][text][code-editor]")
{
    const QString source = GenerateCppFile();
    CodeEditor editor;
    PrepareEditor(editor, CodeLanguage::PlainText, source);
    MeasureTyping(editor,
                  "100,000-line C++ typing",
                  QStringLiteral("plain text: no C++ highlighter before M3"),
                  source);
}

TEST_CASE("corpus: 8 MiB JSON typing baseline", "[.benchmark][editor][text][code-editor]")
{
    const QString source = GenerateJsonFile();
    CodeEditor editor;
    PrepareEditor(editor, CodeLanguage::PlainText, source);
    MeasureTyping(editor, "8 MiB JSON typing", QStringLiteral("plain text: no JSON highlighter before M3"), source);
}

TEST_CASE("corpus: single 1 MiB line typing baseline", "[.benchmark][editor][text][code-editor]")
{
    const QString source(LONG_LINE_BYTES, QLatin1Char('x'));
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::PlainText);
    editor.resize(VIEW_WIDTH, VIEW_HEIGHT);
    editor.setPlainText(source);
    editor.show();
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishEvents();
    MeasureTyping(editor,
                  "single 1 MiB line typing",
                  QStringLiteral("long-line layout is the known Qt bottleneck; not qualified against the budget"),
                  source);
}

TEST_CASE("corpus: typing during diagnostics baseline", "[.benchmark][editor][text][diagnostics][code-editor]")
{
    constexpr int32 SHADER_LINES = 2000;
    constexpr int32 DIAGNOSTIC_COUNT = 200;
    constexpr int32 DIAGNOSTIC_LINE_STRIDE = 10;
    constexpr int32 DIAGNOSTIC_LENGTH = 6;
    QString source;
    const QString line = QStringLiteral("float4 Shade(float4 value) { return value; }\n");
    for (int32 repeat = 0; repeat < SHADER_LINES; ++repeat)
    {
        source += line;
    }
    CodeEditor editor;
    PrepareEditor(editor, CodeLanguage::Slang, source);
    QList<CodeDiagnostic> diagnostics;
    for (int32 index = 0; index < DIAGNOSTIC_COUNT; ++index)
    {
        CodeDiagnostic diagnostic;
        diagnostic.Line = index * DIAGNOSTIC_LINE_STRIDE + 1;
        diagnostic.Column = 1;
        diagnostic.Length = DIAGNOSTIC_LENGTH;
        diagnostic.Severity = CodeDiagnosticSeverity::Warning;
        diagnostic.Message = QStringLiteral("corpus analysis note");
        diagnostics.append(diagnostic);
    }
    editor.SetDiagnostics(diagnostics, true);
    FinishEvents();
    MeasureTyping(editor,
                  "typing during diagnostics",
                  QStringLiteral("200 static build markers; no live analysis before M4"),
                  source);
}

TEST_CASE("corpus: multi-cursor pre-feature typing baseline", "[.benchmark][editor][text][code-editor]")
{
    // Multiple cursors arrive in M2. This records the current cost of applying
    // N separate inserts at N line starts in one edit block, which the M2
    // one-edit-set path must not regress below.
    constexpr int32 LINES_PER_CURSOR = 4;
    QString source;
    const QString line = QStringLiteral("value = value + 1\n");
    for (int32 repeat = 0; repeat < CURSOR_CORPUS_COUNT * LINES_PER_CURSOR; ++repeat)
    {
        source += line;
    }
    CodeEditor editor;
    PrepareEditor(editor, CodeLanguage::PlainText, source);
    QVector<int64> samples;
    samples.reserve(SAMPLE_COUNT);
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        QTextCursor cursor(editor.document());
        cursor.beginEditBlock();
        for (int32 target = 0; target < CURSOR_CORPUS_COUNT; ++target)
        {
            cursor.setPosition(editor.document()->findBlockByNumber(target * LINES_PER_CURSOR).position());
            cursor.insertText(QStringLiteral("x"));
        }
        cursor.endEditBlock();
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishEvents();
    }
    const CodeLatencyProbe idleProbe;
    ReportSamples("100-position edit block",
                  QStringLiteral("pre-M2: 100 programmatic inserts, no key events"),
                  static_cast<int32>(source.size()),
                  editor.blockCount(),
                  samples,
                  idleProbe);
    CHECK(editor.toPlainText().compare(source) == 0);
}

// Measure real key-event typing with `cursorCount` cursors (1 plus the
// secondaries) through the M2 multi-cursor edit-set path: a CodeFileDocument
// host bound through a CodeDocumentController, so a keystroke is distributed as
// one CodeEditSet and the view re-synchronizes, exactly as in the editor. This
// is the M2 typing budget the gate records (1-cursor and 100-cursor).
void MeasureMultiCursorTyping(const char* workload, int32 cursorCount)
{
    constexpr int32 LINES_PER_CURSOR = 4;
    QByteArray source;
    const QByteArray line("value = value + 1\n");
    for (int32 repeat = 0; repeat < cursorCount * LINES_PER_CURSOR; ++repeat)
    {
        source.append(line);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = QDir(directory.path()).filePath(QStringLiteral("budget.txt"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(source) == source.size());
    file.close();

    CodeFileDocument host(LoadCodeFile(path), 1);
    CodeEditor editor;
    CodeDocumentController controller(editor);
    editor.resize(VIEW_WIDTH, VIEW_HEIGHT);
    editor.show();
    controller.Bind(&host);
    FinishEvents();

    // A caret at the start of every fourth line: the primary on the first, a
    // secondary on each of the rest (line N starts at N * line length).
    QTextCursor primary(editor.document());
    primary.setPosition(0);
    editor.setTextCursor(primary);
    QList<CodeSecondaryCursor> cursors;
    cursors.reserve(cursorCount - 1);
    for (int32 index = 1; index < cursorCount; ++index)
    {
        const int32 position = index * LINES_PER_CURSOR * static_cast<int32>(line.size());
        cursors.append(CodeSecondaryCursor{ .Anchor = position, .Caret = position });
    }
    editor.GetMultiCursor().AdoptSecondaryCursors(cursors);
    REQUIRE(editor.GetMultiCursor().GetCursorCount() == cursorCount);
    FinishEvents();

    CodeLatencyProbe& probe = editor.GetLatencyProbe();
    probe.Start();
    QVector<int64> samples;
    samples.reserve(SAMPLE_COUNT);
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(&editor, Qt::Key_X);
        FinishEvents();
        samples.append(timer.nsecsElapsed());
        QList<CodeTextSelection> undone;
        static_cast<void>(host.Undo(undone));
        controller.Synchronize();
        editor.GetMultiCursor().AdoptSecondaryCursors(cursors);
        FinishEvents();
    }
    probe.Stop();
    ReportSamples(workload,
                  QStringLiteral("M2 multi-cursor edit-set path, %1 cursors").arg(cursorCount),
                  static_cast<int32>(source.size()),
                  editor.blockCount(),
                  samples,
                  probe);
}

TEST_CASE("corpus: 1-cursor typing through the edit-set path", "[.benchmark][editor][text][code-editor]")
{
    // The single-cursor budget: one key, distributed through the same edit-set
    // path the 100-cursor case uses, so the two are directly comparable.
    MeasureMultiCursorTyping("1-cursor typing (edit-set path)", 1);
}

TEST_CASE("corpus: 100-cursor typing through the edit-set path", "[.benchmark][editor][text][code-editor]")
{
    // The 100-cursor budget (architecture's 8 ms 100-cursor workload), now
    // measured through the real multi-cursor path M2 added, not a programmatic
    // edit block.
    MeasureMultiCursorTyping("100-cursor typing (edit-set path)", CURSOR_CORPUS_COUNT);
}

TEST_CASE("corpus: long-line file opened through the file host", "[.benchmark][editor][text][code-editor][file-host]")
{
    // The M0 baseline measured a single 1 MiB line at about 1 s per keystroke
    // because Qt lays out the whole block. The file document host must keep such
    // files off that path: a line wider than CODE_LONG_LINE_UTF16_UNITS gets no
    // per-line formats or bracket matching (CE22). This case opens a long-line
    // file through the host and records how long load + detection + profiling
    // take, so the degraded mode's open cost is comparable with the "single
    // 1 MiB line typing" baseline above. Typing in the degraded mode is still a
    // Qt layout cost until the M2/M3 view mitigation lands; that is why this
    // case times the host open path, not per-keystroke layout.
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QByteArray bytes(LONG_LINE_BYTES, 'x');
    bytes.append('\n');
    const QString path = QDir(directory.path()).filePath(QStringLiteral("long_line.txt"));
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(bytes) == bytes.size());
    }

    QVector<int64> samples;
    samples.reserve(SAMPLE_COUNT);
    bool isWithinLongLineThreshold = false;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        const CodeFileLoadResult result = LoadCodeFile(path);
        samples.append(timer.nsecsElapsed());
        // The single long line is within the normal byte band, so the host opens
        // it editable; the long-line view mitigation (CE22) is what the degraded
        // mode uses, and the threshold constant keeps per-line work off it.
        isWithinLongLineThreshold =
            static_cast<usize>(LONG_LINE_BYTES) > CODE_LONG_LINE_UTF16_UNITS && result.Status == CodeFileLoadStatus::Ok;
    }
    CHECK(isWithinLongLineThreshold == true);
    Sort(samples.data(), static_cast<usize>(samples.size()), [](int64 left, int64 right) noexcept {
        return left < right;
    });
    const QString summary =
        QStringLiteral("BASELINE | long-line file host open | bytes=%1 samples=%2 | open us p50=%3 p95=%4 max=%5 | "
                       "long-line threshold=%6 UTF-16 units | %7 / %8; Qt %9 | compare with 'single 1 MiB line typing'")
            .arg(bytes.size())
            .arg(samples.size())
            .arg(GetNearestRankPercentile(samples, MEDIAN_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(GetNearestRankPercentile(samples, TAIL_PERCENTILE) / NANOSECONDS_PER_MICROSECOND)
            .arg(samples.last() / NANOSECONDS_PER_MICROSECOND)
            .arg(CODE_LONG_LINE_UTF16_UNITS)
            .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QString::fromUtf8(qVersion()));
    WARN(summary.toStdString());
}
