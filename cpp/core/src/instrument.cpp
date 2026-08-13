#include "volforge/instrument.hpp"

#include <algorithm>
#include <stdexcept>

namespace volforge {

std::size_t InstrumentRegistry::KeyHash::operator()(const Key& k) const noexcept {
    // Splitmix-style mixing. The fields are small and highly correlated across
    // a chain (same underlying, same expiry, adjacent strikes), so a naive
    // xor-shift hash clusters badly.
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](std::int32_t v) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    };
    mix(k.underlying);
    mix(k.kind);
    mix(k.expiry);
    mix(k.strike);
    mix(k.right);
    return static_cast<std::size_t>(h);
}

InstrumentRegistry::Key InstrumentRegistry::key_of(const InstrumentSpec& s) {
    return Key{
        static_cast<std::int32_t>(s.underlying),
        static_cast<std::int32_t>(s.kind),
        s.expiry.yyyymmdd,
        s.strike.minor,
        s.kind == InstrumentKind::Option ? static_cast<std::int32_t>(s.right) : 0,
    };
}

UnderlyingId InstrumentRegistry::intern_underlying(std::string_view name) {
    std::string key(name);
    if (auto it = underlying_ids_.find(key); it != underlying_ids_.end()) {
        return it->second;
    }
    const auto id = static_cast<UnderlyingId>(static_cast<std::int32_t>(underlyings_.size()));
    underlyings_.emplace_back(std::move(key));
    underlying_ids_.emplace(underlyings_.back(), id);
    return id;
}

std::string_view InstrumentRegistry::underlying_name(UnderlyingId id) const {
    const auto i = static_cast<std::int32_t>(id);
    if (i < 0 || static_cast<std::size_t>(i) >= underlyings_.size()) {
        throw std::out_of_range("unknown underlying id");
    }
    return underlyings_[static_cast<std::size_t>(i)];
}

InstrumentId InstrumentRegistry::add(const InstrumentSpec& spec) {
    const Key k = key_of(spec);
    if (auto it = by_key_.find(k); it != by_key_.end()) {
        return it->second;
    }
    const auto id = static_cast<InstrumentId>(static_cast<std::int32_t>(specs_.size()));
    InstrumentSpec stored = spec;
    stored.id = id;
    specs_.push_back(stored);
    by_key_.emplace(k, id);
    return id;
}

const InstrumentSpec& InstrumentRegistry::spec(InstrumentId id) const {
    const auto i = index_of(id);
    if (i < 0 || static_cast<std::size_t>(i) >= specs_.size()) {
        throw std::out_of_range("unknown instrument id");
    }
    return specs_[static_cast<std::size_t>(i)];
}

std::optional<InstrumentId> InstrumentRegistry::find_option(
    UnderlyingId underlying, Date expiry, Price strike, Right right) const {
    const Key k{static_cast<std::int32_t>(underlying),
                static_cast<std::int32_t>(InstrumentKind::Option),
                expiry.yyyymmdd, strike.minor, static_cast<std::int32_t>(right)};
    if (auto it = by_key_.find(k); it != by_key_.end()) return it->second;
    return std::nullopt;
}

std::optional<InstrumentId> InstrumentRegistry::find_spot(UnderlyingId underlying) const {
    const Key k{static_cast<std::int32_t>(underlying),
                static_cast<std::int32_t>(InstrumentKind::Spot), 0, 0, 0};
    if (auto it = by_key_.find(k); it != by_key_.end()) return it->second;
    return std::nullopt;
}

std::vector<Date> InstrumentRegistry::expiries(UnderlyingId underlying) const {
    std::vector<Date> out;
    for (const auto& s : specs_) {
        if (s.underlying != underlying) continue;
        if (s.kind == InstrumentKind::Spot) continue;
        if (!s.expiry.valid()) continue;
        out.push_back(s.expiry);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace volforge
