#!/usr/bin/env bash
# Compare sleep and spin pacing using the same binary on two physical cores.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${NANOLOGGER_BUILD_DIR:-$project_root/build-benchmark}
samples=${NANOLOGGER_BENCH_SAMPLES:-1000000}
rate=${NANOLOGGER_BENCH_RATE:-100000}
rounds=${NANOLOGGER_AB_ROUNDS:-5}

cpu_pair=$(
  lscpu -p=CPU,CORE | awk -F, '
    /^#/ { next }
    first_cpu == "" { first_cpu=$1; first_core=$2; next }
    $2 != first_core { print first_cpu, $1; exit }
  '
)
[[ -n "$cpu_pair" ]] || { echo "Could not find two physical cores" >&2; exit 1; }
read -r producer_cpu consumer_cpu <<< "$cpu_pair"

build_dir=$build_root/pacing
cmake -S "$project_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel "$(nproc)"
ctest --test-dir "$build_dir" --output-on-failure

echo "Pacing comparison: producer CPU $producer_cpu, consumer CPU $consumer_cpu"
for ((r = 1; r <= rounds; ++r)); do
  variants=(sleep spin)
  if ((r % 2 == 0)); then variants=(spin sleep); fi
  for pacing in "${variants[@]}"; do
    echo "=== burst round $r pacing $pacing ==="
    "$build_dir/nanologger_bench" --mode burst --pacing "$pacing" \
      --samples "$samples" --rate "$rate" --burst-size 100 \
      --diagnostics 1 --producer-cpu "$producer_cpu" \
      --consumer-cpu "$consumer_cpu"
  done
done
