#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/create-tarball.sh --dir PATH --output PATH

DIR_PATH=""
OUTPUT_PATH=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)    DIR_PATH="$2"; shift 2 ;;
    --output) OUTPUT_PATH="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

[[ -n "$DIR_PATH" ]]    || { echo "Error: --dir is required" >&2; exit 1; }
[[ -n "$OUTPUT_PATH" ]] || { echo "Error: --output is required" >&2; exit 1; }
[[ -d "$DIR_PATH" ]]    || { echo "Error: Directory not found: $DIR_PATH" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT_PATH")"

# Top-level dir inside the tarball, derived from the artifact filename
TOP_DIR=$(basename "$OUTPUT_PATH" .tar.gz)

STAGING=$(mktemp -d)
trap "chmod -R u+w '${STAGING}' 2>/dev/null; rm -rf '${STAGING}'" EXIT
# Nix store outputs are read-only, fix permissions so the extracted tree is writable for end users.
cp -a "$DIR_PATH" "${STAGING}/${TOP_DIR}"

tar -czf "$OUTPUT_PATH" -C "$STAGING" "$TOP_DIR"

echo "Tarball created: ${OUTPUT_PATH}"