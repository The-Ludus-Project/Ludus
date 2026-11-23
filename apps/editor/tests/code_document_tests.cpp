#include "internal/code_assistance.h"
#include "internal/code_document_controller.h"
#include "internal/code_edit_scope.h"
#include "internal/code_edit_set.h"
#include "internal/code_editor_panel.h"
#include "internal/code_formatter.h"
#include "internal/code_language_registry.h"
#include "internal/code_navigation.h"
#include "internal/code_snippet.h"
#include "internal/code_structure_view.h"
#include "internal/code_syntax_service.h"
#include "internal/code_text.h"
#include "internal/document_history.h"
#include "internal/lusl_formatting.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
#if !defined(Q_OS_WASM)
#    if defined(Q_OS_MACOS)
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::MetaModifier;
#    else
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::ControlModifier;
#    endif
#endif
struct MemoryTextState final
{
    QByteArray Text;
    CodeTextSelection Selection;
    bool operator==(const MemoryTextState& other) const
    {
        return Text == other.Text;
    }
};

// Independent single-file document owner: the shared editor has no dependency
// on the render graph's schema, history payload, files or shader compiler.
class MemoryCodeDocument final : public CodeDocumentHost
{
public:
    explicit MemoryCodeDocument(const QByteArray& text)
    {
        mState.Text = text;
    }
    CodeDocumentSnapshot GetSnapshot() const noexcept override
    {
        return {{1, Session, mHistory.Revision()},
                {mState.Text.constData(), static_cast<usize>(mState.Text.size())},
                LimitBytes,
                true};
    }
    CodeEditStatus ApplyEdits(const CodeDocumentIdentity& expected, const CodeEditSet& edits, bool shouldMerge) override
    {
        if (expected != GetSnapshot().Identity)
        {
            return CodeEditStatus::Conflict;
        }
        const BorrowedView<const char> original{mState.Text.constData(), static_cast<usize>(mState.Text.size())};
        QByteArray candidate;
        const CodeEditStatus status = ValidateAndApplyEditSet(original, LimitBytes, edits, candidate);
        if (status != CodeEditStatus::Ok)
        {
            return status;
        }
        if (candidate == mState.Text)
        {
            return CodeEditStatus::Ok;
        }
        const CodeTextSelection before =
            edits.Before.Selections.IsEmpty() == false ? edits.Before.Selections[0] : CodeTextSelection{};
        const CodeTextSelection after =
            edits.After.Selections.IsEmpty() == false ? edits.After.Selections[0] : CodeTextSelection{};
        MemoryTextState previous = mState;
        previous.Selection = before;
        const MemoryTextState next{candidate, after};
        if (mHistory.Commit(previous, next, shouldMerge) == false)
        {
            return CodeEditStatus::Conflict;
        }
        mState = next;
        return CodeEditStatus::Ok;
    }
    void BreakUndoGroup() noexcept override
    {
        mHistory.BreakGroup();
    }
    bool CanUndo() const noexcept override
    {
        return mHistory.CanUndo();
    }
    bool CanRedo() const noexcept override
    {
        return mHistory.CanRedo();
    }
    bool Undo()
    {
        return mHistory.Undo(mState);
    }
    bool Redo()
    {
        return mHistory.Redo(mState);
    }
    const MemoryTextState& GetState() const
    {
        return mState;
    }
    usize LimitBytes = 4096;
    uint64 Session = 1;

private:
    MemoryTextState mState;
    DocumentHistory<MemoryTextState> mHistory;
};

} // namespace

TEST_CASE("owned text history coalesces typing and routes undo without a Qt stack", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("prefix "));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QObject::connect(&editor, &CodeEditor::UndoRequested, &controller, [&]() {
        if (document.Undo() == true)
        {
            controller.Synchronize();
            controller.RestoreSelection(document.GetState().Selection);
        }
    });
    QObject::connect(&editor, &CodeEditor::RedoRequested, &controller, [&]() {
        if (document.Redo() == true)
        {
            controller.Synchronize();
            controller.RestoreSelection(document.GetState().Selection);
        }
    });
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("word"));
    CHECK(document.GetState().Text == QByteArrayLiteral("prefix word"));
    CHECK(document.GetSnapshot().Identity.Revision == 4);
    CHECK(editor.document()->isUndoRedoEnabled() == false);
    CHECK(editor.document()->isUndoAvailable() == false);
    CHECK(editor.CanUndo() == true);
    editor.Undo();
    CHECK(document.GetState().Text == QByteArrayLiteral("prefix "));
    CHECK(editor.toPlainText() == QStringLiteral("prefix "));
    CHECK(editor.textCursor().position() == 7);
    CHECK(editor.CanRedo() == true);
    editor.Redo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word"));
    CHECK(editor.textCursor().position() == 11);
    QTest::keyClicks(&editor, QStringLiteral(" next"));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word "));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word"));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix "));
}

