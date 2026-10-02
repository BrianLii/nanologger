#!/usr/bin/env bash
# A/B: current tree (A) against the tree with scripts/ab/$NANOLOGGER_AB_PATCH
# applied (B). Both builds share the benchmark source; the two binaries run
# alternately, $NANOLOGGER_AB_ROUNDS rounds per workload.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${NANOLOGGER_BUILD_DIR:-$project_root/build-ab}
patch_name=${NANOLOGGER_AB_PATCH:-shared-cursor.patch}
rounds=${NANOLOGGER_AB_ROUNDS:-5}

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
  ctest --test-dir "$build_root/$v" --output-on-failure
done

command -v perf >/dev/null || dnf install -y -q perf >/dev/null

cpus=(--producer-cpu "$producer_cpu" --consumer-cpu "$consumer_cpu")
workloads=(
  "steady --mode steady --samples 200000 --rate 100000"
  "burst --mode burst --samples 1000000 --rate 100000 --burst-size 100"
  "unthrottled --mode burst --samples 2000000 --rate 1000000000"
)

echo "A = current tree, B = $patch_name; producer CPU $producer_cpu, consumer CPU $consumer_cpu"
for w in "${workloads[@]}"; do
  read -r name args <<< "$w"
  for ((r = 1; r <= rounds; r++)); do
    for v in a b; do
      echo "=== $name round $r variant ${v^^} ==="
      # A run with drops exits 1; its counts are still the result.
      "$build_root/$v/nanologger_bench" $args --pacing sleep "${cpus[@]}" || true
    done
  done
done

echo "=== perf stat, producer core, burst workload ==="
for v in a b; do
  echo "--- variant ${v^^} ---"
  perf stat -C "$producer_cpu" -e cycles,instructions,L1-dcache-load-misses -- \
    "$build_root/$v/nanologger_bench" --mode burst --samples 1000000 \
    --rate 100000 --burst-size 100 --pacing sleep "${cpus[@]}" 2>&1 | grep -E 'accepted=|cycles|instructions|L1-dcache' || true
done
