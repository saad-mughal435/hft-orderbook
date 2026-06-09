#pragma once

#include <cstddef>
#include <cstdint>

#include "itch/messages.hpp"

namespace hftob {
namespace itch {

/// Total on-the-wire length of an ITCH 5.0 message (including the 1-byte type
/// code), as carried by the BinaryFILE / MoldUDP64 2-byte length prefix.
/// Returns 0 for an unrecognised type.
std::size_t message_length(char type);

/// Decode one ITCH message. `buf` points at the type byte and `len` is the
/// message length (the value from the framing prefix). Returns false if the
/// buffer is too short for the type or the type is not handled.
///
/// Fields are extracted by explicit big-endian byte assembly - never by casting
/// a packed struct over the wire (the layouts have misaligned multi-byte fields
/// and are big-endian, so a cast would be UB / wrong on little-endian hosts).
bool decode(const std::uint8_t* buf, std::size_t len, Message& out);

}  // namespace itch
}  // namespace hftob
