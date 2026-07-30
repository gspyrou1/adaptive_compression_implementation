# Adaptive Dictionary Compression for String Columns

A from-scratch C++14 implementation of the adaptive dictionary compression scheme described in:

> Yannis Foufoulas, Lefteris Sidirourgos, Eleftherios Stamatogiannakis, Yannis Ioannidis.
> **"Adaptive Compression for Fast Scans on String Columns."** SIGMOD '21, pp. 554-562.
> [doi:10.1145/3448016.3452798](https://doi.org/10.1145/3448016.3452798)
> (a copy is included in this repo as `adaptive_compression.pdf`)

The repository contains the compressor, the decompressor, the three query
operators the paper defines over the compressed format (full scan, filtered scan,
random access), a benchmark harness, a DuckDB/Parquet baseline for comparison, and
the write-up of the measured results (`report.tex`, with raw numbers in
`test_results.txt` and `duckdb_results.txt`).

---

## Table of contents

- [The problem, in one page](#the-problem-in-one-page)
- [Quick start](#quick-start)
- [How the format works](#how-the-format-works)
  - [Pages and differential sequences](#pages-and-differential-sequences)
  - [The three page types](#the-three-page-types)
  - [Choosing local vs. differential](#choosing-local-vs-differential)
  - [A worked example](#a-worked-example)
- [The operators](#the-operators)
- [Binary file format](#binary-file-format)
- [Tools and usage](#tools-and-usage)
- [Benchmark results](#benchmark-results)
- [Repository layout](#repository-layout)
- [Paper-to-code map](#paper-to-code-map)
- [Limitations and design notes](#limitations-and-design-notes)
- [Tuning](#tuning)

---

## The problem, in one page

Columnar OLAP systems dictionary-encode string columns: store each distinct string
once in a dictionary, and store one integer *offset* per row. There are two classic
ways to scope that dictionary, and each of them loses something.

| | Local (block-level) dictionary | Global (table-level) dictionary |
|---|---|---|
| Used by | Parquet, ORC | IBM DB2 BLU, SAP HANA |
| Offset width | Small, since each block has few distinct values | Large, since it covers every distinct value in the table |
| Duplicate values | Repeated in every block that contains them | Stored once |
| Filtered scan | One dictionary lookup per block | One lookup total, but every block's offsets must be scanned |
| Random access | Cheap, because the block is self-contained | Needs the (big) global dictionary |

The paper's insight is that you do not have to pick one. Encode a column as a run of
pages where the first is self-contained and the following ones store only the values
that are new relative to their predecessors, the same idea as I-frames and P-frames
in video compression. The result behaves like a global dictionary for storage, since
duplicates across pages are not repeated, and like a local dictionary for queries,
since offsets stay narrow and pages stay individually skippable. A cost function
decides at compression time when the next page should restart the run.

This implementation reproduces that scheme, its cost function, its page-skipping
statistics, and its two-page random access, and measures all of it against DuckDB
reading Parquet/Snappy and Parquet/Zstd.

---

## Quick start

Requires `g++` with C++14 and `make`. No third-party dependencies.

```sh
make            # builds ./compress, ./decompress, ./bench
```

Create a small headerless CSV to play with:

```sh
python3 -c "
import random; random.seed(7)
cities = ['Athens','Berlin','Cairo','Delhi','Athens','Berlin']
with open('sample.csv','w') as f:
    for i in range(200000):
        f.write('%d,%s,user_%d,%d\n' % (i, random.choice(cities), random.randrange(50000), random.randrange(1000)))
"
```

Compress columns 2 and 3 (indices are **1-based**), then decompress them back:

```sh
./compress sample.csv 2 3 > sample.comp
./decompress sample.comp > out.csv

# verify the round-trip
cut -d, -f2,3 sample.csv > expected.csv
cmp expected.csv out.csv && echo "ROUND-TRIP OK"
```

Measure it:

```sh
./bench sample.csv --runs 3 2 3
```

For each column this prints the compression ratio and page-type mix, full-scan
throughput, filtered-scan timings with the page-skip breakdown, and random-access
latency:

```
--- Column 2 | 200000 rows | raw: 1.1 MB ---
  COMPRESSION  (3 runs, warm cache)
    Time(ms)         Rows/s      MB/s     Ratio   Local%   Diff%   Raw%   AvgDict
        12.7       15761437      85.2     5.66x    25.0%   75.0%   0.0%         1

  DECOMPRESSION / FULL SCAN  (3 runs, warm cache)
    Time(ms)         Rows/s      MB/s
         2.5       79160644     427.8

  FILTERED SCAN  (3 runs, warm cache)
  Target (truncated)               Time(ms)    Matched    Sel%%   ZMSkip  DictSkip  Scanned
  Athens                               0.71      66928  33.464%        0         0        4
  Delhi                                0.37      33468  16.734%        0         0        4

  RANDOM ACCESS  (10 queries x 3 runs)
       Avg(ms)      Min(ms)      Max(ms)
        0.0000       0.0000       0.0001

  Compressed: 0.19 MB  |  Ratio: 5.66x  |  Pages: 1 local, 3 diff, 0 raw
```

---

## How the format works

### Pages and differential sequences

A column is cut into fixed-size **pages** of `kDefaultPageSize = 65536` rows
(`ac_types.h`). The page size is kept small on purpose: it holds down the memory
footprint and cache pressure, and it bounds every page's offset index to at most 2
bytes in the common case (2^16 values). Pages are grouped into **differential
sequences**:

```
 page:   0        1        2        3        4        5        6        7
       ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
       │local │ │ diff │ │ diff │ │ diff │ │local │ │ diff │ │ raw  │ │local │
       └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘
       └─────── sequence 1 ──────────────┘ └─ sequence 2 ──┘  break   └─ seq 3
```

A sequence starts with a self-contained page and continues with pages that store
only new values. Sequences are a natural split point for parallelism, and the file
can be cut there, much like a Parquet row group.

### The three page types

| | `isLocal` | `isRaw` | Dictionary holds | Offsets index into |
|---|---|---|---|---|
| **Local** | `true` | `false` | all distinct values in the page, sorted | its own dictionary |
| **Differential** | `false` | `false` | only values new to the sequence, sorted | the **cumulative** dictionary (local page's dict plus every diff dict so far) |
| **Raw** | `true` | `true` | nothing | nothing (values stored verbatim in `rawValues`) |

**Raw pages** implement the paper's 80% distinct-value threshold (§3). If more than
`kDistinctRatioDisable = 0.80` of a page's values are distinct, dictionary encoding
cannot pay for itself, so the page stores its values as they are, with no dictionary
and no offsets. A raw page also breaks the current sequence, so the next page starts
a new one. It carries `isLocal = true` because every consumer needs to treat it as a
sequence boundary.

Offsets are stored at variable width (1, 2 or 4 bytes), chosen per page from the
cardinality it has to address (`width_for_cardinality` in `ac_utils.cpp`). This is
the quantity the cost function is fighting over. A differential dictionary keeps
growing, and once it crosses 256 or 65,536 entries, every row in every later page of
that sequence pays an extra byte.

Each page also carries two pairs of min/max statistics:

- `pageMin` / `pageMax` cover all rows of the page. This is a classic zone map.
- `diffMin` / `diffMax` cover only the newly added values on a differential page.
  This is the paper's *page omittance* refinement (§3.2.4). It is a much tighter
  filter than the zone map when the question is only "was this value introduced
  here?", so it eliminates far more dictionary searches.

### Choosing local vs. differential

`AdaptiveDictionaryEncoder::encode` (`ac_encoder.cpp`) applies four rules per page,
in order:

1. **First page of a sequence** goes local. There is no prior context to be
   differential against.
2. **Low cross-page repetition** goes local. If fewer than
   `kMinCrossPageRepeatRatio = 10%` of the page's rows repeat a value already in the
   cumulative dictionary, differential encoding buys almost nothing. Before
   committing to the reset, the encoder consults a **Bloom filter** built over the
   dictionary that started the sequence (`ac_bloom.cpp`, 10% false-positive rate,
   double-hashed FNV-1a). Because the filter over-estimates membership, this check
   errs on the conservative side and avoids tearing down a healthy sequence just
   because one page dipped below the threshold. This follows the paper's §3.1 remark
   about using a Bloom filter to cheaply re-check the rate of new values.
3. **Offset width would not grow**, so use differential unconditionally. If nothing
   gets wider, differential is free savings and the cost function is not needed.
4. **Offset width would grow**, so evaluate the cost function, Equation 1 of the
   paper:

   ```
   diffcount * (difOffsetSize - diffavg) - locOffsetSize - (locPageSize - difPageSize) > 0
   ```

   Here `diffcount` is the number of differential pages in the current sequence,
   `difOffsetSize` the total offset bytes if this page is differential, `diffavg` the
   average differential-dictionary size so far, and `loc*` / `dif*` the projected page
   sizes either way. If the expression is positive, the predicted future cost of the
   wider offsets across the rest of the sequence outweighs this page's savings, so
   the encoder starts a fresh local page.

Rule 4 matters because the decision is not local to the current page. Choosing
differential now makes every later page in the sequence pay for a bigger dictionary,
so the encoder uses the sequence's own history to predict what the next pages will
look like.

### A worked example

Two pages of a `city` column, with the dictionary state after each:

```
page 0 (local)                        page 1 (differential)
  rows:  Athens Berlin Athens Cairo     rows:  Berlin Delhi Athens Delhi
  dict:  [Athens, Berlin, Cairo]        dict:  [Delhi]              <- new values only
  offs:  [0, 1, 0, 2]                   offs:  [1, 3, 0, 3]         <- into cumulative dict
  pageMin/Max: Athens / Cairo           cumulative: [Athens, Berlin, Cairo, Delhi]
  diffMin/Max: Athens / Cairo           pageMin/Max: Athens / Delhi
  dictSizes:   []                       diffMin/Max: Delhi / Delhi  <- tight: only "Delhi" is new
  diffDepth:   0                        dictSizes:   [3, 1]
                                        diffDepth:   1
```

`Athens` and `Berlin` are stored once for both pages, yet page 1's offsets are still
only one byte wide. A filtered scan for `Cairo` on page 1 can skip the dictionary
search outright, since `Cairo` sorts below `diffMin`, and having failed to find it in
the cumulative dictionary it skips the offset scan too.

---

## The operators

All four work directly on the compressed representation. None of them materialises
the column unless asked to.

### Full scan / decompression, `ac_decompressor.cpp` (§3.2.2)

Walks pages in order while maintaining the cumulative dictionary. A local page resets
it, a differential page appends to it, and a raw page copies values straight out.
Every offset then indexes the cumulative dictionary directly. The cumulative
dictionary is a vector of `const std::string*` pointing into the pages themselves.
The `EncodedColumn` outlives the call, so no intermediate string is ever copied and
the only allocations are the output values.

### Filtered scan, `ac_scanner.cpp` (§3.2.3, Algorithm 2)

Equality predicate, returns matching row indices. Three layers of skipping run before
any offset is touched:

1. **Zone map.** If the target falls outside `[pageMin, pageMax]`, no row in this page
   can match, so skip it. Counted as `ZMSkip`.
2. **Differential min/max.** On a diff page, if the target falls outside
   `[diffMin, diffMax]` it was not introduced here, so this page's dictionary is not
   even searched.
3. **Dictionary miss.** If the target still is not in the cumulative dictionary, the
   page cannot contain it, so skip it. Counted as `DictSkip`.

Only surviving pages get an offset scan, comparing each offset against the single
integer the target resolved to. The scanner is the one consumer that never rebuilds
the cumulative dictionary. It only tracks whether the target is in it and at which
index, which is exactly the paper's observation that a filtered scan needs one lookup
per sequence rather than one per page. Once the target's offset is known it stays
valid for the rest of the sequence, so later diff dictionaries are skipped outright.

### Random access, `ac_random_access.cpp` (§3.2.5)

`random_access(col, rowIndex)` returns one value in a fixed number of page reads, no
matter how long the sequence is. The mechanism is the `dictSizes` list in each
differential page header, which records the size of every dictionary in the sequence
up to and including that page:

1. Walk page headers to find the page holding `rowIndex` (headers only, so no
   dictionaries or offsets are read) and read the offset.
2. Prefix-sum `dictSizes` to find which page in the sequence owns that offset, and
   read only that page's dictionary.

That is two page reads. Local and raw pages resolve in one. Without `dictSizes` this
would require rebuilding the cumulative dictionary from the sequence start.

`batch_random_access(col, sortedRowIndices)` (§3.2.4) is the bulk form used after a
filtered scan. It walks pages once, maintaining the cumulative dictionary
incrementally, and resolves every requested index that lands in the current page.
Cost is O(pages + indices) rather than O(indices x pages).

`filtered_scan_random_access(predicateCol, target, payloadCol)` composes the two into
the paper's "filter one column, fetch another" pattern.

---

## Binary file format

Written by `ac_serial.cpp`. All integers are little-endian, and strings are a `u32`
byte length followed by raw bytes.

```
FILE
  magic            "ACMP1"          5 bytes
  rowCount         u32
  originalColCount u32              columns in the source CSV (informational)
  encodedColCount  u32
  column[0..encodedColCount)

COLUMN
  columnIndex1Based u32
  isString          u8
  dictionaryEnabled u8
  if !dictionaryEnabled:            numeric column, no pages at all
    count           u32
    values          string * count
  else:
    pageSize        u32             informational; decoding does not need it
    pageCount       u32
    page[0..pageCount)

PAGE
  pageType          u8              0 = local, 1 = differential, 2 = raw
  if pageType == 2 (raw):
    rowCount        u32
    pageMin, pageMax                string, string
    count           u32
    rawValues       string * count
  else:
    offsetWidth     u8              1, 2 or 4
    rowCount        u32
    diffDepth       u32             pages back to the sequence-start local page
    dictSizesCount  u32             empty on local pages
    dictSizes       u32 * dictSizesCount
    pageMin, pageMax, diffMin, diffMax    string * 4
    dictSize        u32
    dictionary      string * dictSize     sorted
    offsetCount     u32
    offsets         offsetWidth bytes * offsetCount
```

Two notes for anyone editing this. Fields are positional with no per-page framing, so
`write_column` and `read_column` have to change together and in the same order. And
`dictSizes` / `diffDepth` are pure redundancy that exists only to make random access a
two-page read, so the encoder is responsible for keeping them consistent.

**Numeric columns bypass the scheme entirely.** `is_string_column()` (`ac_utils.cpp`)
returns false when every value parses as a number. Such a column is stored as raw
values with `dictionaryEnabled = false` and no pages at all. Every operator has a
branch for this, and `bench` reports the column as skipped.

---

## Tools and usage

### `compress`

```
./compress <csv-file> <col1> [col2 ...] > out.comp
```

Column indices are 1-based, deduplicated and sorted. The container is written to
stdout. Only the selected columns are encoded.

### `decompress`

```
./decompress <file.comp>
```

Verifies the `ACMP1` magic, decodes every stored column, checks each has exactly
`rowCount` values, and writes them to stdout as CSV in the order they were stored.
It emits only the columns that were compressed, so a round-trip check has to compare
against those source columns (for example with `cut -d, -f2,3`).

### `bench`

```
./bench <csv_file> [--max-rows N] [--runs N] <col1> [col2 ...]
```

- `--max-rows N` stops reading after N rows. This is needed for the very large files;
  `run_bench.sh` caps `views_stats.csv` at 5M rows for memory.
- `--runs N` sets the timed runs per measurement (default 5, plus 1 warmup). All
  reported timings are medians, measured in memory with warm caches.

Filtered-scan targets are picked automatically from the data at the 1/4, 1/2 and 3/4
positions of the column, so they are guaranteed to exist and to span a range of
selectivities. Random access samples 10 row indices spread evenly across the column.

There is no unit-test framework in this repo. `bench` plus a `compress | decompress`
round-trip is how changes get validated.

### `run_bench.sh`

```
./run_bench.sh [--runs N] | tee test_results.txt
```

Runs the full suite over three tables of the OpenAIRE/UDFBench dataset, chosen to
span the cardinality spectrum: `views_stats.csv` (5M rows, extreme repetition),
`artifact_authors.csv` (9.9M rows, high-cardinality long strings, and the closest to
the paper's own workload), and `artifacts.csv` (3.8M rows, mixed: unique IDs,
free-text titles, categoricals, booleans). The script documents each column's
cardinality character inline.

> **The dataset is not in this repository.** `benchmark/` is gitignored. Both
> `run_bench.sh` and `duckdb_bench.py` expect the CSVs at
> `benchmark/udfbench-dataset/dataset/csvs/large/`.

### `duckdb_bench.py`

```
python3 duckdb_bench.py        # requires: pip install duckdb
```

The baseline. Runs DuckDB 1.5.2 in memory over the same datasets and the same filter
targets, measuring CSV full scan, CSV to Parquet conversion (Snappy and Zstd),
Parquet full scan, and filtered scan on both. All columns are ingested as `VARCHAR`
(`all_varchar=true`) so the comparison against a string-only implementation is fair.
Output goes to `duckdb_results.txt`.

---

## Benchmark results

Numbers below come from `report.tex`, `test_results.txt` and `duckdb_results.txt`.
One caveat carried over from the report's methodology: our encoder measures only the
tested string columns, while DuckDB compresses the whole file including numeric ones,
so the raw-size baselines differ.

**Compression.** Adaptive encoding wins decisively on repetitive columns and loses on
free text:

| Column | Raw MB | Ours | DuckDB PQ/Zstd (whole file) |
|---|---|---|---|
| `views_stats` col 4, project ID | 219.3 | **45.79x** | 24.49x |
| `views_stats` col 3, data source | 37.5 | 7.84x | 24.49x |
| `artifacts` col 7, access type | 40.6 | 11.26x | 3.71x |
| `artifacts` col 3, publisher | 46.6 | 5.39x | 3.71x |
| `artifact_authors` col 1, artifact ID | 435.7 | 5.47x | 14.13x |
| `artifacts` col 2, title (unique free text) | 313.5 | 0.96x | 3.71x |

The page mixes explain the extremes. The project-ID column comes out as 2 local and
75 diff pages, while the unique title column is 58 raw pages out of 58: every page
trips the 80% distinct threshold, so the "compressed" output is the raw strings plus
a 4-byte length prefix each, which is slight expansion.

**Filtered scan.** Competitive with DuckDB over Parquet, and faster on low-cardinality
columns:

| Filter | Matched | Ours (ms) | PQ/Snappy (ms) | PQ/Zstd (ms) |
|---|---|---|---|---|
| `views_stats` month = `2019/05` | 24,400 | **2.11** | 3.7 | 4.1 |
| `views_stats` month = `2023/01` | 116,640 | **3.04** | 3.9 | 4.7 |
| `views_stats` source = `IRUS-UK` | 1,141,124 | 3.49 | 1.6 | 1.5 |
| `artifact_authors` name = `Y. G. Xie` | 50 | **9.32** | 106.1 | 107.6 |
| `artifact_authors` last = `Shin` | 963 | **5.36** | 59.0 | 57.7 |
| `artifacts` type = `publication` | 3,013,657 | 8.87 | 4.1 | 3.9 |

**Full scan.** DuckDB's vectorised Parquet reader is faster at materialising whole
columns; our decompressor peaks around 1.06 GB/s per column. The compressed format is
built for skipping rather than for bulk materialisation.

**Random access.** Sub-microsecond point queries. `bench` reports the average at its
0.0001 ms print resolution, while `report.tex`'s table records 1.1 to 1.2 µs from an
earlier build. The cost stays flat across every column type and sequence length,
which is what the `dictSizes` two-page read is for. DuckDB has no row-level
point-query primitive over CSV or Parquet without a full scan or a prebuilt index, so
it is not compared here.

**The honest trade-off**, spelled out in the report's discussion: raw pages fix the
expansion problem on unique columns, but they are opaque to the scanner. They carry
no dictionary, so they cannot be dict-skipped. `artifacts` col 1 went from 57 of 58
pages skipped to scanning all 58 (about 17.6 ms), and `artifact_authors` col 3
dropped from 27-106 to 6-62 pages skipped out of 152. That is still far ahead of
Parquet's 104-109 ms on that column, but it is a real regression against the pre-raw-page
encoding.

---

## Repository layout

| File | Role |
|---|---|
| `ac_types.h` | Shared vocabulary: `EncodedPage`, `EncodedColumn`, `DecodedColumn`, tuning constants. Included first by everything. |
| `ac_utils.{h,cpp}` | `width_for_cardinality`, `is_numeric` / `is_string_column`, dictionary size estimation, `min_max`. |
| `ac_csv.{h,cpp}` | CSV reading. Handles quoted fields and `""` escapes; rows are normalised to the first row's width. |
| `ac_bloom.{h,cpp}` | Bloom filter (double-hashed FNV-1a) used by encoder rule 2. |
| `ac_encoder.{h,cpp}` | **The core algorithm.** Page splitting, local/diff/raw selection, cost function, sequence bookkeeping. |
| `ac_serial.{h,cpp}` | `EncodedColumn` to and from `ACMP1` bytes. |
| `ac_decompressor.{h,cpp}` | Full scan / decode. |
| `ac_scanner.{h,cpp}` | Filtered scan, skip statistics, filtered-scan-random-access. |
| `ac_random_access.{h,cpp}` | Point query (two-page read) and batch point query. |
| `main.cpp` / `main_decompress.cpp` | CLIs for `compress` and `decompress`. |
| `bench.cpp` | Benchmark harness. |
| `run_bench.sh` | Full suite over the three UDFBench tables. |
| `duckdb_bench.py` | DuckDB/Parquet baseline. |
| `report.tex` | Write-up of the evaluation. |
| `test_results.txt`, `duckdb_results.txt` | Raw benchmark output backing the report. |
| `adaptive_compression.pdf` | The paper. |
| `CLAUDE.md` | Orientation notes for Claude Code, including cross-file invariants. |

The `Makefile` lists header dependencies explicitly rather than generating them, so a
new `.cpp` needs three edits: the object list, the `clean` rule, and a dependency
line.

---

## Paper-to-code map

| Paper section | Implemented in |
|---|---|
| §3.1, selection between differential and local; Equation 1; 10% repeat guard; 80% distinct guard; Bloom filter | `ac_encoder.cpp` (rules 1-4), `ac_bloom.cpp` |
| §3.2.1, compression, Algorithm 1 | `AdaptiveDictionaryEncoder::encode` |
| §3.2.2, full scan / decompression | `ac_decompressor.cpp` |
| §3.2.3, filtered scan, Algorithm 2 | `ac_scanner.cpp` |
| §3.2.4, page omittance (`diffMin`/`diffMax`), batch retrieval | `ac_scanner.cpp`, `batch_random_access` in `ac_random_access.cpp` |
| §3.2.5, random access in a fixed number of page reads | `random_access` via `dictSizes` and `diffDepth` |
| Figure 2, structure of an adaptively encoded attribute | `ac_serial.cpp` layout |

Not implemented: the paper's Python/Dask integration, per-page secondary codecs (the
format allows compressing the sorted dictionaries or offsets with ZLIB, RLE or
bit-packing on top, but this implementation stores them plainly), and partition-level
parallelism across sequences.

---

## Limitations and design notes

- **In-memory only.** The whole column is read, encoded and queried in RAM. There is
  no buffer pool, mmap or I/O layer, so page-skip counts are a proxy for the I/O the
  format would save rather than measured I/O.
- **Equality predicates only.** `filtered_scan` tests `value == target`. Range
  predicates would fit naturally with the sorted dictionaries and zone maps but are
  not implemented.
- **CSV input is headerless and line-based.** The first line is data, not a header,
  and quoted fields containing embedded newlines are not supported, since `read_csv`
  splits on `\n` before parsing quotes.
- **4-byte length prefix per string** in the serialised format. On unique columns
  stored as raw pages, this prefix is the residual 0.92-0.96x expansion.
- **Raw pages cannot be dict-skipped.** See the trade-off note in the results.
- **Row counts are `uint32_t`** throughout, capping a column at roughly 4.29B rows.
- `bench` encodes each column once as a probe to detect numeric columns and again for
  timing, so its wall-clock is roughly double the reported compression time.

---

## Tuning

The knobs all live in `ac_types.h`:

| Constant | Default | Effect |
|---|---|---|
| `kDefaultPageSize` | 65536 | Rows per page. Larger pages compress better but skip worse and cost more per random access. Keeping it at 2^16 bounds most offsets to 2 bytes. |
| `kDistinctRatioDisable` | 0.80 | Distinct-value ratio above which a page is stored raw. Lowering it makes more pages raw, which means less expansion on unique data but less skipping. Raising it does the reverse. |
| `kMinCrossPageRepeatRatio` | 0.10 | Cross-page repetition below which the encoder prefers a fresh local page. |

The Bloom filter's false-positive rate (10%, in `ac_bloom.h`) is a fourth knob. It
biases rule 2 toward keeping sequences alive.

---

## References

1. Y. Foufoulas, L. Sidirourgos, E. Stamatogiannakis, Y. Ioannidis. *Adaptive
   Compression for Fast Scans on String Columns.* SIGMOD '21.
   <https://doi.org/10.1145/3448016.3452798>
2. UDFBench / OpenAIRE dataset, the benchmark corpus used here.
