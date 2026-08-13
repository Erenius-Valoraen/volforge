#include "volforge/types.hpp"

#include <array>
#include <cstdio>

namespace volforge {

std::string Date::to_string() const {
    if (!valid()) return "invalid-date";
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d", year(), month(), day());
    return std::string(buf.data());
}

const char* to_string(Right r) {
    switch (r) {
        case Right::Call: return "CE";
        case Right::Put:  return "PE";
    }
    return "?";
}

const char* to_string(InstrumentKind k) {
    switch (k) {
        case InstrumentKind::Spot:   return "spot";
        case InstrumentKind::Future: return "future";
        case InstrumentKind::Option: return "option";
    }
    return "?";
}

}  // namespace volforge
