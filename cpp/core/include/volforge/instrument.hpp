// Instrument identity.
//
// Vendor symbol strings ("NIFTY03JUL2523000CE.NFO") are an adapter concern and
// deliberately do not appear in the engine. The engine works in terms of what an
// instrument *is* — underlying, expiry, strike, right — because that is what
// strategies select on. A strategy asks for the 0.30-delta call two days out; it
// never names a symbol.

#pragma once

#include "volforge/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace volforge {

// Interned handle for an underlying ("NIFTY", "BANKNIFTY").
enum class UnderlyingId : std::int32_t { Invalid = -1 };

struct InstrumentSpec {
    InstrumentId   id         = InstrumentId::Invalid;
    UnderlyingId   underlying = UnderlyingId::Invalid;
    InstrumentKind kind       = InstrumentKind::Spot;

    Date  expiry;         // unset for Spot
    Price strike;         // zero unless kind == Option
    Right right = Right::Call;   // meaningful only when kind == Option

    Qty   lot_size  = 1;
    Price tick_size = Price::from_minor(5);   // 0.05 on NSE index options

    [[nodiscard]] bool is_option() const { return kind == InstrumentKind::Option; }
};

// Maps between vendor-agnostic instrument descriptions and compact ids.
//
// Ids are assigned in insertion order and are stable for the lifetime of the
// registry, so they can be used as array indices. A registry is expected to
// outlive any single session: an instrument that appears on several days keeps
// one id, which is what makes multi-day positions expressible.
class InstrumentRegistry {
public:
    UnderlyingId intern_underlying(std::string_view name);
    [[nodiscard]] std::string_view underlying_name(UnderlyingId) const;

    // Registers the instrument, or returns the existing id if an identical
    // instrument is already known.
    InstrumentId add(const InstrumentSpec& spec);

    [[nodiscard]] const InstrumentSpec& spec(InstrumentId) const;
    [[nodiscard]] std::size_t size() const { return specs_.size(); }
    [[nodiscard]] std::span<const InstrumentSpec> all() const { return specs_; }

    // Lookup by identity rather than by name.
    [[nodiscard]] std::optional<InstrumentId> find_option(
        UnderlyingId underlying, Date expiry, Price strike, Right right) const;
    [[nodiscard]] std::optional<InstrumentId> find_spot(UnderlyingId underlying) const;

    // Every distinct expiry known for an underlying, ascending. The chain query
    // layer builds "nearest expiry" and "dte >= n" on top of this.
    [[nodiscard]] std::vector<Date> expiries(UnderlyingId underlying) const;

private:
    struct Key {
        std::int32_t underlying;
        std::int32_t kind;
        std::int32_t expiry;
        std::int32_t strike;
        std::int32_t right;

        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key&) const noexcept;
    };

    static Key key_of(const InstrumentSpec& s);

    std::vector<InstrumentSpec>                    specs_;
    std::unordered_map<Key, InstrumentId, KeyHash> by_key_;
    std::vector<std::string>                       underlyings_;
    std::unordered_map<std::string, UnderlyingId>  underlying_ids_;
};

}  // namespace volforge
