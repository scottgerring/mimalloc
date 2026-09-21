#!/usr/bin/env python3
"""Reproduce mimalloc pprof unwinding and stack-attribution results."""

from __future__ import annotations

import random
import re
import resource
import signal
import subprocess
import sys
from pathlib import Path


MODES = ("FULL", "ON")
ATTRIBUTION_RUNS = 100
BULK_BYTES = 256 * 64
TRIGGER_BYTES = 80
EXPECTED_STACK = ("pprof_stack_leaf", "pprof_stack_middle", "pprof_stack_root")
STACK_COUNT = re.compile(r"^\s*(\d+)\s+(pprof_(?:bulk|trigger)_leaf)\s*$", re.MULTILINE)


def run(
    command: list[str], *, capture: bool = False, check: bool = True, announce: bool = True
):
    if announce:
        print("+", " ".join(command), file=sys.stderr)
    return subprocess.run(
        command,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def build(source_root: Path, build_root: Path, mode: str) -> Path:
    build_dir = build_root / mode.lower()
    run(
        [
            "cmake",
            "-S",
            str(source_root),
            "-B",
            str(build_dir),
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            f"-DMI_PROFILE={mode}",
            "-DMI_BUILD_TESTS=ON",
            "-DMI_BUILD_STATIC=ON",
            "-DMI_BUILD_SHARED=OFF",
            "-DMI_BUILD_OBJECT=OFF",
        ]
    )
    run(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "mimalloc-test-pprof-stacks",
            "-j",
        ]
    )
    return build_dir / "mimalloc-test-pprof-stacks"


def disable_core_dump() -> None:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def cold_result(binary: Path, profile_base: Path) -> str:
    completed = subprocess.run(
        [str(binary), "cold", str(profile_base)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        preexec_fn=disable_core_dump,
    )
    if completed.returncode < 0:
        return signal.Signals(-completed.returncode).name
    return f"exit {completed.returncode}"


def pprof_traces(
    binary: Path, profile: Path, sample_index: str, *, announce: bool = True
) -> str:
    return run(
        [
            "go",
            "tool",
            "pprof",
            "-traces",
            f"-sample_index={sample_index}",
            "-unit=count" if sample_index == "alloc_objects" else "-unit=bytes",
            str(binary),
            str(profile),
        ],
        capture=True,
        announce=announce,
    ).stdout


def unwind_stack(binary: Path, profile_base: Path) -> str:
    profile = Path(f"{profile_base}.0001.pb")
    profile.unlink(missing_ok=True)
    run([str(binary), "unwind", str(profile_base)])
    output = pprof_traces(binary, profile, "alloc_bytes")

    positions = [output.find(frame) for frame in EXPECTED_STACK]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise RuntimeError("pprof output did not contain the expected ordered stack")
    return " → ".join(EXPECTED_STACK)


def attribution_counts(binary: Path, profile_base: Path) -> tuple[int, int]:
    profile = Path(f"{profile_base}.0001.pb")
    profile.unlink(missing_ok=True)
    run(
        [str(binary), "attribution", str(profile_base)],
        capture=True,
        announce=False,
    )
    output = pprof_traces(binary, profile, "alloc_objects", announce=False)
    counts = {"pprof_bulk_leaf": 0, "pprof_trigger_leaf": 0}
    for value, name in STACK_COUNT.findall(output):
        counts[name] += int(value)
    profile.unlink()
    return counts["pprof_bulk_leaf"], counts["pprof_trigger_leaf"]


def bootstrap_interval(rows: list[tuple[int, int]]) -> tuple[float, float]:
    generator = random.Random(0)
    ratios = []
    for _ in range(20_000):
        sample = [rows[generator.randrange(len(rows))] for _ in rows]
        bulk = sum(row[0] for row in sample)
        trigger = sum(row[1] for row in sample)
        ratios.append(trigger / (bulk + trigger))
    ratios.sort()
    return ratios[499], ratios[19_499]


def main() -> int:
    source_root = Path(__file__).resolve().parent.parent
    build_root = source_root / "out" / "pprof-stacks"
    unwind_rows = []
    attribution_rows = []

    for mode in MODES:
        binary = build(source_root, build_root, mode)
        unwind_rows.append(
            (
                mode,
                cold_result(binary, build_root / f"{mode.lower()}-cold"),
                unwind_stack(binary, build_root / f"{mode.lower()}-unwind"),
            )
        )

        process_counts = []
        for index in range(ATTRIBUTION_RUNS):
            process_counts.append(
                attribution_counts(
                    binary, build_root / f"{mode.lower()}-attribution-{index:03d}"
                )
            )
        bulk_total = sum(row[0] for row in process_counts)
        trigger_total = sum(row[1] for row in process_counts)
        total = bulk_total + trigger_total
        low, high = bootstrap_interval(process_counts)
        attribution_rows.append(
            (mode, bulk_total, trigger_total, trigger_total / total, low, high)
        )

    print("### Unwinding")
    print()
    print("| Build mode | First unwind | After priming `backtrace()` |")
    print("|---|---|---|")
    for mode, cold, stack in unwind_rows:
        print(f"| `MI_PROFILE={mode}` | {cold} | `{stack}` |")

    expected = TRIGGER_BYTES / (BULK_BYTES + TRIGGER_BYTES)
    print()
    print("### Attribution")
    print()
    print(
        "| Build mode | Fresh processes | Bulk-stack callbacks | "
        "Trigger-stack callbacks | Trigger share (95% CI) |"
    )
    print("|---|---:|---:|---:|---:|")
    for mode, bulk, trigger, share, low, high in attribution_rows:
        print(
            f"| `MI_PROFILE={mode}` | {ATTRIBUTION_RUNS} | {bulk} | {trigger} | "
            f"{share:.2%} ({low:.2%}–{high:.2%}) |"
        )
    print()
    print(f"Ground-truth trigger byte share: {expected:.3%}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
