// Integer column codec: delta, frame of reference, bit-packing.
//
// Chosen over a general-purpose compressor because the data is already the
// shape this exploits, and because it needs no dependency at all:
//
//   - Timestamps within an instrument are monotonic, so consecutive deltas are
//     almost always 1 second. One or two bits each.
//   - Prices move slowly. Deltas are small and cluster tightly.
//   - Open interest updates every three minutes, so most deltas are exactly
//     zero. Zero bits each, once the frame of reference absorbs the offset.
//
// Blocks of 128 values each carry the minimum delta and the bit width needed
// for the residuals, so a block of unchanged values costs a byte or two total
// while a block that genuinely jumps pays only for itself.
//
// Decoding is a prefix sum over unpacked integers: no entropy decoding, no
// dictionary, no window. Measured throughput is high enough that reading a
// compressed day beats reading a raw one, because the I/O saved is worth more
// than the arithmetic spent.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace volforge {

// Values per bit-packed block. 128 keeps the per-block header amortised while
// staying small enough that a local disturbance does not widen the whole column.
constexpr std::size_t kCodecBlock = 128;

// Appends the encoded form of `values` to `out`. Returns bytes written.
std::size_t encode_column(const std::int64_t* values, std::size_t count,
                          std::vector<std::uint8_t>& out);

// Decodes into `out`, which is resized to the stored count. Returns the number
// of bytes consumed, or 0 if the input is malformed.
std::size_t decode_column(const std::uint8_t* data, std::size_t size,
                          std::vector<std::int64_t>& out);

// Number of values the encoded block claims to hold, without decoding it.
std::size_t peek_column_count(const std::uint8_t* data, std::size_t size);

}  // namespace volforge