TEST_CASE("multi-line commands are one authoritative history entry", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("one\ntwo\nthree"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    editor.selectAll();
    editor.IndentSelection();
    CHECK(document.GetState().Text == QByteArrayLiteral("    one\n    two\n    three"));
    CHECK(document.GetSnapshot().Identity.Revision == 1);
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(editor.toPlainText() == QStringLiteral("one\ntwo\nthree"));
    CHECK(document.CanUndo() == false);
}

TEST_CASE("replacing the whole text keeps the inserted text in the bound document", "[editor][text][history]")
{
    // setPlainText reports its clear and its insertion at one document revision; both must reach the owner.
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("float3 Red() { return float3(1, 0, 0); }"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    REQUIRE(editor.toPlainText() == QStringLiteral("float3 Red() { return float3(1, 0, 0); }"));
    editor.setPlainText(QStringLiteral("float3 Blue() { return float3(0, 0, 1); }"));
    CHECK(document.GetState().Text == QByteArrayLiteral("float3 Blue() { return float3(0, 0, 1); }"));
    CHECK(editor.toPlainText() == QStringLiteral("float3 Blue() { return float3(0, 0, 1); }"));
    // The replacement is one edit: a single undo restores the previous text, not an empty document.
    REQUIRE(document.Undo() == true);
    CHECK(document.GetState().Text == QByteArrayLiteral("float3 Red() { return float3(1, 0, 0); }"));
    CHECK(document.CanUndo() == false);
}

TEST_CASE("syntax reapply survives the document being replaced before its next slice", "[editor][text][syntax]")
{
    // A pending reapply resumes from a block of the replaced text; it must look the block up again.
    CodeEditor editor;
    QString longSource;
    constexpr int32 SOURCE_LINE_COUNT = 2000;
    for (int32 line = 0; line < SOURCE_LINE_COUNT; ++line)
    {
        longSource += QStringLiteral("float value%1 = %1.0;\n").arg(line);
    }
    editor.setPlainText(longSource);
    editor.SetLanguage(CodeLanguage::Slang);
    QCoreApplication::processEvents();
    editor.SetLanguage(CodeLanguage::PlainText);
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("float shorter = 1.0;"));
    QTest::qWait(0);
    QTest::qWait(0);
    CHECK(editor.toPlainText() == QStringLiteral("float shorter = 1.0;"));
}

TEST_CASE("Unicode input and CRLF survive document history and formatting", "[editor][text][history]")
{
    CodeEditor editor;
    const QByteArray original = QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\nnext").toUtf8();
    MemoryCodeDocument document(original);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QSignalSpy errors(&controller, &CodeDocumentController::Failed);
    CHECK(editor.toPlainText().contains(QString::fromUtf8("文字😀\nnext")) == true);
    editor.GoToLine(2);
    QInputMethodEvent preedit(QStringLiteral("候"), {});
    QApplication::sendEvent(&editor, &preedit);
    CHECK(document.GetSnapshot().Identity.Revision == 0);
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("新😀"));
    QApplication::sendEvent(&editor, &commit);
    CHECK(document.GetState().Text == QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\n新😀next").toUtf8());
    CHECK(errors.count() == 0);
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == original);
    editor.GoToLine(1);
    editor.moveCursor(QTextCursor::EndOfBlock);
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(document.GetState().Text == QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\n\r\nnext").toUtf8());
}

TEST_CASE("Shift Return writes a source newline with the document's line ending", "[editor][text][history]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QByteArray lineEnding = QByteArrayLiteral("\n");
    SECTION("LF") {}
    SECTION("CRLF")
    {
        lineEnding = QByteArrayLiteral("\r\n");
    }
    const QByteArray firstLine = QByteArrayLiteral("void SamplePointOnEdges()");
    const QByteArray original = firstLine + "{}" + lineEnding;
    MemoryCodeDocument document(original);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(static_cast<int32>(firstLine.size()));
    editor.setTextCursor(cursor);

    QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);

    CHECK(document.GetState().Text == firstLine + lineEnding + "{}" + lineEnding);
    CHECK(editor.document()->findBlockByNumber(1).text() == QStringLiteral("{}"));
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == original);
    CHECK(editor.textCursor().blockNumber() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("opening a Slang draft repairs code line separators as one undo step", "[editor][text][history]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString prefix = QStringLiteral("\ufeffString text = \"left\\\"\u2028right\";\r\n"
                                          "String raw = R\"tag(left\"\r\n\u2028right)tag\";\r\n"
                                          "/* block \u2028 comment */\r\n"
                                          "// continued \\\r\n\u2028comment\r\n");
    const QString signature = QStringLiteral("void SamplePointOnEdges()");
    const QByteArray original = (prefix + signature + QStringLiteral("\u2028{}\r\n")).toUtf8();
    const QByteArray repaired = (prefix + signature + QStringLiteral("\r\n{}\r\n")).toUtf8();
    MemoryCodeDocument document(original);
    CodeDocumentController controller(editor);
    QSignalSpy accepted(&controller, &CodeDocumentController::TextAccepted);
    controller.Bind(&document);
    editor.moveCursor(QTextCursor::End);
    const int32 position = editor.textCursor().position();
    QCoreApplication::processEvents();

    CHECK(document.GetState().Text == repaired);
    CHECK(editor.textCursor().position() == position);
    CHECK(accepted.count() == 1);
    REQUIRE(document.Undo() == true);
    controller.Synchronize(true);
    QCoreApplication::processEvents();
    CHECK(document.GetState().Text == original);
    CHECK(editor.document()->toRawText().contains(QChar::LineSeparator) == true);
    CHECK(accepted.count() == 1);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("a queued Slang line repair skips a tab that has rebound", "[editor][text][history]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QByteArray original = QStringLiteral("void Original()\u2028{}").toUtf8();
    MemoryCodeDocument originalDocument(original);
    MemoryCodeDocument currentDocument(QByteArrayLiteral("void Current() {}"));
    CodeDocumentController controller(editor);
    controller.Bind(&originalDocument);
    controller.Bind(&currentDocument);
    QCoreApplication::processEvents();

    CHECK(originalDocument.GetState().Text == original);
    CHECK(currentDocument.GetState().Text == QByteArrayLiteral("void Current() {}"));
    CHECK(currentDocument.CanUndo() == false);
}

TEST_CASE("Slang paste repairs code separators and keeps separators in strings", "[editor][text][history]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QByteArray original = QByteArrayLiteral("void SamplePointOnEdges(){}\r\n");
    MemoryCodeDocument document(original);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(static_cast<int32>(original.indexOf('{')));
    editor.setTextCursor(cursor);
    QApplication::clipboard()->setText(QStringLiteral("\u2028{ String label = \"left\u2028right\"; }\u2028"));
    editor.paste();

    CHECK(document.GetState().Text ==
          QStringLiteral("void SamplePointOnEdges()\r\n{ String label = \"left\u2028right\"; }\r\n{}\r\n").toUtf8());
    CHECK(editor.textCursor().blockNumber() == 2);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetState().Text == original);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("rejected text input preserves owner and view without an undo entry", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("ab"));
    document.LimitBytes = 3;
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QSignalSpy errors(&controller, &CodeDocumentController::Failed);
    editor.selectAll();
    QApplication::clipboard()->setText(QStringLiteral("oversized"));
    editor.paste();
    CHECK(document.GetState().Text == QByteArrayLiteral("ab"));
    CHECK(editor.toPlainText() == QStringLiteral("ab"));
    CHECK(document.CanUndo() == false);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("ab"));
    CHECK(errors.count() == 1);
}

TEST_CASE("stale formatting cannot replace current text or cross a binding", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("text"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    const CodeTextCapture captured = controller.Capture();
    QTest::keyClicks(&editor, QStringLiteral("new"));
    const QByteArray edited = document.GetState().Text;
    CHECK(controller.ApplyServiceText(captured.Identity, QByteArrayLiteral("formatted"), {}) ==
          CodeEditStatus::Conflict);
    CHECK(document.GetState().Text == edited);
    const CodeTextCapture oldBinding = controller.Capture();
    ++document.Session;
    controller.Synchronize();
    CHECK(controller.ApplyServiceText(oldBinding.Identity, QByteArrayLiteral("formatted"), {}) ==
          CodeEditStatus::Conflict);
    CHECK(document.GetState().Text == edited);
}

TEST_CASE("UTF-8 positions distinguish bytes UTF-16 surrogates BOM and CRLF", "[editor][text][positions]")
{
    const QByteArray text = QString::fromUtf8("a😀\r\n文字").toUtf8();
    usize byte = 99;
    REQUIRE(GetCodeByteOffset(text, 3, byte) == true);
    CHECK(byte == 5);
    REQUIRE(GetCodeByteOffset(text, 4, byte) == true);
    CHECK(byte == 7);
    CHECK(GetCodeByteOffset(text, 2, byte) == false);
    CHECK(byte == 7);
    int32 position = 99;
    CHECK(GetCodeCursorPosition(text, 6, position) == false);
    CHECK(position == 99);
    REQUIRE(GetCodeCursorPosition(text, 7, position) == true);
    CHECK(position == 4);
    const QByteArray before = QString::fromUtf8("a😀z").toUtf8();
    const QByteArray after = QString::fromUtf8("a😁z").toUtf8();
    const CodeTextEdit edit = MakeCodeTextEdit(before, after);
    CHECK(edit.StartBytes == 1);
    CHECK(edit.RemovedBytes == 4);
    CHECK(edit.Inserted.GetSize() == 4);
}

#if !defined(Q_OS_WASM)
TEST_CASE("formatter applies one history group and preserves Unicode selection and CRLF", "[editor][text][format]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString program = FindShaderFormatter();
    if (program.isEmpty() == true)
    {
        SKIP("Pinned native clang-format is unavailable");
    }
    QFile style(QDir(directory.path()).filePath(QStringLiteral(".clang-format")));
    REQUIRE(style.open(QIODevice::WriteOnly) == true);
    style.write("BasedOnStyle: LLVM\nIndentWidth: 2\n");
    style.close();
    CodeEditor editor;
    const QByteArray initial =
        QString::fromUtf8("// 文字😀\r\nfloat4 Shade(){return float4(1,2,3,4);}\r\n"
                          "struct Payload{float4 Value;};\r\n"
                          "T Identity<T>(T value){return value;}\r\n"
                          "RWStructuredBuffer<Payload> Output;\r\n"
                          "[shader(\"compute\")]\r\n[numthreads(1,1,1)]\r\n"
                          "void ComputeMain(uint3 dispatchThreadId:SV_DispatchThreadID){Payload payload;"
                          "payload.Value=Identity<float4>(float4(1,2,3,4));Output[dispatchThreadId.x]=payload;}\r\n")
            .toUtf8();
    MemoryCodeDocument document(initial);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    const isize byte = initial.indexOf("Shade");
    controller.RestoreSelection({static_cast<usize>(byte + 5), static_cast<usize>(byte)});
    CodeFormatter formatter(controller);
    QSignalSpy finished(&formatter, &CodeFormatter::Finished);
    REQUIRE(formatter.Start(
                MakeShaderFormatter(program, QDir(directory.path()).filePath(QStringLiteral("test.slang")))) == true);
    CHECK(formatter.IsRunning() == true);
    REQUIRE(finished.wait(12000) == true);
    INFO(formatter.GetMessage().toStdString());
    REQUIRE(finished.at(0).at(0).toBool() == true);
    CHECK(document.GetState().Text.contains("float4 Shade() { return float4(1, 2, 3, 4); }") == true);
    CHECK(document.GetState().Text.contains("\r\n") == true);
    CHECK(document.GetState().Text.contains("[numthreads(1, 1, 1)]") == true);
    CHECK(document.GetState().Text.contains("Identity<float4>") == true);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("Shade"));
    CHECK(editor.textCursor().anchor() > editor.textCursor().position());
    CHECK(document.GetSnapshot().Identity.Revision == 1);
    const QByteArray formatted = document.GetState().Text;
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == initial);
    REQUIRE(document.Redo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == formatted);
    REQUIRE(formatter.Start(
                MakeShaderFormatter(program, QDir(directory.path()).filePath(QStringLiteral("test.slang")))) == true);
    REQUIRE(finished.wait(12000) == true);
    CHECK(document.GetSnapshot().Identity.Revision == 3); // No-op format adds no history.
}

