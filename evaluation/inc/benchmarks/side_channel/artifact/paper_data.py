#!/usr/bin/env python3
"""Shared JSON readers for artifact table and figure collectors."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import fmean, median
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

SIDE_CHANNEL_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SIDE_CHANNEL_ROOT.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

DEFAULT_BASE_DIR = SIDE_CHANNEL_ROOT / "runs" / "side_channel_inc"
DEFAULT_OUT_DIR = SIDE_CHANNEL_ROOT / "runs" / "side_channel_inc" / "artifact_tables"
DEFAULT_CASES = "13-20"
DEFAULT_LABELS = "inc0p5,inc1p0,inc1p5"
DEFAULT_LABEL_DELTA_MAP = {"inc0p5": 0.005, "inc1p0": 0.01, "inc1p5": 0.015}
ALPHA_BUCKETS = ["0.00", "0.25", "0.50", "0.75", "1.00"]
TIMING_TURN_MODE = {
    "full": "FULL",
    "inc_naive": "INC",
    "inc_regional": "INC",
    "inc_full": "INC",
    "full_inc_naive": "FULL",
    "full_inc_regional": "FULL",
}
EXPECTED_TURN_STAGES = {
    "full": ("SEMINAIVE_FULL", "PRUNING_FULL", "FORWARD_COMPILATION_FULL", "WEIGHTED_MODEL_COUNTING_FULL"),
    "inc_naive": ("SEMINAIVE_INC", "PRUNING_INC", "FORWARD_COMPILATION_INC", "WEIGHTED_MODEL_COUNTING_INC"),
    "inc_regional": ("SEMINAIVE_INC", "PRUNING_INC", "FORWARD_COMPILATION_INC", "WEIGHTED_MODEL_COUNTING_INC"),
    "inc_full": ("SEMINAIVE_INC", "PRUNING_INC", "FORWARD_COMPILATION_FULL", "WEIGHTED_MODEL_COUNTING_FULL"),
    "full_inc_naive": ("SEMINAIVE_FULL", "PRUNING_FULL", "FORWARD_COMPILATION_INC", "WEIGHTED_MODEL_COUNTING_INC"),
    "full_inc_regional": ("SEMINAIVE_FULL", "PRUNING_FULL", "FORWARD_COMPILATION_INC", "WEIGHTED_MODEL_COUNTING_INC"),
}


@dataclass
class StageTimes:
    total: float
    sem: float
    prn: float
    fc: float
    wmc: float


@dataclass
class RegionCoverage:
    view_nodes: int
    affected_nodes: int
    region_nodes: int
    changed_nodes: Optional[int]


@dataclass
class Instance:
    case: str
    delta_label: str
    delta_ratio: Optional[float]
    alpha: Optional[float]
    alpha_bucket: str
    sample: str
    run: str
    elapsed: Dict[str, Optional[float]]
    stages: Dict[str, Optional[StageTimes]]
    stage_full: Optional[StageTimes]
    stage_pinq: Optional[StageTimes]
    graph_nodes: Optional[int]
    graph_edges: Optional[int]
    region_coverage: Optional[RegionCoverage]
    compare_ok: Optional[bool]


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--base-dir", type=Path, default=DEFAULT_BASE_DIR)
    parser.add_argument("--output-dir", default="output")
    parser.add_argument("--cases", default=DEFAULT_CASES)
    parser.add_argument("--labels", default=DEFAULT_LABELS)
    parser.add_argument(
        "--label-delta-map",
        default=",".join(f"{k}={v}" for k, v in DEFAULT_LABEL_DELTA_MAP.items()),
    )
    parser.add_argument("--pinq-mode", choices=["inc_naive", "inc_regional"], default="inc_regional")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)


def parse_cases(spec: str) -> List[str]:
    values: set[str] = set()
    for part in spec.split(","):
        item = part.strip()
        if not item:
            continue
        if "-" in item:
            start, end = item.split("-", 1)
            start_match = re.fullmatch(r"([A-Za-z]+)?(\d+)", start)
            end_match = re.fullmatch(r"([A-Za-z]+)?(\d+)", end)
            if not start_match or not end_match:
                raise ValueError(f"invalid case range: {item}")
            prefix = (start_match.group(1) or "").upper()
            end_prefix = (end_match.group(1) or prefix).upper()
            if prefix != end_prefix:
                raise ValueError(f"invalid mixed-prefix case range: {item}")
            lo = int(start_match.group(2))
            hi = int(end_match.group(2))
            if hi < lo:
                raise ValueError(f"invalid case range: {item}")
            for value in range(lo, hi + 1):
                values.add(f"{prefix}{value}" if prefix else f"P{value}")
        else:
            match = re.fullmatch(r"([A-Za-z]+)?(\d+)", item)
            if not match:
                raise ValueError(f"invalid case: {item}")
            prefix = (match.group(1) or "P").upper()
            values.add(f"{prefix}{int(match.group(2))}")

    def key(name: str) -> Tuple[str, int, str]:
        match = re.fullmatch(r"([A-Za-z]+)(\d+)", name)
        if match:
            return (match.group(1), int(match.group(2)), name)
        return (name, 0, name)

    return sorted(values, key=key)


def parse_labels(spec: str) -> List[str]:
    labels = [x.strip() for x in spec.split(",") if x.strip()]
    if not labels:
        raise ValueError("labels cannot be empty")
    return labels


def parse_float_map(spec: str) -> Dict[str, float]:
    out: Dict[str, float] = {}
    for part in spec.split(","):
        item = part.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid map entry: {item}")
        key, raw = item.split("=", 1)
        key = key.strip()
        if key:
            out[key] = float(raw.strip())
    return out


def load_instances_from_args(args: argparse.Namespace) -> List[Instance]:
    return load_instances(
        base_dir=args.base_dir,
        output_dir=args.output_dir,
        cases=parse_cases(args.cases),
        labels=parse_labels(args.labels),
        ratio_map=parse_float_map(args.label_delta_map) if args.label_delta_map.strip() else {},
        pinq_mode=args.pinq_mode,
    )


def _elapsed(entry: object) -> Optional[float]:
    if not isinstance(entry, dict):
        return None
    raw = entry.get("elapsed_s", entry.get("elapsed"))
    if raw is None:
        return None
    try:
        val = float(raw)
    except (TypeError, ValueError):
        return None
    return val if val > 0.0 else None


def _exit_ok(entry: object) -> bool:
    return isinstance(entry, dict) and entry.get("exit") in (0, None)


def _pick_turn(turns: Sequence[dict], mode_key: str, include_first: bool = False) -> Optional[dict]:
    for idx, turn in enumerate(turns):
        if idx == 0 and not include_first:
            continue
        if mode_key in str(turn.get("mode", "")).upper():
            return turn
    return None


def _turn_matches_stage_contract(turn: dict, contract_key: Optional[str]) -> bool:
    if contract_key is None:
        return True
    expected = EXPECTED_TURN_STAGES.get(contract_key)
    if not expected:
        return True
    raw_stages = turn.get("raw_stages")
    if not isinstance(raw_stages, list) or not raw_stages:
        return True
    names = {str(stage.get("name", "")) for stage in raw_stages if isinstance(stage, dict)}
    return all(name in names for name in expected)


def _extract_stages(entry: object, mode_key: str, contract_key: Optional[str] = None) -> Optional[StageTimes]:
    if not isinstance(entry, dict):
        return None
    log = entry.get("log") if isinstance(entry.get("log"), dict) else {}
    turns = ((log.get("stages") or {}) if isinstance(log, dict) else {}).get("turns") or []
    include_first = bool(entry.get("materialized_final_input")) and contract_key == "full"
    turn = _pick_turn(turns, mode_key, include_first=include_first)
    if not isinstance(turn, dict):
        return None
    if not _turn_matches_stage_contract(turn, contract_key):
        return None
    stages = turn.get("stages") or {}
    try:
        sem = float(stages.get("SEM", 0.0) or 0.0)
        prn = float(stages.get("PRN", 0.0) or 0.0)
        fc = float(stages.get("FC", 0.0) or 0.0)
        wmc = float(stages.get("WMC", 0.0) or 0.0)
        total = sem + prn + fc + wmc
    except (TypeError, ValueError):
        return None
    if total <= 0.0:
        total = float(turn.get("time_seconds", 0.0) or 0.0)
    if total <= 0.0:
        return None
    return StageTimes(total=total, sem=sem, prn=prn, fc=fc, wmc=wmc)


def _to_int(value: object) -> Optional[int]:
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return None


def _extract_graph_totals_from_stdout_stats(stdout_stats: object) -> List[Tuple[int, int]]:
    if not isinstance(stdout_stats, dict):
        return []
    totals: List[Tuple[int, int]] = []
    for key, block in stdout_stats.items():
        if not str(key).isdigit() or not isinstance(block, dict):
            continue
        candidates: List[dict] = []
        if "apply_delta_graph" in block:
            candidates.append(block)
        candidates.extend(v for v in block.values() if isinstance(v, dict) and "apply_delta_graph" in v)
        for candidate in candidates:
            graph = candidate.get("apply_delta_graph") or {}
            nodes = _to_int(graph.get("totalNodes"))
            edges = _to_int(graph.get("totalEdges"))
            if nodes is not None and edges is not None:
                totals.append((nodes, edges))
    return totals


def _log_turns(entry: object) -> List[dict]:
    if not isinstance(entry, dict):
        return []
    log = entry.get("log") if isinstance(entry.get("log"), dict) else {}
    turns = ((log.get("stages") or {}) if isinstance(log, dict) else {}).get("turns") or []
    return [turn for turn in turns if isinstance(turn, dict)]


def _stage_infos(entry: object, turn_mode: str, stage_name: str, include_first: bool = False) -> List[dict]:
    infos: List[dict] = []
    turn = _pick_turn(_log_turns(entry), turn_mode, include_first=include_first)
    if not isinstance(turn, dict):
        return infos
    for stage in turn.get("raw_stages") or []:
        if not isinstance(stage, dict) or stage.get("name") != stage_name:
            continue
        info = stage.get("info")
        if isinstance(info, dict):
            infos.append(info)
    return infos


def _stage_logs(entry: object, turn_mode: str, stage_name: str) -> List[str]:
    lines: List[str] = []
    turn = _pick_turn(_log_turns(entry), turn_mode)
    if not isinstance(turn, dict):
        return lines
    for stage in turn.get("raw_stages") or []:
        if not isinstance(stage, dict) or stage.get("name") != stage_name:
            continue
        logs = stage.get("logs")
        if not isinstance(logs, dict):
            continue
        for block in logs.values():
            if isinstance(block, list):
                lines.extend(str(line) for line in block)
    return lines


def _extract_graph_totals_from_log(entry: object) -> List[Tuple[int, int]]:
    totals: List[Tuple[int, int]] = []
    for turn in _log_turns(entry):
        for stage in turn.get("raw_stages") or []:
            if not isinstance(stage, dict):
                continue
            info = stage.get("info") if isinstance(stage.get("info"), dict) else {}
            after_nodes = _to_int(info.get("after_nodes"))
            after_edges = _to_int(info.get("after_edges"))
            if after_nodes is not None and after_edges is not None:
                totals.append((after_nodes, after_edges))
            graph_nodes = _to_int(info.get("graph_nodes"))
            graph_edges = _to_int(info.get("graph_edges"))
            if graph_nodes is not None and graph_edges is not None:
                totals.append((graph_nodes, graph_edges))
            for log_lines in (stage.get("logs") or {}).values() if isinstance(stage.get("logs"), dict) else []:
                if not isinstance(log_lines, list):
                    continue
                for line in log_lines:
                    match = re.search(r"apply_delta_graph:\s*totalNodes=(\d+)\s+totalEdges=(\d+)", str(line))
                    if match:
                        totals.append((int(match.group(1)), int(match.group(2))))
    return totals


def _extract_pruned_view_nodes(full_entry: object) -> Optional[int]:
    values: List[int] = []
    for info in _stage_infos(full_entry, "FULL", "PRUNING_FULL", include_first=True):
        nodes = _to_int(info.get("after_nodes"))
        if nodes is not None and nodes > 0:
            values.append(nodes)
    return values[-1] if values else None


def _changed_nodes_from_logs(lines: List[str]) -> Optional[int]:
    for line in lines:
        match = re.search(r"delta counts:\s+insNodes=(\d+)\s+insEdges=\d+\s+delNodes=(\d+)\s+delEdges=\d+", line)
        if match:
            return int(match.group(1)) + int(match.group(2))
    return None


def _extract_region_coverage(regional_entry: object, full_entry: object) -> Optional[RegionCoverage]:
    if not isinstance(regional_entry, dict):
        return None
    infos = _stage_infos(regional_entry, "INC", "FORWARD_COMPILATION_INC")
    if not infos:
        return None
    info = next(
        (
            x for x in reversed(infos)
            if "fc_lite_region_nodes" in x or "fc_lite_dr_nodes" in x
            or "fc_lite_changed_nodes" in x or "changed_node_count" in x
        ),
        None,
    )
    if info is None:
        return None

    changed_nodes = _to_int(info.get("fc_lite_changed_nodes") or info.get("changed_node_count"))
    if changed_nodes is None:
        ins_nodes = _to_int(info.get("fc_lite_delta_insert_nodes"))
        del_nodes = _to_int(info.get("fc_lite_delta_delete_nodes"))
        if ins_nodes is not None or del_nodes is not None:
            changed_nodes = (ins_nodes or 0) + (del_nodes or 0)
    if changed_nodes is None:
        changed_nodes = _changed_nodes_from_logs(_stage_logs(regional_entry, "INC", "FORWARD_COMPILATION_INC"))

    region_nodes = _to_int(info.get("fc_lite_region_nodes"))
    affected_nodes = _to_int(info.get("fc_lite_dr_nodes"))
    if affected_nodes is None and changed_nodes is not None:
        affected_nodes = changed_nodes
    if region_nodes is None and changed_nodes is not None:
        ins_nodes = _to_int(info.get("fc_lite_delta_insert_nodes"))
        del_nodes = _to_int(info.get("fc_lite_delta_delete_nodes"))
        if (ins_nodes or 0) == 0 and (del_nodes or 0) > 0:
            region_nodes = changed_nodes
    if region_nodes is None or affected_nodes is None or affected_nodes <= 0:
        return None

    view_nodes = _to_int(info.get("fc_lite_view_nodes")) or _extract_pruned_view_nodes(full_entry)
    if view_nodes is None or view_nodes <= 0:
        return None

    return RegionCoverage(
        view_nodes=view_nodes,
        affected_nodes=affected_nodes,
        region_nodes=region_nodes,
        changed_nodes=changed_nodes,
    )


def _extract_graph_totals(entry: object) -> List[Tuple[int, int]]:
    if not isinstance(entry, dict):
        return []
    totals = _extract_graph_totals_from_stdout_stats(entry.get("stdout_stats"))
    totals.extend(_extract_graph_totals_from_log(entry))
    return totals


def _alpha_from_mix_label(label: str) -> Optional[float]:
    match = re.fullmatch(r"mix-d(\d+)-i(\d+)", label)
    if not match:
        return None
    deletes = int(match.group(1))
    inserts = int(match.group(2))
    total = deletes + inserts
    return None if total <= 0 else float(deletes) / float(total)


def _alpha_from_json(data: dict, label: str) -> Optional[float]:
    raw = data.get("delta_alpha")
    if raw is not None:
        try:
            return float(raw)
        except (TypeError, ValueError):
            pass
    deletes = data.get("delta_deletes")
    inserts = data.get("delta_inserts")
    try:
        d = float(deletes)
        i = float(inserts)
        total = d + i
        if total > 0:
            return d / total
    except (TypeError, ValueError):
        pass
    return _alpha_from_mix_label(label)


def bucket_alpha(alpha: Optional[float]) -> str:
    if alpha is None:
        return "unknown"
    values = [float(x) for x in ALPHA_BUCKETS]
    nearest = min(values, key=lambda x: abs(x - alpha))
    if abs(nearest - alpha) <= 0.05:
        return f"{nearest:.2f}"
    return f"{alpha:.2f}"


def _extract_key_from_path(path: Path, default_case: str) -> Tuple[str, str, str, str]:
    case = default_case
    delta = ""
    sample = ""
    run = ""
    for idx, part in enumerate(path.parts):
        if part.startswith("P") and part[1:].isdigit():
            case = part
        elif part.startswith("delta-"):
            delta = part[len("delta-") :]
        elif part.startswith("sample-"):
            sample = part[len("sample-") :]
        elif part.startswith("run-"):
            run = part[len("run-") :]
        elif idx == len(path.parts) - 1 and part.startswith("delta-") and "-run-" in part:
            stem = part[:-5] if part.endswith(".json") else part
            pieces = stem.split("-")
            if len(pieces) >= 5 and pieces[0] == "delta":
                delta = delta or "-".join(pieces[1:-3])
                sample = sample or pieces[-3]
                run = run or pieces[-1]
    return case, delta, sample, run


def load_instances(
    base_dir: Path,
    output_dir: str,
    cases: List[str],
    labels: List[str],
    ratio_map: Dict[str, float],
    pinq_mode: str,
) -> List[Instance]:
    instances: List[Instance] = []
    for case in cases:
        case_dir = base_dir / case
        if not case_dir.exists():
            continue
        pattern = f"{output_dir}/delta-*/sample-*/run-*/delta-*-run-*.json"
        for json_path in case_dir.glob(pattern):
            try:
                data = json.loads(json_path.read_text(encoding="utf-8"))
            except Exception:
                continue
            path_case, path_delta, path_sample, path_run = _extract_key_from_path(json_path, case)
            delta_label = str(data.get("delta_label") or path_delta)
            if labels and delta_label not in labels:
                continue

            elapsed: Dict[str, Optional[float]] = {}
            stage_by_mode: Dict[str, Optional[StageTimes]] = {}
            for key in ["full", "inc_naive", "inc_regional", "inc_full", "full_inc_naive", "full_inc_regional"]:
                entry = data.get(key)
                stages = _extract_stages(entry, TIMING_TURN_MODE[key], key) if _exit_ok(entry) else None
                stage_by_mode[key] = stages
                elapsed[key] = stages.total if stages is not None else None

            full_entry = data.get("full")
            pinq_entry = data.get(pinq_mode)
            stage_full = _extract_stages(full_entry, "FULL", "full") if _exit_ok(full_entry) else None
            stage_pinq = _extract_stages(pinq_entry, "INC", pinq_mode) if _exit_ok(pinq_entry) else None
            region_coverage = (
                _extract_region_coverage(data.get("inc_regional"), full_entry)
                if _exit_ok(data.get("inc_regional")) and _exit_ok(full_entry)
                else None
            )

            graph_nodes: Optional[int] = None
            graph_edges: Optional[int] = None
            if isinstance(pinq_entry, dict):
                totals = _extract_graph_totals(pinq_entry)
                if totals:
                    graph_nodes = max(n for n, _ in totals)
                    graph_edges = max(e for _, e in totals)

            alpha = _alpha_from_json(data, delta_label)
            instances.append(
                Instance(
                    case=str(data.get("case") or path_case),
                    delta_label=delta_label,
                    delta_ratio=ratio_map.get(delta_label),
                    alpha=alpha,
                    alpha_bucket=bucket_alpha(alpha),
                    sample=str(data.get("delta_sample") or path_sample),
                    run=str(data.get("delta_run") or path_run),
                    elapsed=elapsed,
                    stages=stage_by_mode,
                    stage_full=stage_full,
                    stage_pinq=stage_pinq,
                    graph_nodes=graph_nodes,
                    graph_edges=graph_edges,
                    region_coverage=region_coverage,
                    compare_ok=(data.get("compare") or {}).get("ok") if isinstance(data.get("compare"), dict) else None,
                )
            )
    instances.sort(key=lambda x: (x.case, delta_sort_key(x.delta_label, ratio_map), sort_id(x.sample), sort_id(x.run)))
    return instances


def sort_id(raw: str) -> Tuple[int, int, str]:
    try:
        return (0, int(raw), "")
    except (TypeError, ValueError):
        return (1, 0, str(raw))


def delta_sort_key(label: str, ratio_map: Dict[str, float]) -> Tuple[int, float, str]:
    if label in ratio_map:
        return (0, ratio_map[label], label)
    match = re.fullmatch(r"inc(\d+)p(\d+)", label)
    if match:
        return (1, float(f"{int(match.group(1))}.{match.group(2)}"), label)
    match = re.fullmatch(r"inc(\d+)", label)
    if match:
        return (1, float(int(match.group(1))), label)
    return (2, 0.0, label)


def alpha_sort_key(bucket: str) -> Tuple[int, float, str]:
    if bucket == "unknown":
        return (1, 0.0, bucket)
    try:
        return (0, float(bucket), bucket)
    except ValueError:
        return (2, 0.0, bucket)


def safe_speedup(num: Optional[float], den: Optional[float]) -> Optional[float]:
    if num is None or den is None or den <= 0.0:
        return None
    return num / den


def stage_metric(inst: Instance, mode_key: str, metric: str) -> Optional[float]:
    stages = inst.stages.get(mode_key)
    if stages is None:
        return None
    if metric == "total":
        return stages.total
    if metric == "sem":
        return stages.sem
    if metric == "prn":
        return stages.prn
    if metric == "fc":
        return stages.fc
    if metric == "wmc":
        return stages.wmc
    raise ValueError(f"unknown stage metric: {metric}")


def geomean(values: Sequence[float]) -> Optional[float]:
    if not values or any(v <= 0.0 for v in values):
        return None
    return math.exp(sum(math.log(v) for v in values) / len(values))


def metric_summary(values: Sequence[float]) -> Dict[str, Optional[float]]:
    if not values:
        return {"n": 0.0, "arith_mean": None, "geom_mean": None, "pct_faster": None, "max": None}
    return {
        "n": float(len(values)),
        "arith_mean": fmean(values),
        "geom_mean": geomean(values),
        "pct_faster": 100.0 * sum(1 for v in values if v > 1.0) / float(len(values)),
        "max": max(values),
    }


def fmt_num(value: Optional[float], digits: int = 6) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def fmt_pct(value: Optional[float]) -> str:
    return "" if value is None else f"{value:.3f}"


def write_tsv(path: Path, fieldnames: List[str], rows: List[dict]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    return path


def benchmark_stats(instances: List[Instance]) -> Tuple[List[dict], List[dict]]:
    by_case_nodes: Dict[str, List[int]] = defaultdict(list)
    by_case_edges: Dict[str, List[int]] = defaultdict(list)
    for inst in instances:
        if inst.graph_nodes is not None and inst.graph_edges is not None:
            by_case_nodes[inst.case].append(inst.graph_nodes)
            by_case_edges[inst.case].append(inst.graph_edges)

    case_rows: List[dict] = []
    nodes_rep: List[int] = []
    edges_rep: List[int] = []
    for case in sorted(set(by_case_nodes) | set(by_case_edges)):
        nodes = by_case_nodes.get(case, [])
        edges = by_case_edges.get(case, [])
        if not nodes or not edges:
            continue
        node_med = int(round(median(nodes)))
        edge_med = int(round(median(edges)))
        nodes_rep.append(node_med)
        edges_rep.append(edge_med)
        case_rows.append({
            "case": case,
            "node_median": str(node_med),
            "edge_median": str(edge_med),
            "node_samples": str(len(nodes)),
            "edge_samples": str(len(edges)),
        })

    summary_rows: List[dict] = []
    if nodes_rep and edges_rep:
        summary_rows = [
            {"stat": "Med", "nodes": f"{median(nodes_rep):.3f}", "edges": f"{median(edges_rep):.3f}", "n_cases": str(len(nodes_rep))},
            {"stat": "Avg", "nodes": f"{fmean(nodes_rep):.3f}", "edges": f"{fmean(edges_rep):.3f}", "n_cases": str(len(nodes_rep))},
            {"stat": "Max", "nodes": f"{max(nodes_rep):.3f}", "edges": f"{max(edges_rep):.3f}", "n_cases": str(len(nodes_rep))},
        ]
    return case_rows, summary_rows


def speedup_summary(instances: List[Instance], mode_key: str, variant: str) -> List[dict]:
    values = [s for s in (safe_speedup(inst.elapsed.get("full"), inst.elapsed.get(mode_key)) for inst in instances) if s is not None]
    summary = metric_summary(values)
    return [{
        "variant": variant,
        "mode_key": mode_key,
        "arith_mean": fmt_num(summary["arith_mean"]),
        "geom_mean": fmt_num(summary["geom_mean"]),
        "pct_faster_gt1": fmt_pct(summary["pct_faster"]),
        "max": fmt_num(summary["max"]),
        "n": str(int(summary["n"])),
    }]


def speedup_grid(
    instances: List[Instance],
    numerator: str,
    denominator: str,
    labels: Optional[List[str]] = None,
    ratio_map: Optional[Dict[str, float]] = None,
) -> Tuple[List[dict], List[dict]]:
    grouped: Dict[Tuple[str, str], List[float]] = defaultdict(list)
    for inst in instances:
        value = safe_speedup(inst.elapsed.get(numerator), inst.elapsed.get(denominator))
        if value is not None:
            grouped[(inst.delta_label, inst.alpha_bucket)].append(value)

    sort_map = ratio_map or DEFAULT_LABEL_DELTA_MAP
    grid_labels = labels if labels is not None else sorted({inst.delta_label for inst in instances}, key=lambda x: delta_sort_key(x, sort_map))
    buckets = ALPHA_BUCKETS
    long_rows: List[dict] = []
    grid_rows: List[dict] = []
    for label in sorted(grid_labels, key=lambda x: delta_sort_key(x, sort_map)):
        grid = {"delta_label": label}
        for bucket in buckets:
            vals = grouped.get((label, bucket), [])
            avg = fmean(vals) if vals else None
            grid[bucket] = fmt_num(avg)
            long_rows.append({
                "delta_label": label,
                "alpha_bucket": bucket,
                "avg_speedup": fmt_num(avg),
                "n": str(len(vals)),
            })
        grid_rows.append(grid)
    return long_rows, grid_rows


def region_delta_percent_rows(instances: List[Instance]) -> Tuple[List[dict], List[dict]]:
    grouped: Dict[str, List[RegionCoverage]] = defaultdict(list)
    all_coverages: List[RegionCoverage] = []
    for inst in instances:
        cov = inst.region_coverage
        if cov is None:
            continue
        grouped[inst.alpha_bucket].append(cov)
        all_coverages.append(cov)

    def summarize(regime: str, values: List[RegionCoverage]) -> Tuple[dict, List[dict]]:
        if not values:
            row = {
                "regime": regime,
                "affected_pct": "",
                "region_pct": "",
                "changed_pct": "",
                "region_over_affected_pct": "",
                "affected_nodes_mean": "",
                "region_nodes_mean": "",
                "changed_nodes_mean": "",
                "view_nodes_mean": "",
                "n": "0",
            }
            return row, [
                {"regime": regime, "series": "affected_delta", "pct_mean": "", "nodes_mean": "", "n": "0"},
                {"regime": regime, "series": "detected_region", "pct_mean": "", "nodes_mean": "", "n": "0"},
            ]

        affected_pct = [100.0 * x.affected_nodes / x.view_nodes for x in values if x.view_nodes > 0]
        region_pct = [100.0 * x.region_nodes / x.view_nodes for x in values if x.view_nodes > 0]
        region_over_affected = [
            100.0 * x.region_nodes / x.affected_nodes for x in values if x.affected_nodes > 0
        ]
        changed_values = [x for x in values if x.changed_nodes is not None and x.view_nodes > 0]
        changed_pct = [100.0 * float(x.changed_nodes or 0) / x.view_nodes for x in changed_values]
        affected_nodes = [float(x.affected_nodes) for x in values]
        region_nodes = [float(x.region_nodes) for x in values]
        changed_nodes = [float(x.changed_nodes or 0) for x in changed_values]
        view_nodes = [float(x.view_nodes) for x in values]
        row = {
            "regime": regime,
            "affected_pct": fmt_num(fmean(affected_pct) if affected_pct else None),
            "region_pct": fmt_num(fmean(region_pct) if region_pct else None),
            "changed_pct": fmt_num(fmean(changed_pct) if changed_pct else None),
            "region_over_affected_pct": fmt_num(fmean(region_over_affected) if region_over_affected else None),
            "affected_nodes_mean": fmt_num(fmean(affected_nodes) if affected_nodes else None),
            "region_nodes_mean": fmt_num(fmean(region_nodes) if region_nodes else None),
            "changed_nodes_mean": fmt_num(fmean(changed_nodes) if changed_nodes else None),
            "view_nodes_mean": fmt_num(fmean(view_nodes) if view_nodes else None),
            "n": str(len(values)),
        }
        long_rows = [
            {
                "regime": regime,
                "series": "affected_delta",
                "pct_mean": row["affected_pct"],
                "nodes_mean": row["affected_nodes_mean"],
                "n": row["n"],
            },
            {
                "regime": regime,
                "series": "detected_region",
                "pct_mean": row["region_pct"],
                "nodes_mean": row["region_nodes_mean"],
                "n": row["n"],
            },
        ]
        if changed_values:
            long_rows.append({
                "regime": regime,
                "series": "changed_delta",
                "pct_mean": row["changed_pct"],
                "nodes_mean": row["changed_nodes_mean"],
                "n": str(len(changed_values)),
            })
        return row, long_rows

    summary_rows: List[dict] = []
    long_rows: List[dict] = []
    for bucket in ALPHA_BUCKETS:
        row, rows = summarize(f"alpha={bucket}", grouped.get(bucket, []))
        summary_rows.append(row)
        long_rows.extend(rows)
    row, rows = summarize("Overall", all_coverages)
    summary_rows.append(row)
    long_rows.extend(rows)
    return summary_rows, long_rows


def print_written(paths: Iterable[Path]) -> None:
    for path in paths:
        print(path)
