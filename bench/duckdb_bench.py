"""
DuckDB benchmark on the same udfbench datasets used by tools/bench.cpp.

Operations timed (5 runs, median reported):
  - CSV full scan       : SELECT all targeted columns, forces full read
  - Parquet conversion  : COPY CSV -> Parquet (snappy / zstd)
  - Parquet full scan   : same query on Parquet file
  - Filtered scan       : SELECT COUNT(*) WHERE col = val  (CSV and Parquet)

Columns and filter targets match the columns tested in results/test_results.txt.
"""

import duckdb
import os
import time
import statistics
import tempfile

N_RUNS   = 5
N_WARMUP = 1

# Resolved from the repo root so the script runs from any directory.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "benchmark/udfbench-dataset/dataset/csvs/large")

# ---------------------------------------------------------------------------
# Each entry: (label, csv_path, max_rows, col_indices_to_scan, filter_targets)
# col_indices: 0-based column positions (DuckDB column names are resolved at runtime
#              to handle both "column0" for <10 cols and "column00" for >=10 cols)
# filter_targets: list of (col_index, value) pairs matching results/test_results.txt
# ---------------------------------------------------------------------------
DATASETS = [
    {
        "label":    "views_stats.csv  [5 000 000 rows]",
        "csv":      f"{DATA}/views_stats.csv",
        "max_rows": 5_000_000,
        "scan_idx": [0, 1, 2, 3],
        "filters": [
            (0, "2023/01"),
            (0, "2019/05"),
            (0, "2022/07"),
            (2, "OpenAIRE"),
            (2, "IRUS-UK"),
        ],
    },
    {
        "label":    "artifact_authors.csv  [all rows]",
        "csv":      f"{DATA}/artifact_authors.csv",
        "max_rows": 0,
        "scan_idx": [0, 2, 4],
        "filters": [
            (2, "Shin, Y. H."),
            (2, "Y. G. Xie"),
            (2, "Mahakud B."),
            (4, "Shin"),
        ],
    },
    {
        "label":    "artifacts.csv  [all rows]",
        "csv":      f"{DATA}/artifacts.csv",
        "max_rows": 0,
        "scan_idx": [0, 1, 2, 3, 6, 8, 12],
        "filters": [
            (6,  "Open Access"),
            (6,  "not available"),
            (8,  "false"),
            (12, "dataset"),
            (12, "publication"),
        ],
    },
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def median_ms(fn, warmup=N_WARMUP, runs=N_RUNS):
    for _ in range(warmup):
        fn()
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t0) * 1000.0)
    return statistics.median(times)


def file_mb(path):
    if not os.path.exists(path):
        return 0.0
    return os.path.getsize(path) / (1024 ** 2)


def print_line(c="-", w=88):
    print(c * w)


def col_expr(cols):
    """Build a SELECT expression that forces all columns to be read."""
    return ", ".join(f"count({c})" for c in cols)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

con = duckdb.connect()

