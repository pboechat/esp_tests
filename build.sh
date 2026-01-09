#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
  cat >&2 <<'EOF'
Usage:
  build.sh -t <target> [-p <project>]

Args:
  -t, --target   (required) ESP-IDF target string (e.g., esp32, esp32c3, esp32s3)
  -p, --project  (optional) Project subdirectory name under the script directory
  -h, --help     Show this help
EOF
}

on_err() {
  local exit_code=$?
  local line_no=${1:-?}
  local cmd=${2:-?}
  echo "Error: command failed (exit=${exit_code}) at line ${line_no}: ${cmd}" >&2
  exit "$exit_code"
}

trap 'on_err "$LINENO" "$BASH_COMMAND"' ERR

source "${SCRIPT_DIR}/sourceme"

TARGET=""
PROJECT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -t|--target)
      [[ $# -ge 2 ]] || die "Missing value for $1"
      TARGET="$2"
      shift 2
      ;;
    -p|--project)
      [[ $# -ge 2 ]] || die "Missing value for $1"
      PROJECT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      die "Unknown option: $1 (use --help)"
      ;;
    *)
      die "Unexpected positional argument: $1 (use --help)"
      ;;
  esac
done

[[ -n "$TARGET" ]] || { usage; die "--target is required"; }

if [[ -n "$PROJECT" ]]; then
  d="${SCRIPT_DIR}/${PROJECT}"
  if is_project "$d"; then
    build_project "$TARGET" "$d"
  fi
else
  shopt -s nullglob
  for d in "${SCRIPT_DIR}"/*/; do
    # `*/` glob ensures it's a directory; trim trailing slash for prettiness
    d="${d%/}"
    if is_project "$d"; then
      build_project "$TARGET" "$d"
    fi
  done
fi

