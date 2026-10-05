#!/usr/bin/env python3
"""Collect Figure 8 companion region/delta coverage data from run JSON files."""

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
    region_delta_percent_rows,
    write_tsv,
)


def collect(args: argparse.Namespace) -> List[Path]:
    instances = load_instances_from_args(args)
    summary_rows, long_rows = region_delta_percent_rows(instances)
    return [
        write_tsv(
            args.out_dir / "figure8_region_delta_percent.tsv",
            [
                "regime",
                "affected_pct",
                "region_pct",
                "changed_pct",
                "region_over_affected_pct",
                "affected_nodes_mean",
                "region_nodes_mean",
                "changed_nodes_mean",
                "view_nodes_mean",
                "n",
            ],
            summary_rows,
        ),
        write_tsv(
            args.out_dir / "figure8_region_delta_percent_long.tsv",
            ["regime", "series", "pct_mean", "nodes_mean", "n"],
            long_rows,
        ),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
