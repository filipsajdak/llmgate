#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
check=""
if [ "${1:-}" = "--check" ]; then
    check=${2:?usage: update_config_docs.sh [--check example|readme BINARY] | [BINARY]}
    shift 2
fi
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$binary" --print-example > "$work/example"
"$binary" --print-config-table > "$work/table"
awk -v table="$work/table" '
/<!-- config-table:begin -->/ { print; while ((getline row < table) > 0) print row; skipping = 1; next }
/<!-- config-table:end -->/ { skipping = 0 }
!skipping { print }
' "$root/README.md" > "$work/readme"

same() {
    if cmp -s "$1" "$2"; then
        return 0
    fi
    diff -u "$2" "$1" || true
    echo "$2 is stale: run tools/update_config_docs.sh" >&2
    return 1
}

case "$check" in
    "")
        cp "$work/example" "$root/config.example.jsonc"
        cp "$work/readme" "$root/README.md"
        ;;
    example) same "$work/example" "$root/config.example.jsonc" ;;
    readme) same "$work/readme" "$root/README.md" ;;
    *) echo "unknown check: $check" >&2; exit 2 ;;
esac
