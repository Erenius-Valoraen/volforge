"""A complete, dependency-free reader for the .vfday format.

This exists to prove the format description in README.md is implementable: it was
written from that document alone and validates against the original vendor CSVs.
Use it as a specification you can execute, or as the starting point for a reader
in another language.

It is NOT the fast path. Decoding is written for clarity, one value at a time,
and runs a few hundred times slower than the C++ reader. See the performance
note in README.md for what to use instead and how to vectorise this if you do
want it in Python.

    python reference_reader.py <file.vfday>                 # summary
    python reference_reader.py <file.vfday> --instrument 5  # decode one
"""

from __future__ import annotations

import struct
import sys
import zlib
from dataclasses import dataclass

MAGIC = b"VFDAY003"
VERSION = 3
HEADER_FMT = "<8sIIiiq" + "qII" * 4 + "qII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)          # 112
TABLE_FMT = "<IIBBHiiii"
TABLE_SIZE = struct.calcsize(TABLE_FMT)            # 28
# table_index i32, row_count u32, blob_offset i64, blob_bytes u32, blob_crc u32,
# first_ts i64, last_ts i64
DIR_FMT = "<iIqIIqq"
DIR_SIZE = struct.calcsize(DIR_FMT)                # 40

BLOCK = 128
COLUMNS = ("ts", "last", "bid", "ask", "bid_qty", "ask_qty", "last_qty", "open_interest")
RIGHTS = {0: "CE", 1: "PE"}
KINDS = {0: "spot", 1: "future", 2: "option"}


# --- primitives --------------------------------------------------------------

def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    """LEB128, unsigned, seven bits per byte, high bit continues."""
    value = 0
    shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def unzigzag(v: int) -> int:
    return (v >> 1) ^ -(v & 1)


def unpack_bits(buf: bytes, pos: int, count: int, bits: int) -> tuple[list[int], int]:
    """`count` values of `bits` bits each, least significant bit first."""
    if bits == 0:
        return [0] * count, pos
    out = []
    acc = 0
    held = 0
    mask = (1 << bits) - 1
    for _ in range(count):
        while held < bits:
            acc |= buf[pos] << held
            pos += 1
            held += 8
        out.append(acc & mask)
        acc >>= bits
        held -= bits
    return out, pos


def decode_column(buf: bytes, pos: int) -> tuple[list[int], int]:
    """One column: delta, frame of reference, common divisor, bit-packing."""
    count, pos = read_varint(buf, pos)
    if count == 0:
        return [], pos

    first, pos = read_varint(buf, pos)
    values = [unzigzag(first)]

    scale, pos = read_varint(buf, pos)
    if scale == 0:
        raise ValueError("scale must be at least 1")

    while len(values) < count:
        n = min(BLOCK, count - len(values))

        packed_min, pos = read_varint(buf, pos)
        min_delta = unzigzag(packed_min)

        bits = buf[pos]
        pos += 1

        if bits == 64:                       # stored raw, eight bytes each
            residuals = list(struct.unpack_from(f"<{n}Q", buf, pos))
            pos += n * 8
        elif bits > 56:
            raise ValueError(f"width {bits} is never emitted")
        else:
            residuals, pos = unpack_bits(buf, pos, n, bits)

        previous = values[-1]
        for r in residuals:
            # Wrapping arithmetic, matching the encoder exactly.
            previous = (previous + (r + min_delta) * scale) & 0xFFFFFFFFFFFFFFFF
            values.append(previous - (1 << 64) if previous >> 63 else previous)

    return values, pos


# --- file --------------------------------------------------------------------

@dataclass
class Instrument:
    underlying: str
    kind: str
    right: str
    expiry: int          # yyyymmdd
    strike: float
    lot_size: int
    tick_size: float
    row_count: int
    first_ns: int
    last_ns: int
    _blob_offset: int
    _blob_bytes: int
    _blob_crc: int

    def symbol(self) -> str:
        """Reconstructs the vendor ticker, e.g. NIFTY03JUL2523000CE."""
        months = ("JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")
        y, m, d = self.expiry // 10000, (self.expiry // 100) % 100, self.expiry % 100
        strike = int(self.strike) if self.strike == int(self.strike) else self.strike
        return f"{self.underlying}{d:02d}{months[m - 1]}{y % 100:02d}{strike}{self.right}"