TEST_CASE("formatter failures cancellation and stale results preserve the authoritative source",
          "[editor][text][format]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const CodeFormatterTool profile = MakeShaderFormatter({}, {});
    const QString fixture = QDir(directory.path()).filePath(QStringLiteral("formatter-") + profile.MinimumVersion);
    REQUIRE(QFile::copy(QString::fromUtf8(LUDUS_FORMATTER_FIXTURE), fixture) == true);
    REQUIRE(QFile::setPermissions(fixture, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) == true);
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("intx;"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    CodeFormatter formatter(controller);
    QSignalSpy finished(&formatter, &CodeFormatter::Finished);
    CodeFormatterTool tool{QString::fromUtf8(LUDUS_TEST_PYTHON),
                           {QStringLiteral("success")},
                           profile.MinimumVersion,
                           {fixture}};
    bool shouldSucceed = false;
    bool isTimeoutExpected = false;
    SECTION("success")
    {
        shouldSucceed = true;
    }
    SECTION("move caret during formatting")
    {
        shouldSucceed = true;
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() { editor.moveCursor(QTextCursor::End); });
    }
    SECTION("split Unicode scalar")
    {
        editor.setPlainText(QString::fromUtf8("😀text"));
        tool.Arguments = {QStringLiteral("split")};
    }
    SECTION("newer release of the pinned major")
    {
        shouldSucceed = true;
        tool.MinimumVersion = profile.MinimumVersion.section(QLatin1Char('.'), 0, 0) + QStringLiteral(".0.0");
    }
    SECTION("older release of the pinned major")
    {
        tool.MinimumVersion = profile.MinimumVersion.section(QLatin1Char('.'), 0, 0) + QStringLiteral(".999.0");
    }
    SECTION("wrong version")
    {
        tool.MinimumVersion = QStringLiteral("0.0.0");
    }
    SECTION("malformed version")
    {
        tool.MinimumVersion = QStringLiteral("23.1");
    }
    SECTION("missing executable")
    {
        tool.Program += QStringLiteral("-missing");
    }
    SECTION("process failure")
    {
        tool.Arguments = {QStringLiteral("failure")};
    }
    SECTION("malformed XML")
    {
        tool.Arguments = {QStringLiteral("malformed")};
    }
    SECTION("invalid range")
    {
        tool.Arguments = {QStringLiteral("range")};
    }
    SECTION("transport capacity")
    {
        tool.Arguments = {QStringLiteral("transport")};
    }
    SECTION("decoded capacity")
    {
        document.LimitBytes = 5;
    }
    SECTION("timeout")
    {
        // The fixture sleeps far past this deadline; the default deadline would
        // make the suite wait 10 seconds for the same outcome.
        constexpr int32 SHORT_TIMEOUT_MILLISECONDS = 1500;
        isTimeoutExpected = true;
        tool.Arguments = {QStringLiteral("timeout")};
        tool.TimeoutMilliseconds = SHORT_TIMEOUT_MILLISECONDS;
    }
    SECTION("non-positive deadline")
    {
        tool.TimeoutMilliseconds = 0;
        CHECK(formatter.Start(tool) == false);
        CHECK(formatter.IsRunning() == false);
        CHECK(document.GetState().Text == QByteArrayLiteral("intx;"));
        return;
    }
    SECTION("cancel")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &formatter, &CodeFormatter::Cancel);
    }
    SECTION("edit while formatting")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() { editor.insertPlainText(QStringLiteral("//new\n")); });
    }
    SECTION("replace binding while formatting")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() {
            ++document.Session;
            controller.Synchronize();
        });
    }
    const QByteArray captured = document.GetState().Text;
    REQUIRE(formatter.Start(tool) == true);
    if (formatter.IsRunning() == true)
    {
        CHECK(formatter.Start(tool) == false); // One in-flight job.
    }
    // QProcess can report a missing executable synchronously on Windows.
    REQUIRE((finished.isEmpty() == false || finished.wait(12000) == true));
    INFO(formatter.GetMessage().toStdString());
    CHECK(finished.at(0).at(0).toBool() == shouldSucceed);
    CHECK(formatter.IsRunning() == false);
    if (isTimeoutExpected == true)
    {
        CHECK(formatter.GetMessage().contains(QStringLiteral("timed out")) == true);
    }
    if (shouldSucceed == true)
    {
        CHECK(document.GetState().Text == QByteArrayLiteral("int x;"));
    }
    else
    {
        CHECK(document.GetState().Text.contains(captured) == true);
    }
    CHECK(document.GetSnapshot().Identity.Revision <= 1);
    // Failure/cancellation leaves the service reusable.
    document.LimitBytes = 4096;
    tool = {QString::fromUtf8(LUDUS_TEST_PYTHON), {QStringLiteral("success")}, profile.MinimumVersion, {fixture}};
    REQUIRE(formatter.Start(tool) == true);
    REQUIRE(finished.wait(12000) == true);
    CHECK(formatter.IsRunning() == false);
}
#endif

TEST_CASE("unbound editor publishes native undo availability for its command host", "[editor][text][history]")
{
    CodeEditor editor;
    QSignalSpy availability(&editor, &CodeEditor::HistoryAvailabilityChanged);
    editor.insertPlainText(QStringLiteral("text"));
    CHECK(editor.CanUndo() == true);
    CHECK(availability.count() > 0);
    const int32 beforeUndo = static_cast<int32>(availability.count());
    editor.Undo();
    CHECK(editor.CanRedo() == true);
    CHECK(availability.count() > beforeUndo);
}

#if !defined(Q_OS_WASM)
TEST_CASE("Shader completion maps UTF-16 ranges and additional edits into one draft undo group",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host(
        QByteArray("// additional-edit \xF0\x9F\x98\x80\r\nvoid Main() {\r\n    scene.Ve\r\n}\r\n"));
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              QStringLiteral("// generated\n"),
                              QStringLiteral("\n// generated end"),
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.resize(800, 500);
    editor.show();
    editor.setFocus();
    editor.GoToLine(3, 13);
    const QByteArray original = host.GetState().Text;
    QTest::keyClick(&editor, Qt::Key_Space, ASSISTANCE_MODIFIER);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    REQUIRE(popupOrNull->count() == 1);
    REQUIRE(popupOrNull->item(0)->toolTip().contains(QStringLiteral("&lt;b&gt;untrusted markup&lt;/b&gt;")));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text.contains("scene.Version\r\n"));
    REQUIRE(host.GetState().Text.startsWith("// added\r\n// additional-edit"));
    REQUIRE(host.GetState().Text.contains("\xF0\x9F\x98\x80"));
    REQUIRE(host.Undo());
    document.Synchronize(true);
    REQUIRE(host.GetState().Text == original);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Shader assistance rejects generated edits and expands snippets", "[editor][text][assistance]")
{
    for (const QByteArray& marker : {QByteArray("generated-edit"), QByteArray("snippet")})
    {
        MemoryCodeDocument host("// " + marker + "\nscene.Ve");
        CodeEditor editor;
        CodeDocumentController document(editor);
        document.Bind(&host);
        editor.SetLanguage(CodeLanguage::Slang);
        CodeAssistance assistance(editor, document);
        QTemporaryDir temporary;
        QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  QStringLiteral("// generated\n"),
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
        editor.show();
        editor.setFocus();
        editor.moveCursor(QTextCursor::End);
        QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
        assistance.Request(false);
        QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
        REQUIRE(popupOrNull != nullptr);
        REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
        QTest::keyClick(&editor, Qt::Key_Return);
        if (marker == "snippet")
        {
            // `${1:Version}` inserts its placeholder text and selects it as the first stop.
            REQUIRE(host.GetState().Text.endsWith("scene.Version"));
            CHECK(host.CanUndo() == true);
            CHECK(editor.IsInSnippet() == true);
            CHECK(editor.textCursor().selectedText() == QStringLiteral("Version"));
            continue;
        }
        REQUIRE(host.CanUndo() == false);
        REQUIRE(host.GetState().Text.endsWith("scene.Ve"));
        REQUIRE(status.last().at(0).toString().isEmpty() == false);
    }
}

