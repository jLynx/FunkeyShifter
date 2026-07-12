"""Decode U.B. Funkeys resistor-pad measurements.

Pad numbering used here matches the photo/notes:

    1  2  3  4  5

The most useful measurement is with one probe on pad 1, then measuring to
pad 2, pad 3, pad 4, and pad 5:

    R12, R13, R14, R15

Cyby's board guide encodes each Funkey as R3 R2 R1 R4:

    R3 -> hundreds digit
    R2 -> tens digit
    R1 -> ones digit
    R4 -> checksum digit

From the real-board measurements, those physical readings map to those resistor
positions like this:

    R12 -> R4/checksum digit
    R13 -> R1/ones digit
    R14 -> R2/tens digit
    R15 -> R3/hundreds digit

The firmware then sends the digits as ADC buckets in this order:

    ones, tens, hundreds, checksum
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from typing import Iterable


# The ADC bucket centers used by src/main.c.
ADC_BUCKETS = [278, 375, 465, 554, 643, 713, 783, 844, 899, 942]
BASELINE_ADC = 177

# Nominal digit-to-resistor buckets from Cyby's U.B. Funkeys Board
# Reproduction Guide. Values are the decoded resistor-label values in kOhm.
RESISTANCE_BUCKETS_KOHM = {
    0: 1.020,
    1: 18.300,
    2: 39.300,
    3: 68.300,
    4: 114.000,
    5: 164.000,
    6: 244.000,
    7: 364.000,
    8: 564.000,
    9: 914.000,
}

RESISTANCE_WARN_DELTA_KOHM = 12.0


@dataclass(frozen=True)
class DigitMatch:
    digit: int
    measured_kohm: float
    bucket_kohm: float

    @property
    def error_kohm(self) -> float:
        return self.measured_kohm - self.bucket_kohm

    @property
    def trusted(self) -> bool:
        return abs(self.error_kohm) <= RESISTANCE_WARN_DELTA_KOHM


@dataclass(frozen=True)
class DecodeResult:
    label: str
    left_reference_kohm: tuple[float, float, float, float]
    checksum: DigitMatch
    ones: DigitMatch
    tens: DigitMatch
    hundreds: DigitMatch

    @property
    def digits(self) -> tuple[int, int, int, int]:
        return (
            self.ones.digit,
            self.tens.digit,
            self.hundreds.digit,
            self.checksum.digit,
        )

    @property
    def funkey_id(self) -> int:
        return self.hundreds.digit * 100 + self.tens.digit * 10 + self.ones.digit

    @property
    def expected_checksum(self) -> int:
        return (self.ones.digit + self.tens.digit + self.hundreds.digit) % 10

    @property
    def checksum_ok(self) -> bool:
        return self.checksum.digit == self.expected_checksum

    @property
    def untrusted_matches(self) -> list[DigitMatch]:
        return [
            match
            for match in (self.checksum, self.ones, self.tens, self.hundreds)
            if not match.trusted
        ]


def nearest_resistance_digit(measured_kohm: float) -> DigitMatch:
    digit = min(
        RESISTANCE_BUCKETS_KOHM,
        key=lambda candidate: abs(RESISTANCE_BUCKETS_KOHM[candidate] - measured_kohm),
    )
    return DigitMatch(digit, measured_kohm, RESISTANCE_BUCKETS_KOHM[digit])


def decode_left_reference(readings_kohm: Iterable[float], label: str) -> DecodeResult:
    r12, r13, r14, r15 = tuple(readings_kohm)
    return DecodeResult(
        label=label,
        left_reference_kohm=(r12, r13, r14, r15),
        checksum=nearest_resistance_digit(r12),
        ones=nearest_resistance_digit(r13),
        tens=nearest_resistance_digit(r14),
        hundreds=nearest_resistance_digit(r15),
    )


def left_reference_from_center(readings_kohm: Iterable[float]) -> tuple[float, float, float, float]:
    """Convert pad-3-reference readings into pad-1-reference readings.

    Input order is:

        pad3-to-pad1, pad3-to-pad2, pad3-to-pad4, pad3-to-pad5

    This works because the pads behave like taps on one resistor ladder.
    """

    r31, r32, r34, r35 = tuple(readings_kohm)
    return (
        abs(r32 - r31),
        r31,
        abs(r34 - r31),
        abs(r35 - r31),
    )


def raw_packet_from_id(funkey_id: int) -> bytes:
    if not 0 <= funkey_id <= 999:
        raise ValueError("raw packet encoding supports decimal IDs 0..999")

    digits = [
        funkey_id % 10,
        (funkey_id // 10) % 10,
        (funkey_id // 100) % 10,
    ]
    digits.append(sum(digits) % 10)

    packet = [0] * 7
    for index, digit in enumerate(digits):
        adc = ADC_BUCKETS[digit]
        packet[index] = adc & 0xFF
        packet[4] |= ((adc >> 8) & 0x03) << (index * 2)

    packet[5] = BASELINE_ADC & 0xFF
    packet[6] = (BASELINE_ADC >> 8) & 0xFF
    return bytes(packet)


def adc_values_for_digits(digits: Iterable[int]) -> list[int]:
    return [ADC_BUCKETS[digit] for digit in digits]


def format_packet(packet: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in packet)


def print_result(result: DecodeResult) -> None:
    print(f"--- {result.label} ---")
    print(
        "Pad-1 readings R12/R13/R14/R15 kOhm: "
        + ", ".join(f"{value:.2f}" for value in result.left_reference_kohm)
    )
    print(
        "Digits ones/tens/hundreds/checksum: "
        f"{list(result.digits)}"
    )
    print(
        "Resistance matches: "
        f"checksum={describe_match(result.checksum)}, "
        f"ones={describe_match(result.ones)}, "
        f"tens={describe_match(result.tens)}, "
        f"hundreds={describe_match(result.hundreds)}"
    )
    print(
        f"Checksum: measured {result.checksum.digit}, "
        f"expected {result.expected_checksum}, "
        f"{'OK' if result.checksum_ok else 'FAIL'}"
    )
    print(f"Decimal ID: {result.funkey_id}")
    print(f"Hex suffix: {result.funkey_id:08X}")
    print(f"ADC values: {adc_values_for_digits(result.digits)}")
    print(f"Generated raw packet: {format_packet(raw_packet_from_id(result.funkey_id))}")

    if result.untrusted_matches:
        print(
            "Warning: at least one reading is far from the calibrated buckets. "
            "Measure more known figures and extend RESISTANCE_BUCKETS_KOHM."
        )
    print()


def describe_match(match: DigitMatch) -> str:
    return (
        f"{match.digit} ({match.measured_kohm:.2f}k -> "
        f"{match.bucket_kohm:.2f}k, error {match.error_kohm:+.2f}k)"
    )


def print_samples() -> None:
    samples = [
        ("Speed Racer left-reference", [39.56, 359.10, 111.40, 18.12]),
        ("Chim-Chim left-reference", [18.02, 239.30, 109.50, 17.89]),
        (
            "Speed Racer from pad-3 reference",
            left_reference_from_center([359.10, 398.60, 470.60, 377.20]),
        ),
        (
            "Chim-Chim from pad-3 reference",
            left_reference_from_center([239.50, 257.50, 350.10, 257.40]),
        ),
        (
            "Flurry from pad-3 reference",
            left_reference_from_center([1.001, 554.10, 554.70, 1.988]),
        ),
        (
            "Webley from pad-3 reference",
            left_reference_from_center([39.66, 57.50, 954.00, 40.67]),
        ),
        (
            "Wasabi from pad-3 reference",
            left_reference_from_center([362.20, 380.00, 472.20, 363.30]),
        ),
    ]

    for label, readings in samples:
        print_result(decode_left_reference(readings, label))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--left",
        nargs=4,
        type=float,
        metavar=("R12", "R13", "R14", "R15"),
        help="pad-1-reference readings in kOhm",
    )
    group.add_argument(
        "--center",
        nargs=4,
        type=float,
        metavar=("R31", "R32", "R34", "R35"),
        help="pad-3-reference readings in kOhm: pad1, pad2, pad4, pad5",
    )
    parser.add_argument("--label", default="measurement")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if args.left is not None:
        print_result(decode_left_reference(args.left, args.label))
        return

    if args.center is not None:
        readings = left_reference_from_center(args.center)
        print_result(decode_left_reference(readings, args.label))
        return

    print_samples()


if __name__ == "__main__":
    main()
