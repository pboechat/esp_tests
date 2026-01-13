#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence, Tuple

import matplotlib
import matplotlib.pyplot as plt
from capture_utils import (DEFAULT_PULSE_TOLERANCE, NEC_BIT0_HIGH_US,
                           NEC_BIT1_HIGH_US, NEC_BIT_LOW_US,
                           NEC_LEADER_HIGH_US, NEC_LEADER_LOW_US,
                           NEC_TOTAL_BITS, RC5_HALF_BIT_US, RC5_TOTAL_BITS,
                           SIRC_LEADER_MARK_US, SIRC_LEADER_SPACE_US,
                           SIRC_MARK_US, SIRC_SPACE0_US, SIRC_SPACE1_US,
                           Protocol, Symbol, build_waveform, detect_protocol,
                           flatten_symbols, get_sirc_bit_length, load_symbols)

scripts_dir = Path(__file__).resolve().parent
if str(scripts_dir) not in sys.path:
    sys.path.insert(0, str(scripts_dir))


matplotlib.use("Agg")


@dataclass
class NecFrame:
    bits: List[int]
    address: int
    address_inv: int
    command: int
    command_inv: int
    bit_windows_us: List[Tuple[float, float]]


@dataclass
class SircFrame:
    bits: List[int]
    command: int
    address: int
    extended: int
    bit_length: int
    bit_windows_us: List[Tuple[float, float]]


@dataclass
class Rc5Frame:
    bits: List[int]
    toggle: int
    address: int
    command: int
    bit_windows_us: List[Tuple[float, float]]


def _within(measured: float, target: float, tolerance: float) -> bool:
    return abs(measured - target) <= target * tolerance


def _bits_to_int_lsb(bits: Sequence[int]) -> int:
    value = 0
    for idx, bit in enumerate(bits):
        value |= (bit & 0x1) << idx
    return value


def _bits_to_int_msb(bits: Sequence[int]) -> int:
    value = 0
    for bit in bits:
        value = (value << 1) | (bit & 0x1)
    return value


def decode_nec(symbols: Sequence[Symbol], tolerance: float) -> NecFrame:
    if not symbols:
        raise ValueError("No symbols to decode")

    leader = symbols[0]
    if not (_within(leader[1], NEC_LEADER_LOW_US, tolerance) and _within(leader[3], NEC_LEADER_HIGH_US, tolerance)):
        raise ValueError("First symbol does not match NEC leader timing")

    bits: List[int] = []
    bit_windows: List[Tuple[float, float]] = []
    elapsed_us = leader[1] + leader[3]

    for symbol in symbols[1:]:
        low_us = symbol[1]
        high_us = symbol[3]
        start_us = elapsed_us
        duration_us = low_us + high_us
        elapsed_us += duration_us

        if not _within(low_us, NEC_BIT_LOW_US, tolerance):
            continue

        if _within(high_us, NEC_BIT0_HIGH_US, tolerance):
            bit = 0
        elif _within(high_us, NEC_BIT1_HIGH_US, tolerance):
            bit = 1
        else:
            break

        bits.append(bit)
        bit_windows.append((start_us, start_us + duration_us))
        if len(bits) == NEC_TOTAL_BITS:
            break

    if len(bits) != NEC_TOTAL_BITS:
        raise ValueError(
            f"Expected {NEC_TOTAL_BITS} bits but decoded {len(bits)}")

    address = _bits_to_int_lsb(bits[0:8])
    address_inv = _bits_to_int_lsb(bits[8:16])
    command = _bits_to_int_lsb(bits[16:24])
    command_inv = _bits_to_int_lsb(bits[24:32])

    return NecFrame(
        bits=bits,
        address=address,
        address_inv=address_inv,
        command=command,
        command_inv=command_inv,
        bit_windows_us=bit_windows,
    )


