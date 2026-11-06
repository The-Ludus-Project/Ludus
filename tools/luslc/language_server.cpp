// The LUSL language server (docs/modules/tooling/shader-language.md#language-server).
//
// Thanks to Microsoft, Language Server Protocol 3.17 (Base Protocol, text synchronization,
// publishDiagnostics, definition, hover, documentSymbol, formatting and completion),
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/,
// whose message shapes this follows. Positions are UTF-16, the protocol's default encoding.

#include "language_server.hpp"

#include <ludus/foundation/parsing/json.hpp>
#include <ludus/tooling/shader_language/formatter.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

namespace ludus::tools::luslc
{
namespace
{
namespace parsing = ludus::foundation::parsing;

/// Largest message body; a whole snapshot fits with its JSON escaping.
constexpr usize MAX_MESSAGE_BYTES = usize{2} * compiler::MAX_SOURCE_BYTES;
/// Largest header block of one message.
constexpr usize MAX_HEADER_BYTES = 8192;
/// Bytes read from standard input per call.
constexpr usize READ_CHUNK_BYTES = 65536;
/// Open documents the server tracks; each joins the snapshot of the others.
constexpr usize MAX_DOCUMENTS = compiler::MAX_SOURCE_FILES;
/// JSON-RPC error codes.
constexpr std::string_view METHOD_NOT_FOUND_CODE = "-32601";
constexpr std::string_view INVALID_REQUEST_CODE = "-32600";
/// LSP DiagnosticSeverity.
constexpr uint64 SEVERITY_ERROR = 1;
constexpr uint64 SEVERITY_WARNING = 2;
/// LSP TextDocumentSyncKind.Incremental.
constexpr uint64 SYNC_INCREMENTAL = 2;
/// LSP SymbolKind.
constexpr uint64 SYMBOL_METHOD = 6;
constexpr uint64 SYMBOL_FIELD = 8;
constexpr uint64 SYMBOL_ENUM = 10;
constexpr uint64 SYMBOL_INTERFACE = 11;
constexpr uint64 SYMBOL_FUNCTION = 12;
constexpr uint64 SYMBOL_VARIABLE = 13;
constexpr uint64 SYMBOL_CONSTANT = 14;
constexpr uint64 SYMBOL_ENUM_MEMBER = 22;
constexpr uint64 SYMBOL_STRUCT = 23;
/// LSP CompletionItemKind.
constexpr uint64 COMPLETION_FUNCTION = 3;
constexpr uint64 COMPLETION_FIELD = 5;
constexpr uint64 COMPLETION_VARIABLE = 6;
constexpr uint64 COMPLETION_INTERFACE = 8;
constexpr uint64 COMPLETION_ENUM = 13;
constexpr uint64 COMPLETION_KEYWORD = 14;
constexpr uint64 COMPLETION_CONSTANT = 21;
constexpr uint64 COMPLETION_STRUCT = 22;
/// Code units of a code point outside the Basic Multilingual Plane in UTF-16.
constexpr uint32 SURROGATE_PAIR_UNITS = 2;
/// UTF-8 lead byte masks.
constexpr uint8 CONTINUATION_MASK = 0xC0U;
constexpr uint8 CONTINUATION_BYTE = 0x80U;
constexpr uint8 FOUR_BYTE_LEAD = 0xF0U;
/// Hexadecimal digits of a percent escape.
constexpr uint32 HEXADECIMAL_BASE = 16;
constexpr uint32 DECIMAL_DIGIT_LIMIT = 10;
constexpr std::string_view FILE_SCHEME = "file://";
/// Characters a file URI keeps unescaped.
constexpr std::string_view URI_SAFE = "/-._~";
/// Scalar spellings offered by completion, with their vector and matrix forms.
constexpr std::string_view SCALAR_NAMES[] =
    {"bool", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float16", "float32", "float64"};
constexpr std::string_view RESOURCE_NAMES[] =
    {"Texture2D", "RWTexture2D", "SamplerState", "ConstantBuffer", "StructuredBuffer", "RWStructuredBuffer"};
constexpr uint32 MIN_COMPONENTS = 2;
constexpr uint32 MAX_COMPONENTS = 4;

[[nodiscard]] bool IsIdentifierByte(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
           value == '_';
}

[[nodiscard]] uint32 GetHexadecimalValue(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return static_cast<uint32>(value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return static_cast<uint32>(value - 'a') + DECIMAL_DIGIT_LIMIT;
    }
    if (value >= 'A' && value <= 'F')
    {
        return static_cast<uint32>(value - 'A') + DECIMAL_DIGIT_LIMIT;
    }
    return HEXADECIMAL_BASE;
}

[[nodiscard]] bool AppendText(std::string_view text, Array<char>& inoutText) noexcept
{
    return inoutText.TryAddRange(std::span<const char>(text.data(), text.size())) == true;
}

[[nodiscard]] std::string_view GetView(const Array<char>& text) noexcept
{
    return std::string_view(text.GetData(), text.GetSize());
}

/// The local path of a `file://` URI, percent-decoded; false for other schemes.
[[nodiscard]] bool DecodeFileUri(std::string_view uri, Array<char>& outPath) noexcept
{
    outPath.Clear();
    if (uri.starts_with(FILE_SCHEME) == false)
    {
        return false;
    }
    uri.remove_prefix(FILE_SCHEME.size());
    // `file://host/path`: only the empty host (`file:///path`) names a local file.
    if (uri.empty() == true || uri.front() != '/')
    {
        return false;
    }
    for (usize i = 0; i < uri.size(); ++i)
    {
        char value = uri[i];
        if (value == '%' && i + 2 < uri.size())
        {
            const uint32 high = GetHexadecimalValue(uri[i + 1]);
            const uint32 low = GetHexadecimalValue(uri[i + 2]);
            if (high < HEXADECIMAL_BASE && low < HEXADECIMAL_BASE)
            {
                value = static_cast<char>(high * HEXADECIMAL_BASE + low);
                i += 2;
            }
        }
        if (outPath.TryAdd(value) == false)
        {
            return false;
        }
    }
    return true;
}

/// A `file://` URI of the local path `path`.
[[nodiscard]] bool EncodeFileUri(std::string_view path, Array<char>& outUri) noexcept
{
    constexpr std::string_view DIGITS = "0123456789ABCDEF";
    outUri.Clear();
    bool isOk = AppendText(FILE_SCHEME, outUri);
    for (const char value : path)
    {
        if (IsIdentifierByte(value) == true || URI_SAFE.find(value) != std::string_view::npos)
        {
            isOk = isOk == true && outUri.TryAdd(value) == true;
            continue;
        }
        const uint8 byte = static_cast<uint8>(value);
        isOk = isOk == true && outUri.TryAdd('%') == true && outUri.TryAdd(DIGITS[byte / HEXADECIMAL_BASE]) == true &&
               outUri.TryAdd(DIGITS[byte % HEXADECIMAL_BASE]) == true;
    }
    return isOk;
}

/// A protocol position: zero-based line and UTF-16 column.
struct Position final
{
    uint64 Line = 0;
    uint64 Character = 0;
};

/// The position of byte `offset` in `text`.
[[nodiscard]] Position GetPosition(std::string_view text, usize offset) noexcept
{
    Position position;
    usize i = 0;
    while (i < offset && i < text.size())
    {
        const uint8 byte = static_cast<uint8>(text[i]);
        if (byte == '\n')
        {
            ++position.Line;
            position.Character = 0;
        }
        else if ((byte & CONTINUATION_MASK) != CONTINUATION_BYTE)
        {
            position.Character += byte >= FOUR_BYTE_LEAD ? SURROGATE_PAIR_UNITS : 1;
        }
        ++i;
    }
    return position;
}

/// The byte offset of `position` in `text`, clamped to the line's end.
[[nodiscard]] usize GetOffset(std::string_view text, const Position& position) noexcept
{
    usize offset = 0;
    for (uint64 line = 0; line < position.Line && offset < text.size(); ++offset)
    {
        line += text[offset] == '\n' ? 1 : 0;
    }
    uint64 character = 0;
    while (offset < text.size() && text[offset] != '\n' && character < position.Character)
    {
        const uint8 byte = static_cast<uint8>(text[offset]);
        character += byte >= FOUR_BYTE_LEAD ? SURROGATE_PAIR_UNITS : 1;
        ++offset;
        while (offset < text.size() && (static_cast<uint8>(text[offset]) & CONTINUATION_MASK) == CONTINUATION_BYTE)
        {
            ++offset;
        }
    }
    return offset;
}

/// A request or notification: its id (absent for notifications) and parameters.
struct Message final
{
    parsing::JsonValue Id;
    parsing::JsonValue Parameters;
};

/// JSON-RPC error codes the server answers with.
enum class ErrorCode : uint8
{
    InvalidRequest,
    MethodNotFound,
};

/// One open document.
struct Document final
{
    Array<char> Uri;
    /// Path relative to the workspace root, as the snapshot names it.
    Array<char> Path;
    Array<char> Text;
    bool IsInsideRoot = false;
};

/// The front-end results of the last analyzed document.
struct Analysis final
{
    Snapshot Files;
    language::PreprocessRequest Request;
    language::PreprocessResult Preprocessed;
    language::SyntaxTree Tree;
    language::SemanticModel Model;
    bool HasTree = false;
    bool HasModel = false;
    /// Index of the analyzed document in mDocuments, or MAX_DOCUMENTS.
    usize Document = MAX_DOCUMENTS;
};

class LanguageServer final
{
public:
    explicit LanguageServer(const LanguageServerOptions& options) noexcept : mOptions(options) {}

    [[nodiscard]] int Run() noexcept
    {
        if (mOutput.TryResize(MAX_MESSAGE_BYTES) == false || mAnalysis.Files.Initialize() == false)
        {
            WriteError("luslc lsp: out of memory\n");
            return EXIT_FAILURE_CODE;
        }
        if (mOptions.Root.empty() == false && openRoot(mOptions.Root) == false)
        {
            WriteError("luslc lsp: the --root directory cannot be opened\n");
            return EXIT_FAILURE_CODE;
        }
        Array<char> body;
        while (mHasExited == false)
        {
            if (readMessage(body) == false)
            {
                // End of input without `exit`.
                return EXIT_FAILURE_CODE;
            }
            handleMessage(GetView(body));
        }
        return mHasShutDown == true ? EXIT_SUCCESS_CODE : EXIT_FAILURE_CODE;
    }

private:
    static constexpr int EXIT_SUCCESS_CODE = 0;
    static constexpr int EXIT_FAILURE_CODE = 1;

    // ----- Transport ------------------------------------------------------------------------------

    [[nodiscard]] bool fill() noexcept
    {
        StaticArray<char, READ_CHUNK_BYTES> chunk{};
        while (true)
        {
#if defined(LUDUS_PLATFORM_WINDOWS)
            const isize count = ::_read(STANDARD_INPUT, chunk.GetData(), static_cast<unsigned int>(chunk.GetSize()));
#else
            const isize count = ::read(STANDARD_INPUT, chunk.GetData(), chunk.GetSize());
#endif
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                return false;
            }
            return mInput.TryAddRange(std::span<const char>(chunk.GetData(), static_cast<usize>(count))) == true;
        }
    }

    /// Read one `Content-Length` framed message body.
    [[nodiscard]] bool readMessage(Array<char>& outBody) noexcept
    {
        constexpr std::string_view SEPARATOR = "\r\n\r\n";
        constexpr std::string_view LENGTH_HEADER = "Content-Length:";
        usize headerEnd = std::string_view::npos;
        while ((headerEnd = GetView(mInput).find(SEPARATOR)) == std::string_view::npos)
        {
            if (mInput.GetSize() > MAX_HEADER_BYTES || fill() == false)
            {
                return false;
            }
        }
        const std::string_view headers = GetView(mInput).substr(0, headerEnd);
        const usize field = headers.find(LENGTH_HEADER);
        if (field == std::string_view::npos)
        {
            return false;
        }
        usize length = 0;
        bool hasDigit = false;
        for (usize i = field + LENGTH_HEADER.size(); i < headers.size() && headers[i] != '\r'; ++i)
        {
            if (headers[i] >= '0' && headers[i] <= '9')
            {
                length = length * DECIMAL_BASE + static_cast<usize>(headers[i] - '0');
                hasDigit = true;
                if (length > MAX_MESSAGE_BYTES)
                {
                    return false;
                }
            }
        }
        const usize start = headerEnd + SEPARATOR.size();
        while (hasDigit == true && mInput.GetSize() < start + length)
        {
            if (fill() == false)
            {
                return false;
            }
        }
        outBody.Clear();
        if (hasDigit == false || outBody.TryAddRange(std::span<const char>(mInput.GetData() + start, length)) == false)
        {
            return false;
        }
        // Keep the bytes of the next message.
        Array<char> rest;
        if (rest.TryAddRange(
                std::span<const char>(mInput.GetData() + start + length, mInput.GetSize() - start - length)) == false)
        {
            return false;
        }
        mInput = Move(rest);
        return true;
    }

    void writeMessage(parsing::JsonWriter& inoutWriter) noexcept
    {
        std::span<const uint8> body;
        if (inoutWriter.Finish(body) != parsing::ParseStatus::Ok)
        {
            WriteError("luslc lsp: a response exceeded the message budget\n");
            return;
        }
        Line header;
        header.Append("Content-Length: ");
        header.AppendUnsigned(body.size());
        header.Append("\r\n\r\n");
        (void)WriteAll(STANDARD_OUTPUT, header.GetView());
        (void)WriteAll(STANDARD_OUTPUT, std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
    }

    [[nodiscard]] parsing::JsonWriter makeWriter() noexcept
    {
        return parsing::JsonWriter(std::span<uint8>(mOutput.GetData(), mOutput.GetSize()));
    }

    /// Start a response to `id` (its JSON value, echoed as received).
    void beginResponse(parsing::JsonWriter& inoutWriter, parsing::JsonValue id) noexcept
    {
        inoutWriter.Raw("{\"jsonrpc\":\"2.0\",\"id\":");
        writeId(inoutWriter, id);
        inoutWriter.Raw(",\"result\":");
    }

    static void writeId(parsing::JsonWriter& inoutWriter, parsing::JsonValue id) noexcept
    {
        std::string_view text;
        uint64 number = 0;
        if (id.String(text) == true)
        {
            inoutWriter.String(text);
        }
        else if (id.Integer(number) == true)
        {
            inoutWriter.Integer(number);
        }
        else
        {
            inoutWriter.Raw("null");
        }
    }

    void respondError(parsing::JsonValue id, ErrorCode error, std::string_view message) noexcept
    {
        const std::string_view code = error == ErrorCode::MethodNotFound ? METHOD_NOT_FOUND_CODE : INVALID_REQUEST_CODE;
        parsing::JsonWriter writer = makeWriter();
        writer.Raw("{\"jsonrpc\":\"2.0\",\"id\":");
        writeId(writer, id);
        writer.Raw(",\"error\":{\"code\":");
        writer.Raw(code);
        writer.Raw(",\"message\":");
        writer.String(message);
        writer.Raw("}}");
        writeMessage(writer);
    }

    void respondNull(parsing::JsonValue id) noexcept
    {
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("null}");
        writeMessage(writer);
    }

    // ----- Dispatch -------------------------------------------------------------------------------

    void handleMessage(std::string_view body) noexcept
    {
        parsing::JsonLimits limits;
        limits.MaxInputBytes = MAX_MESSAGE_BYTES;
        limits.MaxStringBytes = compiler::MAX_SOURCE_BYTES;
        limits.MaxObjectMembers = parsing::MAX_JSON_OBJECT_MEMBERS;
        limits.MaxDepth = parsing::MAX_JSON_DEPTH;
        limits.MaxWorkspaceBytes = MAX_MESSAGE_BYTES * 4; // ludus-style: magic-number yyjson node overhead per byte
        parsing::JsonDocument document;
        parsing::ParseError error;
        if (document.Read(body, error, limits) != parsing::ParseStatus::Ok || document.Root().Object() == false)
        {
            respondError(parsing::JsonValue{}, ErrorCode::InvalidRequest, "the message is not a JSON object");
            return;
        }
        const parsing::JsonValue root = document.Root();
        std::string_view method;
        const parsing::JsonValue id = root.Get("id");
        const bool isRequest = id.Valid() == true && id.Null() == false;
        if (root.Get("method").String(method) == false)
        {
            // A response to a request of ours; the server sends none.
            return;
        }
        const parsing::JsonValue parameters = root.Get("params");
        const Message request{ .Id = id, .Parameters = parameters };
        if (mHasShutDown == true && method != "exit")
        {
            if (isRequest == true)
            {
                respondError(id, ErrorCode::InvalidRequest, "the server is shut down");
            }
            return;
        }
        if (method == "initialize")
        {
            initialize(request);
        }
        else if (method == "shutdown")
        {
            mHasShutDown = true;
            respondNull(id);
        }
        else if (method == "exit")
        {
            mHasExited = true;
        }
        else if (method == "textDocument/didOpen")
        {
            didOpen(parameters);
        }
        else if (method == "textDocument/didChange")
        {
            didChange(parameters);
        }
        else if (method == "textDocument/didClose")
        {
            didClose(parameters);
        }
        else if (method == "textDocument/definition")
        {
            respondDefinition(request);
        }
        else if (method == "textDocument/hover")
        {
            respondHover(request);
        }
        else if (method == "textDocument/documentSymbol")
        {
            respondSymbols(request);
        }
        else if (method == "textDocument/formatting")
        {
            respondFormatting(request);
        }
        else if (method == "textDocument/completion")
        {
            respondCompletion(request);
        }
        else if (isRequest == true)
        {
            respondError(id, ErrorCode::MethodNotFound, "the LUSL language server does not implement this method");
        }
    }

    [[nodiscard]] bool openRoot(std::string_view path) noexcept
    {
        mRootPath.Clear();
        if (AppendText(path, mRootPath) == false)
        {
            return false;
        }
        while (mRootPath.GetSize() > 1 && mRootPath[mRootPath.GetSize() - 1] == '/')
        {
            mRootPath.RemoveLast();
        }
        mHasRoot = mRoot.Open(GetView(mRootPath)).Succeeded() == true;
        return mHasRoot;
    }

    void initialize(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        std::string_view rootUri;
        Array<char> path;
        if (mHasRoot == false && parameters.Get("rootUri").String(rootUri) == true &&
            DecodeFileUri(rootUri, path) == true)
        {
            (void)openRoot(GetView(path));
        }
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("{\"capabilities\":{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":{\"openClose\":true,"
                   "\"change\":");
        writer.Integer(SYNC_INCREMENTAL);
        writer.Raw("},\"definitionProvider\":true,\"hoverProvider\":true,\"documentSymbolProvider\":true,"
                   "\"documentFormattingProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
                   "\"serverInfo\":{\"name\":\"luslc\"}}}");
        writeMessage(writer);
    }

    // ----- Documents ------------------------------------------------------------------------------

    [[nodiscard]] usize findDocument(std::string_view uri) const noexcept
    {
        for (usize i = 0; i < mDocuments.GetSize(); ++i)
        {
            if (GetView(mDocuments[i].Uri) == uri)
            {
                return i;
            }
        }
        return MAX_DOCUMENTS;
    }

    [[nodiscard]] usize getDocument(parsing::JsonValue parameters) const noexcept
    {
        std::string_view uri;
        if (parameters.Get("textDocument").Get("uri").String(uri) == false)
        {
            return MAX_DOCUMENTS;
        }
        return findDocument(uri);
    }

    void didOpen(parsing::JsonValue parameters) noexcept
    {
        const parsing::JsonValue item = parameters.Get("textDocument");
        std::string_view uri;
        std::string_view text;
        if (item.Get("uri").String(uri) == false || item.Get("text").String(text) == false)
        {
            return;
        }
        usize index = findDocument(uri);
        if (index == MAX_DOCUMENTS)
        {
            if (mDocuments.GetSize() >= MAX_DOCUMENTS)
            {
                WriteError("luslc lsp: too many open documents\n");
                return;
            }
            Document document;
            Array<char> path;
            if (AppendText(uri, document.Uri) == false || DecodeFileUri(uri, path) == false)
            {
                return;
            }
            const std::string_view local = GetView(path);
            const std::string_view root = GetView(mRootPath);
            document.IsInsideRoot = mHasRoot == true && local.size() > root.size() + 1 &&
                                    local.starts_with(root) == true && local[root.size()] == '/';
            if (AppendText(document.IsInsideRoot == true ? local.substr(root.size() + 1) : local, document.Path) ==
                    false ||
                mDocuments.TryAdd(Move(document)) == false)
            {
                return;
            }
            index = mDocuments.GetSize() - 1;
        }
        mDocuments[index].Text.Clear();
        if (AppendText(text, mDocuments[index].Text) == true)
        {
            publish(index);
        }
    }

    void didChange(parsing::JsonValue parameters) noexcept
    {
        const usize index = getDocument(parameters);
        const parsing::JsonValue changes = parameters.Get("contentChanges");
        if (index == MAX_DOCUMENTS || changes.Array() == false)
        {
            return;
        }
        for (usize i = 0; i < changes.Count(); ++i)
        {
            const parsing::JsonValue change = changes.At(i);
            std::string_view text;
            if (change.Get("text").String(text) == false)
            {
                continue;
            }
            Array<char>& current = mDocuments[index].Text;
            const parsing::JsonValue range = change.Get("range");
            Position start;
            Position end;
            if (range.Valid() == false || readPosition(range.Get("start"), start) == false ||
                readPosition(range.Get("end"), end) == false)
            {
                current.Clear();
                (void)AppendText(text, current);
                continue;
            }
            const usize first = GetOffset(GetView(current), start);
            const usize last = GetOffset(GetView(current), end);
            Array<char> edited;
            if (AppendText(GetView(current).substr(0, first), edited) == false || AppendText(text, edited) == false ||
                AppendText(GetView(current).substr(last < first ? first : last), edited) == false)
            {
                return;
            }
            current = Move(edited);
        }
        publish(index);
    }

    void didClose(parsing::JsonValue parameters) noexcept
    {
        const usize index = getDocument(parameters);
        if (index == MAX_DOCUMENTS)
        {
            return;
        }
        Array<char> uri = Move(mDocuments[index].Uri);
        mDocuments.RemoveAt(index);
        mAnalysis.Document = MAX_DOCUMENTS;
        parsing::JsonWriter writer = makeWriter();
        writer.Raw("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
        writer.String(GetView(uri));
        writer.Raw(",\"diagnostics\":[]}}");
        writeMessage(writer);
    }

    [[nodiscard]] static bool readPosition(parsing::JsonValue value, Position& outPosition) noexcept
    {
        return value.Get("line").Integer(outPosition.Line) == true &&
               value.Get("character").Integer(outPosition.Character) == true;
    }

    // ----- Analysis -------------------------------------------------------------------------------

    /// Check document `index` with the other open documents in place of their files on disk.
    [[nodiscard]] bool analyze(usize index) noexcept
    {
        Analysis& analysis = mAnalysis;
        analysis.Document = MAX_DOCUMENTS;
        analysis.HasTree = false;
        analysis.HasModel = false;
        analysis.Files.Clear();
        const Document& document = mDocuments[index];
        if (analysis.Files.TryAddText(GetView(document.Path), GetView(document.Text)) == false)
        {
            return false;
        }
        for (usize i = 0; i < mDocuments.GetSize(); ++i)
        {
            if (i != index && mDocuments[i].IsInsideRoot == true &&
                analysis.Files.TryAddText(GetView(mDocuments[i].Path), GetView(mDocuments[i].Text)) == false)
            {
                return false;
            }
        }
        if (mHasRoot == true && document.IsInsideRoot == true &&
            analysis.Files.AddDependencies(mRoot, mOptions.IncludeDirectories) == false)
        {
            return false;
        }
        analysis.Request = language::PreprocessRequest{};
        analysis.Request.Files = analysis.Files.GetFiles();
        analysis.Request.Source = MakeView(GetView(document.Path));
        analysis.Request.IncludeDirectories = mOptions.IncludeDirectories;
        analysis.Document = index;
        if (language::Preprocess(analysis.Request, analysis.Preprocessed) != language::Status::Ready)
        {
            return true;
        }
        analysis.HasTree =
            language::Parse(analysis.Request, analysis.Preprocessed, analysis.Tree) == language::Status::Ready;
        if (analysis.HasTree == false)
        {
            return true;
        }
        language::CheckOptions options;
        options.Style = mOptions.Style;
        (void)language::Check(analysis.Request, analysis.Preprocessed, analysis.Tree, options, analysis.Model);
        analysis.HasModel = analysis.Model.Symbols.IsEmpty() == false;
        return true;
    }

    /// Analyze document `index` unless the last analysis is of it.
    [[nodiscard]] bool ensureAnalysis(usize index) noexcept
    {
        return mAnalysis.Document == index || analyze(index) == true;
    }

    void writeRange(parsing::JsonWriter& inoutWriter, std::string_view text, usize first, usize last) noexcept
    {
        const Position start = GetPosition(text, first);
        const Position end = GetPosition(text, last);
        inoutWriter.Raw("{\"start\":{\"line\":");
        inoutWriter.Integer(start.Line);
        inoutWriter.Raw(",\"character\":");
        inoutWriter.Integer(start.Character);
        inoutWriter.Raw("},\"end\":{\"line\":");
        inoutWriter.Integer(end.Line);
        inoutWriter.Raw(",\"character\":");
        inoutWriter.Integer(end.Character);
        inoutWriter.Raw("}}");
    }

    /// The end of the identifier, or the next character, at `offset`.
    [[nodiscard]] static usize getWordEnd(std::string_view text, usize offset) noexcept
    {
        usize end = offset;
        while (end < text.size() && IsIdentifierByte(text[end]) == true)
        {
            ++end;
        }
        return end == offset && offset < text.size() ? offset + 1 : end;
    }

    void writeDiagnostics(parsing::JsonWriter& inoutWriter,
                          const Array<language::Diagnostic>& diagnostics,
                          bool& inoutIsFirst) noexcept
    {
        const std::string_view text = GetView(mDocuments[mAnalysis.Document].Text);
        for (const language::Diagnostic& diagnostic : diagnostics)
        {
            usize first = 0;
            Line message;
            // A problem in an included file is shown on the document's first line, naming its place.
            if (diagnostic.Location.FileIndex == 0)
            {
                first = diagnostic.Location.ByteOffset;
            }
            else if (diagnostic.Location.IsValid() == true &&
                     diagnostic.Location.FileIndex != language::CONFIGURATION_SOURCE)
            {
                message.Append(mAnalysis.Files.GetPath(diagnostic.Location.FileIndex));
                message.Append(":");
                message.AppendUnsigned(diagnostic.Location.Line);
                message.Append(": ");
            }
            message.Append(language::GetDiagnosticCodeName(diagnostic.Code));
            message.Append(": ");
            message.Append(diagnostic.Message.GetView());
            Line code;
            code.Append("LUSL");
            code.AppendUnsigned(static_cast<uint64>(diagnostic.Code), DIAGNOSTIC_CODE_DIGITS);
            inoutWriter.Raw(inoutIsFirst == true ? "{\"range\":" : ",{\"range\":");
            inoutIsFirst = false;
            writeRange(inoutWriter, text, first, getWordEnd(text, first));
            inoutWriter.Raw(",\"severity\":");
            inoutWriter.Integer(diagnostic.Level == language::Severity::Error ? SEVERITY_ERROR : SEVERITY_WARNING);
            inoutWriter.Raw(",\"code\":");
            inoutWriter.String(code.GetView());
            inoutWriter.Raw(",\"source\":\"luslc\",\"message\":");
            inoutWriter.String(message.GetView());
            inoutWriter.Raw("}");
        }
    }

    void publish(usize index) noexcept
    {
        if (analyze(index) == false)
        {
            return;
        }
        parsing::JsonWriter writer = makeWriter();
        writer.Raw("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
        writer.String(GetView(mDocuments[index].Uri));
        writer.Raw(",\"diagnostics\":[");
        bool isFirst = true;
        writeDiagnostics(writer, mAnalysis.Preprocessed.Diagnostics, isFirst);
        writeDiagnostics(writer, mAnalysis.Tree.Diagnostics, isFirst);
        if (mAnalysis.HasTree == true)
        {
            writeDiagnostics(writer, mAnalysis.Model.Diagnostics, isFirst);
        }
        writer.Raw("]}}");
        writeMessage(writer);
    }

    // ----- Queries --------------------------------------------------------------------------------

    /// The preprocessed token of the analyzed document at `offset`, or NO_TOKEN.
    [[nodiscard]] uint32 findToken(usize offset) const noexcept
    {
        const Array<language::Token>& tokens = mAnalysis.Preprocessed.Tokens;
        for (uint32 i = 0; i < tokens.GetSize(); ++i)
        {
            const language::SourceLocation& location = tokens[i].Location;
            if (location.FileIndex == 0 && tokens[i].ExpansionIndex == language::NO_EXPANSION &&
                location.ByteOffset <= offset && offset < location.ByteOffset + tokens[i].SpellingSize)
            {
                return i;
            }
        }
        return language::NO_TOKEN;
    }

    [[nodiscard]] bool hasSymbol(uint32 node) const noexcept
    {
        return mAnalysis.HasModel == true && node < mAnalysis.Model.NodeSymbols.GetSize() &&
               mAnalysis.Model.NodeSymbols[node] != language::NO_SYMBOL;
    }

    /// The syntax node whose principal token is `token`, preferring one bound to a symbol. A callee
    /// name is bound through its call, whose first child it is.
    [[nodiscard]] uint32 findNode(uint32 token) const noexcept
    {
        uint32 found = language::NO_NODE;
        for (uint32 i = 0; i < mAnalysis.Tree.Nodes.GetSize(); ++i)
        {
            if (mAnalysis.Tree.Nodes[i].Token != token)
            {
                continue;
            }
            if (hasSymbol(i) == true)
            {
                return i;
            }
            found = found == language::NO_NODE ? i : found;
        }
        for (uint32 i = 0; i < mAnalysis.Tree.Nodes.GetSize() && found != language::NO_NODE; ++i)
        {
            if (mAnalysis.Tree.Nodes[i].Kind == language::SyntaxKind::CallExpression &&
                mAnalysis.Tree.Nodes[i].FirstChild == found && hasSymbol(i) == true)
            {
                return i;
            }
        }
        return found;
    }

    /// The node at the request's position, or NO_NODE; `outDocument` is its document.
    [[nodiscard]] uint32 locate(parsing::JsonValue parameters, usize& outDocument) noexcept
    {
        outDocument = getDocument(parameters);
        Position position;
        if (outDocument == MAX_DOCUMENTS || readPosition(parameters.Get("position"), position) == false ||
            ensureAnalysis(outDocument) == false || mAnalysis.HasTree == false)
        {
            return language::NO_NODE;
        }
        const uint32 token = findToken(GetOffset(GetView(mDocuments[outDocument].Text), position));
        return token == language::NO_TOKEN ? language::NO_NODE : findNode(token);
    }

    [[nodiscard]] std::string_view spellToken(uint32 token) const noexcept
    {
        return token < mAnalysis.Preprocessed.Tokens.GetSize()
                   ? language::GetTokenSpelling(mAnalysis.Request,
                                                mAnalysis.Preprocessed,
                                                mAnalysis.Preprocessed.Tokens[token])
                   : std::string_view{};
    }

    void respondDefinition(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        usize document = MAX_DOCUMENTS;
        const uint32 node = locate(parameters, document);
        const uint32 symbol = node != language::NO_NODE && mAnalysis.HasModel == true
                                  ? mAnalysis.Model.NodeSymbols[node]
                                  : language::NO_SYMBOL;
        const uint32 name =
            symbol != language::NO_SYMBOL ? mAnalysis.Model.Symbols[symbol].NameToken : language::NO_TOKEN;
        if (name == language::NO_TOKEN || name >= mAnalysis.Preprocessed.Tokens.GetSize())
        {
            respondNull(id);
            return;
        }
        const language::Token& token = mAnalysis.Preprocessed.Tokens[name];
        const usize file = token.Location.FileIndex;
        Array<char> path;
        Array<char> uri;
        if (file == 0)
        {
            (void)AppendText(GetView(mDocuments[document].Uri), uri);
        }
        else if (AppendText(GetView(mRootPath), path) == false || path.TryAdd('/') == false ||
                 AppendText(mAnalysis.Files.GetPath(file), path) == false || EncodeFileUri(GetView(path), uri) == false)
        {
            respondNull(id);
            return;
        }
        const std::string_view text = mAnalysis.Files.GetText(file);
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("{\"uri\":");
        writer.String(GetView(uri));
        writer.Raw(",\"range\":");
        writeRange(writer, text, token.Location.ByteOffset, token.Location.ByteOffset + token.SpellingSize);
        writer.Raw("}}");
        writeMessage(writer);
    }

    /// A one-line description of `symbol`, as hover shows it.
    void describeSymbol(uint32 symbol, Line& inoutLine) const noexcept
    {
        const language::Symbol& declared = mAnalysis.Model.Symbols[symbol];
        const auto typeName = [this](uint32 type) { // ludus-style: inferred-type: lambda closure
            return language::FormatTypeName(mAnalysis.Model, type, mAnalysis.Request, mAnalysis.Preprocessed);
        };
        const std::string_view name = spellToken(declared.NameToken);
        switch (declared.Kind)
        {
            case language::SymbolKind::Function:
            case language::SymbolKind::Intrinsic: {
                const language::TypeNameText result = typeName(declared.Type);
                inoutLine.Append(result.GetView());
                inoutLine.Append(" ");
                inoutLine.Append(name);
                inoutLine.Append("(");
                for (uint32 i = 0; i < declared.MemberCount; ++i)
                {
                    const language::Symbol& parameter =
                        mAnalysis.Model.Symbols[mAnalysis.Model.Members[declared.FirstMember + i]];
                    const language::TypeNameText type = typeName(parameter.Type);
                    inoutLine.Append(i == 0 ? "" : ", ");
                    inoutLine.Append(parameter.Direction == language::ParameterDirection::Out     ? "out "
                                     : parameter.Direction == language::ParameterDirection::Inout ? "inout "
                                                                                                  : "in ");
                    inoutLine.Append(type.GetView());
                    inoutLine.Append(" ");
                    inoutLine.Append(spellToken(parameter.NameToken));
                }
                inoutLine.Append(")");
                return;
            }
            case language::SymbolKind::Struct:
                inoutLine.Append("struct ");
                inoutLine.Append(name);
                return;
            case language::SymbolKind::Interface:
                inoutLine.Append("interface ");
                inoutLine.Append(name);
                return;
            case language::SymbolKind::Enum:
                inoutLine.Append("enum class ");
                inoutLine.Append(name);
                return;
            case language::SymbolKind::Concept:
                inoutLine.Append("concept ");
                inoutLine.Append(name);
                return;
            case language::SymbolKind::Space:
                inoutLine.Append("space ");
                inoutLine.Append(name);
                return;
            case language::SymbolKind::Enumerator: {
                inoutLine.Append(spellToken(mAnalysis.Model.Symbols[declared.Parent].NameToken));
                inoutLine.Append("::");
                inoutLine.Append(name);
                return;
            }
            default: {
                const language::TypeNameText type = typeName(declared.Type);
                inoutLine.Append(declared.IsConstant == true ? "static const " : "");
                inoutLine.Append(type.GetView());
                inoutLine.Append(" ");
                inoutLine.Append(name);
                return;
            }
        }
    }

    void respondHover(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        usize document = MAX_DOCUMENTS;
        const uint32 node = locate(parameters, document);
        if (node == language::NO_NODE || mAnalysis.HasModel == false)
        {
            respondNull(id);
            return;
        }
        Line text;
        const uint32 symbol = mAnalysis.Model.NodeSymbols[node];
        const uint32 type = mAnalysis.Model.NodeTypes[node];
        if (symbol != language::NO_SYMBOL)
        {
            describeSymbol(symbol, text);
        }
        else if (type != language::NO_TYPE)
        {
            const language::TypeNameText name =
                language::FormatTypeName(mAnalysis.Model, type, mAnalysis.Request, mAnalysis.Preprocessed);
            text.Append(name.GetView());
        }
        else
        {
            respondNull(id);
            return;
        }
        Line markdown;
        markdown.Append("```lusl\n");
        markdown.Append(text.GetView());
        markdown.Append("\n```");
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("{\"contents\":{\"kind\":\"markdown\",\"value\":");
        writer.String(markdown.GetView());
        writer.Raw("}}}");
        writeMessage(writer);
    }

    [[nodiscard]] static uint64 getSymbolKind(const language::Symbol& declared, bool isMember) noexcept
    {
        switch (declared.Kind)
        {
            case language::SymbolKind::Function:
                return isMember == true ? SYMBOL_METHOD : SYMBOL_FUNCTION;
            case language::SymbolKind::Struct:
                return SYMBOL_STRUCT;
            case language::SymbolKind::Enum:
                return SYMBOL_ENUM;
            case language::SymbolKind::Enumerator:
                return SYMBOL_ENUM_MEMBER;
            case language::SymbolKind::Interface:
            case language::SymbolKind::Concept:
                return SYMBOL_INTERFACE;
            case language::SymbolKind::Field:
                return SYMBOL_FIELD;
            default:
                return declared.IsConstant == true ? SYMBOL_CONSTANT : SYMBOL_VARIABLE;
        }
    }

    /// Write the DocumentSymbol of the declaration `node`; false when it names nothing.
    [[nodiscard]] bool writeSymbol(parsing::JsonWriter& inoutWriter, uint32 node, bool isMember, bool isFirst) noexcept
    {
        const uint32 symbol =
            node < mAnalysis.Model.NodeSymbols.GetSize() ? mAnalysis.Model.NodeSymbols[node] : language::NO_SYMBOL;
        if (symbol == language::NO_SYMBOL)
        {
            return false;
        }
        const language::Symbol& declared = mAnalysis.Model.Symbols[symbol];
        const language::SyntaxNode& syntax = mAnalysis.Tree.Nodes[node];
        const Array<language::Token>& tokens = mAnalysis.Preprocessed.Tokens;
        if (declared.NameToken >= tokens.GetSize() || tokens[declared.NameToken].Location.FileIndex != 0 ||
            syntax.FirstToken >= tokens.GetSize() || syntax.LastToken >= tokens.GetSize())
        {
            return false;
        }
        const std::string_view text = GetView(mDocuments[mAnalysis.Document].Text);
        const language::Token& name = tokens[declared.NameToken];
        const language::Token& first = tokens[syntax.FirstToken];
        const language::Token& last = tokens[syntax.LastToken];
        const usize start = first.Location.FileIndex == 0 ? first.Location.ByteOffset : name.Location.ByteOffset;
        const usize end = last.Location.FileIndex == 0 ? last.Location.ByteOffset + last.SpellingSize
                                                       : name.Location.ByteOffset + name.SpellingSize;
        inoutWriter.Raw(isFirst == true ? "{\"name\":" : ",{\"name\":");
        inoutWriter.String(spellToken(declared.NameToken));
        inoutWriter.Raw(",\"kind\":");
        inoutWriter.Integer(getSymbolKind(declared, isMember));
        inoutWriter.Raw(",\"range\":");
        writeRange(inoutWriter, text, start, end < start ? start : end);
        inoutWriter.Raw(",\"selectionRange\":");
        writeRange(inoutWriter, text, name.Location.ByteOffset, name.Location.ByteOffset + name.SpellingSize);
        if (declared.Kind == language::SymbolKind::Struct || declared.Kind == language::SymbolKind::Enum ||
            declared.Kind == language::SymbolKind::Interface)
        {
            inoutWriter.Raw(",\"children\":[");
            bool isChildFirst = true;
            for (uint32 child = syntax.FirstChild; child != language::NO_NODE;
                 child = mAnalysis.Tree.Nodes[child].NextSibling)
            {
                if (writeSymbol(inoutWriter, child, true, isChildFirst) == true)
                {
                    isChildFirst = false;
                }
            }
            inoutWriter.Raw("]");
        }
        inoutWriter.Raw("}");
        return true;
    }

    void respondSymbols(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        const usize document = getDocument(parameters);
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("[");
        if (document != MAX_DOCUMENTS && ensureAnalysis(document) == true && mAnalysis.HasModel == true)
        {
            bool isFirst = true;
            for (uint32 node = mAnalysis.Tree.Nodes[0].FirstChild; node != language::NO_NODE;
                 node = mAnalysis.Tree.Nodes[node].NextSibling)
            {
                const bool isBlock = mAnalysis.Tree.Nodes[node].Kind == language::SyntaxKind::ExternSlangBlock;
                for (uint32 item = isBlock == true ? mAnalysis.Tree.Nodes[node].FirstChild : node;
                     item != language::NO_NODE;
                     item = isBlock == true ? mAnalysis.Tree.Nodes[item].NextSibling : language::NO_NODE)
                {
                    if (writeSymbol(writer, item, false, isFirst) == true)
                    {
                        isFirst = false;
                    }
                }
            }
        }
        writer.Raw("]}");
        writeMessage(writer);
    }

    void respondFormatting(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        const usize document = getDocument(parameters);
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        language::FormatResult result;
        if (document == MAX_DOCUMENTS ||
            language::FormatSource(GetView(mDocuments[document].Text), result) != language::Status::Ready)
        {
            // A source that does not lex is left alone; its diagnostics say why.
            writer.Raw("[]}");
            writeMessage(writer);
            return;
        }
        const std::string_view text = GetView(mDocuments[document].Text);
        const std::string_view formatted(result.Text.GetData(), result.Text.GetSize());
        if (formatted == text)
        {
            writer.Raw("[]}");
            writeMessage(writer);
            return;
        }
        writer.Raw("[{\"range\":");
        writeRange(writer, text, 0, text.size());
        writer.Raw(",\"newText\":");
        writer.String(formatted);
        writer.Raw("}]}");
        writeMessage(writer);
    }

    static void
    writeCompletion(parsing::JsonWriter& inoutWriter, std::string_view label, uint64 kind, bool& inoutIsFirst) noexcept
    {
        inoutWriter.Raw(inoutIsFirst == true ? "{\"label\":" : ",{\"label\":");
        inoutIsFirst = false;
        inoutWriter.String(label);
        inoutWriter.Raw(",\"kind\":");
        inoutWriter.Integer(kind);
        inoutWriter.Raw("}");
    }

    [[nodiscard]] static uint64 getCompletionKind(const language::Symbol& declared) noexcept
    {
        switch (declared.Kind)
        {
            case language::SymbolKind::Function:
            case language::SymbolKind::Intrinsic:
                return COMPLETION_FUNCTION;
            case language::SymbolKind::Struct:
                return COMPLETION_STRUCT;
            case language::SymbolKind::Enum:
                return COMPLETION_ENUM;
            case language::SymbolKind::Interface:
            case language::SymbolKind::Concept:
                return COMPLETION_INTERFACE;
            case language::SymbolKind::Field:
                return COMPLETION_FIELD;
            default:
                return declared.IsConstant == true ? COMPLETION_CONSTANT : COMPLETION_VARIABLE;
        }
    }

    /// Fields and methods of the struct before a `.` at `offset`, or false when there is none.
    [[nodiscard]] bool
    writeMemberCompletions(parsing::JsonWriter& inoutWriter, usize offset, bool& inoutIsFirst) noexcept
    {
        const std::string_view text = GetView(mDocuments[mAnalysis.Document].Text);
        usize dot = offset;
        while (dot > 0 && IsIdentifierByte(text[dot - 1]) == true)
        {
            --dot;
        }
        if (dot == 0 || text[dot - 1] != '.' || dot < 2)
        {
            return false;
        }
        const uint32 token = findToken(dot - 2);
        const uint32 node = token == language::NO_TOKEN ? language::NO_NODE : findNode(token);
        if (node == language::NO_NODE || mAnalysis.HasModel == false)
        {
            return true;
        }
        const uint32 type = mAnalysis.Model.NodeTypes[node];
        if (type == language::NO_TYPE || mAnalysis.Model.Types[type].Kind != language::TypeKind::Struct)
        {
            return true;
        }
        const language::Symbol& structure = mAnalysis.Model.Symbols[mAnalysis.Model.Types[type].Symbol];
        for (uint32 i = 0; i < structure.MemberCount; ++i)
        {
            const language::Symbol& member =
                mAnalysis.Model.Symbols[mAnalysis.Model.Members[structure.FirstMember + i]];
            writeCompletion(inoutWriter, spellToken(member.NameToken), getCompletionKind(member), inoutIsFirst);
        }
        return true;
    }

    void respondCompletion(const Message& message) noexcept
    {
        const parsing::JsonValue id = message.Id;
        const parsing::JsonValue parameters = message.Parameters;
        const usize document = getDocument(parameters);
        Position position;
        parsing::JsonWriter writer = makeWriter();
        beginResponse(writer, id);
        writer.Raw("[");
        bool isFirst = true;
        if (document != MAX_DOCUMENTS && readPosition(parameters.Get("position"), position) == true &&
            ensureAnalysis(document) == true)
        {
            const usize offset = GetOffset(GetView(mDocuments[document].Text), position);
            if (writeMemberCompletions(writer, offset, isFirst) == false)
            {
                writeNameCompletions(writer, isFirst);
            }
        }
        writer.Raw("]}");
        writeMessage(writer);
    }

    /// Keywords, built-in types and every module-level name.
    void writeNameCompletions(parsing::JsonWriter& inoutWriter, bool& inoutIsFirst) noexcept
    {
        const BorrowedView<const std::string_view> keywords = language::GetKeywords();
        for (usize i = 0; i < keywords.GetSize(); ++i)
        {
            writeCompletion(inoutWriter, keywords[i], COMPLETION_KEYWORD, inoutIsFirst);
        }
        for (const std::string_view scalar : SCALAR_NAMES)
        {
            writeCompletion(inoutWriter, scalar, COMPLETION_STRUCT, inoutIsFirst);
            for (uint32 size = MIN_COMPONENTS; size <= MAX_COMPONENTS; ++size)
            {
                Line vector;
                vector.Append(scalar);
                vector.Append("x");
                vector.AppendUnsigned(size);
                writeCompletion(inoutWriter, vector.GetView(), COMPLETION_STRUCT, inoutIsFirst);
            }
        }
        for (const std::string_view resource : RESOURCE_NAMES)
        {
            writeCompletion(inoutWriter, resource, COMPLETION_STRUCT, inoutIsFirst);
        }
        if (mAnalysis.HasModel == false)
        {
            return;
        }
        for (const language::Symbol& declared : mAnalysis.Model.Symbols)
        {
            const bool isModuleLevel =
                declared.Parent == language::NO_SYMBOL &&
                (declared.Kind == language::SymbolKind::Intrinsic || declared.Kind == language::SymbolKind::Function ||
                 declared.Kind == language::SymbolKind::Struct || declared.Kind == language::SymbolKind::Enum ||
                 declared.Kind == language::SymbolKind::Interface || declared.Kind == language::SymbolKind::Concept ||
                 (declared.Kind == language::SymbolKind::Variable &&
                  (declared.IsConstant == true || declared.IsGroupshared == true ||
                   declared.Slot != language::NO_LOCATION || declared.IsExtern == true)));
            if (isModuleLevel == true)
            {
                const std::string_view name =
                    declared.NameToken == language::NO_TOKEN ? std::string_view{} : spellToken(declared.NameToken);
                if (name.empty() == false)
                {
                    writeCompletion(inoutWriter, name, getCompletionKind(declared), inoutIsFirst);
                }
            }
        }
    }

    const LanguageServerOptions& mOptions;
    filesystem::Directory mRoot;
    Array<char> mRootPath;
    bool mHasRoot = false;
    bool mHasShutDown = false;
    bool mHasExited = false;
    Array<char> mInput;
    Array<uint8> mOutput;
    Array<Document> mDocuments;
    Analysis mAnalysis;
};
} // namespace

int RunLanguageServer(const LanguageServerOptions& options) noexcept
{
    LanguageServer server(options);
    return server.Run();
}
} // namespace ludus::tools::luslc
