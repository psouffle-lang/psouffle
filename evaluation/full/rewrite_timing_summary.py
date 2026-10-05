#!/usr/bin/env python3
import json
from pathlib import Path

roots = [
    (Path("artifact/runs/rq1_rewrite_contribution_full"), {"sidechannel", "taint"}),
    (Path("artifact/runs/rq1_rewrite_contribution_dis_exact"), {"symbolization"}),
]

records = []
for root, wanted in roots:
    for path in root.rglob("*.json"):
        if path.name == "run.meta.json":
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            turn = (data.get("turns") or [])[0]
        except (OSError, ValueError, TypeError, IndexError):
            continue
        category = next((x for x in ("sidechannel", "taint", "symbolization")
                         if x in path.parts), None)
        if category not in wanted:
            continue
        info = None
        for stage in turn.get("stages") or []:
            if stage.get("name") == "FC_WMC_HYBRID":
                info = stage.get("info") or {}
                break
        if info is None:
            continue
        prefix = "implicit_graph_" if "implicit_graph_bdd_compile_ms" in info else "graph_rewrite_"
        def value(suffix):
            try:
                return float(info.get(prefix + suffix, 0))
            except (TypeError, ValueError):
                return 0.0
        total_ms = 1000.0 * float(turn.get("time_seconds", 0))
        bdd_compile = value("bdd_compile_ms")
        bdd_init = value("bdd_manager_init_ms")
        bdd_wmc = value("bdd_wmc_ms")
        fast_general = value("fast_general_ms")
        apply_ms = value("apply_ms")
        solve_region = bdd_init + bdd_compile + bdd_wmc + fast_general + apply_ms
        try:
            rewrite_ms = float(info.get(
                "implicit_total_ms" if prefix == "implicit_graph_" else "graph_rewrite_total_ms", 0))
        except (TypeError, ValueError):
            rewrite_ms = 0.0
        general = value("general_regions")
        fast_regions = value("fast_general_regions")
        records.append((category, total_ms, bdd_compile, solve_region, general, fast_regions, rewrite_ms))

def report(label, rows):
    total = sum(x[1] for x in rows)
    compile_ms = sum(x[2] for x in rows)
    solve_ms = sum(x[3] for x in rows)
    general = sum(x[4] for x in rows)
    fast_regions = sum(x[5] for x in rows)
    rewrite_ms = sum(x[6] for x in rows)
    compile_avg = sum((x[2] / x[1]) for x in rows if x[1]) / len(rows)
    solve_avg = sum((x[3] / x[1]) for x in rows if x[1]) / len(rows)
    print(f"{label}\tprofiles={len(rows)}"
          f"\tbdd_compile_weighted_pct={100*compile_ms/total:.6f}"
          f"\tbdd_compile_arithmetic_pct={100*compile_avg:.6f}"
          f"\tsolve_region_weighted_pct={100*solve_ms/total:.6f}"
          f"\tsolve_region_arithmetic_pct={100*solve_avg:.6f}"
          f"\tgeneral_regions={general:.0f}"
          f"\tfast_general_regions={fast_regions:.0f}"
          f"\tbdd_fallback_regions={general-fast_regions:.0f}")
    print(f"{label}_within_rewrite"
          f"\tbdd_compile_pct={100*compile_ms/rewrite_ms if rewrite_ms else 0:.6f}"
          f"\tsolve_region_pct={100*solve_ms/rewrite_ms if rewrite_ms else 0:.6f}")

for cat in ("sidechannel", "taint", "symbolization"):
    report(cat, [x for x in records if x[0] == cat])
report("all", records)
