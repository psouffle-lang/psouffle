#!/usr/bin/env python3
"""Collect paper Figure 8 regional-vs-naive 3x5 speedup grid from run JSON files."""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.artifact.paper_data import (
    ALPHA_BUCKETS,
    add_common_args,
    fmt_num,
    fmt_pct,
    load_instances_from_args,
    metric_summary,
    parse_labels,
    print_written,
    safe_speedup,
    stage_metric,
    write_tsv,
)


def collect(args: argparse.Namespace) -> List[Path]:
    instances = load_instances_from_args(args)
    grouped_grid = defaultdict(list)
    grouped = defaultdict(list)
    values = []
    labels = parse_labels(args.labels)
    for inst in instances:
        value = safe_speedup(stage_metric(inst, "inc_naive", "fc"), stage_metric(inst, "inc_regional", "fc"))
        if value is None:
            continue
        grouped_grid[(inst.delta_label, inst.alpha_bucket)].append(value)
        grouped[inst.alpha_bucket].append(value)
        values.append(value)

    long_rows = []
    grid_rows = []
    for label in labels:
        grid = {"delta_label": label}
        for bucket in ALPHA_BUCKETS:
            vals = grouped_grid.get((label, bucket), [])
            avg = sum(vals) / len(vals) if vals else None
            grid[bucket] = fmt_num(avg)
            long_rows.append({
                "delta_label": label,
                "alpha_bucket": bucket,
                "avg_speedup": fmt_num(avg),
                "n": str(len(vals)),
            })
        grid_rows.append(grid)

    summary_rows = []
    for bucket in ALPHA_BUCKETS:
        vals = grouped.get(bucket, [])
        summary = metric_summary(vals)
        summary_rows.append({
            "regime": f"alpha={bucket}",
            "arith mean": fmt_num(summary["arith_mean"]),
            "geom mean": fmt_num(summary["geom_mean"]),
            "% faster": fmt_pct(summary["pct_faster"]),
            "max": fmt_num(summary["max"]),
            "n": str(int(summary["n"])),
        })
    summary = metric_summary(values)
    summary_rows.append({
        "regime": "Overall",
        "arith mean": fmt_num(summary["arith_mean"]),
        "geom mean": fmt_num(summary["geom_mean"]),
        "% faster": fmt_pct(summary["pct_faster"]),
        "max": fmt_num(summary["max"]),
        "n": str(int(summary["n"])),
    })
    return [
        write_tsv(
            args.out_dir / "figure8_regional_vs_naive_grid_long.tsv",
            ["delta_label", "alpha_bucket", "avg_speedup", "n"],
            long_rows,
        ),
        write_tsv(
            args.out_dir / "figure8_regional_vs_naive_grid.tsv",
            ["delta_label"] + ALPHA_BUCKETS,
            grid_rows,
        ),
        write_tsv(
            args.out_dir / "figure8_regional_vs_naive_summary.tsv",
            ["regime", "arith mean", "geom mean", "% faster", "max", "n"],
            summary_rows,
        ),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
