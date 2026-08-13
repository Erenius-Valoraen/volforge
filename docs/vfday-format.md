# NIFTY options tick data — layout and file format

One year of NSE NIFTY options, converted from GFDL vendor archives into a
columnar day store.

| | |
|---|---|
| Sessions | 253 |
| Range | 2025-08-04 … 2026-08-12 |
| Rows | 1,814,910,622 |
| Distinct timestamps | 5,679,335 |
| Store size | 8.2 GB (13.6 GB of vendor zips, ~145 GB as CSV) |
| Resolution | 1 second |
| Depth | Top of book only (Level 1) |

Everything here is verifiable: `reference_reader.py` in this directory is a
complete, dependency-free reader written from the format description below, and
it reproduces the original vendor CSVs value for value.

## What is in this directory

```
store/                          253 × .vfday, one per session
extracted/                      vendor daily zips, year/month folders
  2025/AUG_2025/GFDLNFO_TICK_04082025.zip
Nifty (Options) ... .fdmdownload  the original bulk download
reference_reader.py             a readable, executable spec
```

The `.fdmdownload` is 271,621 bytes short of complete — the tail carrying the
outer zip's central directory never arrived. Everything before it is intact, and
all 253 sessions were recovered.

## Reading it

The fast path is the C++ reader, reachable from Python:

```python
import volforge as vf
data = vf.open_store("C:/Users/Abhi/Trade Data/store")
```

Opening validates every file's checksums and takes about 40 ms for the year.
Nothing is decoded until a strategy asks for an instrument.

To read the bytes yourself, `reference_reader.py` is the specification you can
run:

```
python reference_reader.py store/2025-08-04.vfday --instrument 5
```

## Units and conventions, before anything else

Getting these wrong is silent, so they come first.

| Quantity | Type | Unit |
|---|---|---|
| Timestamps | `int64` | **nanoseconds** since the Unix epoch, **UTC** |
| Prices | `int32` | **paise** — hundredths of a rupee. `12345` is ₹123.45 |
| Quantities | `int32` | **contracts**, never lots. One NIFTY lot is 75 contracts |
| Open interest | `int64` | contracts |
| Dates | `int32` | `yyyymmdd`, e.g. `20250804` |

NSE trades at **UTC+05:30**. A session runs 09:15–15:30 local, so its first
timestamp is 03:45:00 UTC. Local wall clock is `(ts / 1e9 + 19800) % 86400`.

Prices are integers on purpose. Option premiums sit on a discrete tick grid that
binary floating point cannot represent, and an accumulated rounding error in a
P&L figure is a wrong answer rather than a display detail.

## What the vendor data actually means

These come from GFDL's own documentation and from inspecting the data. They
change what a correct analysis looks like.

**`last_qty = 0` does not mean no trade.** The exchange emits records with zero
quantity to keep the day's OHLC intact, and a trade landing at the end of a
second can have its price in one record and its quantity in the next. About 83%
of rows carry no trade. Any "did a trade happen" test built on `last_qty > 0`
alone will be wrong at the margins.

**`open_interest` updates every three minutes**, and reads `0` for the first
three minutes of a newly listed strike. Zero does not mean no interest, and a
filter like `oi > 10000` silently excludes strikes that have just been listed —
which are exactly the ones that appear when the market moves fast.

**Only symbols that traded that day are present.** The instrument universe is
"strikes that traded", not "strikes that were listed". A strike quoted all
session but never hit simply has no file.

**`bid` or `ask` can be zero**, meaning one-sided or no market. A long option
with no bid is genuinely worthless; a short with no offer cannot be bought back
at zero. Treating either as a tradeable price invents liquidity.

**Spreads are wide away from the money** — 2.5% on deep in-the-money strikes is
normal. Mid-price is not a price anyone can trade at.

**Lot size is not in the feed.** These files were converted with **75**, correct
for NIFTY across this period. NIFTY has been 25 and 50 historically; if you
extend this store backwards, that value has to change with it.

**One session is damaged.** `2025-11-04` lost the tail of a single contract,
`NIFTY31MAR2629000PE`, in the download. All 1,037 of its other instruments are
intact.

## The `.vfday` format

Little-endian throughout. All structures are packed with no padding.

### File layout

```
offset 0            header                      112 bytes
       112          payload: column blobs       one run per instrument
       table_offset instrument table            28 bytes × instrument_count
       strings_offset  string table             underlying names, not terminated
       timeline_offset timeline                 int64 × timeline_count
       directory_offset directory               40 bytes × instrument_count
```

Metadata sits at the end so the file can be written in a single pass. Every
region carries its own CRC, and the header records the total file length, so
truncation and appending are both detectable.

### Header (112 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0 | `char[8]` | magic, `"VFDAY003"` |
| 8 | `uint32` | version, currently `3` |
| 12 | `uint32` | flags, currently `0` |
| 16 | `int32` | date, `yyyymmdd` |
| 20 | `int32` | instrument_count |
| 24 | `int64` | total_rows |
| 32 | `int64` `uint32` `uint32` | table_offset, table_bytes, table_crc |
| 48 | `int64` `uint32` `uint32` | strings_offset, strings_bytes, strings_crc |
| 64 | `int64` `uint32` `uint32` | timeline_offset, timeline_count, timeline_crc |
| 80 | `int64` `uint32` `uint32` | directory_offset, directory_bytes, directory_crc |
| 96 | `int64` | file_bytes — the whole file |
| 104 | `uint32` | header_crc — over bytes 0…103 |
| 108 | `uint32` | reserved |

