#include "volforge/codec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace volforge {
namespace {

std::uint64_t zigzag(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

std::int64_t unzigzag(std::uint64_t v) {
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
}

void put_varint(std::uint64_t v, std::vector<std::uint8_t>& out) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v) | 0x80);
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// Returns false when the input is truncated or the varint is over-long.
bool get_varint(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& out) {
    out = 0;
    int shift = 0;
    while (p < end) {
        const std::uint8_t byte = *p++;
        out |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

// Widest bit count the packing accumulator can carry safely; see encode_column.
constexpr int kMaxPackedBits = 56;

std::uint64_t gcd_of(std::uint64_t a, std::uint64_t b) {
    while (b != 0) { const std::uint64_t t = a % b; a = b; b = t; }
    return a;
}

int bits_for(std::uint64_t range) {
    int bits = 0;
    while (range > 0) { ++bits; range >>= 1; }
    return bits;
}

void pack(const std::uint64_t* values, std::size_t n, int bits,
          std::vector<std::uint8_t>& out) {
    if (bits == 0) return;
    std::uint64_t acc = 0;
    int held = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc |= (values[i] & ((bits == 64) ? ~0ULL : ((1ULL << bits) - 1))) << held;
        held += bits;
        while (held >= 8) {
            out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
            acc >>= 8;
            held -= 8;
        }
    }
    if (held > 0) out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
}

std::size_t packed_bytes(std::size_t n, int bits) {
    return (n * static_cast<std::size_t>(bits) + 7) / 8;
}

bool unpack(const std::uint8_t*& p, const std::uint8_t* end, std::size_t n, int bits,
            std::uint64_t* out) {
    if (bits == 0) {
        std::fill_n(out, n, 0ULL);
        return true;
    }
    const std::size_t need = packed_bytes(n, bits);
    if (static_cast<std::size_t>(end - p) < need) return false;

    const std::uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    std::uint64_t acc = 0;
    int held = 0;
    const std::uint8_t* q = p;

    for (std::size_t i = 0; i < n; ++i) {
        while (held < bits) {
            acc |= static_cast<std::uint64_t>(*q++) << held;
            held += 8;
        }
        out[i] = acc & mask;
        acc >>= bits;
        held -= bits;
    }
    p += need;
    return true;
}

}  // namespace

std::size_t encode_column(const std::int64_t* values, std::size_t count,
                          std::vector<std::uint8_t>& out) {
    const std::size_t start = out.size();

    put_varint(count, out);
    if (count == 0) return out.size() - start;

    put_varint(zigzag(values[0]), out);

    // Common divisor of every delta, factored out before packing.
    //
    // This is where most of the remaining size goes if it is skipped. Timestamps
    // are nanoseconds a whole second apart, so their deltas are multiples of
    // 1e9 and cost thirty-odd bits each; divided through, they cost two.
    // Quantities move in lot-size steps and prices on a five-paise grid, so both
    // benefit for free.
    std::uint64_t scale = 0;
    {
        std::int64_t previous = values[0];
        for (std::size_t j = 1; j < count && scale != 1; ++j) {
            const std::int64_t d = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(values[j]) - static_cast<std::uint64_t>(previous));
            previous = values[j];
            const std::uint64_t magnitude =
                d < 0 ? (~static_cast<std::uint64_t>(d) + 1) : static_cast<std::uint64_t>(d);
            scale = gcd_of(scale, magnitude);
        }
    }
    if (scale == 0) scale = 1;   // every delta was zero
    put_varint(scale, out);

    std::uint64_t residuals[kCodecBlock];
    std::int64_t  deltas[kCodecBlock];

    std::int64_t previous = values[0];
    std::size_t i = 1;

    while (i < count) {
        const std::size_t n = std::min(kCodecBlock, count - i);

        std::int64_t min_delta = std::numeric_limits<std::int64_t>::max();
        for (std::size_t j = 0; j < n; ++j) {
            // Wrapping arithmetic on purpose: an adversarial column could
            // overflow a signed subtraction, and the decoder wraps identically,
            // so the round trip stays exact either way.
            const std::int64_t raw = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(values[i + j]) - static_cast<std::uint64_t>(previous));
            previous = values[i + j];
            deltas[j] = scale == 1 ? raw : raw / static_cast<std::int64_t>(scale);
            min_delta = std::min(min_delta, deltas[j]);
        }

        std::uint64_t range = 0;
        for (std::size_t j = 0; j < n; ++j) {
            residuals[j] = static_cast<std::uint64_t>(deltas[j]) -
                           static_cast<std::uint64_t>(min_delta);
            range = std::max(range, residuals[j]);
        }

        // The packing accumulator holds at most 64 bits, and up to 7 are already
        // in flight when a value is shifted in, so anything wider than 56 would
        // silently drop its top bits. Those blocks are stored raw instead --
        // rare in practice, and cheaper than getting it subtly wrong.
        int bits = bits_for(range);
        if (bits > kMaxPackedBits) bits = 64;

        put_varint(zigzag(min_delta), out);
        out.push_back(static_cast<std::uint8_t>(bits));

        if (bits == 64) {
            for (std::size_t j = 0; j < n; ++j) {
                for (int b = 0; b < 8; ++b) {
                    out.push_back(static_cast<std::uint8_t>((residuals[j] >> (8 * b)) & 0xFF));
                }
            }
        } else {
            pack(residuals, n, bits, out);
        }

        i += n;
    }
    return out.size() - start;
}

