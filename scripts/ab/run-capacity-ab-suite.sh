#!/usr/bin/env bash
# A/B under the spin-paced burst workload at several ring capacities: the
# current tree (A) against the tree with scripts/ab/$NANOLOGGER_AB_PATCH
# applied (B). Rounds rotate the capacity order and alternate A/B order.
# Output is filtered to the result lines to stay under the SSM output limit.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${NANOLOGGER_BUILD_DIR:-$project_root/build-ab}
patch_name=${NANOLOGGER_AB_PATCH:-4k-pages.patch}
rounds=${NANOLOGGER_AB_ROUNDS:-6}
capacities=(4096 32768 65536)

read -r producer_cpu consumer_cpu < <(
  lscpu -p=CPU,CORE | awk -F, '
    /^#/ { next }
    first_cpu == "" { first_cpu=$1; first_core=$2; next }
    $2 != first_core { print first_cpu, $1; exit }'
)
[[ -n "${consumer_cpu:-}" ]] || { echo "Could not find two physical cores" >&2; exit 1; }

src_b="$build_root/src-b"
rm -rf "$src_b"
mkdir -p "$src_b"
tar -C "$project_root" --exclude './build*' -cf - . | tar -C "$src_b" -xf -
git -C "$src_b" apply "$project_root/scripts/ab/$patch_name"

for v in a b; do
  src=$project_root
  [[ $v == b ]] && src=$src_b
  cmake -S "$src" -B "$build_root/$v" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$build_root/$v" --parallel "$(nproc)" >/dev/null
  ctest --test-dir "$build_root/$v" --output-on-failure | tail -3
  for c in "${capacities[@]}"; do
    c++ -std=c++20 -O3 -DNDEBUG -pthread -DNANOLOGGER_BENCH_QUEUE_SIZE="$c" \
      -I"$src/include" "$src/bench/benchmark.cpp" -o "$build_root/$v-$c"
  done
done

cpus=(--producer-cpu "$producer_cpu" --consumer-cpu "$consumer_cpu")
bench=(--mode burst --pacing spin --rate 100000 --burst-size 100 --diagnostics 1 "${cpus[@]}")
results='^(timer|diagnostics|accepted=[0-9]+ raw)'

echo "A = current tree, B = $patch_name; producer CPU $producer_cpu, consumer CPU $consumer_cpu"
echo "THP enabled: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "THP defrag: $(cat /sys/kernel/mm/transparent_hugepage/defrag)"
echo "=== AnonHugePages one second into a run ==="
for c in "${capacities[@]}"; do
  for v in a b; do
    "$build_root/$v-$c" "${bench[@]}" --samples 300000 >/dev/null &
    sleep 1
    echo "capacity $c variant ${v^^}: $(grep AnonHugePages "/proc/$!/smaps_rollup")"
    wait "$!" || true
  done
done

for ((r = 0; r < rounds; ++r)); do
  variants=(a b)
  if ((r % 2)); then variants=(b a); fi
  for ((i = 0; i < ${#capacities[@]}; ++i)); do
    c=${capacities[(i + r) % ${#capacities[@]}]}
    for v in "${variants[@]}"; do
      echo "=== burst round $((r + 1)) capacity $c variant ${v^^} ==="
      # A run with drops exits 1; its counts are still the result.
      "$build_root/$v-$c" "${bench[@]}" --samples 1000000 | grep -E "$results" || true
    done
  done
done

command -v perf >/dev/null || dnf install -y -q perf >/dev/null
echo "=== perf stat, producer core ==="
for c in "${capacities[@]}"; do
  for v in a b; do
    echo "--- capacity $c variant ${v^^} ---"
    perf stat -C "$producer_cpu" -x, \
      -e cycles,instructions,page-faults,dTLB-load-misses,dTLB-store-misses -- \
      "$build_root/$v-$c" "${bench[@]}" --samples 1000000 2>&1 |
      grep -E '^accepted=[0-9]+ raw|^[0-9<]' | cut -d, -f1,3 || true
  done
done
