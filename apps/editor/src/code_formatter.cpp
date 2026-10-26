#include "internal/code_formatter.h"

#include "internal/code_text.h"
#include "ludus_editor_formatter_config.h"

#include <ludus/foundation/base/parse_number.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

// Thanks to LLVM, "ClangFormat", Standalone Tool, and "Clang-Format Style
// Options": use stdin and byte-based replacement XML, keeping files and undo
// publication owned by Ludus. Parsing yields between slices rather than blocking
// the GUI. Slang support is a conservative C++-syntax profile, not a Slang parser.
// https://clang.llvm.org/docs/ClangFormat.html#standalone-tool
// https://clang.llvm.org/docs/ClangFormatStyleOptions.html
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 FORMAT_TIMEOUT_MILLISECONDS = 10000;
constexpr int64 DECODE_SLICE_MILLISECONDS = 2;
#if !defined(Q_OS_WASM)
// Transport limits for the clang-format child process; the browser has none.
constexpr isize MAX_WIRE_BYTES = isize{8} * 1024 * 1024;
constexpr isize MAX_ERROR_BYTES = isize{8} * 1024;
#endif
constexpr int32 VERSION_COMPONENTS = 3;
const QString FORMATTER_SETTING = QStringLiteral("codeEditor/clangFormat");

usize MapOffset(const QList<CodeFormatReplacement>& replacements, usize offset) noexcept
{
    usize original = 0;
    usize output = 0;
    for (const CodeFormatReplacement& replacement : replacements)
    {
        if (offset < replacement.StartBytes)
        {
            return output + offset - original;
        }
        output += replacement.StartBytes - original;
        if (offset <= replacement.StartBytes + replacement.RemovedBytes)
        {
            return offset == replacement.StartBytes + replacement.RemovedBytes
                       ? output + static_cast<usize>(replacement.Inserted.size())
                       : output +
                             qMin(offset - replacement.StartBytes, static_cast<usize>(replacement.Inserted.size()));
        }
        output += static_cast<usize>(replacement.Inserted.size());
        original = replacement.StartBytes + replacement.RemovedBytes;
    }
    return output + offset - original;
}

struct ToolVersion final
{
    uint64 Major = 0;
    uint64 Minor = 0;
    uint64 Patch = 0;
};

// Parse exactly MAJOR.MINOR.PATCH. Distribution suffixes after '-' or '(' are
// permitted; any other trailing text (for example 23.1.30x) is rejected.
bool ParseToolVersion(const QString& token, ToolVersion& outVersion)
{
    isize end = token.size();
    const isize dash = token.indexOf(QLatin1Char('-'));
    const isize parenthesis = token.indexOf(QLatin1Char('('));
    end = dash >= 0 ? qMin(end, dash) : end;
    end = parenthesis >= 0 ? qMin(end, parenthesis) : end;
    const QStringList components = token.left(end).split(QLatin1Char('.'));
    if (components.size() != VERSION_COMPONENTS)
    {
        return false;
    }
    const auto parseComponent = [&components](int32 index, uint64& outValue) {
        const QByteArray digits = components.at(index).toLatin1();
        return ParseUint64(digits.constData(), static_cast<usize>(digits.size()), outValue) ==
               NumberParseStatus::Success;
    };
    ToolVersion version;
    if (parseComponent(0, version.Major) == false || parseComponent(1, version.Minor) == false ||
        parseComponent(2, version.Patch) == false)
    {
        return false;
    }
    outVersion = version;
    return true;
}

// Parse only the version token after LLVM's explicit label, not a substring
// which could accept 123.1.30 for 23.1.3. As in the init tool checks
// (scripts/python/engine.py), the pin is a minimum within its LLVM major, so
// the reference hosts' newer point releases of that major remain usable.
bool HasCompatibleVersion(const QByteArray& bytes, const QString& minimum)
{
    const QString text = QString::fromUtf8(bytes);
    const QString label = QStringLiteral("clang-format version ");
    const isize start = text.indexOf(label);
    ToolVersion required;
    ToolVersion actual;
    if (start < 0 || ParseToolVersion(minimum, required) == false ||
        ParseToolVersion(text.mid(start + label.size()).section(QLatin1Char(' '), 0, 0).trimmed(), actual) == false)
    {
        return false;
    }
    if (actual.Major != required.Major)
    {
        return false;
    }
    return actual.Minor > required.Minor || (actual.Minor == required.Minor && actual.Patch >= required.Patch);
}
} // namespace

