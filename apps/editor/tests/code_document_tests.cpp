#include "internal/code_document_controller.h"
#include "internal/code_edit_scope.h"
#include "internal/code_editor_panel.h"
#include "internal/code_formatter.h"
#include "internal/code_text.h"
#include "internal/document_history.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QInputMethodEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
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
    CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                             const CodeTextEdit& edit,
                             CodeTextSelection before,
                             CodeTextSelection after,
                             bool shouldMerge) override
    {
        if (expected != GetSnapshot().Identity)
        {
            return CodeEditStatus::Conflict;
        }
        QByteArray candidate = mState.Text;
        candidate.replace(static_cast<isize>(edit.StartBytes),
                          static_cast<isize>(edit.RemovedBytes),
                          edit.Inserted.GetDataOrNull(),
                          static_cast<isize>(edit.Inserted.GetSize()));
        if (static_cast<usize>(candidate.size()) > LimitBytes)
        {
            return CodeEditStatus::Capacity;
        }
        if (candidate == mState.Text)
        {
            return CodeEditStatus::Ok;
        }
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
    CHECK(controller.ApplyFormattedText(captured.Identity, QByteArrayLiteral("formatted"), {}) ==
          CodeEditStatus::Conflict);
    CHECK(document.GetState().Text == edited);
    const CodeTextCapture oldBinding = controller.Capture();
    ++document.Session;
    controller.Synchronize();
    CHECK(controller.ApplyFormattedText(oldBinding.Identity, QByteArrayLiteral("formatted"), {}) ==
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
    const QString fixture = QDir(directory.path()).filePath(QStringLiteral("formatter-") + profile.ExpectedVersion);
    REQUIRE(QFile::copy(QString::fromUtf8(LUDUS_FORMATTER_FIXTURE), fixture) == true);
    REQUIRE(QFile::setPermissions(fixture, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) == true);
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("intx;"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    CodeFormatter formatter(controller);
    QSignalSpy finished(&formatter, &CodeFormatter::Finished);
    CodeFormatterTool tool{fixture, {QStringLiteral("success")}, profile.ExpectedVersion};
    bool shouldSucceed = false;
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
    SECTION("wrong version")
    {
        tool.ExpectedVersion = QStringLiteral("0.0.0");
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
        tool.Arguments = {QStringLiteral("timeout")};
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
    CHECK(formatter.Start(tool) == false); // One in-flight job.
    REQUIRE(finished.wait(12000) == true);
    INFO(formatter.GetMessage().toStdString());
    CHECK(finished.at(0).at(0).toBool() == shouldSucceed);
    CHECK(formatter.IsRunning() == false);
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
    tool = {fixture, {QStringLiteral("success")}, profile.ExpectedVersion};
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
