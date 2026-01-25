#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
  cat >&2 <<'EOF'
Usage:
  configure.sh -t <target> -p <project> [-u <usb_port>]

Args:
  -t, --target   (required) ESP-IDF target string (e.g., esp32, esp32c3, esp32s3)
  -p, --project  (optional) Project subdirectory name under the script directory (default: all projects)
  -u, --usb      (optional) JTAG or UART (default: JTAG)
  -c, --clean    (optional) Clean build directory (if it exists)
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
USB="JTAG"
CLEAN=0

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
    -u|--usb)
      [[ $# -ge 2 ]] || die "Missing value for $1"
      USB="$2"
      shift 2
      ;;
    -c|--clean)
      CLEAN=1
      shift
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

if [[ "$USB" != "JTAG" && "$USB" != "UART" ]]; then
  die "Invalid value for --usb: $USB (must be JTAG or UART)"
fi

if [[ -n "$PROJECT" ]]; then
  d="${SCRIPT_DIR}/${PROJECT}"
  if is_project "$d"; then
    setup_project "${TARGET}" "$d" "${USB}" $CLEAN
  fi
else
  shopt -s nullglob
  for d in "${SCRIPT_DIR}"/*/; do
    # `*/` glob ensures it's a directory; trim trailing slash for prettiness
    d="${d%/}"
    if is_project "$d"; then
      setup_project "${TARGET}" "$d" "${USB}" $CLEAN
    fi
  done
fi