TEST_CASE("Shader signature help highlights the active parameter and escapes documentation",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host("EvaluateMaterial(scene, ");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QTest::keyClick(&editor, Qt::Key_Space, ASSISTANCE_MODIFIER | Qt::ShiftModifier);
    QLabel* signatureOrNull = editor.findChild<QLabel*>(QStringLiteral("codeSignatureHelp"));
    REQUIRE(signatureOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([signatureOrNull]() { return signatureOrNull->isVisible() == true; }));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("<b><u>SurfaceHit hit</u></b>")));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("&lt;script&gt;")));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("/private/tmp/generated")) == false);
    QTest::keyClick(&editor, Qt::Key_PageDown, Qt::AltModifier);
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("uint index")));
    QTest::keyClick(&editor, Qt::Key_Escape);
    REQUIRE(signatureOrNull->isVisible() == false);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Shader replies are discarded after text navigation binding and context changes",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host("// hold-response\nscene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::qWait(100);
    host.Session = 2;
    document.Synchronize();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false); // releases the previous binding's reply
    QTest::qWait(100);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(popupOrNull->isVisible() == false);
    assistance.ClearContext();
    editor.insertPlainText(QStringLiteral("r"));
    REQUIRE(host.GetState().Text.endsWith("scene.Ver"));
    REQUIRE(popupOrNull->isVisible() == false);
}

TEST_CASE("Slang protocol failures and crashes preserve text and allow a fresh retry", "[editor][text][assistance]")
{
    for (const QByteArray& marker : {QByteArray("malformed-server"), QByteArray("crash-server")})
    {
        MemoryCodeDocument host("// " + marker + "\nscene.Ve");
        CodeEditor editor;
        CodeDocumentController document(editor);
        document.Bind(&host);
        editor.SetLanguage(CodeLanguage::Slang);
        CodeAssistance assistance(editor, document);
        QTemporaryDir temporary;
        QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  {},
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
        editor.show();
        editor.setFocus();
        editor.moveCursor(QTextCursor::End);
        QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
        assistance.Request(false);
        REQUIRE(QTest::qWaitFor([&status]() {
            return status.isEmpty() == false && status.last().at(0).toString().contains(QStringLiteral("Slang"));
        }));
        REQUIRE(host.CanUndo() == false);
        const CodeTextCapture capture = document.Capture();
        const QByteArray repaired("// repaired\nscene.Ve");
        REQUIRE(document.ApplyServiceText(capture.Identity,
                                          repaired,
                                          {static_cast<usize>(repaired.size()), static_cast<usize>(repaired.size())}) ==
                CodeEditStatus::Ok);
        assistance.Request(false);
        QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
        REQUIRE(popupOrNull != nullptr);
        REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
        QTest::keyClick(&editor, Qt::Key_Return);
        REQUIRE(host.GetState().Text.endsWith("scene.Version"));
    }
}

TEST_CASE("Capability dispatch disables unadvertised assistance and follows server trigger characters",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host("Ite");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Lusl);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(
            QString::fromUtf8(LUDUS_TEST_PYTHON),
            temporary.path(),
            QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.lusl"))).toString(),
            {},
            {},
            {QString::fromUtf8(LUDUS_LANGUAGE_SERVER_FIXTURE),
             QStringLiteral("--capabilities"),
             QStringLiteral(R"({"completionProvider": {"triggerCharacters": [":"]}, "referencesProvider": true})")},
            {},
            GetLuslServerProfile());
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    // A server without signature help still serves completion (LUSL before M4 failed here).
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    REQUIRE(popupOrNull->count() == 1);
    CHECK(popupOrNull->item(0)->text().startsWith(QStringLiteral("Item")) == true);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QAction* signatureOrNull = nullptr;
    for (QAction* action : editor.actions())
    {
        signatureOrNull = action->objectName() == QStringLiteral("codeEditorSignature") ? action : signatureOrNull;
    }
    REQUIRE(signatureOrNull != nullptr);
    CHECK(signatureOrNull->isEnabled() == false);
    CHECK(signatureOrNull->toolTip() == QStringLiteral("The selected luslc does not support signatureHelp."));
    for (QAction* action : editor.GetNavigation().GetActions())
    {
        if (action->text() == QStringLiteral("Go to Definition"))
        {
            CHECK(action->isEnabled() == false);
            CHECK(action->toolTip().contains(QStringLiteral("does not support definition")) == true);
        }
        if (action->text() == QStringLiteral("Find References"))
        {
            CHECK(action->isEnabled() == true);
        }
    }
    // The server's trigger character requests completion while typing.
    QTest::keyClick(&editor, Qt::Key_End);
    QTest::keyClicks(&editor, QStringLiteral(":"));
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    CHECK(host.CanUndo() == true);
    // A new context returns to the defaults until its server reports capabilities.
    assistance.ClearContext();
    CHECK(signatureOrNull->isEnabled() == true);
}

namespace
{
/// A Slang assistance session against language_server_fixture.py with `options`.
/// Its context prefix contains the diagnosed word, so generated surroundings
/// must never produce markers in the authored text.
struct FakeAssistanceSession final
{
    FakeAssistanceSession(const QByteArray& text, const QStringList& options)
        : Host(text), Document(Editor), Assistance(Editor, Document)
    {
        Document.Bind(&Host);
        Editor.SetLanguage(CodeLanguage::Slang);
        QStringList arguments{QString::fromUtf8(LUDUS_LANGUAGE_SERVER_FIXTURE)};
        arguments.append(options);
        QObject::connect(&Assistance, &CodeAssistance::ContextRequested, &Editor, [this, arguments]() {
            Assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  Temporary.path(),
                                  QUrl::fromLocalFile(Temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  QStringLiteral("// generated missing\n"),
                                  {},
                                  arguments);
        });
        Editor.resize(800, 400);
        Editor.show();
        Editor.setFocus();
    }

    /// Prepare the context through an ordinary completion intent.
    void Prepare()
    {
        Editor.moveCursor(QTextCursor::End);
        Assistance.Request(false);
    }

    MemoryCodeDocument Host;
    CodeEditor Editor;
    CodeDocumentController Document;
    CodeAssistance Assistance;
    QTemporaryDir Temporary;
};

const QStringList DIAGNOSING_SERVER{QStringLiteral("--capabilities"),
                                    QStringLiteral(R"({"completionProvider": {}, "hoverProvider": true})"),
                                    QStringLiteral("--diagnose"),
                                    QStringLiteral("missing")};
} // namespace

TEST_CASE("Analysis diagnostics follow the draft and stay apart from build results", "[editor][text][assistance]")
{
    FakeAssistanceSession session("float a = missing;\nfloat b = 2;\n", DIAGNOSING_SERVER);
    session.Prepare();
    CodeEditor& editor = session.Editor;
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetAnalysisDiagnosticCount() == 1; }));
    CHECK(editor.GetAnalysisDiagnosticRange(0).selectedText() == QStringLiteral("missing"));
    const int32 marked = editor.GetAnalysisDiagnosticRange(0).selectionStart() + 2;
    CHECK(editor.DescribeDiagnosticsAt(marked) == QStringLiteral("Analysis (slangd) error: unknown name missing"));
    // Build results stay their own layer and the build report keeps its label.
    editor.SetDiagnostics({{ .Line = 2, .Column = 7, .Length = 1, .Message = QStringLiteral("build says b") }}, true);
    CHECK(editor.GetDiagnosticCount() == 1);
    CHECK(editor.GetAnalysisDiagnosticCount() == 1);
    CHECK(editor.DescribeLineDiagnostics(1) == QStringLiteral("Error: build says b"));
    // Typing re-synchronizes after a pause and the next report replaces the markers.
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("missing"));
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetAnalysisDiagnosticCount() == 2; }));
    CHECK(editor.GetDiagnosticCount() == 1);
    // A new context clears analysis; the build report is not the client's to clear.
    session.Assistance.ClearContext();
    CHECK(editor.GetAnalysisDiagnosticCount() == 0);
    CHECK(editor.GetDiagnosticCount() == 1);
}