CodeTextSelection CodeFormatResult::MapSelection(CodeTextSelection selection) const noexcept
{
    // A caret in replaced whitespace is clamped to a valid scalar/CRLF boundary.
    const auto mapBoundary = [this](usize offset) {
        usize mapped = qMin(MapOffset(Replacements, offset), static_cast<usize>(Text.size()));
        while (mapped > 0 && IsCodeScalarBoundary(Text, mapped) == false)
        {
            --mapped;
        }
        if (mapped > 0 && mapped < static_cast<usize>(Text.size()) && Text.at(static_cast<isize>(mapped - 1)) == '\r' &&
            Text.at(static_cast<isize>(mapped)) == '\n')
        {
            --mapped;
        }
        return mapped;
    };
    return {mapBoundary(selection.AnchorBytes), mapBoundary(selection.CaretBytes)};
}

CodeFormatter::CodeFormatter(CodeDocumentController& inoutDocument, QObject* parentOrNull)
    : QObject(parentOrNull), mDocument(inoutDocument)
{
    mTimeout.setSingleShot(true);
    mDecodeTimer.setSingleShot(true);
    connect(&mDecodeTimer, &QTimer::timeout, this, &CodeFormatter::decodeSlice);
    connect(&mTimeout, &QTimer::timeout, this, [this]() {
        finish(false, QStringLiteral("Formatting timed out; source was retained."));
    });
    connect(&mDocument, &CodeDocumentController::BindingChanged, this, &CodeFormatter::Cancel);
#if !defined(Q_OS_WASM)
    connect(&mProcess, &QProcess::readyReadStandardOutput, this, &CodeFormatter::readOutput);
    connect(&mProcess, &QProcess::readyReadStandardError, this, &CodeFormatter::readOutput);
    connect(&mProcess, &QProcess::started, this, [this]() {
        if (mPhase == Phase::Format)
        {
            mProcess.write(mCapture.Text);
        }
        mProcess.closeWriteChannel();
    });
    connect(&mProcess, &QProcess::finished, this, [this](int32 exitCode, QProcess::ExitStatus status) {
        processFinished(exitCode, status == QProcess::CrashExit);
    });
    connect(&mProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            if (mPhase == Phase::Stopping)
            {
                mPhase = Phase::Version;
                finish(false, mMessage);
                return;
            }
            finish(false, QStringLiteral("Formatter could not start: ") + mProcess.errorString());
        }
    });
#endif
}

CodeFormatter::~CodeFormatter()
{
    // Do not let QObject teardown call slots through a half-destroyed controller.
    disconnect();
#if !defined(Q_OS_WASM)
    mProcess.disconnect(this);
    mProcess.kill();
#endif
}

bool CodeFormatter::IsAvailable() noexcept
{
#if defined(Q_OS_WASM)
    return false;
#else
    return true;
#endif
}

bool CodeFormatter::IsRunning() const noexcept
{
    return mPhase != Phase::Idle;
}

QString CodeFormatter::GetMessage() const
{
    return mMessage;
}

bool CodeFormatter::Start(const CodeFormatterTool& tool)
{
    if (IsAvailable() == false || IsRunning() == true || mDocument.IsBound() == false || tool.Program.isEmpty() == true)
    {
        mMessage = QStringLiteral("Formatting is unavailable; choose the pinned clang-format executable.");
        return false;
    }
    mCapture = mDocument.Capture();
    mTool = tool;
    mResult = {};
    mResult.Identity = mCapture.Identity;
    mOutput.clear();
    mErrors.clear();
    mMessage = QStringLiteral("Formatting source...");
    mPhase = Phase::Version;
    mTimeout.start(FORMAT_TIMEOUT_MILLISECONDS);
#if !defined(Q_OS_WASM)
    mProcess.start(mTool.Program, {QStringLiteral("--version")});
#endif
    return true;
}

void CodeFormatter::Cancel()
{
    if (IsRunning() == true)
    {
        finish(false, QStringLiteral("Formatting cancelled; source was retained."));
    }
}

