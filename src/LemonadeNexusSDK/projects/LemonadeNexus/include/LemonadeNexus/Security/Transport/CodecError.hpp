#pragma once

#include <cstdint>

namespace nexus::security {

/// Why the security wire codec refused a byte stream (SecurityCodec.hpp,
/// AuthorityChainCodec.hpp).
enum class CodecError : uint16_t {
    Truncated,
    Oversized,
    BadVersion,
    UnknownKind,
    KindMismatch,
    CountTooLarge,
    LengthTooLarge,
    TrailingBytes,
    BadValue,
};

}  // namespace nexus::security
