#!/usr/bin/env python3
"""Collect paper Figure 6 delta-turn speedup grid from run JSON files."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.artifact.paper_data import (
    ALPHA_BUCKETS,
    add_common_args,
    load_instances_from_args,
    parse_float_map,
    parse_labels,
    print_written,
    speedup_grid,
    write_tsv,
)


def collect(args: argparse.Namespace) -> List[Path]:
    long_rows, grid_rows = speedup_grid(
        load_instances_from_args(args),
        "full",
        args.pinq_mode,
        labels=parse_labels(args.labels),
        ratio_map=parse_float_map(args.label_delta_map) if args.label_delta_map.strip() else {},
    )
    return [
        write_tsv(
            args.out_dir / "figure6_end_to_end_grid_long.tsv",
            ["delta_label", "alpha_bucket", "avg_speedup", "n"],
            long_rows,
        ),
        write_tsv(
            args.out_dir / "figure6_end_to_end_grid.tsv",
            ["delta_label"] + ALPHA_BUCKETS,
            grid_rows,
        ),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
