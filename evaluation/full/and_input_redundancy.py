#!/usr/bin/env python3
"""Measure independently proven AND-input opportunities before/after rewrite.

This runner enables only the read-only detector. Counts are opportunities in
each snapshot, not mutations or a promise that all inputs can be removed at once.
"""

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time


REPORT_SCHEMA = "and-input-redundancy-v1"
STATS_KEYS = (
    "nodes", "edges", "input_associations", "eligible_definitions",
    "proven_input_associations", "affected_edges", "distinct_redundant_nodes",
    "recursive_nodes", "analysis_ms",
)
PHASES = ("before_rewrite", "after_rewrite")
DETECTOR_EFFECTS = {
    "deleted_input_associations": 0,
    "cleaned_nodes": 0,
    "cleaned_hyperedges": 0,
    "additional_siso_rewrites": 0,
}
PIPELINE_KEYS = (
    "before_prune_nodes", "before_prune_edges", "after_prune_nodes",
    "after_prune_edges", "rewrite_final_nodes", "rewrite_final_edges",
)


def read_report(path):
    if not path.is_file():
        return None, f"missing report: {path.name}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("schema") != REPORT_SCHEMA:
            raise ValueError("unexpected report schema")
        stats = data["stats"]
        if not isinstance(stats, dict) or any(key not in stats for key in STATS_KEYS):
            raise ValueError("incomplete report stats")
        proofs = data["proofs"]
        if not isinstance(proofs, list) or len(proofs) != stats["proven_input_associations"]:
            raise ValueError("proof count does not match report stats")
        redundant_relations = Counter(proof["redundant"]["tuple"]["rel"] for proof in proofs)
        target_relations = Counter(proof["head"]["tuple"]["rel"] for proof in proofs)
        return {"path": str(path), "stats": stats,
                "complete_derivations": data.get("complete_derivations"),
                "read_only": data.get("read_only"),
                "independent_certificates": data.get("independent_certificates"),
                "proven_by_redundant_relation": dict(sorted(redundant_relations.items())),
                "proven_by_target_relation": dict(sorted(target_relations.items()))}, None
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        return None, f"invalid report {path.name}: {exc}"


def number(value):
    try:
        parsed = float(value)
        if not math.isfinite(parsed):
            return None
        return int(parsed) if parsed.is_integer() else parsed
    except (ValueError, TypeError):
        return None


