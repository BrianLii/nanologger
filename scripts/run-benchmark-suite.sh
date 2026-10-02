#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${NANOLOGGER_BUILD_DIR:-$project_root/build-benchmark}
suite=${1:-steady}

usage() {
  echo "Usage: $0 [steady|pacing|ring-capacity]" >&2
}

find_cpu_pair() {
  local cpu_pair
  cpu_pair=$(
    lscpu -p=CPU,CORE | awk -F, '
      /^#/ { next }
      first_cpu == "" { first_cpu=$1; first_core=$2; next }
      $2 != first_core { print first_cpu, $1; exit }
    '
  )
  [[ -n "$cpu_pair" ]] || {
    echo "Could not find two distinct physical CPU cores" >&2
    exit 1
  }
  read -r producer_cpu consumer_cpu <<< "$cpu_pair"
  printf '%s %s\n' "$producer_cpu" "$consumer_cpu"
}

build_and_test() {
  local build_dir=$1
  cmake -S "$project_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" --parallel "$(nproc)"
  ctest --test-dir "$build_dir" --output-on-failure
}

case "$suite" in
  steady)
    build_dir=$build_root
    samples=${NANOLOGGER_BENCH_SAMPLES:-100000}
    rate=${NANOLOGGER_BENCH_RATE:-100000}
    read -r producer_cpu consumer_cpu < <(find_cpu_pair)
    build_and_test "$build_dir"
    echo "=== NanoLogger binary: steady ==="
    "$build_dir/nanologger_bench" --mode steady --samples "$samples" \
      --rate "$rate" --producer-cpu "$producer_cpu" \
      --consumer-cpu "$consumer_cpu"
    ;;
  pacing)
    build_dir=$build_root/pacing
    samples=${NANOLOGGER_BENCH_SAMPLES:-1000000}
    rate=${NANOLOGGER_BENCH_RATE:-100000}
    rounds=${NANOLOGGER_AB_ROUNDS:-5}
    read -r producer_cpu consumer_cpu < <(find_cpu_pair)
    build_and_test "$build_dir"
    echo "Pacing comparison: producer CPU $producer_cpu, consumer CPU $consumer_cpu"
    for ((round = 1; round <= rounds; ++round)); do
      variants=(sleep spin)
      if ((round % 2 == 0)); then variants=(spin sleep); fi
      for pacing in "${variants[@]}"; do
        echo "=== burst round $round pacing $pacing ==="
        "$build_dir/nanologger_bench" --mode burst --pacing "$pacing" \
          --samples "$samples" --rate "$rate" --burst-size 100 \
          --diagnostics 1 --producer-cpu "$producer_cpu" \
          --consumer-cpu "$consumer_cpu"
      done
    done
    ;;
  ring-capacity)
    build_dir=$build_root/ring-capacity
    read -r producer_cpu consumer_cpu < <(find_cpu_pair)
    cmake -S "$project_root" -B "$build_dir/default" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$build_dir/default" --parallel "$(nproc)" >/dev/null
    ctest --test-dir "$build_dir/default" --output-on-failure
    for capacity in 4096 32768 65536; do
      c++ -std=c++20 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic \
        -DNANOLOGGER_BENCH_QUEUE_SIZE="$capacity" -I"$project_root/include" \
        "$project_root/bench/benchmark.cpp" -o "$build_dir/bench-$capacity"
    done

    echo "Ring capacity sweep: producer CPU $producer_cpu, consumer CPU $consumer_cpu"
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
        "$build_dir/bench-$capacity" --mode burst --pacing spin \
          --samples 1000000 --rate 100000 --burst-size 100 --diagnostics 1 \
          --producer-cpu "$producer_cpu" --consumer-cpu "$consumer_cpu"
      done
    done
    ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    usage
    exit 2
    ;;
esac