for ds in DATASETS:
    label    = ds["label"]
    csv_path = ds["csv"]
    max_rows = ds["max_rows"]

    limit_clause = f"LIMIT {max_rows}" if max_rows else ""

    # Parquet output paths (written to /tmp so we don't pollute the repo)
    tmp_dir    = tempfile.mkdtemp()
    csv_sub    = f"{tmp_dir}/data.csv"          # limited CSV copy
    pq_snappy  = f"{tmp_dir}/data.snappy.parquet"
    pq_zstd    = f"{tmp_dir}/data.zstd.parquet"

    print()
    print("=" * 88)
    print(f"  {label}")
    print("=" * 88)

    # Read everything as VARCHAR — our implementation treats all values as strings,
    # so this is the fair comparison. It also avoids auto-detection errors on
    # columns with mixed or malformed values (e.g. "0000-00-00" date fields).
    read_fn = f"read_csv_auto('{csv_path}', header=false, all_varchar=true)"

    # ------------------------------------------------------------------
    # 0. Row count
    # ------------------------------------------------------------------
    total_rows = con.execute(
        f"SELECT count(*) FROM {read_fn} {limit_clause}"
    ).fetchone()[0]
    print(f"  Rows: {total_rows:,}    CSV size: {file_mb(csv_path):.1f} MB")

    # ------------------------------------------------------------------
    # 1. Export limited CSV so Parquet files cover the same row range
    # ------------------------------------------------------------------
    read_sub_fn = f"read_csv_auto('{csv_sub}', header=false, all_varchar=true)"

    print(f"\n  Exporting {total_rows:,} rows to tmp CSV...", end=" ", flush=True)
    t0 = time.perf_counter()
    con.execute(
        f"COPY (SELECT * FROM {read_fn} {limit_clause}) "
        f"TO '{csv_sub}' (HEADER false, DELIMITER ',')"
    )
    print(f"{(time.perf_counter()-t0)*1000:.0f} ms")

    csv_mb = file_mb(csv_sub)

    # ------------------------------------------------------------------
    # Resolve actual DuckDB column names from 0-based indices.
    # DuckDB names columns "column0".."column9" when total < 10,
    # but "column00".."column09" etc. when total >= 10.
    # ------------------------------------------------------------------
    actual_col_names = [
        row[0] for row in
        con.execute(f"DESCRIBE SELECT * FROM {read_sub_fn} LIMIT 0").fetchall()
    ]
    def col_name(idx): return actual_col_names[idx]

    cols    = [col_name(i) for i in ds["scan_idx"]]
    filters = [(col_name(i), v) for i, v in ds["filters"]]

    # ------------------------------------------------------------------
    # 2. CSV → Parquet (snappy and zstd), timed
    # ------------------------------------------------------------------
    print()
    print_line()
    print(f"  {'FORMAT':<14} {'Write(ms)':>10} {'File(MB)':>10} {'Ratio vs CSV':>14}")
    print_line()

    def write_snappy():
        con.execute(
            f"COPY (SELECT * FROM read_csv_auto('{csv_sub}', header=false, all_varchar=true)) "
            f"TO '{pq_snappy}' (FORMAT parquet, COMPRESSION snappy)"
        )
    def write_zstd():
        con.execute(
            f"COPY (SELECT * FROM read_csv_auto('{csv_sub}', header=false, all_varchar=true)) "
            f"TO '{pq_zstd}' (FORMAT parquet, COMPRESSION zstd)"
        )

    # Run once first (create the files), then time subsequent writes
    write_snappy(); write_zstd()
    t_snappy = median_ms(write_snappy)
    t_zstd   = median_ms(write_zstd)

    mb_snappy = file_mb(pq_snappy)
    mb_zstd   = file_mb(pq_zstd)

    print(f"  {'CSV (raw)':<14} {'—':>10} {csv_mb:>10.1f} {'1.00x':>14}")
    print(f"  {'Parquet/snappy':<14} {t_snappy:>10.0f} {mb_snappy:>10.1f} "
          f"{csv_mb/mb_snappy if mb_snappy else 0:>13.2f}x")
    print(f"  {'Parquet/zstd':<14} {t_zstd:>10.0f} {mb_zstd:>10.1f} "
          f"{csv_mb/mb_zstd if mb_zstd else 0:>13.2f}x")

    # ------------------------------------------------------------------
    # 3. Full scan — force-read the tested columns from CSV and Parquet
    # ------------------------------------------------------------------
    expr = col_expr(cols)

    def scan_csv():
        con.execute(f"SELECT {expr} FROM read_csv_auto('{csv_sub}', header=false, all_varchar=true)")

    def scan_pq_snappy():
        con.execute(f"SELECT {expr} FROM '{pq_snappy}'")

    def scan_pq_zstd():
        con.execute(f"SELECT {expr} FROM '{pq_zstd}'")

    t_csv_scan     = median_ms(scan_csv)
    t_snappy_scan  = median_ms(scan_pq_snappy)
    t_zstd_scan    = median_ms(scan_pq_zstd)

    print()
    print_line()
    print(f"  FULL SCAN ({len(cols)} columns, {N_RUNS} runs, warm cache)")
    print_line()
    print(f"  {'Source':<20} {'Time(ms)':>10} {'vs CSV':>10}")
    print_line()
    print(f"  {'CSV':<20} {t_csv_scan:>10.1f} {'—':>10}")
    print(f"  {'Parquet/snappy':<20} {t_snappy_scan:>10.1f} "
          f"{t_csv_scan/t_snappy_scan if t_snappy_scan else 0:>9.2f}x")
    print(f"  {'Parquet/zstd':<20} {t_zstd_scan:>10.1f} "
          f"{t_csv_scan/t_zstd_scan if t_zstd_scan else 0:>9.2f}x")

    # ------------------------------------------------------------------
    # 4. Filtered scan — same targets as results/test_results.txt
    # ------------------------------------------------------------------
    print()
    print_line()
    print(f"  FILTERED SCAN  ({N_RUNS} runs, warm cache)")
    print_line()
    print(f"  {'Filter':<36} {'CSV(ms)':>9} {'PQ/snappy(ms)':>15} {'PQ/zstd(ms)':>13} {'Matched':>9}")
    print_line()

    for col, val in filters:
        matched = con.execute(
            f"SELECT count(*) FROM '{pq_snappy}' WHERE {col} = '{val}'"
        ).fetchone()[0]

        def fs_csv(c=col, v=val):
            con.execute(
                f"SELECT count(*) FROM read_csv_auto('{csv_sub}', header=false, all_varchar=true) "
                f"WHERE {c} = '{v}'"
            )
        def fs_snappy(c=col, v=val):
            con.execute(f"SELECT count(*) FROM '{pq_snappy}' WHERE {c} = '{v}'")
        def fs_zstd(c=col, v=val):
            con.execute(f"SELECT count(*) FROM '{pq_zstd}' WHERE {c} = '{v}'")

        t_fc  = median_ms(fs_csv)
        t_fs  = median_ms(fs_snappy)
        t_fz  = median_ms(fs_zstd)

        label_str = f"{col} = '{val}'"
        if len(label_str) > 34:
            label_str = label_str[:31] + "..."
        print(f"  {label_str:<36} {t_fc:>9.1f} {t_fs:>15.1f} {t_fz:>13.1f} {matched:>9,}")

    # Cleanup
    for f in [csv_sub, pq_snappy, pq_zstd]:
        try: os.remove(f)
        except: pass
    try: os.rmdir(tmp_dir)
    except: pass

print()
print("=" * 88)
print("  Done.")
print("=" * 88)
