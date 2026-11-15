#pragma once

#include <string_view>

namespace ludus::world_demo
{
/// @brief Producer name the World-demo game reports to the editor's hosted GPU recordings.
///
/// Sent with every recording acknowledgement while the game runs in a hosted window
/// (`LUDUS_HOSTED_WINDOW`); the editor writes it into the recording's export. At most
/// platform::HOSTED_RECORDING_PRODUCER_NAME_BYTES bytes of UTF-8 without zero bytes.
inline constexpr std::string_view HOSTED_PRODUCER_NAME = "ludus_world_demo";
} // namespace ludus::world_demo
