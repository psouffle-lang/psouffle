#!/usr/bin/env python3
"""Aggregate simple-pattern vs general-SISO rewrite contributions from RQ1 profiles."""

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


KEYS = (
    "implicit_overlay_all_facts_regions", "implicit_overlay_single_regions",
    "implicit_overlay_linear_regions", "implicit_overlay_parallel_regions",
    "implicit_overlay_fan_out_regions", "implicit_graph_detected_regions",
    "implicit_graph_general_regions", "implicit_graph_nodes_removed",
    "implicit_graph_edges_removed", "implicit_graph_edges_added",
    "implicit_general_nodes_removed", "implicit_general_edges_removed",
    "implicit_general_edges_added",
    "implicit_materialized_nodes_before", "implicit_materialized_edges_before",
    "implicit_materialized_nodes_after", "implicit_materialized_edges_after",
    "graph_rewrite_rewritten_regions", "graph_rewrite_general_regions",
    "graph_rewrite_general_nodes_removed", "graph_rewrite_general_edges_removed",
    "graph_rewrite_general_edges_added", "rewrite_nodes_removed",
    "rewrite_edges_removed", "rewrite_edges_added",
)


def stage_info(path: Path):
    data = json.loads(path.read_text(encoding="utf-8"))
    turns = data.get("turns") or []
    if not turns:
        return None
    prune = {}
    hybrid = None
    for stage in turns[0].get("stages") or []:
        info = stage.get("info") or {}
        if stage.get("name") == "PRUNING":
            prune = info
        if stage.get("name") == "FC_WMC_HYBRID" and (
                "rewrite_simple_regions" in info or
                "implicit_graph_general_regions" in info or
                "graph_rewrite_general_nodes_removed" in info):
            hybrid = info
    return (prune, hybrid) if hybrid is not None else None


def category(path: Path):
    parts = path.parts
    for name in ("sidechannel", "taint", "symbolization"):
        if name in parts:
            return name
    if "dis" in parts:
        return "symbolization"
    return "unknown"


def case_key(path: Path):
    cat = category(path)
    marker = "symbolization" if cat == "symbolization" else cat
    parts = path.parts
    if marker in parts:
        i = parts.index(marker)
        if cat == "taint" and i + 2 < len(parts) and parts[i + 1] == "runs":
            return cat, parts[i + 2]
        if i + 1 < len(parts):
            return cat, parts[i + 1]
    return cat, "unknown"


