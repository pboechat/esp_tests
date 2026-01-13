#!/usr/bin/env python3

from __future__ import annotations
from capture_utils import build_waveform, load_symbols
import matplotlib.pyplot as plt
import matplotlib

import argparse
import sys
from pathlib import Path
from typing import Sequence

scripts_dir = Path(__file__).resolve().parent
if str(scripts_dir) not in sys.path:
    sys.path.insert(0, str(scripts_dir))


matplotlib.use("Agg")


def plot_waveform(times_ms: Sequence[float], levels: Sequence[int], output_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 4))
    ax.step(times_ms, levels, where="post")
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Level")
    ax.set_ylim(-0.2, 1.2)
    ax.set_title("IR Capture Waveform")
    ax.grid(True, which="both", axis="both", linestyle="--", alpha=0.4)
    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot IR capture CSV to PNG"
    )
    parser.add_argument(
        "-i", "--input",
        type=Path,
        required=True,
        help="Path to the CSV produced by the firmware dump",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        required=True,
        help="Destination path for the generated PNG",
    )
    args = parser.parse_args()

    symbols = load_symbols(args.input)
    times_ms, levels = build_waveform(symbols)
    plot_waveform(times_ms, levels, args.output)

    sys.exit(0)


if __name__ == "__main__":
    main()
