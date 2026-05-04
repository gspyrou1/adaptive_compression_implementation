#include "ac_csv.h"
#include "ac_decompressor.h"
#include "ac_encoder.h"
#include "ac_random_access.h"
#include "ac_scanner.h"
#include "ac_serial.h"
#include "ac_types.h"
#include "ac_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static int N_RUNS   = 5;  // timed runs per measurement; overridden by --runs
static int N_WARMUP = 1;  // warm-up runs before timing (not counted)

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------

using Clock = std::chrono::high_resolution_clock;

static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

static double compute_median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---------------------------------------------------------------------------
// Metrics helpers
// ---------------------------------------------------------------------------

static uint64_t compute_raw_bytes(const std::vector<std::string>& values) {
    uint64_t total = 0;
    for (const std::string& s : values) total += s.size();
    return total;
}

static uint64_t compute_compressed_bytes(const ac::EncodedColumn& col) {
    std::ostringstream oss;
    ac::write_column(oss, col);
    return static_cast<uint64_t>(oss.str().size());
}

struct PageStats {
    uint32_t totalPages  = 0;
    uint32_t localPages  = 0;
    uint32_t diffPages   = 0;
    double   avgDictSize = 0.0;
    uint64_t compBytes   = 0;
    double   compRatio   = 0.0;
};

static PageStats compute_page_stats(const ac::EncodedColumn& col,
                                    uint64_t rawBytes) {
    PageStats s;

    if (!col.dictionaryEnabled) {
        s.compBytes = rawBytes;
        s.compRatio = 1.0;
        return s;
    }

    uint64_t dictSizeSum = 0;
    for (const ac::EncodedPage& p : col.pages) {
        ++s.totalPages;
        if (p.isLocal) ++s.localPages;
        else           ++s.diffPages;
        dictSizeSum += p.dictionary.size();
    }

    if (s.totalPages > 0) {
        s.avgDictSize = static_cast<double>(dictSizeSum)
                        / static_cast<double>(s.totalPages);
    }

    s.compBytes = compute_compressed_bytes(col);
    s.compRatio = (s.compBytes == 0)
        ? 1.0
        : static_cast<double>(rawBytes) / static_cast<double>(s.compBytes);

    return s;
}

// ---------------------------------------------------------------------------
// Timed encode / decode / scan
// ---------------------------------------------------------------------------

static std::pair<double, ac::EncodedColumn> time_encode(
        uint32_t colIdx,
        const std::vector<std::string>& values,
        std::function<ac::EncodedColumn(uint32_t,
                                        const std::vector<std::string>&)> fn) {
    ac::EncodedColumn result;
    for (int i = 0; i < N_WARMUP; ++i) result = fn(colIdx, values);

    std::vector<double> times;
    times.reserve(static_cast<size_t>(N_RUNS));
    for (int i = 0; i < N_RUNS; ++i) {
        auto t0 = Clock::now();
        result  = fn(colIdx, values);
        auto t1 = Clock::now();
        times.push_back(elapsed_ms(t0, t1));
    }
    return std::make_pair(compute_median(times), result);
}

static double time_decode(
        const ac::EncodedColumn& col,
        std::function<ac::DecodedColumn(const ac::EncodedColumn&)> fn) {
    for (int i = 0; i < N_WARMUP; ++i) fn(col);

    std::vector<double> times;
    times.reserve(static_cast<size_t>(N_RUNS));
    for (int i = 0; i < N_RUNS; ++i) {
        auto t0 = Clock::now();
        fn(col);
        auto t1 = Clock::now();
        times.push_back(elapsed_ms(t0, t1));
    }
    return compute_median(times);
}

// Runs filtered_scan N_RUNS times and returns (median_ms, stats_from_last_run).
static std::pair<double, ac::ScanStats> time_scan(
        const ac::EncodedColumn& col,
        const std::string& target) {
    ac::ScanStats stats;
    for (int i = 0; i < N_WARMUP; ++i) ac::filtered_scan(col, target, stats);

    std::vector<double> times;
    times.reserve(static_cast<size_t>(N_RUNS));
    for (int i = 0; i < N_RUNS; ++i) {
        auto t0 = Clock::now();
        ac::filtered_scan(col, target, stats);
        auto t1 = Clock::now();
        times.push_back(elapsed_ms(t0, t1));
    }
    return std::make_pair(compute_median(times), stats);
}

// ---------------------------------------------------------------------------
// Output formatting
// ---------------------------------------------------------------------------

static void print_line(char c = '-', int width = 96) {
    for (int i = 0; i < width; ++i) std::putchar(c);
    std::putchar('\n');
}

// ---------------------------------------------------------------------------
// Pick sample scan targets
//
// We pick 3 values spread across the column: first-quarter, mid, last-quarter.
// These are guaranteed to exist and give a variety of selectivities.
// ---------------------------------------------------------------------------

