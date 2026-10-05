#!/usr/bin/env python3
"""Run side-channel artifact experiments and write per-delta JSON files."""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List

SIDE_CHANNEL_ROOT = Path(__file__).resolve().parents[1]
RUNNER = SIDE_CHANNEL_ROOT / "cli" / "side_channel_inc.py"
DEFAULT_BASE_DIR = SIDE_CHANNEL_ROOT / "runs" / "side_channel_inc"


@dataclass(frozen=True)
class Profile:
    cases: str
    labels: str
    samples: int
    runs: int


PROFILES = {
    "smoke": Profile(cases="13", labels="inc0p5", samples=1, runs=1),
    "representative": Profile(cases="13,16,20", labels="inc0p5,inc1p0,inc1p5", samples=2, runs=1),
    "complete": Profile(cases="13-20", labels="inc0p5,inc1p0,inc1p5", samples=5, runs=5),
}


def _profile(name: str) -> Profile:
    normalized = "representative" if name == "representitive" else name
    if normalized not in PROFILES:
        raise SystemExit(f"unknown profile {name}; choices: smoke, representative, complete")
    return PROFILES[normalized]


def _run(cmd: List[str]) -> None:
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=["smoke", "representative", "representitive", "complete"])
    parser.add_argument("--base-dir", type=Path, default=DEFAULT_BASE_DIR)
    parser.add_argument("--souffle-bin", default=None)
    parser.add_argument("--compile-timeout", type=int, default=300)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--cases", default=None, help="Override the profile case set")
    parser.add_argument("--delta-labels", default=None, help="Override the profile delta labels")
    parser.add_argument("--delta-samples", type=int, default=None, help="Override the profile samples per label")
    parser.add_argument("--runs", "--delta-runs", dest="delta_runs", type=int, default=None, help="Override the profile runs per delta")
    parser.add_argument("--skip-delta", action="store_true", help="Use existing delta files instead of preparing the alpha-grid set")
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument(
        "--no-materialized-full",
        dest="materialized_full",
        action="store_false",
        default=True,
        help="Disable the default materialized final-input full reference.",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    profile = _profile(args.profile)
    cases = args.cases or profile.cases
    labels = args.delta_labels or profile.labels
    samples = args.delta_samples if args.delta_samples is not None else profile.samples
    runs = args.delta_runs if args.delta_runs is not None else profile.runs
    common = [sys.executable, str(RUNNER), "--base-dir", str(args.base_dir)]
    if args.quiet:
        common.append("--quiet")

    if not args.skip_delta:
        _run([
            *common,
            "delta",
            "--cases",
            cases,
            "--delta-strategy",
            "alpha-grid",
            "--sets",
            str(samples),
            "--cleanup",
        ])

    if not args.skip_compile:
        compile_cmd = [
            *common,
            "compile",
            "--cases",
            cases,
            "--timeout",
            str(args.compile_timeout),
            "--jobs",
            str(args.jobs),
        ]
        if args.souffle_bin:
            compile_cmd.extend(["--souffle-bin", args.souffle_bin])
        _run(compile_cmd)

    run_cmd = [
        *common,
        "run",
        "--cases",
        cases,
        "--timeout",
        str(args.timeout),
        "--delta-labels",
        labels,
        "--delta-samples",
        str(samples),
        "--delta-runs",
        str(runs),
    ]
    if args.materialized_full:
        run_cmd.append("--materialized-full")
    _run(run_cmd)


if __name__ == "__main__":
    main()