def decode_sirc(symbols: Sequence[Symbol], protocol: Protocol, tolerance: float) -> SircFrame:
    bit_length = get_sirc_bit_length(protocol)
    if bit_length == 0:
        raise ValueError("Unsupported SIRC variant")
    leader = symbols[0]
    if not (
        _within(leader[1], SIRC_LEADER_MARK_US, tolerance)
        and _within(leader[3], SIRC_LEADER_SPACE_US, tolerance)
    ):
        raise ValueError("First symbol does not match SIRC leader timing")

    bits: List[int] = []
    bit_windows: List[Tuple[float, float]] = []
    elapsed_us = leader[1] + leader[3]

    for symbol in symbols[1:]:
        mark_us = symbol[1]
        space_us = symbol[3]
        start_us = elapsed_us
        duration_us = mark_us + space_us
        elapsed_us += duration_us

        if not _within(mark_us, SIRC_MARK_US, tolerance):
            continue
        if _within(space_us, SIRC_SPACE0_US, tolerance):
            bit = 0
        elif _within(space_us, SIRC_SPACE1_US, tolerance):
            bit = 1
        else:
            break

        bits.append(bit)
        bit_windows.append((start_us, start_us + duration_us))
        if len(bits) == bit_length:
            break

    if len(bits) != bit_length:
        raise ValueError(f"Expected {bit_length} bits but decoded {len(bits)}")

    command_bits = bits[0:7]
    address_bits = bits[7:12]
    extended_bits = bits[12:]
    command = _bits_to_int_lsb(command_bits)
    address = _bits_to_int_lsb(address_bits)
    extended = _bits_to_int_lsb(extended_bits) if extended_bits else 0

    return SircFrame(
        bits=bits,
        command=command,
        address=address,
        extended=extended,
        bit_length=bit_length,
        bit_windows_us=bit_windows,
    )


def decode_rc5(symbols: Sequence[Symbol], tolerance: float) -> Rc5Frame:
    segments = [(level, dur)
                for level, dur in flatten_symbols(symbols) if dur > 0]
    if not segments:
        raise ValueError("No segments available for RC5 decode")

    half_levels: List[int] = []
    half_windows: List[Tuple[float, float]] = []
    current_time = 0.0
    expected_halves = RC5_TOTAL_BITS * 2

    for level, duration in segments:
        if len(half_levels) >= expected_halves:
            break
        if _within(duration, RC5_HALF_BIT_US, tolerance):
            count = 1
        elif _within(duration, RC5_HALF_BIT_US * 2, tolerance):
            count = 2
        else:
            raise ValueError("RC5 pulse width does not match expected timing")
        half_duration = duration / count
        for _ in range(count):
            if len(half_levels) >= expected_halves:
                break
            half_levels.append(level)
            half_windows.append((current_time, current_time + half_duration))
            current_time += half_duration

    if len(half_levels) < expected_halves or len(half_levels) % 2 != 0:
        raise ValueError("Incomplete RC5 frame")

    bit_windows: List[Tuple[float, float]] = []
    bits: List[int] = []
    first_pair = (half_levels[0], half_levels[1])
    if first_pair[0] == first_pair[1]:
        raise ValueError("RC5 Manchester pair missing transition")
    zero_pair = (first_pair[1], first_pair[0])

    for idx in range(0, expected_halves, 2):
        first = half_levels[idx]
        second = half_levels[idx + 1]
        bit_windows.append((half_windows[idx][0], half_windows[idx + 1][1]))
        pair = (first, second)
        if pair == first_pair:
            bits.append(1)
        elif pair == zero_pair:
            bits.append(0)
        else:
            raise ValueError("RC5 Manchester phase drift detected")

    if len(bits) != RC5_TOTAL_BITS:
        raise ValueError("RC5 bit count mismatch")

    toggle = bits[2]
    address_bits = bits[3:8]
    command_bits = bits[8:14]
    address = _bits_to_int_msb(address_bits)
    command = _bits_to_int_msb(command_bits)
    if bits[1] == 0:
        command |= 0x40  # field bit extends the command range

    return Rc5Frame(
        bits=bits,
        toggle=toggle,
        address=address,
        command=command,
        bit_windows_us=bit_windows,
    )


