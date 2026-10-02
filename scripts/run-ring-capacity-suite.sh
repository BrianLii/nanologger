#!/usr/bin/env bash
# Compare ring capacities on the same AWS host under spin-paced burst load.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${NANOLOGGER_BUILD_DIR:-$project_root/build-ring-capacity}

cpu_pair=$(
  lscpu -p=CPU,CORE | awk -F, '
    /^#/ { next }
    first_cpu == "" { first_cpu=$1; first_core=$2; next }
    $2 != first_core { print first_cpu, $1; exit }'
)
[[ -n "$cpu_pair" ]] || { echo "Could not find two physical cores" >&2; exit 1; }
read -r producer_cpu consumer_cpu <<< "$cpu_pair"

cmake -S "$project_root" -B "$build_root/default" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build_root/default" --parallel "$(nproc)" >/dev/null
ctest --test-dir "$build_root/default" --output-on-failure

for capacity in 4096 32768 65536; do
  c++ -std=c++20 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic \
    -DNANOLOGGER_BENCH_QUEUE_SIZE="$capacity" -I"$project_root/include" \
    "$project_root/bench/benchmark.cpp" -o "$build_root/bench-$capacity"
done

echo "AWS ring capacity sweep: producer CPU $producer_cpu, consumer CPU $consumer_cpu"
orders=(
  "4096 32768 65536"
  "4096 65536 32768"
  "32768 4096 65536"
  "32768 65536 4096"
  "65536 4096 32768"
  "65536 32768 4096"
)
for ((round = 0; round < ${#orders[@]}; ++round)); do
  for capacity in ${orders[round]}; do
    echo "=== burst round $((round + 1)) capacity $capacity ==="
    "$build_root/bench-$capacity" --mode burst --pacing spin \
      --samples 1000000 --rate 100000 --burst-size 100 --diagnostics 1 \
      --producer-cpu "$producer_cpu" --consumer-cpu "$consumer_cpu"
  done
done