TEST_CASE("Analysis reports for an older version are discarded", "[editor][text][assistance]")
{
    QStringList options = DIAGNOSING_SERVER;
    options.append(QStringLiteral("--stale-diagnostics"));
    FakeAssistanceSession session("float a = missing;\n", options);
    QSignalSpy status(&session.Assistance, &CodeAssistance::StatusChanged);
    session.Prepare();
    REQUIRE(QTest::qWaitFor([&status]() { return status.isEmpty() == false; }));
    QTest::qWait(500);
    CHECK(session.Editor.GetAnalysisDiagnosticCount() == 0);
}

TEST_CASE("Hover shows the server's text with diagnostics and never prepares a context", "[editor][text][assistance]")
{
    QStringList options = DIAGNOSING_SERVER;
    options.append({QStringLiteral("--hold"), QStringLiteral("textDocument/hover")});
    FakeAssistanceSession session("float a = missing;\n", options);
    CodeEditor& editor = session.Editor;
    QSignalSpy contexts(&session.Assistance, &CodeAssistance::ContextRequested);
    QAction* hoverOrNull = nullptr;
    for (QAction* action : editor.actions())
    {
        hoverOrNull = action->objectName() == QStringLiteral("codeEditorHover") ? action : hoverOrNull;
    }
    REQUIRE(hoverOrNull != nullptr);
    // No context yet: hover describes local diagnostics only and starts nothing.
    hoverOrNull->trigger();
    QTest::qWait(100);
    CHECK(contexts.isEmpty() == true);
    CHECK(editor.GetHoverText().isEmpty() == true);
    session.Prepare();
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetAnalysisDiagnosticCount() == 1; }));
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.GetAnalysisDiagnosticRange(0).selectionStart() + 1);
    editor.setTextCursor(cursor);
    // The first hover reply is held; an edit makes it stale before it arrives.
    hoverOrNull->trigger();
    QTest::qWait(100);
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("x"));
    session.Assistance.Request(false); // any request releases the held reply
    QTest::qWait(300);
    CHECK(editor.GetHoverText().isEmpty() == true);
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setTextCursor(cursor);
    hoverOrNull->trigger();
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetHoverText().isEmpty() == false; }));
    CHECK(editor.GetHoverText().startsWith(QStringLiteral("hover ")) == true);
    CHECK(editor.GetHoverText().contains(QStringLiteral("Analysis (slangd) error: unknown name missing")) == true);
    // Resting the pointer requests a hover where it rests.
    QSignalSpy requests(&editor, &CodeEditor::HoverRequested);
    QTest::mouseMove(editor.viewport(), editor.cursorRect(cursor).center());
    REQUIRE(QTest::qWaitFor([&requests]() { return requests.isEmpty() == false; }));
}

TEST_CASE("Go to Declaration, Type Definition and Symbol in Workspace follow the server's capabilities",
          "[editor][text][assistance][code-navigation]")
{
    FakeAssistanceSession session(
        "struct Target {};\nTarget value;\n",
        {QStringLiteral("--capabilities"),
         QStringLiteral(R"({"completionProvider": {}, "declarationProvider": true, "typeDefinitionProvider": true,
                            "workspaceSymbolProvider": true})"),
         QStringLiteral("--target"),
         QStringLiteral("Target")});
    CodeEditor& editor = session.Editor;
    const auto findAction = [&editor](const QString& text) {
        QAction* foundOrNull = nullptr;
        for (QAction* action : editor.GetNavigation().GetActions())
        {
            foundOrNull = action->text() == text ? action : foundOrNull;
        }
        REQUIRE(foundOrNull != nullptr);
        return foundOrNull;
    };
    QAction* implementation = findAction(QStringLiteral("Go to Implementation"));
    session.Prepare();
    // Capability dispatch: the server advertises no implementation provider.
    REQUIRE(QTest::qWaitFor([implementation]() { return implementation->isEnabled() == false; }));
    CHECK(implementation->toolTip().contains(QStringLiteral("does not support implementation")) == true);
    CHECK(implementation->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_F12));
    QTest::keyClick(&editor, Qt::Key_Escape);
    const auto placeCaret = [&editor]() {
        QTextCursor cursor(editor.document());
        cursor.setPosition(static_cast<int32>(editor.toPlainText().lastIndexOf(QStringLiteral("Target"))) + 2);
        editor.setTextCursor(cursor);
    };
    for (const CodeNavigationKind kind : {CodeNavigationKind::Declaration, CodeNavigationKind::TypeDefinition})
    {
        placeCaret();
        session.Assistance.RequestNavigation(kind);
        // A single location jumps straight to it, as Go to Definition does.
        REQUIRE(QTest::qWaitFor([&editor]() { return editor.textCursor().blockNumber() == 0; }));
        CHECK(editor.textCursor().selectedText() == QStringLiteral("Target"));
    }
    placeCaret();
    session.Assistance.RequestNavigation(CodeNavigationKind::WorkspaceSymbols);
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetNavigation().IsChoosing() == true; }));
    QListWidget* resultsOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeNavigationResults"));
    REQUIRE(resultsOrNull != nullptr);
    REQUIRE(resultsOrNull->count() == 1);
    CHECK(resultsOrNull->item(0)->text().startsWith(QStringLiteral("Target")) == true);
    CHECK(session.Host.CanUndo() == false);
}

TEST_CASE("Semantic tokens and folding ranges follow the draft and drop stale replies", "[editor][text][assistance]")
{
    // The context prefix "// generated missing\n" holds a token and the start of a
    // range on its line 0: generated text must not produce either.
    FakeAssistanceSession session("float a = missing;\nfloat b = missing;\nfloat c;\n",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {}, "foldingRangeProvider": true,
                            "semanticTokensProvider": {"legend": {"tokenTypes": ["function"], "tokenModifiers": []},
                                                       "full": true}})"),
                                   QStringLiteral("--tokens"),
                                   QStringLiteral("missing"),
                                   QStringLiteral("--folding-ranges"),
                                   QStringLiteral(R"([{"startLine": 1, "endLine": 3, "kind": "imports"},
                           {"startLine": 0, "endLine": 2, "kind": "imports"}])")});
    CodeEditor& editor = session.Editor;
    const auto areTokensOnWord = [&editor]() {
        const QString text = editor.toPlainText();
        for (const CodeSyntaxToken& token : editor.GetSemanticTokens())
        {
            if (QStringView(text).sliced(token.GetStart(), token.GetEnd() - token.GetStart()) !=
                    QStringLiteral("missing") ||
                token.GetRole() != CodeSyntaxRole::Function)
            {
                return false;
            }
        }
        return true;
    };
    session.Prepare();
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetSemanticTokens().size() == 2; }));
    CHECK(areTokensOnWord() == true);
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetStructure().GetServerFolds().size() == 1; }));
    const CodeSyntaxFold fold = editor.GetStructure().GetServerFolds().constFirst();
    CHECK(fold.FirstLine == 0);
    CHECK(fold.LastLine == 2);
    CHECK(fold.Kind == CodeFoldKind::Imports);
    QTest::keyClick(&editor, Qt::Key_Escape);

    // Typing moves the shown tokens at once, and the next synchronization asks again.
    QTextCursor cursor(editor.document());
    editor.setTextCursor(cursor);
    QTest::keyClicks(&editor, QStringLiteral("x"));
    CHECK(editor.GetSemanticTokens().size() == 2);
    CHECK(areTokensOnWord() == true);
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("missing"));
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetSemanticTokens().size() == 3; }));
    CHECK(areTokensOnWord() == true);

    // A new context removes both layers.
    session.Assistance.ClearContext();
    CHECK(editor.GetSemanticTokens().isEmpty() == true);
    CHECK(editor.GetStructure().GetServerFolds().isEmpty() == true);
}