def read_debugger(output_dir):
    """Read current full-run counters, leaving unavailable metrics unset."""
    paths = sorted(output_dir.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    for path in paths:
        if path.name.startswith("and-redundancy-"):
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            turns = data.get("turns") or []
            if not turns:
                continue
            turn = turns[0]
            stages = turn.get("stages") or []
            if not isinstance(stages, list):
                continue
        except (OSError, ValueError, TypeError, AttributeError):
            continue
        result = {"path": str(path), "status": data.get("status"), "pipeline": {},
                  "detector_timings": {}}
        # Between stages, Debugger::addInfo stores counters on the turn.
        # Before-rewrite detection runs immediately after PRUNING ends.
        scopes = [{"info": turn.get("info") or {}}, *stages]
        for scope in scopes:
            if not isinstance(scope, dict):
                continue
            name = scope.get("name")
            info = scope.get("info") or {}
            if not isinstance(info, dict):
                continue
            for key in PIPELINE_KEYS:
                if key in info:
                    result["pipeline"][key] = number(info[key])
            for phase in PHASES:
                for metric in ("analysis_ms", "write_ms"):
                    key = f"and_input_redundancy_{phase}_{metric}"
                    if key in info:
                        result["detector_timings"][f"{phase}_{metric}"] = number(info[key])
            if name in ("FC_WMC_HYBRID", "FORWARD_COMPILATION") and "live_nodes" in info:
                # With rewrite, FullPipeline sums Cudd_ReadNodeCount immediately
                # after formula compilation of each slow component. total_nodes
                # instead describes the final manager and is not this sum.
                result["bdd_live_nodes"] = number(info["live_nodes"])
                result["bdd_live_nodes_source"] = f"{name}.info.live_nodes"
                result["bdd_stage_status"] = scope.get("status")
        return result
    return {}


def run_case(binary, case_dir, output_root, timeout):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
    run_dir = output_root / case_dir.name / f"run_{stamp}"
    output_dir = run_dir / "output"
    output_dir.mkdir(parents=True)
    cmd = [str(binary), "-F", str((case_dir / "input").resolve()),
           "-D", str(output_dir), "--rewrite", "--dump=and-redundancy",
           "-j1", "--det-opt", "--profile-stage=fc,wmc"]
    record = {"case": case_dir.name, "cmd": cmd, "run_dir": str(run_dir),
              "status": "ok", "notes": []}
    started = time.perf_counter()
    with (run_dir / "run.log").open("w", encoding="utf-8") as log:
        try:
            # subprocess.run kills and waits for the generated executable on
            # timeout; its report files remain available for partial collection.
            proc = subprocess.run(cmd, cwd=run_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout, check=False)
            record["exit_code"] = proc.returncode
            if proc.returncode != 0:
                record["status"] = "error"
        except subprocess.TimeoutExpired:
            record.update(status="timeout", exit_code=124)
            record["notes"].append(f"run exceeded {timeout} seconds")
        except OSError as exc:
            record.update(status="error", exit_code=127)
            record["notes"].append(str(exc))
    # Includes detection, report writes, inference, output and process shutdown.
    record["wall_seconds"] = time.perf_counter() - started
    for phase in PHASES:
        filename = f"and-redundancy-{phase.replace('_', '-')}.json"
        report, error = read_report(output_dir / filename)
        record[phase] = report
        if error:
            record["notes"].append(error)
            if record["status"] == "ok":
                record["status"] = "incomplete"
        if report is not None and report["complete_derivations"] is False:
            record["notes"].append(f"incomplete derivation sources: {phase}")
            if record["status"] == "ok":
                record["status"] = "incomplete"
    record["debugger"] = read_debugger(output_dir)
    timings = record["debugger"].get("detector_timings", {})
    for phase in PHASES:
        report = record[phase]
        if report is not None:
            report["write_ms"] = timings.get(f"{phase}_write_ms")
    (run_dir / "run.meta.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def write_summary(output_root, binary, cases_root, timeout, records):
    summary = {
        "schema": "and-input-redundancy-audit-v1", "binary": str(binary),
        "cases_root": str(cases_root), "timeout_seconds": timeout,
        "detector_mutates_graph": False,
        "detector_effects": DETECTOR_EFFECTS,
        "proof_scope": "Each input association is proven independently in its snapshot.",
        "bdd_live_nodes_semantics": "Sum of Cudd_ReadNodeCount after compiling each slow component.",
        "records": records,
    }
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    columns = ["case", "status", "exit_code", "wall_seconds", *DETECTOR_EFFECTS]
    columns += [f"{phase}_{key}" for phase in PHASES
                for key in (*STATS_KEYS, "write_ms", "complete_derivations",
                            "conflict_input_associations")]
    columns += [*PIPELINE_KEYS, "bdd_live_nodes", "bdd_stage_status", "bdd_live_nodes_source",
                "run_dir", "notes"]
    with (output_root / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for record in records:
            row = {key: record.get(key) for key in ("case", "status", "exit_code", "wall_seconds", "run_dir")}
            row.update(DETECTOR_EFFECTS)
            for phase in PHASES:
                report = record[phase] or {}
                for key in STATS_KEYS:
                    row[f"{phase}_{key}"] = report.get("stats", {}).get(key)
                row[f"{phase}_write_ms"] = report.get("write_ms")
                row[f"{phase}_complete_derivations"] = report.get("complete_derivations")
                if report:
                    row[f"{phase}_conflict_input_associations"] = report[
                        "proven_by_redundant_relation"].get("data_object_conflict", 0)
            debugger = record["debugger"]
            row.update(debugger.get("pipeline", {}))
            for key in ("bdd_live_nodes", "bdd_stage_status", "bdd_live_nodes_source"):
                row[key] = debugger.get(key)
            row["notes"] = "; ".join(record["notes"])
            writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Newly compiled full-only Symbolization executable.")
    parser.add_argument("--cases-root", type=Path, default=Path(__file__).resolve().parent / "symbolization")
    parser.add_argument("--output-root", type=Path, required=True,
                        help="Generated audit outputs; use an ignored directory under build/.")
    parser.add_argument("--timeout", type=float, default=300, help="Wall-clock timeout per case, seconds.")
    parser.add_argument("--cases", nargs="+", help="Optional comma/space-separated case names.")
    args = parser.parse_args()
    binary = args.binary.resolve()
    cases_root = args.cases_root.resolve()
    output_root = args.output_root.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"binary is not executable: {binary}")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if not cases_root.is_dir():
        parser.error(f"cases root does not exist: {cases_root}")
    cases = sorted(p for p in cases_root.iterdir() if p.is_dir() and (p / "input").is_dir())
    if args.cases:
        selected = {name for value in args.cases for name in re.split(r"[,\s]+", value) if name}
        missing = selected - {p.name for p in cases}
        if missing:
            parser.error(f"unknown cases: {', '.join(sorted(missing))}")
        cases = [p for p in cases if p.name in selected]
    if not cases:
        parser.error("no case input directories found")
    output_root.mkdir(parents=True, exist_ok=True)
    records = []
    for case_dir in cases:
        record = run_case(binary, case_dir, output_root, args.timeout)
        records.append(record)
        write_summary(output_root, binary, cases_root, args.timeout, records)
        counts = [str((record[phase] or {}).get("stats", {}).get("proven_input_associations", "?"))
                  for phase in PHASES]
        print(f"{case_dir.name}: {record['status']} before={counts[0]} after={counts[1]} "
              f"wall_seconds={record['wall_seconds']:.3f}", flush=True)
    print(f"Wrote {output_root / 'summary.json'} and {output_root / 'summary.csv'}")
    return int(any(record["status"] != "ok" for record in records))


if __name__ == "__main__":
    raise SystemExit(main())
