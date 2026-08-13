import os, glob, time, numpy as np, pyarrow as pa, pyarrow.csv as pacsv, pyarrow.parquet as pq

SRC = os.path.expanduser("~/Downloads/GFDLNFO_TICK_01072025/GFDLNFO_TICK_01072025")
OUT = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(OUT, "opt_cache.parquet")
UNIQ = os.path.join(OUT, "uniq.parquet")
files = sorted(glob.glob(os.path.join(SRC, "*.csv")))
src_bytes = sum(os.path.getsize(f) for f in files)
print(f"files: {len(files)}   CSV total: {src_bytes/1e6:.1f} MB")

DELTA = "DELTA_BINARY_PACKED"


def build():
    ro = pacsv.ReadOptions(use_threads=True)
    co = pacsv.ConvertOptions(column_types={
        "Ticker": pa.string(), "Date": pa.string(), "Time": pa.string(),
        "LTP": pa.float64(), "BuyPrice": pa.float64(), "SellPrice": pa.float64(),
        "BuyQty": pa.int64(), "SellQty": pa.int64(), "LTQ": pa.int64(),
        "OpenInterest": pa.int64(),
    })
    t0 = time.time()
    raw = pa.concat_tables([pacsv.read_csv(f, read_options=ro, convert_options=co)
                            for f in files])
    print(f"parse CSV: {time.time()-t0:.1f}s   rows: {raw.num_rows:,}")

    pq.write_table(raw, os.path.join(OUT, "a_naive.parquet"), compression="snappy")

    t0 = time.time()
    tick = np.asarray(raw.column("Ticker"))
    uniq, inst_id = np.unique(tick, return_inverse=True)
    inst_id = inst_id.astype(np.int32)

    tv = np.asarray(raw.column("Time")).astype("U8").view("U1").reshape(-1, 8)
    def d2(a, b):
        return (tv[:, a].view(np.uint32) - 48) * 10 + (tv[:, b].view(np.uint32) - 48)
    secs = (d2(0, 1) * 3600 + d2(3, 4) * 60 + d2(6, 7)).astype(np.int32)

    def px(name):
        return np.rint(raw.column(name).to_numpy(zero_copy_only=False) * 100).astype(np.int32)
    def qty(name):
        return raw.column(name).to_numpy().astype(np.int32)

    opt = pa.table({
        "inst": pa.array(inst_id), "sec": pa.array(secs),
        "ltp": pa.array(px("LTP")), "bid": pa.array(px("BuyPrice")),
        "ask": pa.array(px("SellPrice")), "bidq": pa.array(qty("BuyQty")),
        "askq": pa.array(qty("SellQty")), "ltq": pa.array(qty("LTQ")),
        "oi": pa.array(qty("OpenInterest")),
    })
    opt = opt.take(pa.array(np.lexsort((secs, inst_id))))  # sort by (inst, sec)
    print(f"encode+sort: {time.time()-t0:.1f}s")

    pq.write_table(opt, CACHE, compression="lz4")
    pq.write_table(pa.table({"t": pa.array(uniq.tolist())}), UNIQ)
    return opt, uniq


if os.path.exists(CACHE):
    opt, uniq = pq.read_table(CACHE), np.array(pq.read_table(UNIQ).column("t").to_pylist())
    print("loaded encoded table from cache")
else:
    opt, uniq = build()

cols = [c for c in opt.column_names]
pq.write_table(opt, os.path.join(OUT, "b_int_zstd.parquet"),
               compression="zstd", compression_level=9)
pq.write_table(opt, os.path.join(OUT, "c_delta_zstd.parquet"),
               compression="zstd", compression_level=9, use_dictionary=False,
               column_encoding={c: DELTA for c in cols})

print(f"\n{'variant':24} {'MB':>9} {'vs CSV':>9}  {'bytes/row':>9}")
print(f"{'CSV (source)':24} {src_bytes/1e6:9.1f} {1.0:8.2f}x {src_bytes/opt.num_rows:9.1f}")
for n in ["a_naive.parquet", "b_int_zstd.parquet", "c_delta_zstd.parquet"]:
    p = os.path.join(OUT, n)
    if not os.path.exists(p):
        continue
    s = os.path.getsize(p)
    print(f"{n:24} {s/1e6:9.1f} {src_bytes/s:8.2f}x {s/opt.num_rows:9.2f}")

best = os.path.join(OUT, "c_delta_zstd.parquet")
t0 = time.time(); full = pq.read_table(best); t_full = time.time() - t0
print(f"\nfull-day load ({full.num_rows:,} rows, 9 cols): {t_full:.2f}s")

t0 = time.time(); _ = pq.read_table(best, columns=["inst", "sec", "ltp"]); t_3 = time.time() - t0
print(f"3-column projection load:                {t_3:.2f}s")

target = int(np.where(uniq == "NIFTY03JUL2523000CE.NFO")[0][0])
t0 = time.time()
one = pq.read_table(best, filters=[("inst", "==", target)])
print(f"single-instrument slice ({one.num_rows:,} rows):  {(time.time()-t0)*1000:.0f}ms")

ltp = full.column("ltp").to_numpy()
t0 = time.time(); _ = np.diff(ltp).astype(np.int64).sum(); t_vec = time.time() - t0
print(f"vectorized pass over {len(ltp):,} rows:      {t_vec*1000:.0f}ms")

t0 = time.time()
acc = 0
for v in ltp[:1_000_000].tolist():
    acc += v
t_loop = time.time() - t0
print(f"pure-Python loop over 1,000,000 rows:     {t_loop*1000:.0f}ms  "
      f"({t_loop*1e9/1e6:.0f} ns/row)")