TEST_CASE("Snippet tab stops edit mirrors together and end at the final position", "[editor][text][assistance]")
{
    CodeSnippet snippet;
    REQUIRE(ParseCodeSnippet(QStringLiteral("${1:value} = ${2:other} + $1;$0"), snippet) == true);
    MemoryCodeDocument host(snippet.Text.toUtf8() + "\n");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    editor.show();
    editor.setFocus();
    editor.StartSnippet(0, snippet);
    REQUIRE(editor.IsInSnippet() == true);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value"));
    // The mirror is a secondary cursor, so typing replaces both.
    QTest::keyClicks(&editor, QStringLiteral("x"));
    CHECK(editor.toPlainText() == QStringLiteral("x = other + x;\n"));
    QTest::keyClick(&editor, Qt::Key_Tab);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("other"));
    QTest::keyClick(&editor, Qt::Key_Backtab);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("x"));
    QTest::keyClick(&editor, Qt::Key_Tab);
    QTest::keyClicks(&editor, QStringLiteral("y"));
    // Past the last stop the caret goes to $0 and Tab indents again.
    QTest::keyClick(&editor, Qt::Key_Tab);
    CHECK(editor.IsInSnippet() == false);
    CHECK(editor.textCursor().position() == static_cast<int32>(QStringLiteral("x = y + x;").size()));
    CHECK(editor.toPlainText() == QStringLiteral("x = y + x;\n"));

    // Escape, or moving the caret out of the snippet, ends it as well.
    REQUIRE(ParseCodeSnippet(QStringLiteral("${1:x} = ${2:y} + $1;"), snippet) == true);
    REQUIRE(editor.toPlainText().startsWith(snippet.Text) == true);
    editor.StartSnippet(0, snippet);
    REQUIRE(editor.IsInSnippet() == true);
    QTest::keyClick(&editor, Qt::Key_Escape);
    CHECK(editor.IsInSnippet() == false);
    editor.StartSnippet(0, snippet);
    REQUIRE(editor.IsInSnippet() == true);
    editor.moveCursor(QTextCursor::End);
    CHECK(editor.IsInSnippet() == false);
    // A snippet longer than the document starts nothing.
    REQUIRE(ParseCodeSnippet(QStringLiteral("${1:a much longer placeholder than the text}"), snippet) == true);
    editor.StartSnippet(0, snippet);
    CHECK(editor.IsInSnippet() == false);
}

TEST_CASE("Snippet completions expand and visit their stops", "[editor][text][assistance]")
{
    FakeAssistanceSession session("float a = ",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {}})"),
                                   QStringLiteral("--snippet"),
                                   QStringLiteral("Call(${1:value}, ${2:factor})$0")});
    CodeEditor& editor = session.Editor;
    session.Prepare();
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Return);
    REQUIRE(session.Host.GetState().Text == "float a = Call(value, factor)");
    CHECK(editor.IsInSnippet() == true);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value"));
    QTest::keyClicks(&editor, QStringLiteral("1"));
    QTest::keyClick(&editor, Qt::Key_Tab);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("factor"));
    QTest::keyClicks(&editor, QStringLiteral("2"));
    QTest::keyClick(&editor, Qt::Key_Tab);
    CHECK(editor.IsInSnippet() == false);
    CHECK(session.Host.GetState().Text == "float a = Call(1, 2)");
    CHECK(editor.textCursor().position() == static_cast<int32>(editor.toPlainText().size()));
}

TEST_CASE("A snippet transform is refused without editing", "[editor][text][assistance]")
{
    FakeAssistanceSession session("float a = ",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {}})"),
                                   QStringLiteral("--snippet"),
                                   QStringLiteral("${1/(.*)/upper/}")});
    CodeEditor& editor = session.Editor;
    QSignalSpy status(&session.Assistance, &CodeAssistance::StatusChanged);
    session.Prepare();
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(session.Host.GetState().Text == "float a = ");
    CHECK(session.Host.CanUndo() == false);
    REQUIRE(status.isEmpty() == false);
    CHECK(status.last().first().toString().contains(QStringLiteral("snippet transform")) == true);
}

TEST_CASE("Completion resolve fills in the highlighted suggestion's documentation", "[editor][text][assistance]")
{
    FakeAssistanceSession session("float a = ",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {"resolveProvider": true}})"),
                                   QStringLiteral("--resolve")});
    CodeEditor& editor = session.Editor;
    session.Prepare();
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    REQUIRE(popupOrNull->count() == 2);
    // Only the highlighted item is resolved, and only once.
    REQUIRE(QTest::qWaitFor([&editor]() {
        return editor.GetCompletionDocumentation().startsWith(QStringLiteral("Documentation for Item")) == true;
    }));
    CHECK(popupOrNull->item(0)->text().endsWith(QStringLiteral("resolved detail")) == true);
    CHECK(popupOrNull->item(1)->text().contains(QStringLiteral("resolved")) == false);
    QTest::keyClick(&editor, Qt::Key_Down);
    REQUIRE(QTest::qWaitFor([&editor]() {
        return editor.GetCompletionDocumentation().startsWith(QStringLiteral("Documentation for Other")) == true;
    }));
    QTest::keyClick(&editor, Qt::Key_Up);
    CHECK(editor.GetCompletionDocumentation().startsWith(QStringLiteral("Documentation for Item")) == true);
    QTest::keyClick(&editor, Qt::Key_Escape);
    CHECK(editor.GetCompletionDocumentation().isEmpty() == true);
}

TEST_CASE("Inlay hints cover the authored source and follow a server refresh", "[editor][text][assistance]")
{
    // The first reply has no hints; the server then asks for a refresh.
    FakeAssistanceSession session("float a = Call(missing);\nfloat b = Call(missing);\n",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {}, "inlayHintProvider": true})"),
                                   QStringLiteral("--hints"),
                                   QStringLiteral("missing"),
                                   QStringLiteral("--refresh-hints")});
    CodeEditor& editor = session.Editor;
    session.Prepare();
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetInlayHints().size() == 2; }));
    const QString text = editor.toPlainText();
    CHECK(editor.GetInlayHints().at(0).Position == text.indexOf(QStringLiteral("missing")));
    CHECK(editor.GetInlayHints().at(1).Position == text.lastIndexOf(QStringLiteral("missing")));
    CHECK(editor.GetInlayHints().at(0).Label == QStringLiteral("name:"));
    CHECK(editor.GetInlayHints().at(0).Kind == CodeInlayHintKind::Parameter);
    CHECK(session.Host.CanUndo() == false);
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("Call(missing);"));
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetInlayHints().size() == 3; }));
    session.Assistance.ClearContext();
    CHECK(editor.GetInlayHints().isEmpty() == true);
}

TEST_CASE("A semantic token reply for an earlier draft is discarded", "[editor][text][assistance]")
{
    FakeAssistanceSession session("float a = missing;\n",
                                  {QStringLiteral("--capabilities"),
                                   QStringLiteral(R"({"completionProvider": {}, "hoverProvider": true,
                            "semanticTokensProvider": {"legend": {"tokenTypes": ["function"], "tokenModifiers": []},
                                                       "full": true}})"),
                                   QStringLiteral("--tokens"),
                                   QStringLiteral("missing"),
                                   QStringLiteral("--hold"),
                                   QStringLiteral("textDocument/semanticTokens/full")});
    CodeEditor& editor = session.Editor;
    QAction* hoverOrNull = nullptr;
    for (QAction* action : editor.actions())
    {
        hoverOrNull = action->objectName() == QStringLiteral("codeEditorHover") ? action : hoverOrNull;
    }
    REQUIRE(hoverOrNull != nullptr);
    session.Prepare();
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Escape);
    // The first token reply is held. An edit at the start makes it describe the
    // previous text; a hover, which sends the draft without asking for layers,
    // releases it before the next synchronization asks again.
    QTextCursor cursor(editor.document());
    editor.setTextCursor(cursor);
    QTest::keyClicks(&editor, QStringLiteral("x"));
    hoverOrNull->trigger();
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetHoverText().isEmpty() == false; }));
    // Applied, the stale reply would mark "issing" at the old offset.
    const QString text = editor.toPlainText();
    for (const CodeSyntaxToken& token : editor.GetSemanticTokens())
    {
        CHECK(QStringView(text).sliced(token.GetStart(), token.GetEnd() - token.GetStart()) ==
              QStringLiteral("missing"));
    }
    REQUIRE(QTest::qWaitFor([&editor]() { return editor.GetSemanticTokens().size() == 1; }));
    CHECK(editor.GetSemanticTokens().constFirst().GetStart() == text.indexOf(QStringLiteral("missing")));
}

