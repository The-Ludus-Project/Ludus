#include "internal/code_text_detect.h"

#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <string_view>

// Encoding, line-ending and indentation detection for the file document host
// (CE04, CE22). Pure over the loaded bytes: the background loader calls this
// before any widget exists. UTF-8 validity is proven through FoundationStrings'
// ValidateUtf8 so the same boundary rule applies as in the document seam.

namespace ludus::editor
{
using foundation::StringStatus;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
using foundation::Utf8View;
using foundation::ValidateUtf8;

namespace
{
/// Count LF and CRLF endings, decide the dominant kind and the line count. The
/// scan treats a lone CR (old Mac) as a non-ending byte; only LF and CRLF count,
/// matching what the host writes. A file with both LF and CRLF is Mixed.
void detectLineEndings(std::string_view text, CodeTextProfile& outProfile)
{
    usize lineFeedCount = 0;
    usize carriageReturnLineFeedCount = 0;
    const usize size = text.size();
    for (usize index = 0; index < size; ++index)
    {
        if (text[index] == '\n')
        {
            if (index > 0 && text[index - 1] == '\r')
            {
                ++carriageReturnLineFeedCount;
            }
            else
            {
                ++lineFeedCount;
            }
        }
    }
    const usize endingCount = lineFeedCount + carriageReturnLineFeedCount;
    outProfile.LineCount = endingCount + 1;
    if (lineFeedCount > 0 && carriageReturnLineFeedCount > 0)
    {
        outProfile.LineEnding = CodeLineEnding::Mixed;
    }
    else if (carriageReturnLineFeedCount > 0)
    {
        outProfile.LineEnding = CodeLineEnding::CrLf;
    }
    else
    {
        outProfile.LineEnding = CodeLineEnding::Lf;
    }
    outProfile.LineEndingSource = CodeTextProfileSource::Detected;
}

/// Detect the indentation style and width from the first indented lines. Spaces
/// win ties; the width is the most common positive leading-space run, clamped to
/// the accepted range. A file with only tab indentation reports Tabs.
void detectIndentation(std::string_view text, CodeTextProfile& outProfile)
{
    usize spaceIndentedLines = 0;
    usize tabIndentedLines = 0;
    // Histogram of leading-space widths in [MIN, MAX]; index 0 unused.
    usize widthVotes[CODE_INDENT_WIDTH_MAX + 1] = {};
    const usize size = text.size();
    usize index = 0;
    while (index < size)
    {
        usize spaceRun = 0;
        bool hasTab = false;
        usize cursor = index;
        while (cursor < size && (text[cursor] == ' ' || text[cursor] == '\t'))
        {
            if (text[cursor] == '\t')
            {
                hasTab = true;
            }
            else if (hasTab == false)
            {
                ++spaceRun;
            }
            ++cursor;
        }
        const bool isBlankOrLineBreak = cursor >= size || text[cursor] == '\n' || text[cursor] == '\r';
        if (isBlankOrLineBreak == false)
        {
            if (hasTab == true && spaceRun == 0)
            {
                ++tabIndentedLines;
            }
            else if (spaceRun > 0)
            {
                ++spaceIndentedLines;
                if (spaceRun >= CODE_INDENT_WIDTH_MIN && spaceRun <= CODE_INDENT_WIDTH_MAX)
                {
                    ++widthVotes[spaceRun];
                }
            }
        }
        // Advance to the next line.
        while (cursor < size && text[cursor] != '\n')
        {
            ++cursor;
        }
        index = cursor + 1;
    }
    if (tabIndentedLines > spaceIndentedLines)
    {
        outProfile.Indent.Style = CodeIndentStyle::Tabs;
        outProfile.Indent.Width = CODE_INDENT_WIDTH_DEFAULT;
    }
    else
    {
        outProfile.Indent.Style = CodeIndentStyle::Spaces;
        uint32 bestWidth = CODE_INDENT_WIDTH_DEFAULT;
        usize bestVotes = 0;
        for (usize width = CODE_INDENT_WIDTH_MIN; width <= CODE_INDENT_WIDTH_MAX; ++width)
        {
            if (widthVotes[width] > bestVotes)
            {
                bestVotes = widthVotes[width];
                bestWidth = static_cast<uint32>(width);
            }
        }
        outProfile.Indent.Width = bestWidth;
    }
    outProfile.Indent.Source = CodeTextProfileSource::Detected;
}

/// Apply the size and line-count thresholds (CE22) once the text is known.
void classifySize(usize editableByteCount, CodeTextProfile& outProfile)
{
    CodeFileSizeMode mode = ClassifyCodeFileSize(editableByteCount);
    if (mode == CodeFileSizeMode::Normal && outProfile.LineCount > CODE_FILE_NORMAL_LIMIT_LINES)
    {
        mode = CodeFileSizeMode::Large;
    }
    outProfile.SizeMode = mode;
    if (mode == CodeFileSizeMode::VeryLarge || mode == CodeFileSizeMode::Refused)
    {
        outProfile.IsReadOnly = true;
    }
}
} // namespace

usize DetectCodeTextProfile(const char* bytes, usize byteCount, CodeTextProfile& outProfile) noexcept
{
    outProfile = CodeTextProfile{};
    usize bomLength = 0;
    const char* content = bytes;
    usize contentCount = byteCount;
    if (byteCount >= CODE_UTF8_BOM_LENGTH && bytes != nullptr && bytes[0] == CODE_UTF8_BOM_0 &&
        bytes[1] == CODE_UTF8_BOM_1 && bytes[2] == CODE_UTF8_BOM_2)
    {
        bomLength = CODE_UTF8_BOM_LENGTH;
        content = bytes + CODE_UTF8_BOM_LENGTH;
        contentCount = byteCount - CODE_UTF8_BOM_LENGTH;
        outProfile.Encoding = CodeTextEncoding::Utf8Bom;
    }
    else
    {
        outProfile.Encoding = CodeTextEncoding::Utf8;
    }
    const std::string_view text(content == nullptr ? "" : content, contentCount);
    Utf8View proof;
    usize errorOffset = 0;
    if (ValidateUtf8(text, proof, errorOffset) != StringStatus::Ok)
    {
        outProfile.Encoding = CodeTextEncoding::InvalidUtf8;
        outProfile.IsReadOnly = true;
        // Size still governs whether an invalid file can be shown at all.
        classifySize(contentCount, outProfile);
        return bomLength;
    }
    detectLineEndings(text, outProfile);
    detectIndentation(text, outProfile);
    classifySize(contentCount, outProfile);
    return bomLength;
}
} // namespace ludus::editor
