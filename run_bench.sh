#!/bin/bash
# Runs the full benchmark suite across all three test files.
#
# Usage:
#   ./run_bench.sh              -- runs all three files
#   ./run_bench.sh --runs 3     -- override number of timed runs (default 5)
#
# Output is written to stdout. Redirect to save:
#   ./run_bench.sh | tee results.txt

set -e

BENCH=./bench
LARGE=benchmark/udfbench-dataset/dataset/csvs/large
EXTRA_ARGS=""

# Pass --runs N through if provided.
if [ "$1" = "--runs" ] && [ -n "$2" ]; then
    EXTRA_ARGS="--runs $2"
fi

# --------------------------------------------------------------------------
# views_stats.csv
# Goal 1: Biggest absolute test (90M rows, limited to 5M for memory)
# Goal 2: Extreme repetition -- col 1 (month strings), col 3 (source name)
#
# Columns:
#   1 = month (e.g. "2018/04")             -- very low cardinality
#   2 = artifact ID (hash)                 -- high cardinality
#   3 = data source (e.g. "OpenAIRE")      -- extremely low cardinality
#   4 = project ID (hash)                  -- high cardinality
#   5 = count (numeric, will be skipped)
# --------------------------------------------------------------------------
echo ""
echo "######################################################################"
echo "# views_stats.csv  [--max-rows 5000000]"
echo "######################################################################"
$BENCH "$LARGE/views_stats.csv" --max-rows 5000000 $EXTRA_ARGS 1 2 3 4

# --------------------------------------------------------------------------
# artifact_authors.csv
# Goal: High cardinality long strings; closest to the paper's own benchmark.
#
# Columns:
#   1 = artifact ID (hash string)          -- high cardinality
#   3 = full display name                  -- high cardinality, long strings
#   5 = last name only                     -- moderate cardinality
#   6 = author position (numeric, skipped)
# --------------------------------------------------------------------------
echo ""
echo "######################################################################"
echo "# artifact_authors.csv  [all rows]"
echo "######################################################################"
$BENCH "$LARGE/artifact_authors.csv" $EXTRA_ARGS 1 3 5

# --------------------------------------------------------------------------
# artifacts.csv
# Goal: Mixed workload -- 16 columns spanning IDs, titles, categoricals, booleans.
#
# Columns:
#   1  = artifact ID (hash)                -- high cardinality
#   2  = title (free text)                 -- very high cardinality, long
#   3  = publisher name                    -- moderate cardinality
#   4  = journal name                      -- moderate cardinality
#   6  = year (numeric, skipped)
#   7  = access type (e.g. "Open Access")  -- very low cardinality
#   9  = boolean (true/false)              -- cardinality 2
#   13 = artifact type (e.g. "publication")-- very low cardinality
# --------------------------------------------------------------------------
echo ""
echo "######################################################################"
echo "# artifacts.csv  [all rows]"
echo "######################################################################"
$BENCH "$LARGE/artifacts.csv" $EXTRA_ARGS 1 2 3 4 7 9 13
