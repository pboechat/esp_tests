#!/usr/bin/env python3

from __future__ import annotations

scripts_dir = Path(__file__).resolve().parent
if str(scripts_dir) not in sys.path:
    sys.path.insert(0, str(scripts_dir))

import argparse
import sys
from pathlib import Path

from capture_utils import (DEFAULT_PULSE_TOLERANCE, detect_protocol,
                           load_symbols)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Detect the IR protocol used in the CSV capture"
    )
    parser.add_argument(
        "-i", "--input",
        type=Path,
        required=True,
        help="Path to the CSV produced by the firmware dump",
    )
    parser.add_argument(
        "-t", "--tolerance",
        type=float,
        default=DEFAULT_PULSE_TOLERANCE,
        help="Relative tolerance (0-1) when matching pulse durations",
    )
    args = parser.parse_args()
    symbols = load_symbols(args.input)
    protocol = detect_protocol(symbols, tolerance=args.tolerance)
    print(f"Detected protocol: {protocol.name}")
    sys.exit(0)


if __name__ == "__main__":
    main()
