#pragma once
#include <ludus/tooling/shader_compiler/compiler.hpp>

namespace ludus::tooling::shader_compiler::internal
{
[[nodiscard]] bool EncodeRequest(const Request& request, foundation::Array<foundation::uint8>& outBytes) noexcept;
[[nodiscard]] bool DecodeResult(foundation::BorrowedView<const foundation::uint8> bytes, Result& outResult) noexcept;
/// Parse, compile and serialize a bounded request; false on allocation/encoding failure.
/// connectionLanes is the number of requests the serving worker compiles at once; every reply
/// reports it as `concurrency`, so clients bound how many requests they keep outstanding.
[[nodiscard]] bool Process(Compiler& inoutCompiler,
                           foundation::BorrowedView<const foundation::uint8> bytes,
                           foundation::uint32 connectionLanes,
                           foundation::Array<foundation::uint8>& outBytes) noexcept;
[[nodiscard]] bool IsValid(const Request& request) noexcept;
} // namespace ludus::tooling::shader_compiler::internal
