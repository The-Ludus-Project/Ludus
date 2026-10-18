#pragma once
#include <ludus/tooling/shader_compiler/compiler.hpp>

namespace ludus::tooling::shader_compiler::internal
{
[[nodiscard]] bool EncodeRequest(const Request& request, foundation::Array<foundation::uint8>& outBytes) noexcept;
[[nodiscard]] bool DecodeResult(foundation::BorrowedView<const foundation::uint8> bytes, Result& outResult) noexcept;
/// Parse, compile and serialize a bounded request; false on allocation/encoding failure.
[[nodiscard]] bool Process(Compiler& inoutCompiler,
                           foundation::BorrowedView<const foundation::uint8> bytes,
                           foundation::Array<foundation::uint8>& outBytes) noexcept;
[[nodiscard]] bool IsValid(const Request& request) noexcept;
} // namespace ludus::tooling::shader_compiler::internal