TEST_CASE("Escape cancels pending shader assistance before a delayed reply", "[editor][text][assistance]")
{
    MemoryCodeDocument host("// hold-response\nscene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::qWait(100);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(100);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(popupOrNull->isVisible() == false);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Typing while shader assistance starts retains the latest completion intent", "[editor][text][assistance]")
{
    MemoryCodeDocument host("scene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        QTimer::singleShot(200, &assistance, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  {},
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::keyClicks(&editor, QStringLiteral("r"));
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text == QByteArray("scene.Version"));
    REQUIRE(host.Undo());
    document.Synchronize(true);
    REQUIRE(host.GetState().Text == QByteArray("scene.Ver"));
}

TEST_CASE("Completion capacity rejection is visible beside the unchanged source", "[editor][text][assistance]")
{
    MemoryCodeDocument host("scene.Ve");
    host.LimitBytes = static_cast<usize>(host.GetState().Text.size());
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
    assistance.Request(false);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text == QByteArray("scene.Ve"));
    REQUIRE(host.CanUndo() == false);
    REQUIRE(status.last().at(0).toString().contains(QStringLiteral("byte limit")));
}

TEST_CASE("Native Slang completion and signature help share the versioned editing controller",
          "[editor][text][assistance][slang-native]")
{
    const QString server = qEnvironmentVariable("LUDUS_TEST_SLANGD");
    if (server.isEmpty() == true)
    {
        SKIP("Set LUDUS_TEST_SLANGD to exercise the installed Slang server.");
    }
    const QString prefix = QStringLiteral("struct SceneContext { uint Version; };\nfloat EvaluateMaterial(SceneContext "
                                          "scene, float value) { return value; }\n");
    const QString source = QStringLiteral("// Unicode: \U0001f600\nvoid Main(SceneContext scene) { scene.Ve; }\n");
    MemoryCodeDocument host(source.toUtf8());
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QFile unit(temporary.filePath(QStringLiteral("unit.slang")));
    REQUIRE(unit.open(QIODevice::WriteOnly) == true);
    REQUIRE(unit.write((prefix + source).toUtf8()) > 0);
    unit.close();
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(server, temporary.path(), QUrl::fromLocalFile(unit.fileName()).toString(), prefix, {});
    });
    editor.resize(900, 480);
    editor.show();
    editor.setFocus();
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(static_cast<int32>(source.indexOf(QStringLiteral("Ve"))) + 2);
    editor.setTextCursor(cursor);
    assistance.Request(false);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }, 15000));
    REQUIRE(popupOrNull->item(0)->text().startsWith(QStringLiteral("Version")));
    const QString capture = qEnvironmentVariable("LUDUS_ASSISTANCE_CAPTURE_DIR");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(editor.grab().save(QDir(capture).filePath(QStringLiteral("source.png"))) == true);
        REQUIRE(popupOrNull->grab().save(QDir(capture).filePath(QStringLiteral("completion.png"))) == true);
    }
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text.contains("scene.Version;"));
    const QByteArray call("void Main(SceneContext scene) { EvaluateMaterial(scene, ); }\n");
    const usize caret = static_cast<usize>(call.indexOf(", ")) + 2;
    REQUIRE(document.ApplyServiceText(document.Capture().Identity, call, {caret, caret}) == CodeEditStatus::Ok);
    assistance.Request(true);
    QLabel* signatureOrNull = editor.findChild<QLabel*>(QStringLiteral("codeSignatureHelp"));
    REQUIRE(signatureOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([signatureOrNull]() { return signatureOrNull->isVisible() == true; }, 15000));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("<b><u>float value</u></b>")));
    if (capture.isEmpty() == false)
    {
        REQUIRE(signatureOrNull->grab().save(QDir(capture).filePath(QStringLiteral("signature.png"))) == true);
    }
}

#endif

#if !defined(Q_OS_WASM)
TEST_CASE("Semantic navigation uses current draft context and does not create document edits",
          "[editor][code-navigation][assistance]")
{
    MemoryCodeDocument host("// 😀\nfloat Value() { return 1; }\nfloat Main() { return Value(); }\n");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              QStringLiteral("// generated\n"),
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    const CodeDocumentIdentity identity = host.GetSnapshot().Identity;
    assistance.RequestNavigation(CodeNavigationKind::Definition);
    REQUIRE(QTest::qWaitFor([&]() { return editor.textCursor().selectedText() == QStringLiteral("Value"); }));
    CHECK(host.GetSnapshot().Identity == identity);
    CHECK(host.CanUndo() == false);
    editor.GetNavigation().Back();
    CHECK(editor.textCursor().position() == editor.toPlainText().size());
    assistance.RequestNavigation(CodeNavigationKind::Symbols);
    REQUIRE(QTest::qWaitFor([&]() { return editor.GetNavigation().IsChoosing(); }));
    QLineEdit* filterOrNull = editor.findChild<QLineEdit*>(QStringLiteral("codeNavigationFilter"));
    REQUIRE(filterOrNull != nullptr);
    QTest::keyClick(filterOrNull, Qt::Key_Return);
    REQUIRE(editor.textCursor().selectedText() == QStringLiteral("Value"));
    CHECK(host.GetSnapshot().Identity == identity);
    assistance.RequestNavigation(CodeNavigationKind::References);
    REQUIRE(QTest::qWaitFor([&]() { return editor.GetNavigation().IsChoosing(); }));
    QTest::keyClick(filterOrNull, Qt::Key_Escape);
    CHECK(host.GetSnapshot().Identity == identity);
}

TEST_CASE("Pending navigation cannot publish after caret binding context or text changes",
          "[editor][code-navigation][assistance]")
{
    MemoryCodeDocument host("// hold-navigation\nfloat Value() { return 1; }\n");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.RequestNavigation(CodeNavigationKind::Definition);
    QTest::qWait(150);
    SECTION("Caret changed")
    {
        editor.moveCursor(QTextCursor::Start);
    }
    SECTION("Binding changed")
    {
        ++host.Session;
        document.Synchronize();
    }
    SECTION("Context changed")
    {
        assistance.ClearContext();
    }
    SECTION("Text changed")
    {
        editor.insertPlainText(QStringLiteral("// changed"));
    }
    const int32 position = editor.textCursor().position();
    assistance.RequestNavigation(CodeNavigationKind::References); // peer releases obsolete definition first
    QTest::qWait(150);
    CHECK(editor.textCursor().position() == position);
    CHECK(editor.GetNavigation().IsChoosing() == false);
    CHECK(editor.textCursor().selectedText() != QStringLiteral("Value"));
}
#endif