void CodeFormatter::finish(bool isSuccessful, const QString& message)
{
    mTimeout.stop();
    mDecodeTimer.stop();
    mMessage = message;
    if (mPhase == Phase::Idle || mPhase == Phase::Stopping)
    {
        return;
    }
#if !defined(Q_OS_WASM)
    if (mProcess.state() != QProcess::NotRunning)
    {
        mPhase = Phase::Stopping;
        mProcess.kill();
        // Reaping is asynchronous. A new request cannot share this process yet.
        return;
    }
#endif
    mPhase = Phase::Idle;
    mOutput.clear();
    mReader.clear();
    mCapture.Text.clear();
    mResult = {};
    Q_EMIT Finished(isSuccessful, mMessage);
}

void CodeFormatter::readOutput()
{
#if !defined(Q_OS_WASM)
    if (mPhase == Phase::Stopping || mPhase == Phase::Idle)
    {
        mProcess.readAllStandardOutput();
        mProcess.readAllStandardError();
        return;
    }
    const QByteArray output = mProcess.readAllStandardOutput();
    const QByteArray errors = mProcess.readAllStandardError();
    if (output.size() > MAX_WIRE_BYTES - mOutput.size() || errors.size() > MAX_ERROR_BYTES - mErrors.size())
    {
        finish(false, QStringLiteral("Formatter output exceeded its transport limit; source was retained."));
        return;
    }
    mOutput += output;
    mErrors += errors;
#endif
}

void CodeFormatter::processFinished(int32 exitCode, bool hasCrashed)
{
    if (mPhase == Phase::Stopping)
    {
        mPhase = Phase::Decode;
        finish(false, mMessage);
        return;
    }
    readOutput();
    if (mPhase == Phase::Idle)
    {
        return;
    }
    if (exitCode != 0 || hasCrashed == true)
    {
        finish(false, QStringLiteral("Formatter failed; source was retained. ") + QString::fromUtf8(mErrors).trimmed());
        return;
    }
    if (mPhase == Phase::Version)
    {
        if (HasCompatibleVersion(mOutput, mTool.MinimumVersion) == false)
        {
            finish(false,
                   QStringLiteral("Formatting requires clang-format ") + mTool.MinimumVersion +
                       QStringLiteral(" or a newer release of the same major version."));
            return;
        }
        mOutput.clear();
        mErrors.clear();
        mPhase = Phase::Format;
#if !defined(Q_OS_WASM)
        mProcess.start(mTool.Program, mTool.Arguments);
#endif
        return;
    }
    mReader.clear();
    mReader.addData(mOutput);
    mCopiedBytes = 0;
    mHasRoot = false;
    mHasClosedRoot = false;
    mPhase = Phase::Decode;
    mDecodeTimer.start(0);
}

