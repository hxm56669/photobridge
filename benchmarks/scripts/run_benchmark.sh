#!/usr/bin/env bash
set -euo pipefail

if (( $# < 1 )); then
    echo "Usage: $0 {v1-baseline|v2-sqlite|v3-copy|v4-blake3|v5-io_uring} [photobridge_bench options]" >&2
    exit 2
fi

version=$1
shift
case "$version" in
    v1-baseline|v2-sqlite|v3-copy|v4-blake3|v5-io_uring) ;;
    *) echo "Unknown benchmark result version: $version" >&2; exit 2 ;;
esac

for argument in "$@"; do
    if [[ "$argument" == --output ]]; then
        echo "run_benchmark.sh manages --output; omit it from the extra options" >&2
        exit 2
    fi
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/../.." && pwd)
cd -- "$project_dir"

result_dir="$project_dir/benchmark-results/$version"
mkdir -p -- "$result_dir"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"

cmake --preset bench-release
cmake --build --preset bench-release -j2
bash "$script_dir/collect_env.sh" "$result_dir/$run_id.env.txt"
"$project_dir/build/bench-release/photobridge_bench" \
    --output "$result_dir/$run_id.json" "$@"

printf 'Report: %s\nEnvironment: %s\n' \
    "$result_dir/$run_id.json" "$result_dir/$run_id.env.txt"