#if !defined(Q_OS_WASM)
TEST_CASE("Native Slang resolves authored symbols references and generated API definitions",
          "[editor][code-navigation][slang-native]")
{
    const QString server = qEnvironmentVariable("LUDUS_TEST_SLANGD");
    if (server.isEmpty() == true)
    {
        SKIP("Set LUDUS_TEST_SLANGD for compiler-backed navigation acceptance.");
    }
    const QString prefix = QStringLiteral("struct SceneContext { float Exposure; };\n");
    const QString source = QStringLiteral(
        "// Unicode: \U0001f600\nfloat Value() { return 1; }\nfloat Main(SceneContext scene) { return Value(); }\n"
        "float Twice(float amount) { return amount * 2; }\nfloat Use() { return Twice(3); }\n");
    MemoryCodeDocument host(source.toUtf8());
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QFile unit(temporary.filePath(QStringLiteral("unit.slang")));
    REQUIRE(unit.open(QIODevice::WriteOnly) == true);
    REQUIRE(unit.write((prefix + source).toUtf8()) > 0);
    unit.close();
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(server, temporary.path(), QUrl::fromLocalFile(unit.fileName()).toString(), prefix, {});
    });
    editor.resize(900, 480);
    editor.show();
    editor.setFocus();
    QTextCursor cursor(editor.document());
    cursor.setPosition(static_cast<int32>(source.lastIndexOf(QStringLiteral("Value"))) + 2);
    editor.setTextCursor(cursor);
    assistance.RequestNavigation(CodeNavigationKind::Definition);
    REQUIRE(
        QTest::qWaitFor([&]() { return editor.textCursor().selectedText().contains(QStringLiteral("Value")); }, 15000));
    CHECK(editor.textCursor().blockNumber() == 1);
    CHECK(host.CanUndo() == false);
    // slangd's semantic tokens color authored names after the astral character on
    // the first line; the generated prefix's names never reach the view.
    const auto hasToken = [&editor, &source](const QString& name, CodeSyntaxRole role) {
        for (const CodeSyntaxToken& token : editor.GetSemanticTokens())
        {
            if (token.GetRole() == role &&
                QStringView(source).sliced(token.GetStart(), token.GetEnd() - token.GetStart()) == name)
            {
                return true;
            }
        }
        return false;
    };
    REQUIRE(QTest::qWaitFor([&]() { return hasToken(QStringLiteral("Value"), CodeSyntaxRole::Function); }, 15000));
    CHECK(hasToken(QStringLiteral("SceneContext"), CodeSyntaxRole::Type) == true);
    CHECK(hasToken(QStringLiteral("scene"), CodeSyntaxRole::Parameter) == true);
    // Inlay hints name the parameter an argument binds.
    // slangd sends none until it has read its configuration, then asks for a refresh.
    const int32 argument = static_cast<int32>(source.lastIndexOf(QStringLiteral("3)")));
    REQUIRE(QTest::qWaitFor(
        [&]() {
            for (const CodeInlayHint& hint : editor.GetInlayHints())
            {
                if (hint.Position == argument && hint.Kind == CodeInlayHintKind::Parameter &&
                    hint.Label.startsWith(QStringLiteral("amount")) == true)
                {
                    return true;
                }
            }
            return false;
        },
        15000));
    assistance.RequestNavigation(CodeNavigationKind::Symbols);
    REQUIRE(QTest::qWaitFor([&]() { return editor.GetNavigation().IsChoosing(); }, 15000));
    QListWidget* resultsOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeNavigationResults"));
    REQUIRE(resultsOrNull != nullptr);
    REQUIRE(resultsOrNull->count() >= 2);
    const QString capture = qEnvironmentVariable("LUDUS_NAVIGATION_CAPTURE_DIR");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(editor.grab().save(QDir(capture).filePath(QStringLiteral("source.png"))) == true);
        REQUIRE(resultsOrNull->parentWidget()->grab().save(QDir(capture).filePath(QStringLiteral("symbols.png"))) ==
                true);
    }
    editor.GetNavigation().Dismiss();
    editor.setFocus();
    cursor.setPosition(static_cast<int32>(source.lastIndexOf(QStringLiteral("Value"))) + 2);
    editor.setTextCursor(cursor);
    QSignalSpy referenceStatus(&assistance, &CodeAssistance::StatusChanged);
    assistance.RequestNavigation(CodeNavigationKind::References);
    REQUIRE(QTest::qWaitFor(
        [&]() {
            return editor.GetNavigation().IsChoosing() == true ||
                   (referenceStatus.isEmpty() == false &&
                    referenceStatus.last().first().toString().contains(QStringLiteral("does not support")) == true);
        },
        15000));
    if (editor.GetNavigation().IsChoosing() == true)
    {
        CHECK(resultsOrNull->count() >= 1);
        editor.GetNavigation().Dismiss();
    }
    else
    {
        for (QAction* action : editor.GetNavigation().GetActions())
        {
            if (action->text() == QStringLiteral("Find References"))
            {
                CHECK(action->isEnabled() == false);
            }
        }
    }
    editor.setFocus();
    cursor.setPosition(static_cast<int32>(source.indexOf(QStringLiteral("SceneContext"))) + 2);
    editor.setTextCursor(cursor);
    class DefinitionViewHost final : public CodeNavigationViewHost
    {
    public:
        CodeNavigation* OpenNavigationTarget(const CodeNavigationTarget& target) override
        {
            Destination = target;
            Preview.setPlainText(target.Text);
            return &Preview.GetNavigation();
        }

        CodeNavigation* ActivateNavigationView(CodeNavigation* viewOrNull, const QString&) override
        {
            return viewOrNull;
        }

        CodeNavigationTarget Destination;
        CodeEditor Preview;
    };
    DefinitionViewHost views;
    editor.GetNavigation().SetViewHost(&views);
    assistance.RequestNavigation(CodeNavigationKind::Definition);
    REQUIRE(QTest::qWaitFor([&]() { return views.Destination.IsReadOnly == true; }, 15000));
    CHECK(views.Destination.Text.contains(prefix) == true);
    CHECK(views.Preview.textCursor().selectedText() == QStringLiteral("SceneContext"));
    editor.GetNavigation().SetViewHost(nullptr);
    CHECK(host.GetState().Text == source.toUtf8());
    CHECK(host.CanUndo() == false);
}
#endif

namespace
{
/// The foreground of the character at `position` in the editor's document, as its highlighter set it.
QColor GetForeground(const CodeEditor& editor, int position)
{
    const QTextBlock block = editor.document()->findBlock(position);
    const int offset = position - block.position();
    for (const QTextLayout::FormatRange& range : block.layout()->formats())
    {
        if (offset >= range.start && offset < range.start + range.length)
        {
            return range.format.foreground().color();
        }
    }
    return {};
}
} // namespace

TEST_CASE("LUSL sources color their keywords and fixed-width types", "[editor][text][lusl]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("concept Opaque<T> = true;\nstruct Light\n{\n    float32x3 Color;\n};\n"));
    editor.SetLanguage(CodeLanguage::Lusl);
    const QString text = editor.toPlainText();
    const QColor conceptKeyword = GetForeground(editor, static_cast<int>(text.indexOf(QStringLiteral("concept"))));
    const QColor structure = GetForeground(editor, static_cast<int>(text.indexOf(QStringLiteral("struct"))));
    const QColor vector = GetForeground(editor, static_cast<int>(text.indexOf(QStringLiteral("float32x3"))));
    const QColor light = GetForeground(editor, static_cast<int>(text.indexOf(QStringLiteral("Light"))));
    // `concept` is a LUSL keyword the Slang lexer does not know.
    CHECK(conceptKeyword.isValid() == true);
    CHECK(conceptKeyword == structure);
    CHECK(vector.isValid() == true);
    CHECK(vector != structure);
    // A name after `struct` is a declared type.
    CHECK(light == vector);
    editor.SetLanguage(CodeLanguage::Slang);
    CHECK(GetForeground(editor, static_cast<int>(text.indexOf(QStringLiteral("concept")))) != structure);
}

TEST_CASE("LUSL formats in process as one undoable edit", "[editor][text][lusl]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("static const uint32 COUNT=4;\n"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    editor.SetLanguage(CodeLanguage::Lusl);
    QObject::connect(&editor, &CodeEditor::UndoRequested, &controller, [&]() {
        if (document.Undo() == true)
        {
            controller.Synchronize();
            controller.RestoreSelection(document.GetState().Selection);
        }
    });
    CHECK(FormatLuslDocument(controller) == QStringLiteral("Source formatted; Save publishes the draft."));
    CHECK(document.GetState().Text == QByteArrayLiteral("static const uint32 COUNT = 4;\n"));
    CHECK(editor.toPlainText() == QStringLiteral("static const uint32 COUNT = 4;\n"));
    CHECK(FormatLuslDocument(controller) == QStringLiteral("Source is already formatted."));
    editor.Undo();
    CHECK(document.GetState().Text == QByteArrayLiteral("static const uint32 COUNT=4;\n"));

    MemoryCodeDocument broken(QByteArrayLiteral("/* unterminated\n"));
    controller.Bind(&broken);
    CHECK(FormatLuslDocument(controller).contains(QStringLiteral("source was retained")) == true);
    CHECK(broken.GetState().Text == QByteArrayLiteral("/* unterminated\n"));
}