def print_nec_summary(frame: NecFrame) -> None:
    addr_ok = (frame.address ^ frame.address_inv) & 0xFF == 0xFF
    cmd_ok = (frame.command ^ frame.command_inv) & 0xFF == 0xFF
    print("Decoded NEC frame:")
    print(
        f"  Address      : 0x{frame.address:02X} (invert 0x{frame.address_inv:02X}) [{'OK' if addr_ok else 'FAIL'}]"
    )
    print(
        f"  Command      : 0x{frame.command:02X} (invert 0x{frame.command_inv:02X}) [{'OK' if cmd_ok else 'FAIL'}]"
    )
    raw_bytes = [
        _bits_to_int_lsb(frame.bits[i:i + 8])
        for i in range(0, len(frame.bits), 8)
    ]
    payload_hex = " ".join(f"0x{byte:02X}" for byte in raw_bytes)
    payload_word = sum(bit << idx for idx, bit in enumerate(frame.bits))
    print("  Raw bits     :", " ".join(
        "".join(str(frame.bits[i + j]) for j in range(8))
        for i in range(0, len(frame.bits), 8)
    ), "(LSB-first per byte)")
    print(f"  Payload HEX  : {payload_hex} (word 0x{payload_word:08X})")


def print_sirc_summary(frame: SircFrame) -> None:
    print(f"Decoded SIRC {frame.bit_length}-bit frame:")
    print(f"  Command : 0x{frame.command:02X}")
    print(f"  Address : 0x{frame.address:02X}")
    if frame.bit_length > 12:
        ext_bits = frame.bit_length - 12
        hex_width = max(1, (ext_bits + 3) // 4)
        print(f"  Extended: 0x{frame.extended:0{hex_width}X}")
    print("  Raw bits :", "".join(str(b) for b in frame.bits), "(LSB-first)")


def print_rc5_summary(frame: Rc5Frame) -> None:
    print("Decoded RC5 frame:")
    print(f"  Start bits : {frame.bits[0]} {frame.bits[1]}")
    print(f"  Toggle     : {frame.toggle}")
    print(f"  Address    : 0x{frame.address:02X}")
    print(f"  Command    : 0x{frame.command:02X}")
    print("  Raw bits   :", " ".join(str(b)
          for b in frame.bits), "(MSB-first)")


def plot_bits(
    times_ms: Sequence[float],
    levels: Sequence[int],
    bit_windows_us: Sequence[Tuple[float, float]],
    bit_labels: Sequence[str],
    title: str,
    group_breaks: Sequence[int],
    output_path: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(12, 4))
    ax.step(times_ms, levels, where="post")
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Level")
    ax.set_ylim(-0.2, 1.2)
    ax.set_title(title)
    ax.grid(True, which="both", axis="both", linestyle="--", alpha=0.4)

    for idx, (start_us, end_us) in enumerate(bit_windows_us):
        mid_ms = ((start_us + end_us) / 2.0) / 1000.0
        label = bit_labels[idx] if idx < len(bit_labels) else f"b{idx}"
        ax.text(mid_ms, 1.05, label, ha="center",
                va="bottom", fontsize=6, rotation=60)
        if (idx + 1) in group_breaks:
            ax.axvline(end_us / 1000.0, color="crimson",
                       linestyle="--", alpha=0.35)

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path)
    plt.close(fig)


def plot_nec(symbols: Sequence[Symbol], frame: NecFrame, output_path: Path) -> None:
    times_ms, levels = build_waveform(symbols)
    labels = [f"{idx}:{bit}" for idx, bit in enumerate(frame.bits)]
    plot_bits(
        times_ms,
        levels,
        frame.bit_windows_us,
        labels,
        title=(
            f"NEC Frame — Addr 0x{frame.address:02X}/~0x{frame.address_inv:02X} "
            f"Cmd 0x{frame.command:02X}/~0x{frame.command_inv:02X}"
        ),
        group_breaks=[8, 16, 24, 32],
        output_path=output_path,
    )


