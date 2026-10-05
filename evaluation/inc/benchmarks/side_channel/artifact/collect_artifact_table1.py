#!/usr/bin/env python3
"""Collect paper Table 1 benchmark statistics from run JSON files."""

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
    benchmark_stats,
    load_instances_from_args,
    print_written,
    write_tsv,
)


def collect(args: argparse.Namespace) -> List[Path]:
    instances = load_instances_from_args(args)
    case_rows, summary_rows = benchmark_stats(instances)
    out_dir = args.out_dir
    return [
        write_tsv(
            out_dir / "table1_case_graph_sizes.tsv",
            ["case", "node_median", "edge_median", "node_samples", "edge_samples"],
            case_rows,
        ),
        write_tsv(
            out_dir / "table1_benchmark_stats.tsv",
            ["stat", "nodes", "edges", "n_cases"],
            summary_rows,
        ),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
