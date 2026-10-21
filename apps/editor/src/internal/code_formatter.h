#pragma once

#include "internal/code_document_controller.h"

#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QXmlStreamReader>
#if !defined(Q_OS_WASM)
#    include <QProcess>
#endif

namespace ludus::editor
{
/// Language/tool profile. Arguments are passed directly; never interpreted by a shell.
/// The first provider uses clang-format replacement XML; future scripting providers
/// can supply their own profile/decoder without changing document/history ownership.
struct CodeFormatterTool final
{
    QString Program;
    QStringList Arguments;
    QString ExpectedVersion;
};

/// Replacement positions refer to the captured UTF-8 source, in ascending order.
struct CodeFormatReplacement final
{
    foundation::usize StartBytes = 0;
    foundation::usize RemovedBytes = 0;
    QByteArray Inserted;
};

/// Successful output owns all bytes. Identity is checked again at publication.
struct CodeFormatResult final
{
    CodeDocumentIdentity Identity;
    QByteArray Text;
    QList<CodeFormatReplacement> Replacements;
    [[nodiscard]] CodeTextSelection MapSelection(CodeTextSelection selection) const noexcept;
};

/// One bounded asynchronous native formatter process. GUI thread only. Never
/// opens/saves a source file; browser capability is explicitly unavailable.
/// Destruction cancels work; the document controller must outlive this object.
class CodeFormatter final : public QObject
{
    Q_OBJECT
public:
    explicit CodeFormatter(CodeDocumentController& inoutDocument, QObject* parentOrNull = nullptr);
    ~CodeFormatter() override;
    /// False if unavailable, unbound or already running. A successful launch
    /// reports completion through Finished; failure leaves the document untouched.
    [[nodiscard]] bool Start(const CodeFormatterTool& tool);
    void Cancel();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] static bool IsAvailable() noexcept;
    [[nodiscard]] QString GetMessage() const;

Q_SIGNALS:
    void Finished(bool isSuccessful, const QString& message);

private:
    enum class Phase : foundation::uint8
    {
        Idle,
        Version,
        Format,
        Decode,
        Stopping
    };
    void readOutput();
    void processFinished(foundation::int32 exitCode, bool hasCrashed);
    void decodeSlice();
    void finish(bool isSuccessful, const QString& message);
    CodeDocumentController& mDocument;
    CodeFormatterTool mTool;
    CodeTextCapture mCapture;
    CodeFormatResult mResult;
    QByteArray mOutput;
    QByteArray mErrors;
    QString mMessage;
    QXmlStreamReader mReader;
    QTimer mTimeout;
    QTimer mDecodeTimer;
    Phase mPhase = Phase::Idle;
    foundation::usize mCopiedBytes = 0;
    bool mHasRoot = false;
    bool mHasClosedRoot = false;
#if !defined(Q_OS_WASM)
    QProcess mProcess;
#endif
};

/// Pinned Slang formatting profile; sourcePath locates the authored style file.
[[nodiscard]] CodeFormatterTool MakeShaderFormatter(const QString& program, const QString& sourcePath);
/// Finds the configured host tool without installation or other side effects.
[[nodiscard]] QString FindShaderFormatter();
} // namespace ludus::editor
