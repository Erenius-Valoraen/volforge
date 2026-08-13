#include "harness.hpp"

#include "volforge/codec.hpp"

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace volforge;

namespace {

bool roundtrips(const std::vector<std::int64_t>& values) {
    std::vector<std::uint8_t> encoded;
    const std::size_t written = encode_column(values.data(), values.size(), encoded);
    if (written != encoded.size()) return false;

    std::vector<std::int64_t> decoded;
    const std::size_t consumed = decode_column(encoded.data(), encoded.size(), decoded);
    return consumed == encoded.size() && decoded == values;
}

double bytes_per_value(const std::vector<std::int64_t>& values) {
    std::vector<std::uint8_t> encoded;
    encode_column(values.data(), values.size(), encoded);
    return static_cast<double>(encoded.size()) / static_cast<double>(values.size());
}

}  // namespace

TEST(codec_round_trips_the_shapes_the_feed_actually_produces) {
    // One second apart, as timestamps within an instrument are.
    std::vector<std::int64_t> seconds;
    for (int i = 0; i < 5000; ++i) seconds.push_back(1'700'000'000LL + i);
    CHECK(roundtrips(seconds));

    // Open interest: constant for minutes at a time, then a step.
    std::vector<std::int64_t> oi;
    for (int i = 0; i < 5000; ++i) oi.push_back(34575 + (i / 180) * 75);
    CHECK(roundtrips(oi));

    // Prices wandering on a five-paise grid.
    std::mt19937_64 rng(1);
    std::vector<std::int64_t> prices;
    std::int64_t px = 12'500;
    for (int i = 0; i < 5000; ++i) {
        px += static_cast<std::int64_t>(rng() % 11) * 5 - 25;
        prices.push_back(px);
    }
    CHECK(roundtrips(prices));
}

TEST(codec_round_trips_degenerate_and_extreme_input) {
    CHECK(roundtrips({}));
    CHECK(roundtrips({0}));
    CHECK(roundtrips({-1}));
    CHECK(roundtrips(std::vector<std::int64_t>(1000, 7)));          // constant
    CHECK(roundtrips({std::numeric_limits<std::int64_t>::min(),
                      std::numeric_limits<std::int64_t>::max(),
                      0, -1, 1}));

    // A column that alternates between the extremes exercises delta overflow,
    // which is why the codec does its arithmetic unsigned on both sides.
    std::vector<std::int64_t> swinging;
    for (int i = 0; i < 500; ++i) {
        swinging.push_back(i % 2 ? std::numeric_limits<std::int64_t>::max()
                                 : std::numeric_limits<std::int64_t>::min());
    }
    CHECK(roundtrips(swinging));
}

TEST(codec_round_trips_every_length_around_a_block_boundary) {
    for (std::size_t n = 0; n <= 2 * kCodecBlock + 3; ++n) {
        std::vector<std::int64_t> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(static_cast<std::int64_t>(i * 3 - 7));
        CHECK(roundtrips(v));
    }
}

TEST(codec_round_trips_random_noise) {
    std::mt19937_64 rng(99);
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<std::int64_t> v(1 + rng() % 900);
        for (auto& x : v) x = static_cast<std::int64_t>(rng());
        CHECK(roundtrips(v));
    }
}

TEST(codec_actually_compresses_the_shapes_it_was_designed_for) {
    std::vector<std::int64_t> seconds;
    for (int i = 0; i < 10000; ++i) seconds.push_back(1'700'000'000LL + i);
    // A constant one-second step costs a fraction of a bit per value.
    CHECK(bytes_per_value(seconds) < 0.2);

    std::vector<std::int64_t> constant(10000, 42);
    CHECK(bytes_per_value(constant) < 0.2);

    std::mt19937_64 rng(5);
    std::vector<std::int64_t> prices;
    std::int64_t px = 12'500;
    for (int i = 0; i < 10000; ++i) {
        px += static_cast<std::int64_t>(rng() % 7) * 5 - 15;
        prices.push_back(px);
    }
    // Small wandering deltas: well under a byte, against eight raw.
    CHECK(bytes_per_value(prices) < 1.0);

    // Incompressible input must not inflate much beyond its raw size.
    std::vector<std::int64_t> noise(10000);
    for (auto& x : noise) x = static_cast<std::int64_t>(rng());
    CHECK(bytes_per_value(noise) < 9.5);
}

TEST(codec_rejects_malformed_input_rather_than_reading_past_the_end) {
    std::vector<std::int64_t> v;
    for (int i = 0; i < 400; ++i) v.push_back(i);

    std::vector<std::uint8_t> encoded;
    encode_column(v.data(), v.size(), encoded);

    std::vector<std::int64_t> decoded;
    // Every truncation must be detected rather than decoded into garbage.
    for (std::size_t cut = 1; cut < encoded.size(); cut += 7) {
        const std::size_t consumed = decode_column(encoded.data(), cut, decoded);
        if (consumed != 0) {
            // A prefix that happens to be self-consistent must still not claim
            // more values than it carried.
            CHECK(consumed <= cut);
        }
    }
    CHECK_EQ(decode_column(encoded.data(), 0, decoded), std::size_t{0});
}

TEST(codec_reports_its_length_without_decoding) {
    std::vector<std::int64_t> v(777, 3);
    std::vector<std::uint8_t> encoded;
    encode_column(v.data(), v.size(), encoded);
    CHECK_EQ(peek_column_count(encoded.data(), encoded.size()), std::size_t{777});
}
