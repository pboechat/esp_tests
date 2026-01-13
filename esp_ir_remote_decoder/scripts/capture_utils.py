#!/usr/bin/env python3

from __future__ import annotations

import csv
from enum import Enum, auto
from pathlib import Path
from typing import List, Sequence, Tuple

Symbol = Tuple[int, int, int, int]


class Protocol(Enum):
    UNKNOWN = auto()
    NEC = auto()
    NEC_REPEAT = auto()
    SIRC_12 = auto()
    SIRC_15 = auto()
    SIRC_20 = auto()
    RC5 = auto()


NEC_TOTAL_BITS = 32
NEC_LEADER_LOW_US = 9000
NEC_LEADER_HIGH_US = 4500
NEC_REPEAT_HIGH_US = 2250
NEC_BIT_LOW_US = 560
NEC_BIT0_HIGH_US = 560
NEC_BIT1_HIGH_US = 1690

SIRC_MARK_US = 600
SIRC_SPACE0_US = 600
SIRC_SPACE1_US = 1200
SIRC_LEADER_MARK_US = 2400
SIRC_LEADER_SPACE_US = 600
SIRC_VARIANT_BITS = {
    Protocol.SIRC_12: 12,
    Protocol.SIRC_15: 15,
    Protocol.SIRC_20: 20,
}

RC5_TOTAL_BITS = 14
RC5_HALF_BIT_US = 889

DEFAULT_PULSE_TOLERANCE = 0.30


def get_sirc_bit_length(protocol: Protocol) -> int:
    return SIRC_VARIANT_BITS.get(protocol, 0)


def within(measured: int, target: int, tolerance: float) -> bool:
    if target <= 0:
        return False
    return abs(measured - target) <= target * tolerance


def load_symbols(csv_path: Path) -> List[Symbol]:
    symbols: List[Symbol] = []
    with csv_path.open("r", newline="") as handle:
        reader = csv.reader(handle)
        for row in reader:
            if not row:
                continue
            if len(row) == 1 and row[0].strip().upper() == "END":
                break
            if len(row) < 5:
                continue
            try:
                level0 = int(row[1].strip())
                duration0 = int(row[2].strip())
                level1 = int(row[3].strip())
                duration1 = int(row[4].strip())
            except ValueError:
                continue
            symbols.append((level0, duration0, level1, duration1))
    if not symbols:
        raise ValueError("No valid symbols parsed from CSV")
    return symbols


def build_waveform(symbols: Sequence[Symbol]) -> Tuple[List[float], List[int]]:
    times_us: List[float] = [0.0]
    levels: List[int] = [symbols[0][0]]

    def append_segment(level: int, duration_us: int) -> None:
        if duration_us <= 0:
            return
        start = times_us[-1]
        if levels[-1] != level:
            times_us.append(start)
            levels.append(level)
        times_us.append(start + duration_us)
        levels.append(level)

    for level0, dur0, level1, dur1 in symbols:
        append_segment(level0, dur0)
        append_segment(level1, dur1)

    if len(times_us) < 2:
        raise ValueError("Waveform is empty after processing symbols")

    times_ms = [t / 1000.0 for t in times_us]
    return times_ms, levels


def detect_protocol(symbols: Sequence[Symbol], tolerance: float = DEFAULT_PULSE_TOLERANCE) -> Protocol:
    if not symbols:
        return Protocol.UNKNOWN
    if _looks_like_nec_repeat(symbols, tolerance):
        return Protocol.NEC_REPEAT
    if _looks_like_nec(symbols, tolerance):
        return Protocol.NEC
    sirc_variant = _looks_like_sirc(symbols, tolerance)
    if sirc_variant:
        return sirc_variant
    if _looks_like_rc5(symbols, tolerance):
        return Protocol.RC5
    return Protocol.UNKNOWN


def _looks_like_nec(symbols: Sequence[Symbol], tolerance: float) -> bool:
    leader = symbols[0]
    if not (within(leader[1], NEC_LEADER_LOW_US, tolerance) and within(leader[3], NEC_LEADER_HIGH_US, tolerance)):
        return False
    bit_slots = _count_nec_data_bits(symbols, tolerance)
    return bit_slots >= NEC_TOTAL_BITS


def _looks_like_nec_repeat(symbols: Sequence[Symbol], tolerance: float) -> bool:
    leader = symbols[0]
    if not (within(leader[1], NEC_LEADER_LOW_US, tolerance) and within(leader[3], NEC_REPEAT_HIGH_US, tolerance)):
        return False
    if len(symbols) < 2:
        return False
    burst = symbols[1]
    return within(burst[1], NEC_BIT_LOW_US, tolerance)


def _count_nec_data_bits(symbols: Sequence[Symbol], tolerance: float) -> int:
    bit_slots = 0
    for symbol in symbols[1:]:
        low_us = symbol[1]
        high_us = symbol[3]
        if not within(low_us, NEC_BIT_LOW_US, tolerance):
            continue
        if within(high_us, NEC_BIT0_HIGH_US, tolerance) or within(high_us, NEC_BIT1_HIGH_US, tolerance):
            bit_slots += 1
        else:
            break
    return bit_slots


def _looks_like_sirc(symbols: Sequence[Symbol], tolerance: float) -> Protocol | None:
    if len(symbols) < 2:
        return None
    leader = symbols[0]
    if not (
        within(leader[1], SIRC_LEADER_MARK_US, tolerance)
        and within(leader[3], SIRC_LEADER_SPACE_US, tolerance)
    ):
        return None
    bits = 0
    for symbol in symbols[1:]:
        mark_us = symbol[1]
        space_us = symbol[3]
        if not within(mark_us, SIRC_MARK_US, tolerance):
            break
        if within(space_us, SIRC_SPACE0_US, tolerance) or within(space_us, SIRC_SPACE1_US, tolerance):
            bits += 1
        else:
            break
        if bits >= max(SIRC_VARIANT_BITS.values()):
            break
    for proto, bit_count in SIRC_VARIANT_BITS.items():
        if bits == bit_count:
            return proto
    return None


def _looks_like_rc5(symbols: Sequence[Symbol], tolerance: float) -> bool:
    segments = flatten_symbols(symbols)
    durations = [dur for _, dur in segments if dur > 0]
    if len(durations) < RC5_TOTAL_BITS * 2:
        return False
    # RC5 encodes every half bit with ~889us pulses and alternates level each half
    max_halves = RC5_TOTAL_BITS * 2
    for dur in durations[:max_halves]:
        if within(dur, RC5_HALF_BIT_US, tolerance):
            continue
        # some captures may merge two halves; allow double-sized durations
        if within(dur, RC5_HALF_BIT_US * 2, tolerance):
            continue
        return False
    for idx in range(1, min(len(segments), max_halves)):
        if segments[idx][0] == segments[idx - 1][0]:
            return False
    return True


def flatten_symbols(symbols: Sequence[Symbol]) -> List[Tuple[int, int]]:
    flattened: List[Tuple[int, int]] = []
    for level0, dur0, level1, dur1 in symbols:
        flattened.append((level0, dur0))
        flattened.append((level1, dur1))
    return flattened
