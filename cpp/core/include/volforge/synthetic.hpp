// Deterministic synthetic market data.
//
// The vendor delivery format is not settled, so the engine is developed against
// generated sessions. The generator deliberately reproduces the awkward
// properties of the real feed rather than an idealised version of it — that is
// the entire point of having it.
//
// Reproduced from the measured sample day:
//   - most observations carry no trade, only a quote update
//   - update frequency falls off sharply away from the money, so series are
//     irregular in time and "one step back" is not "one second back"
//   - far strikes may not print at all for the first part of the session
//   - spreads widen substantially away from the money
//
// Everything is seeded, so a given config always produces byte-identical data.

#pragma once

#include "volforge/memory_source.hpp"

#include <memory>
#include <string>

namespace volforge {

struct SyntheticConfig {
    Date        date{20250701};
    Date        expiry{20250703};
    std::string underlying = "NIFTY";

    Price spot_open   = Price::from_double(25000.0);
    Price strike_step = Price::from_double(50.0);
    int   strikes_each_side = 10;

    int session_open_sec  = 9 * 3600 + 15 * 60;
    int session_close_sec = 15 * 3600 + 30 * 60;

    Qty lot_size = 75;

    std::uint64_t seed = 42;
};

struct SyntheticSession {
    std::shared_ptr<MemorySessionData> data;
    UnderlyingId                       underlying{};
    InstrumentId                       spot = InstrumentId::Invalid;
};

// Registers instruments into `registry` and generates one session.
SyntheticSession make_synthetic_session(InstrumentRegistry& registry,
                                        const SyntheticConfig& cfg = {});

}  // namespace volforge
