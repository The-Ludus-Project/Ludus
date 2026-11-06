#include "internal/lusl_formatting.h"

#include <ludus/foundation/base/core.h>

#include <ludus/tooling/shader_language/formatter.hpp>

#include "internal/code_document_controller.h"

#include <string_view>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace language = tooling::shader_language;

[[nodiscard]] CodeTextSelection ClampSelection(CodeTextSelection selection, usize size) noexcept
{
    selection.AnchorBytes = selection.AnchorBytes < size ? selection.AnchorBytes : size;
    selection.CaretBytes = selection.CaretBytes < size ? selection.CaretBytes : size;
    return selection;
}
} // namespace

QString FormatLuslDocument(CodeDocumentController& inoutDocument)
{
    if (inoutDocument.IsBound() == false)
    {
        return QStringLiteral("Open a LUSL source to format it.");
    }
    const CodeTextCapture capture = inoutDocument.Capture();
    const std::string_view text(capture.Text.constData(), static_cast<usize>(capture.Text.size()));
    language::FormatResult result;
    if (language::FormatSource(text, result) != language::Status::Ready)
    {
        const QString reason = result.Diagnostics.IsEmpty() == true
                                   ? QStringLiteral("it does not lex")
                                   : QString::fromUtf8(result.Diagnostics[0].Message.GetView().data(),
                                                       static_cast<isize>(result.Diagnostics[0].Message.GetSize()));
        return QStringLiteral("LUSL could not format this source (%1); the source was retained. Fix the error and "
                              "format again.")
            .arg(reason);
    }
    const std::string_view formatted(result.Text.GetData(), result.Text.GetSize());
    if (formatted == text)
    {
        return QStringLiteral("Source is already formatted.");
    }
    if (formatted.size() > capture.LimitBytes)
    {
        return QStringLiteral("The formatted source would exceed the file limit; the source was retained.");
    }
    const CodeTextSelection before = inoutDocument.CaptureSelection();
    const CodeTextSelection after = ClampSelection(before, formatted.size());
    const CodeTextEdit edit =
    {
        .StartBytes = 0,
        .RemovedBytes = text.size(),
        .Inserted = {formatted.data(), formatted.size()},
    };
    const CodeTextSelection beforeSelections[] = {before};
    const CodeTextSelection afterSelections[] = {after};
    const CodeEditSet set =
    {
        .Edits = {&edit, 1},
        .Before = {{beforeSelections, 1}},
        .After = {{afterSelections, 1}},
        .Transaction = {},
    };
    return inoutDocument.ApplyEditSet(capture.Identity, set) == CodeEditStatus::Ok
               ? QStringLiteral("Source formatted; Save publishes the draft.")
               : QStringLiteral("Formatting was rejected because the source changed; the source was retained.");
}
} // namespace ludus::editor