def plot_sirc(symbols: Sequence[Symbol], frame: SircFrame, output_path: Path) -> None:
    times_ms, levels = build_waveform(symbols)
    labels: List[str] = []
    for idx in range(frame.bit_length):
        if idx < 7:
            labels.append(f"C{idx}={frame.bits[idx]}")
        elif idx < 12:
            labels.append(f"A{idx - 7}={frame.bits[idx]}")
        else:
            labels.append(f"E{idx - 12}={frame.bits[idx]}")
    breaks = [7, 12]
    plot_bits(
        times_ms,
        levels,
        frame.bit_windows_us,
        labels,
        title=(
            f"SIRC {frame.bit_length}-bit — Cmd 0x{frame.command:02X} Addr 0x{frame.address:02X}"
        ),
        group_breaks=[b for b in breaks if b <
                      frame.bit_length] + [frame.bit_length],
        output_path=output_path,
    )


def plot_rc5(symbols: Sequence[Symbol], frame: Rc5Frame, output_path: Path) -> None:
    times_ms, levels = build_waveform(symbols)
    labels = [
        f"S1={frame.bits[0]}",
        f"S2={frame.bits[1]}",
        f"T={frame.toggle}",
    ]
    labels.extend(f"A{4 - i}={bit}" for i, bit in enumerate(frame.bits[3:8]))
    cmd_names = ["C5", "C4", "C3", "C2", "C1", "C0"]
    labels.extend(f"{name}={bit}" for name,
                  bit in zip(cmd_names, frame.bits[8:]))
    plot_bits(
        times_ms,
        levels,
        frame.bit_windows_us,
        labels,
        title=(
            f"RC5 — Addr 0x{frame.address:02X} Cmd 0x{frame.command:02X} Toggle {frame.toggle}"
        ),
        group_breaks=[1, 2, 3, 8, 14],
        output_path=output_path,
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Decode and visualize IR frames from the CSV capture"
    )
    parser.add_argument(
        "-i",
        "--input",
        type=Path,
        required=True,
        help="Path to the CSV produced by the capture",
    )
    parser.add_argument(
        "-p",
        "--png",
        type=Path,
        help="Optional destination for an annotated PNG",
    )
    parser.add_argument(
        "-t",
        "--tolerance",
        type=float,
        default=DEFAULT_PULSE_TOLERANCE,
        help="Relative tolerance (0-1) when matching pulse durations",
    )
    args = parser.parse_args()

    symbols = load_symbols(args.input)
    protocol = detect_protocol(symbols, tolerance=args.tolerance)
    print(f"Detected protocol: {protocol.name}")

    if protocol == Protocol.NEC_REPEAT:
        print("Detected NEC repeat frame (no new payload to decode).")
        sys.exit(0)
    elif protocol == Protocol.NEC:
        frame = decode_nec(symbols, tolerance=args.tolerance)
        print_nec_summary(frame)
        if args.png:
            plot_nec(symbols, frame, args.png)
        sys.exit(0)
    elif protocol in (Protocol.SIRC_12, Protocol.SIRC_15, Protocol.SIRC_20):
        frame = decode_sirc(symbols, protocol, tolerance=args.tolerance)
        print_sirc_summary(frame)
        if args.png:
            plot_sirc(symbols, frame, args.png)
        sys.exit(0)
    elif protocol == Protocol.RC5:
        frame = decode_rc5(symbols, tolerance=args.tolerance)
        print_rc5_summary(frame)
        if args.png:
            plot_rc5(symbols, frame, args.png)
        sys.exit(0)
    else:
        print("Unsupported or unknown protocol; nothing to decode.")
        sys.exit(1)


if __name__ == "__main__":
    main()
