# Benchmarks

The numbers quoted in [../docs/design.md](../docs/design.md) were produced here rather than
estimated. Anyone should be able to reproduce them or show them to be wrong.

## `encoding_bench.py`

Measures storage encodings against a real vendor sample day: CSV parse time, compressed
size across encoding variants, and query latency for full-day loads, column projections and
single-instrument slices.

Point `SRC` at an extracted vendor day directory before running, and note that it writes
several hundred MB of intermediate files.

```bash
python bench/encoding_bench.py
```

Requires `pyarrow`, `numpy` and `pandas`.

### Result summary

Sample day: NIFTY options, 2025-07-01 — 6,545,762 rows, 904 instruments, 520.1 MB of CSV.

| Variant | Size | vs CSV | Bytes/row |
|---|---:|---:|---:|
| CSV (source) | 520.1 MB | 1.00× | 79.50 |
| Parquet, naive strings + floats + snappy | 48.8 MB | 10.7× | 7.46 |
| Int-encoded + zstd | 40.9 MB | 12.7× | 6.25 |
| Int-encoded + delta + zstd | 30.9 MB | 16.8× | 4.72 |

Sort order matters more than expected — `(instrument, second)` yields 4.72 bytes/row against
15.61 for `(second, instrument)`, a 3.3× difference.

Hardware: 20 cores, 16 GB RAM, Windows 11.
