#!/usr/bin/env python3
"""Build both profiling modes, run the workload, and print the results."""

from __future__ import annotations

import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


RUNS = 10
ROUNDS = 10
MEAN_INTERVAL = 512 * 1024
FIELD_RE = re.compile(r"([a-z_]+)=([0-9]+)")
MODES = ("FULL", "ON")


@dataclass(frozen=True)
class RunResult:
    ground_truth_alloc_space: int
    alloc_callbacks: int
    callback_weighted_bytes: int
    pointer_mismatches: int

    @property
    def weighted_ratio(self) -> float:
        return self.callback_weighted_bytes / self.ground_truth_alloc_space


def run(command: list[str], *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), file=sys.stderr)
    return subprocess.run(
        command,
        check=True,
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
            "mimalloc-test-otel-memory",
            "-j",
        ]
    )
    binary = build_dir / "mimalloc-test-otel-memory"
    if not binary.is_file():
        raise RuntimeError(
            f"{binary} was not built; install the Linux package providing sys/sdt.h"
        )
    return binary


def parse_output(output: str) -> RunResult:
    ground_truth_alloc_space = 0
    adapter: dict[str, int] | None = None
    truth_lines = 0

    for line in output.splitlines():
        fields = {key: int(value) for key, value in FIELD_RE.findall(line)}
        if line.startswith("GROUND_TRUTH "):
            truth_lines += 1
            ground_truth_alloc_space += fields["alloc_space"]
        elif line.startswith("ADAPTER "):
            adapter = fields

    if truth_lines != 3 or adapter is None:
        raise RuntimeError("workload output did not contain the expected result lines")

    return RunResult(
        ground_truth_alloc_space=ground_truth_alloc_space,
        alloc_callbacks=adapter["alloc_callbacks"],
        callback_weighted_bytes=adapter["callback_weighted_bytes"],
        pointer_mismatches=adapter["pointer_mismatches"],
    )


def measure(binary: Path) -> list[RunResult]:
    results = []
    for _ in range(RUNS):
        completed = run(
            [
                str(binary),
                "--startup-delay",
                "0",
                "--rounds",
                str(ROUNDS),
                "--hold-seconds",
                "0",
                "--mean-interval",
                str(MEAN_INTERVAL),
            ],
            capture=True,
        )
        results.append(parse_output(completed.stdout))
    return results


def print_results(results_by_mode: dict[str, list[RunResult]]) -> None:
    print(
        "| Build mode | Runs | "
        "Mean weighted bytes / ground truth | Standard deviation | "
        "Mean allocation callbacks | Mean alloc/free pointer mismatches |"
    )
    print("|---|---:|---:|---:|---:|---:|")
    for mode in MODES:
        results = results_by_mode[mode]
        ratios = [result.weighted_ratio for result in results]
        print(
            f"| `MI_PROFILE={mode}` | {len(results)} | "
            f"{statistics.mean(ratios):.4f} | {statistics.stdev(ratios):.4f} | "
            f"{statistics.mean(result.alloc_callbacks for result in results):.1f} | "
            f"{statistics.mean(result.pointer_mismatches for result in results):.1f} |"
        )


def main() -> int:
    if not sys.platform.startswith("linux"):
        raise RuntimeError("the USDT experiment currently requires Linux")

    source_root = Path(__file__).resolve().parent.parent
    build_root = source_root / "out" / "otel-memory-baseline"
    results_by_mode = {
        mode: measure(build(source_root, build_root, mode)) for mode in MODES
    }
    print_results(results_by_mode)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