def values(prune, info):
    net_keys = ("rewrite_simple_regions", "rewrite_general_regions",
                "rewrite_simple_nodes_net_removed", "rewrite_general_nodes_net_removed",
                "rewrite_simple_edges_net_removed", "rewrite_general_edges_net_removed")
    if "rewrite_simple_regions" in info:
        if not all(key in info for key in net_keys) or not all(
                key in prune for key in ("after_prune_nodes", "after_prune_edges")):
            return None
        return dict(zip(("simple_regions", "general_regions", "simple_nodes_removed",
                         "general_nodes_removed", "simple_net_edges_removed", "general_net_edges_removed"),
                        (int(info[key]) for key in net_keys)),
                    initial_nodes=int(prune["after_prune_nodes"]),
                    initial_edges=int(prune["after_prune_edges"]))
    required = ("graph_rewrite_general_nodes_removed", "graph_rewrite_general_edges_removed",
                "graph_rewrite_general_edges_added") if "graph_rewrite_general_nodes_removed" in info else (
                    "implicit_materialized_nodes_after", "implicit_materialized_edges_after")
    if not all(key in info for key in required):
        return None
    initial_nodes = int(float(prune.get("after_prune_nodes", 0)))
    initial_edges = int(float(prune.get("after_prune_edges", 0)))
    if "graph_rewrite_general_nodes_removed" in info:
        get = lambda k: int(float(info.get(k, 0)))
        general = get("graph_rewrite_general_regions")
        rewritten = get("graph_rewrite_rewritten_regions")
        general_nodes = get("graph_rewrite_general_nodes_removed")
        general_edges = (get("graph_rewrite_general_edges_removed") -
                         get("graph_rewrite_general_edges_added"))
        total_edges = get("rewrite_edges_removed") - get("rewrite_edges_added")
        return {
            "initial_nodes": initial_nodes,
            "initial_edges": initial_edges,
            "simple_regions": max(0, rewritten - general),
            "general_regions": general,
            "simple_nodes_removed": max(0, get("rewrite_nodes_removed") - general_nodes),
            "general_nodes_removed": general_nodes,
            "simple_net_edges_removed": max(0, total_edges - general_edges),
            "general_net_edges_removed": general_edges,
        }
    v = {k: int(float(info.get(k, 0))) for k in KEYS}
    overlay = sum(v[k] for k in KEYS[:5])
    graph_all = v["implicit_graph_detected_regions"]
    general = v["implicit_graph_general_regions"]
    simple_regions = overlay + max(0, graph_all - general)
    materialized_nodes = v["implicit_materialized_nodes_before"]
    materialized_edges = v["implicit_materialized_edges_before"]
    final_nodes = v["implicit_materialized_nodes_after"]
    final_edges = v["implicit_materialized_edges_after"]
    # The materialized rewriter still applies specialized patterns before its
    # bounded general pass, so use the directly instrumented general deltas.
    # Everything else in the end-to-end reduction belongs to simple rewrites.
    general_nodes = v["implicit_general_nodes_removed"]
    general_net_edges = (v["implicit_general_edges_removed"] -
                         v["implicit_general_edges_added"])
    simple_nodes = max(0, initial_nodes - final_nodes - general_nodes)
    simple_net_edges = max(0, initial_edges - final_edges - general_net_edges)
    return {
        "initial_nodes": initial_nodes,
        "initial_edges": initial_edges,
        "simple_regions": simple_regions,
        "general_regions": general,
        "simple_nodes_removed": simple_nodes,
        "general_nodes_removed": general_nodes,
        "simple_net_edges_removed": simple_net_edges,
        "general_net_edges_removed": general_net_edges,
    }


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", type=Path, nargs="+")
    ap.add_argument("--output", type=Path)
    ap.add_argument("--exclude-stage", action="append", default=[],
                    help="Ignore profiles whose path contains this stage name.")
    args = ap.parse_args()
    totals = defaultdict(lambda: defaultdict(int))
    cases = defaultdict(lambda: defaultdict(int))
    files = 0
    missing_counters = 0
    # RQ1 uses one profile per run. The intended contribution experiment uses --runs 1.
    paths = []
    for root in args.roots:
        paths.extend(root.rglob("*.json"))
    for path in sorted(paths):
        if path.name == "run.meta.json":
            continue
        if any(stage in path.parts for stage in args.exclude_stage):
            continue
        try:
            info = stage_info(path)
        except (OSError, ValueError, TypeError):
            continue
        if info is None:
            continue
        cat = category(path)
        prune, hybrid = info
        row = values(prune, hybrid)
        if row is None:
            missing_counters += 1
            continue
        for k, value in row.items():
            totals[cat][k] += value
            totals["all"][k] += value
            cases[case_key(path)][k] += value
        totals[cat]["profiles"] += 1
        totals["all"]["profiles"] += 1
        files += 1

    if not files:
        raise SystemExit("No profiles contain the detailed rewrite contribution counters; "
                         "use FMCAD.py -RQ1 --rq1-collect for aggregate graph reductions.")
    print(f"profiles_missing_detailed_counters={missing_counters}")
    header = ["category", "profiles", "simple_regions", "general_regions",
              "simple_region_pct", "general_region_pct", "simple_nodes_removed",
              "general_nodes_removed", "simple_node_pct", "general_node_pct",
              "simple_net_edges_removed", "general_net_edges_removed",
              "simple_edge_pct", "general_edge_pct"]
    rows = []
    for cat in ("sidechannel", "taint", "symbolization", "all"):
        x = totals[cat]
        rr = x["simple_regions"] + x["general_regions"]
        nr = x["simple_nodes_removed"] + x["general_nodes_removed"]
        er = x["simple_net_edges_removed"] + x["general_net_edges_removed"]
        rows.append([cat, x["profiles"], x["simple_regions"], x["general_regions"],
                     f'{pct(x["simple_regions"], rr):.6f}', f'{pct(x["general_regions"], rr):.6f}',
                     x["simple_nodes_removed"], x["general_nodes_removed"],
                     f'{pct(x["simple_nodes_removed"], nr):.6f}', f'{pct(x["general_nodes_removed"], nr):.6f}',
                     x["simple_net_edges_removed"], x["general_net_edges_removed"],
                     f'{pct(x["simple_net_edges_removed"], er):.6f}',
                     f'{pct(x["general_net_edges_removed"], er):.6f}'])
    output = args.output or args.roots[0] / "rewrite_contribution.tsv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh, delimiter="\t")
        writer.writerow(header)
        writer.writerows(rows)
    print("\t".join(header))
    for row in rows:
        print("\t".join(map(str, row)))
    print(f"profiles={files} output={output}")
    print(f"cases={len(cases)}")
    for metric, simple_key, general_key in (
        ("regions", "simple_regions", "general_regions"),
        ("nodes", "simple_nodes_removed", "general_nodes_removed"),
        ("edges", "simple_net_edges_removed", "general_net_edges_removed"),
    ):
        simple_shares = []
        general_shares = []
        for x in cases.values():
            denom = x[simple_key] + x[general_key]
            if denom:
                simple_shares.append(x[simple_key] / denom)
                general_shares.append(x[general_key] / denom)
        if not simple_shares:
            continue
        am_s = sum(simple_shares) / len(simple_shares)
        am_g = sum(general_shares) / len(general_shares)
        gm_s = (float("nan") if any(v < 0 for v in simple_shares) else
                0.0 if any(v == 0 for v in simple_shares) else
                math.exp(sum(math.log(v) for v in simple_shares) / len(simple_shares)))
        gm_g = (float("nan") if any(v < 0 for v in general_shares) else
                0.0 if any(v == 0 for v in general_shares) else
                math.exp(sum(math.log(v) for v in general_shares) / len(general_shares)))
        print(f"case_average_{metric}\tarithmetic={100*am_s:.6f}/{100*am_g:.6f}"
              f"\tgeometric={100*gm_s:.6f}/{100*gm_g:.6f}"
              f"\tn={len(simple_shares)}"
              f"\tzeros={sum(v == 0 for v in simple_shares)}/{sum(v == 0 for v in general_shares)}")
    for metric, initial_key, simple_key, general_key in (
        ("nodes", "initial_nodes", "simple_nodes_removed", "general_nodes_removed"),
        ("edges", "initial_edges", "simple_net_edges_removed", "general_net_edges_removed"),
    ):
        simple_rates, general_rates = [], []
        for x in cases.values():
            before = x[initial_key]
            if before:
                simple_rates.append(x[simple_key] / before)
                general_rates.append(x[general_key] / before)
        if not simple_rates:
            continue
        am_s = sum(simple_rates) / len(simple_rates)
        am_g = sum(general_rates) / len(general_rates)
        print(f"reduction_rate_contribution_{metric}"
              f"\tarithmetic={100*am_s:.6f}/{100*am_g:.6f}"
              f"\ttotal={100*(am_s+am_g):.6f}\tn={len(simple_rates)}")


if __name__ == "__main__":
    main()
