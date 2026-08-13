// Core value types.
//
// Prices and money are fixed-point integers, never floating point. Option
// premiums are quoted on a discrete tick grid, and binary floating point cannot
// represent that grid exactly — an accumulated rounding error in a P&L figure is
// a wrong answer, not a rounding detail. Doubles appear only in pricing models,
// where the inputs are already approximations.

#pragma once

#include <cstdint>
#include <compare>
#include <cmath>
#include <limits>
#include <string>

namespace volforge {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// Nanoseconds since the Unix epoch, UTC.
//
// The current vendor feed carries only 1-second resolution, but live feeds carry
// more, and the interface should not need to change when the data improves.
// Storage is free to encode this far more compactly; see docs/design.md.
struct Timestamp {
    std::int64_t nanos = 0;

    friend constexpr auto operator<=>(Timestamp, Timestamp) = default;

    static constexpr Timestamp from_seconds(std::int64_t s) { return {s * 1'000'000'000}; }
    static constexpr Timestamp from_millis(std::int64_t ms) { return {ms * 1'000'000}; }

    [[nodiscard]] constexpr std::int64_t seconds() const { return nanos / 1'000'000'000; }
};

constexpr Timestamp kNoTime{std::numeric_limits<std::int64_t>::min()};

// A calendar date encoded as yyyymmdd.
//
// Chosen over a day count because it is readable in a debugger and in file
// names, and it still orders correctly under integer comparison.
struct Date {
    std::int32_t yyyymmdd = 0;

    friend constexpr auto operator<=>(Date, Date) = default;

    [[nodiscard]] constexpr int year()  const { return yyyymmdd / 10000; }
    [[nodiscard]] constexpr int month() const { return (yyyymmdd / 100) % 100; }
    [[nodiscard]] constexpr int day()   const { return yyyymmdd % 100; }
    [[nodiscard]] constexpr bool valid() const { return yyyymmdd > 0; }

    [[nodiscard]] std::string to_string() const;
};

// Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's algorithm).
constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp  = (m + 9u) % 12u;   // March-based month index, avoids a signed mix
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// NSE trades at UTC+5:30. Sessions are described in local wall-clock time
// throughout, so adapters must state the offset rather than assume one.
constexpr int kISTOffsetSeconds = 5 * 3600 + 30 * 60;

// Absolute time for a local wall-clock second within a session.
constexpr Timestamp timestamp_of(Date d, int seconds_of_day, int utc_offset_seconds) {
    const std::int64_t days = days_from_civil(d.year(), static_cast<unsigned>(d.month()),
                                              static_cast<unsigned>(d.day()));
    return Timestamp::from_seconds(days * 86400 + seconds_of_day - utc_offset_seconds);
}

// ---------------------------------------------------------------------------
// Money
// ---------------------------------------------------------------------------

// A price in minor currency units (paise for INR), i.e. hundredths.
//
// int32 spans +/- 21.4 million minor units, roughly +/- 214,000 rupees, which
// comfortably covers index levels and option premiums. Aggregates that can
// exceed it use Money instead.
struct Price {
    std::int32_t minor = 0;

    friend constexpr auto operator<=>(Price, Price) = default;

    static constexpr Price from_minor(std::int32_t m) { return {m}; }
    static Price from_double(double v) { return {static_cast<std::int32_t>(std::llround(v * 100.0))}; }

    [[nodiscard]] constexpr double to_double() const { return static_cast<double>(minor) / 100.0; }

    constexpr Price operator+(Price o) const { return {minor + o.minor}; }
    constexpr Price operator-(Price o) const { return {minor - o.minor}; }
    constexpr Price operator-() const { return {-minor}; }
};

// A monetary amount in minor units. Wider than Price because notionals,
// premiums collected across legs and running P&L all outgrow a single quote.
struct Money {
    std::int64_t minor = 0;

    friend constexpr auto operator<=>(Money, Money) = default;

    [[nodiscard]] constexpr double to_double() const { return static_cast<double>(minor) / 100.0; }

    constexpr Money operator+(Money o) const { return {minor + o.minor}; }
    constexpr Money operator-(Money o) const { return {minor - o.minor}; }
    constexpr Money operator-() const { return {-minor}; }
};

// Quantity in contracts or shares — never in lots. Lot size is a property of the
// instrument, and mixing the two units is a recurring source of position-sizing
// bugs, so the ambiguous unit is simply absent from the type system.
using Qty = std::int32_t;

// price * quantity, widened.
constexpr Money notional(Price p, Qty q) {
    return {static_cast<std::int64_t>(p.minor) * static_cast<std::int64_t>(q)};
}

// ---------------------------------------------------------------------------
// Instrument classification
// ---------------------------------------------------------------------------

enum class Right : std::uint8_t { Call, Put };

enum class InstrumentKind : std::uint8_t {
    Spot,    // an index or cash equity
    Future,
    Option,
};

// A handle into an InstrumentRegistry. Stable within a registry, and compact
// enough to sit in a column without costing anything.
enum class InstrumentId : std::int32_t { Invalid = -1 };

constexpr bool valid(InstrumentId id) { return id != InstrumentId::Invalid; }
constexpr std::int32_t index_of(InstrumentId id) { return static_cast<std::int32_t>(id); }

const char* to_string(Right r);
const char* to_string(InstrumentKind k);

}  // namespace volforge
