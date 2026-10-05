#!/usr/bin/env python3
"""Collect artifact table/figure data from existing run JSON files only."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Callable, List

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.artifact import (
    collect_artifact_figure6,
    collect_artifact_figure7,
    collect_artifact_figure8,
    collect_artifact_figure8_region_delta,
    collect_artifact_table1,
    collect_artifact_table2,
    collect_artifact_table3,
)
from benchmarks.side_channel.artifact.paper_data import add_common_args, print_written

Collector = Callable[[argparse.Namespace], List[Path]]

COLLECTORS: List[Collector] = [
    collect_artifact_table1.collect,
    collect_artifact_table2.collect,
    collect_artifact_table3.collect,
    collect_artifact_figure6.collect,
    collect_artifact_figure7.collect,
    collect_artifact_figure8.collect,
    collect_artifact_figure8_region_delta.collect,
]


def _dedup(paths: List[Path]) -> List[Path]:
    seen = set()
    out: List[Path] = []
    for path in paths:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        out.append(path)
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    args = parser.parse_args()

    written: List[Path] = []
    for collector in COLLECTORS:
        written.extend(collector(args))
    print_written(_dedup(written))


if __name__ == "__main__":
    main()