static std::vector<std::string> pick_scan_targets(
        const std::vector<std::string>& values) {
    const size_t n = values.size();
    std::vector<std::string> targets;
    targets.push_back(values[n / 4]);
    targets.push_back(values[n / 2]);
    targets.push_back(values[n * 3 / 4]);

    // Deduplicate while preserving order.
    std::vector<std::string> unique;
    for (const std::string& t : targets) {
        bool found = false;
        for (const std::string& u : unique) { if (u == t) { found = true; break; } }
        if (!found) unique.push_back(t);
    }
    return unique;
}

// ---------------------------------------------------------------------------
// Per-column benchmark
// ---------------------------------------------------------------------------

static void bench_column(uint32_t colIdx,
                          const std::vector<std::string>& values) {
    const uint32_t totalRows = static_cast<uint32_t>(values.size());
    const uint64_t rawBytes  = compute_raw_bytes(values);

    printf("\n--- Column %u | %u rows | raw: %.1f MB ---\n",
           colIdx, totalRows,
           static_cast<double>(rawBytes) / (1024.0 * 1024.0));

    // Probe once to check whether dictionary encoding applies.
    {
        ac::AdaptiveDictionaryEncoder probe;
        ac::EncodedColumn probeCol = probe.encode(colIdx, values);
        if (!probeCol.dictionaryEnabled) {
            printf("  (numeric column -- dictionary encoding disabled, skipping)\n");
            return;
        }
    }

    // ------------------------------------------------------------------
    // Compression
    // ------------------------------------------------------------------
    ac::AdaptiveDictionaryEncoder enc;

    auto encRes = time_encode(colIdx, values,
        [&](uint32_t idx, const std::vector<std::string>& v) {
            return enc.encode(idx, v);
        });

    const double            encMs   = encRes.first;
    const ac::EncodedColumn encoded  = encRes.second;
    const PageStats         ps       = compute_page_stats(encoded, rawBytes);

    const double rowsPerSecEnc = static_cast<double>(totalRows) / (encMs / 1000.0);
    const double mbPerSecEnc   = static_cast<double>(rawBytes)  / (encMs / 1000.0)
                                 / (1024.0 * 1024.0);
    const double localPct = (ps.totalPages > 0)
        ? 100.0 * ps.localPages / ps.totalPages : 0.0;
    const double diffPct  = 100.0 - localPct;

    printf("  COMPRESSION  (%d runs, warm cache)\n", N_RUNS);
    print_line();
    printf("  %10s %14s %9s %9s %8s %7s %9s\n",
           "Time(ms)", "Rows/s", "MB/s", "Ratio", "Local%", "Diff%", "AvgDict");
    print_line();
    printf("  %10.1f %14.0f %9.1f %8.2fx %7.1f%% %6.1f%% %9.0f\n",
           encMs, rowsPerSecEnc, mbPerSecEnc,
           ps.compRatio, localPct, diffPct, ps.avgDictSize);

    // ------------------------------------------------------------------
    // Decompression / full scan (paper section 3.2.2)
    // ------------------------------------------------------------------
    const double decMs = time_decode(encoded,
        [](const ac::EncodedColumn& c) { return ac::decompress_column(c); });

    const double rowsPerSecDec = static_cast<double>(totalRows) / (decMs / 1000.0);
    const double mbPerSecDec   = static_cast<double>(rawBytes)  / (decMs / 1000.0)
                                 / (1024.0 * 1024.0);

    printf("\n  DECOMPRESSION / FULL SCAN  (%d runs, warm cache)\n", N_RUNS);
    print_line();
    printf("  %10s %14s %9s\n", "Time(ms)", "Rows/s", "MB/s");
    print_line();
    printf("  %10.1f %14.0f %9.1f\n", decMs, rowsPerSecDec, mbPerSecDec);

    // ------------------------------------------------------------------
    // Filtered scan (paper section 3.2.3)
    //
    // For each sample target we report:
    //   - scan time (ms)
    //   - rows matched and selectivity
    //   - pages skipped by zone map / by dict miss / actually scanned
    //
    // This corresponds to the breakdown in paper Tables 2 and 3.
    // ------------------------------------------------------------------
    std::vector<std::string> targets = pick_scan_targets(values);

    printf("\n  FILTERED SCAN  (%d runs, warm cache)\n", N_RUNS);
    print_line();
    printf("  %-30s %10s %10s %8s   %6s %9s %8s\n",
           "Target (truncated)", "Time(ms)", "Matched", "Sel%%",
           "ZMSkip", "DictSkip", "Scanned");
    print_line();

    for (const std::string& target : targets) {
        auto scanRes = time_scan(encoded, target);
        const double       scanMs = scanRes.first;
        const ac::ScanStats& st   = scanRes.second;

        const double sel = (totalRows > 0)
            ? 100.0 * st.rowsMatched / static_cast<double>(totalRows) : 0.0;

        // Truncate the target string for display.
        std::string display = target.size() > 28
            ? target.substr(0, 25) + "..."
            : target;

        printf("  %-30s %10.2f %10u %7.3f%%   %6u %9u %8u\n",
               display.c_str(), scanMs,
               st.rowsMatched, sel,
               st.pagesSkippedZoneMap,
               st.pagesSkippedDictMiss,
               st.pagesScanned);
    }

    // ------------------------------------------------------------------
    // Random access (paper section 3.2.5, Figure 6)
    //
    // Pick N_RANDOM_ROWS row indices spread across the column.  For each,
    // time a single random_access() call and report the average.  This
    // mirrors the paper's experiment of 10 random point queries.
    // ------------------------------------------------------------------
    static const int N_RANDOM_ROWS = 10;

    // Evenly spread row indices across the column.
    std::vector<uint32_t> rowIndices;
    for (int i = 0; i < N_RANDOM_ROWS; ++i) {
        rowIndices.push_back(static_cast<uint32_t>(
            static_cast<uint64_t>(i + 1) * totalRows / (N_RANDOM_ROWS + 1)));
    }

    // Warm up.
    for (uint32_t idx : rowIndices) ac::random_access(encoded, idx);

    // Timed runs: time each individual access, collect all samples.
    std::vector<double> accessTimes;
    accessTimes.reserve(static_cast<size_t>(N_RUNS) * rowIndices.size());
    for (int run = 0; run < N_RUNS; ++run) {
        for (uint32_t idx : rowIndices) {
            auto t0 = Clock::now();
            ac::random_access(encoded, idx);
            auto t1 = Clock::now();
            accessTimes.push_back(elapsed_ms(t0, t1));
        }
    }

    double sumMs = 0.0;
    double minMs = accessTimes[0];
    double maxMs = accessTimes[0];
    for (double t : accessTimes) {
        sumMs += t;
        if (t < minMs) minMs = t;
        if (t > maxMs) maxMs = t;
    }
    const double avgMs = sumMs / static_cast<double>(accessTimes.size());

    printf("\n  RANDOM ACCESS  (%d queries x %d runs)\n", N_RANDOM_ROWS, N_RUNS);
    print_line();
    printf("  %12s %12s %12s\n", "Avg(ms)", "Min(ms)", "Max(ms)");
    print_line();
    printf("  %12.4f %12.4f %12.4f\n", avgMs, minMs, maxMs);

    printf("\n  Compressed: %.2f MB  |  Ratio: %.2fx  |  Pages: %u local, %u diff\n\n",
           static_cast<double>(ps.compBytes) / (1024.0 * 1024.0),
           ps.compRatio, ps.localPages, ps.diffPages);
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Args {
    std::string           csvPath;
    size_t                maxRows = 0;
    std::vector<uint32_t> columns;
};

static Args parse_args(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <csv_file> [--max-rows N] [--runs N] <col1> [col2 ...]\n"
            "  col indices are 1-based\n"
            "  --max-rows N  stop reading after N rows (default: all)\n"
            "  --runs N      timed runs per measurement (default: 5)\n",
            argv[0]);
        std::exit(1);
    }

    Args args;
    args.csvPath = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--max-rows" && i + 1 < argc) {
            args.maxRows = static_cast<size_t>(std::atol(argv[++i]));
        } else if (a == "--runs" && i + 1 < argc) {
            N_RUNS = std::atoi(argv[++i]);
            if (N_RUNS < 1) N_RUNS = 1;
        } else {
            long idx = std::atol(argv[i]);
            if (idx <= 0) {
                fprintf(stderr, "Invalid column index: %s\n", argv[i]);
                std::exit(1);
            }
            args.columns.push_back(static_cast<uint32_t>(idx));
        }
    }

    if (args.columns.empty()) {
        fprintf(stderr, "Error: at least one column index is required.\n");
        std::exit(1);
    }

    return args;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        printf("================================================================================\n");
        printf("FILE:     %s\n", args.csvPath.c_str());
        printf("RUNS:     %d timed + %d warmup\n", N_RUNS, N_WARMUP);
        if (args.maxRows > 0)
            printf("MAX ROWS: %zu\n", args.maxRows);

        fprintf(stderr, "Reading CSV...\n");
        std::vector<std::vector<std::string>> rows =
            ac::read_csv(args.csvPath, args.maxRows);

        if (rows.empty()) throw std::runtime_error("CSV is empty");

        const size_t totalCols = rows.front().size();
        printf("ROWS:     %zu    COLUMNS IN FILE: %zu\n", rows.size(), totalCols);
        printf("================================================================================\n");

        for (uint32_t colIdx : args.columns) {
            if (colIdx > totalCols) {
                fprintf(stderr, "Warning: column %u out of range (%zu cols), skipping.\n",
                        colIdx, totalCols);
                continue;
            }

            std::vector<std::string> values;
            values.reserve(rows.size());
            const size_t col0 = static_cast<size_t>(colIdx - 1);
            for (const std::vector<std::string>& row : rows) {
                values.push_back(row[col0]);
            }

            bench_column(colIdx, values);
        }

        return 0;

    } catch (const std::exception& ex) {
        fprintf(stderr, "Error: %s\n", ex.what());
        return 2;
    }
}
