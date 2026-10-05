#!/usr/bin/env python3
"""Collect paper Table 3 ablation summary from run JSON files."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.artifact.paper_data import (
    add_common_args,
    load_instances_from_args,
    print_written,
    speedup_summary,
    write_tsv,
)


def collect(args: argparse.Namespace) -> List[Path]:
    instances = load_instances_from_args(args)
    no_der_mode = "full_inc_regional" if args.pinq_mode == "inc_regional" else "full_inc_naive"
    rows = []
    rows.extend(speedup_summary(instances, args.pinq_mode, "PINQ"))
    rows.extend(speedup_summary(instances, no_der_mode, "NoDer"))
    rows.extend(speedup_summary(instances, "inc_full", "NoBdd"))
    return [
        write_tsv(
            args.out_dir / "table3_ablation.tsv",
            ["variant", "mode_key", "arith_mean", "geom_mean", "pct_faster_gt1", "max", "n"],
            rows,
        )
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