void CodeFormatter::decodeSlice()
{
    QElapsedTimer slice;
    slice.start();
    while (mReader.atEnd() == false && slice.elapsed() < DECODE_SLICE_MILLISECONDS)
    {
        const QXmlStreamReader::TokenType token = mReader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference)
        {
            finish(false, QStringLiteral("Formatter returned unsupported XML; source was retained."));
            return;
        }
        if (token == QXmlStreamReader::Characters && mReader.isWhitespace() == false)
        {
            finish(false, QStringLiteral("Formatter returned text outside a replacement; source was retained."));
            return;
        }
        if (mReader.isStartElement() == true)
        {
            if (mHasRoot == false && mReader.name() == QLatin1StringView("replacements"))
            {
                mHasRoot = true;
                if (mReader.attributes().value(QLatin1StringView("incomplete_format")) != QLatin1StringView("false"))
                {
                    finish(false, QStringLiteral("Formatter could not parse the entire source; source was retained."));
                    return;
                }
            }
            else if (mHasRoot == true && mHasClosedRoot == false && mReader.name() == QLatin1StringView("replacement"))
            {
                const QByteArray offsetText = mReader.attributes().value(QLatin1StringView("offset")).toLatin1();
                const QByteArray lengthText = mReader.attributes().value(QLatin1StringView("length")).toLatin1();
                uint64 start = 0;
                uint64 length = 0;
                const bool isStartValid =
                    ParseUint64(offsetText.constData(), static_cast<usize>(offsetText.size()), start) ==
                    NumberParseStatus::Success;
                const bool isLengthValid =
                    ParseUint64(lengthText.constData(), static_cast<usize>(lengthText.size()), length) ==
                    NumberParseStatus::Success;
                const usize inputSize = static_cast<usize>(mCapture.Text.size());
                if (isStartValid == false || isLengthValid == false || start < mCopiedBytes || start > inputSize ||
                    length > inputSize - start)
                {
                    finish(false,
                           QStringLiteral("Formatter returned invalid replacement ranges; source was retained."));
                    return;
                }
                if (IsCodeScalarBoundary(mCapture.Text, static_cast<usize>(start)) == false ||
                    IsCodeScalarBoundary(mCapture.Text, static_cast<usize>(start + length)) == false)
                {
                    finish(false, QStringLiteral("Formatter split a Unicode boundary; source was retained."));
                    return;
                }
                const QByteArray inserted =
                    mReader.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement).toUtf8();
                const usize unchanged = static_cast<usize>(start) - mCopiedBytes;
                const usize used = static_cast<usize>(mResult.Text.size());
                if (used > mCapture.LimitBytes || unchanged > mCapture.LimitBytes - used ||
                    static_cast<usize>(inserted.size()) > mCapture.LimitBytes - used - unchanged)
                {
                    finish(false, QStringLiteral("Formatted source exceeds the document limit; source was retained."));
                    return;
                }
                mResult.Text.append(mCapture.Text.constData() + mCopiedBytes, static_cast<isize>(unchanged));
                mResult.Text += inserted;
                mResult.Replacements.append({static_cast<usize>(start), static_cast<usize>(length), inserted});
                mCopiedBytes = static_cast<usize>(start + length);
            }
            else
            {
                finish(false, QStringLiteral("Formatter returned an unexpected XML element; source was retained."));
                return;
            }
        }
        else if (mReader.isEndElement() == true && mReader.name() == QLatin1StringView("replacements"))
        {
            mHasClosedRoot = true;
        }
    }
    if (mReader.atEnd() == false)
    {
        mDecodeTimer.start(0);
        return;
    }
    const usize suffix = static_cast<usize>(mCapture.Text.size()) - mCopiedBytes;
    if (mReader.hasError() == true || mHasClosedRoot == false ||
        suffix > mCapture.LimitBytes - static_cast<usize>(mResult.Text.size()))
    {
        finish(false, QStringLiteral("Formatter returned invalid or oversized output; source was retained."));
        return;
    }
    mResult.Text.append(mCapture.Text.constData() + mCopiedBytes, static_cast<isize>(suffix));
    const CodeTextSelection selection = mResult.MapSelection(mDocument.CaptureSelection());
    const CodeEditStatus status = mDocument.ApplyFormattedText(mResult.Identity, mResult.Text, selection);
    finish(status == CodeEditStatus::Ok,
           status == CodeEditStatus::Ok
               ? QStringLiteral("Source formatted; Save publishes the draft.")
               : QStringLiteral("Formatting result was rejected because the source changed or output was invalid."));
}

CodeFormatterTool MakeShaderFormatter(const QString& program, const QString& sourcePath)
{
    return {program,
            {QStringLiteral("--style=file"),
             QStringLiteral("--assume-filename=") + sourcePath,
             QStringLiteral("--fail-on-incomplete-format"),
             QStringLiteral("--sort-includes=false"),
             QStringLiteral("--qualifier-alignment=Leave"),
             QStringLiteral("--output-replacements-xml")},
            QString::fromLatin1(formatter_config::VERSION)};
}

QString FindShaderFormatter()
{
    QString configured = QSettings().value(FORMATTER_SETTING).toString();
    if (configured.isEmpty() == false)
    {
        return configured;
    }
    QString built = QString::fromUtf8(formatter_config::DEFAULT_PROGRAM);
    if (QFileInfo(built).isExecutable() == true)
    {
        return built;
    }
    QString program = QStandardPaths::findExecutable(QStringLiteral("clang-format-23"));
    return program.isEmpty() == true ? QStandardPaths::findExecutable(QStringLiteral("clang-format")) : program;
}
} // namespace ludus::editor