std::size_t peek_column_count(const std::uint8_t* data, std::size_t size) {
    const std::uint8_t* p = data;
    std::uint64_t count = 0;
    if (!get_varint(p, data + size, count)) return 0;
    return static_cast<std::size_t>(count);
}

std::size_t decode_column(const std::uint8_t* data, std::size_t size,
                          std::vector<std::int64_t>& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;

    std::uint64_t count = 0;
    if (!get_varint(p, end, count)) return 0;

    out.clear();
    out.resize(static_cast<std::size_t>(count));
    if (count == 0) return static_cast<std::size_t>(p - data);

    std::uint64_t first = 0;
    if (!get_varint(p, end, first)) return 0;
    out[0] = unzigzag(first);

    std::uint64_t scale = 0;
    if (!get_varint(p, end, scale) || scale == 0) return 0;

    std::uint64_t residuals[kCodecBlock];
    std::int64_t previous = out[0];
    std::size_t i = 1;

    while (i < count) {
        const std::size_t n = std::min(kCodecBlock, static_cast<std::size_t>(count) - i);

        std::uint64_t packed_min = 0;
        if (!get_varint(p, end, packed_min)) return 0;
        const std::int64_t min_delta = unzigzag(packed_min);

        if (p >= end) return 0;
        const int bits = *p++;
        if (bits < 0 || bits > 64) return 0;

        if (bits == 64) {
            if (static_cast<std::size_t>(end - p) < n * 8) return 0;
            for (std::size_t j = 0; j < n; ++j) {
                std::uint64_t v = 0;
                for (int b = 0; b < 8; ++b) {
                    v |= static_cast<std::uint64_t>(*p++) << (8 * b);
                }
                residuals[j] = v;
            }
        } else if (bits > kMaxPackedBits) {
            return 0;   // encoder never emits these widths
        } else if (!unpack(p, end, n, bits, residuals)) {
            return 0;
        }

        for (std::size_t j = 0; j < n; ++j) {
            std::int64_t delta = static_cast<std::int64_t>(
                residuals[j] + static_cast<std::uint64_t>(min_delta));
            if (scale != 1) delta = static_cast<std::int64_t>(static_cast<std::uint64_t>(delta) *
                                                             scale);
            previous = static_cast<std::int64_t>(static_cast<std::uint64_t>(previous) +
                                                 static_cast<std::uint64_t>(delta));
            out[i + j] = previous;
        }
        i += n;
    }
    return static_cast<std::size_t>(p - data);
}

}  // namespace volforge
