#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
    echo "Usage: $0 OUTPUT_FILE" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/../.." && pwd)
output=$1
mkdir -p -- "$(dirname -- "$output")"

{
    printf 'Collected UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'Git revision: %s\n' "$(git -C "$project_dir" rev-parse HEAD)"
    printf 'Git status:\n'
    git -C "$project_dir" status --short
    printf '\nKernel:\n'
    uname -a
    if command -v lscpu >/dev/null 2>&1; then
        printf '\nCPU:\n'
        lscpu
    fi
    if command -v free >/dev/null 2>&1; then
        printf '\nMemory:\n'
        free -h
    fi
    printf '\nCompiler: '
    c++ --version | head -n 1
    printf 'CMake: '
    cmake --version | head -n 1
    printf '\nProject filesystem:\n'
    df -h -- "$project_dir"
} > "$output"