class VfdayFile:
    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "rb") as fh:
            self.blob = fh.read()

        if len(self.blob) < HEADER_SIZE:
            raise ValueError("shorter than a header")

        h = struct.unpack_from(HEADER_FMT, self.blob, 0)
        (magic, version, _flags, self.date, count, self.total_rows,
         t_off, t_bytes, t_crc, s_off, s_bytes, s_crc,
         tl_off, tl_count, tl_crc, d_off, d_bytes, d_crc,
         file_bytes, header_crc, _reserved) = h

        if magic != MAGIC:
            raise ValueError("not a vfday file")
        if version != VERSION:
            raise ValueError(f"version {version}, expected {VERSION}")
        # The header checksum covers every byte before itself.
        if zlib.crc32(self.blob[:HEADER_SIZE - 8]) != header_crc:
            raise ValueError("header checksum mismatch")
        if file_bytes != len(self.blob):
            raise ValueError("truncated or appended to")

        def region(off, n, crc, what):
            chunk = self.blob[off:off + n]
            if len(chunk) != n:
                raise ValueError(f"{what}: region outside the file")
            if zlib.crc32(chunk) != crc:
                raise ValueError(f"{what} checksum mismatch")
            return chunk

        table = region(t_off, t_bytes, t_crc, "instrument table")
        strings = region(s_off, s_bytes, s_crc, "string table")
        timeline = region(tl_off, tl_count * 8, tl_crc, "timeline")
        directory = region(d_off, d_bytes, d_crc, "directory")

        self.timeline = list(struct.unpack_from(f"<{tl_count}q", timeline, 0))

        self.instruments: list[Instrument] = []
        for i in range(count):
            (idx, rows, blob_off, blob_bytes, blob_crc,
             first_ns, last_ns) = struct.unpack_from(DIR_FMT, directory, i * DIR_SIZE)

            (u_off, u_len, kind, right, _pad,
             expiry, strike_minor, lot, tick_minor) = struct.unpack_from(
                TABLE_FMT, table, idx * TABLE_SIZE)

            self.instruments.append(Instrument(
                underlying=strings[u_off:u_off + u_len].decode(),
                kind=KINDS.get(kind, "?"), right=RIGHTS.get(right, "?"),
                expiry=expiry, strike=strike_minor / 100.0, lot_size=lot,
                tick_size=tick_minor / 100.0, row_count=rows,
                first_ns=first_ns, last_ns=last_ns,
                _blob_offset=blob_off, _blob_bytes=blob_bytes, _blob_crc=blob_crc))

    def quotes(self, index: int) -> dict[str, list[int]]:
        """Decodes one instrument. Prices and quantities stay in integer units."""
        inst = self.instruments[index]
        raw = self.blob[inst._blob_offset:inst._blob_offset + inst._blob_bytes]
        if zlib.crc32(raw) != inst._blob_crc:
            raise ValueError("instrument data checksum mismatch")

        out, pos = {}, 0
        for name in COLUMNS:
            values, pos = decode_column(raw, pos)
            if len(values) != inst.row_count:
                raise ValueError(f"{name}: length disagrees with the directory")
            out[name] = values
        if pos != len(raw):
            raise ValueError("trailing bytes after the last column")
        return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    f = VfdayFile(sys.argv[1])
    print(f"date          {f.date}")
    print(f"instruments   {len(f.instruments)}")
    print(f"rows          {f.total_rows}")
    print(f"timeline      {len(f.timeline)} distinct timestamps")

    if "--instrument" in sys.argv:
        i = int(sys.argv[sys.argv.index("--instrument") + 1])
        inst = f.instruments[i]
        q = f.quotes(i)
        print(f"\n{inst.symbol()}  lot {inst.lot_size}  {inst.row_count} rows")
        print("  ns_utc              last   bid   ask  bidq  askq  ltq       oi")
        for r in range(min(5, inst.row_count)):
            print(f"  {q['ts'][r]}  {q['last'][r]/100:7.2f} {q['bid'][r]/100:6.2f} "
                  f"{q['ask'][r]/100:6.2f} {q['bid_qty'][r]:5d} {q['ask_qty'][r]:5d} "
                  f"{q['last_qty'][r]:5d} {q['open_interest'][r]:8d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