Note `timeline_count` is a **count of `int64` values**, not a byte length.

### Instrument table (28 bytes per entry)

| Type | Field |
|---|---|
| `uint32` | underlying_offset — into the string table |
| `uint32` | underlying_length |
| `uint8` | kind — 0 spot, 1 future, 2 option |
| `uint8` | right — 0 call, 1 put |
| `uint16` | reserved |
| `int32` | expiry, `yyyymmdd` |
| `int32` | strike, paise |
| `int32` | lot_size, contracts |
| `int32` | tick_size, paise |

The file is self-describing: it stores what an instrument **is**, never an id.
Two files can be read into the same registry and their contracts will match.

### Timeline

`timeline_count` `int64` nanosecond timestamps, strictly ascending — every
distinct instant at which any instrument printed.

This is what makes the store cheap to replay. Stepping through a session needs
only this array, so a backtest that touches four strikes decodes four
instruments, not nine hundred. For a typical day it is about 22,500 entries.

### Directory (40 bytes per entry)

| Type | Field |
|---|---|
| `int32` | table_index |
| `uint32` | row_count |
| `int64` | blob_offset — **absolute** file offset |
| `uint32` | blob_bytes |
| `uint32` | blob_crc |
| `int64` | first_ts — nanoseconds |
| `int64` | last_ts |

`first_ts` answers "had this instrument printed by time T?" without decoding
anything, which is what keeps a chain scan cheap.

### Column blobs

Each instrument's blob holds **eight columns**, concatenated, each independently
encoded, in this order:

```
ts, last, bid, ask, bid_qty, ask_qty, last_qty, open_interest
```

Every column decodes to exactly `row_count` values, ascending in `ts`.

### Column encoding

Delta, frame of reference, common divisor, bit-packing. No compression library
is involved; decoding is a prefix sum over unpacked integers.

```
varint   count
if count == 0: done
varint   zigzag(first_value)
varint   scale                      >= 1, the common divisor of all deltas
repeat until count values decoded, in blocks of at most 128:
    varint   zigzag(min_delta)      minimum of (delta / scale) in this block
    uint8    bits                   0…56, or 64
    if bits == 64:  n × uint64      residuals, raw little-endian
    else:           bit-packed residuals, `bits` each, least significant first
```

Reconstruction, starting from the second value:

```
value[i] = value[i-1] + (residual[i] + min_delta) * scale
```

- **varint** is LEB128, unsigned: seven bits per byte, high bit continues.
- **zigzag** maps signed to unsigned as `(n << 1) ^ (n >> 63)`; invert with
  `(v >> 1) ^ -(v & 1)`.
- Arithmetic **wraps** at 64 bits, deliberately, so encoder and decoder agree
  even on adversarial input.
- `bits == 64` means the block was stored raw. Widths above 56 are never
  bit-packed, because the packing accumulator holds 64 bits with up to 7 already
  in flight.

The `scale` step is what makes the format small. Timestamps are nanoseconds one
second apart, so their deltas are multiples of 10⁹ and would cost about thirty
bits each; divided through they cost two. Quantities move in lot-size steps and
prices sit on a five-paise grid, so both benefit from the same pass. Without it
the format produces 11.6 bytes per row; with it, **4.86**.

### Checksums

CRC-32, IEEE 802.3 — polynomial `0xEDB88320` reflected, initial value
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Identical to `zlib.crc32`.

A blob's checksum should be verified **immediately before decoding it**, not
merely at open. Corruption is then caught at the point of use rather than
trusted at the point of entry.

## Performance, and reading this in Python

Measured on one session (7.1 million rows, 8 columns):

| Reader | Values per second | One session | The year |
|---|---:|---:|---:|
| C++ (`volforge`) | ~520 M | ~0.11 s | ~25 s |
| `reference_reader.py`, scalar | ~4.6 M | ~12 s | ~53 min |

So a pure-Python scalar decoder is roughly **115× slower**. That matters less
than it sounds, for two reasons.

**You rarely decode a whole day.** Decoding is per instrument, on demand. A
strategy touching ten strikes decodes about 640,000 values — 0.14 s even in
scalar Python. The 12-second figure is the cost of decoding *everything*, which
a strategy has no reason to do.

**It vectorises well if you do need it.** The bit-unpacking is a shift-and-mask
over a byte array, and the reconstruction is `numpy.cumsum` on the residuals
plus a scalar multiply. A NumPy reader lands within a small factor of C++ and is
maybe forty lines. `reference_reader.py` is deliberately scalar because it exists
to be read, not to be fast.

**If you just want the data fast, use the C++ reader.** `volforge.open_store()`
decodes in native code and hands back the results; Python never touches a
per-value loop.

## Tools

Built from the volforge repository:

```
convert       vendor .zip (or a directory of CSVs) -> .vfday
verify_store  check every file: checksums, timelines, trading hours
inspect_day   dump one session and flag anything outside it
```

`verify_store` takes 0.9 s for the structural pass over the year and 137 s with
`--deep`, which decodes every column and checks every blob checksum. Worth
running after any conversion, or if a drive ever looks suspect.

## Provenance

Converted from GFDL NFO tick archives with volforge's `convert`, lot size 75,
with verification on — every file was read back and compared against its source
before being accepted.

The whole store has been verified end to end: 253 files, all checksums, all
1,814,910,622 rows decoded, every timestamp inside its session's trading hours.
Eight sampled days were additionally re-converted and compared byte for byte.
