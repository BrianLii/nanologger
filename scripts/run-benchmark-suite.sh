#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${NANOLOGGER_BUILD_DIR:-$project_root/build-benchmark}
samples=${NANOLOGGER_BENCH_SAMPLES:-100000}
rate=${NANOLOGGER_BENCH_RATE:-100000}

cpu_pair=$(
  lscpu -p=CPU,CORE \
    | awk -F, '
        /^#/ { next }
        first_cpu == "" { first_cpu=$1; first_core=$2; next }
        $2 != first_core { print first_cpu, $1; exit }
      '
)
if [[ -z "$cpu_pair" ]]; then
  echo "Could not find two distinct physical CPU cores" >&2
  exit 1
fi
read -r producer_cpu consumer_cpu <<< "$cpu_pair"

cmake -S "$project_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel "$(nproc)"
ctest --test-dir "$build_dir" --output-on-failure

echo "=== NanoLogger binary: steady ==="
"$build_dir/nanologger_bench" \
  --mode steady \
  --samples "$samples" \
  --rate "$rate" \
  --producer-cpu "$producer_cpu" \
  --consumer-cpu "$consumer_cpu"
