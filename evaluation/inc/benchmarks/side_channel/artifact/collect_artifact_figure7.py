#!/usr/bin/env python3
"""Collect paper Figure 7 runtime breakdown from run JSON files."""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path
from statistics import fmean
from typing import Dict, List

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.artifact.paper_data import (
    add_common_args,
    load_instances_from_args,
    print_written,
    write_tsv,
)


def _pct_rows(instances) -> tuple[List[dict], List[dict]]:
    full_comp: Dict[str, List[float]] = defaultdict(list)
    pinq_comp: Dict[str, List[float]] = defaultdict(list)
    full_stage: Dict[str, List[float]] = defaultdict(list)
    pinq_stage: Dict[str, List[float]] = defaultdict(list)

    for inst in instances:
        if inst.stage_full is None or inst.stage_pinq is None:
            continue
        full_total = inst.stage_full.sem + inst.stage_full.prn + inst.stage_full.fc + inst.stage_full.wmc
        pinq_total = inst.stage_pinq.sem + inst.stage_pinq.prn + inst.stage_pinq.fc + inst.stage_pinq.wmc
        if full_total <= 0.0 or pinq_total <= 0.0:
            continue
        full_comp["Derivation"].append((inst.stage_full.sem + inst.stage_full.prn) / full_total)
        full_comp["Compilation"].append(inst.stage_full.fc / full_total)
        full_comp["WMC"].append(inst.stage_full.wmc / full_total)
        pinq_comp["Derivation"].append((inst.stage_pinq.sem + inst.stage_pinq.prn) / pinq_total)
        pinq_comp["Compilation"].append(inst.stage_pinq.fc / pinq_total)
        pinq_comp["WMC"].append(inst.stage_pinq.wmc / pinq_total)

        for stage, full_value, pinq_value in [
            ("SEM", inst.stage_full.sem, inst.stage_pinq.sem),
            ("PRN", inst.stage_full.prn, inst.stage_pinq.prn),
            ("FC", inst.stage_full.fc, inst.stage_pinq.fc),
            ("WMC", inst.stage_full.wmc, inst.stage_pinq.wmc),
        ]:
            full_stage[stage].append(full_value / full_total)
            pinq_stage[stage].append(pinq_value / pinq_total)

    comp_rows: List[dict] = []
    for component in ["Derivation", "Compilation", "WMC"]:
        full_values = full_comp.get(component, [])
        pinq_values = pinq_comp.get(component, [])
        if not full_values or not pinq_values:
            continue
        full_pct = 100.0 * fmean(full_values)
        pinq_pct = 100.0 * fmean(pinq_values)
        comp_rows.append({
            "component": component,
            "full_pct": f"{full_pct:.3f}",
            "pinq_pct": f"{pinq_pct:.3f}",
            "delta_pp": f"{pinq_pct - full_pct:.3f}",
            "n": str(min(len(full_values), len(pinq_values))),
        })

    stage_rows: List[dict] = []
    for stage in ["SEM", "PRN", "FC", "WMC"]:
        full_values = full_stage.get(stage, [])
        pinq_values = pinq_stage.get(stage, [])
        if not full_values or not pinq_values:
            continue
        full_pct = 100.0 * fmean(full_values)
        pinq_pct = 100.0 * fmean(pinq_values)
        stage_rows.append({
            "stage": stage,
            "full_pct": f"{full_pct:.3f}",
            "pinq_pct": f"{pinq_pct:.3f}",
            "delta_pp": f"{pinq_pct - full_pct:.3f}",
            "n": str(min(len(full_values), len(pinq_values))),
        })
    return comp_rows, stage_rows


def collect(args: argparse.Namespace) -> List[Path]:
    comp_rows, stage_rows = _pct_rows(load_instances_from_args(args))
    return [
        write_tsv(
            args.out_dir / "figure7_runtime_breakdown.tsv",
            ["component", "full_pct", "pinq_pct", "delta_pp", "n"],
            comp_rows,
        ),
        write_tsv(
            args.out_dir / "figure7_runtime_breakdown_stages.tsv",
            ["stage", "full_pct", "pinq_pct", "delta_pp", "n"],
            stage_rows,
        ),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    print_written(collect(parser.parse_args()))


if __name__ == "__main__":
    main()
