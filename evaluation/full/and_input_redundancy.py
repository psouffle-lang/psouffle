#!/usr/bin/env python3
"""Audit AND-input opportunities or benchmark the opt-in elimination pass.

Audit mode records independent certificates without mutation. Benchmark mode
compares plain inference, SISO, and the pass before SISO without proof dumps.
"""

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import subprocess
import threading
import statistics
import time


REPORT_SCHEMA = "and-input-redundancy-v1"
TIMING_METHOD = "blocking_wait_with_watchdog"
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
    "after_prune_edges", "rewrite_initial_nodes", "rewrite_initial_edges",
    "rewrite_final_nodes", "rewrite_final_edges",
)
PASS_KEYS = (
    "deleted_input_associations", "affected_edges", "rounds", "cleaned_nodes", "cleaned_hyperedges",
    "remaining_input_associations", "remaining_proven_input_associations",
    "initial_input_associations", "final_input_associations", "before_nodes",
    "before_edges", "after_nodes", "after_edges", "detection_ms", "mutation_ms",
    "initialization_ms", "workspace_summary_ms", "cleanup_planning_ms", "cleanup_strategy", "analysis_strategy", "placement", "pruning_ms", "total_ms",
)
REWRITE_KEYS = ("rewrite_simple_regions", "rewrite_general_regions", "graph_rewrite_rewritten_regions",
                "rewrite_ms", "graph_rewrite_total_ms", "implicit_total_ms")
ALIAS_KEYS = ("candidates", "merged_aliases", "query_aliases", "promoted_output_roots",
              "shared_prune_indexes",
              "proven_true_nodes", "proven_derived_true_nodes", "deterministic_proof_edges",
              "input_replacements", "duplicate_inputs_removed", "removed_nodes", "removed_edges",
              "active_input_replacements", "active_duplicate_inputs_removed",
              "removed_owner_edges", "skipped_cycle_aliases", "evidence_conflict_classes",
              "analysis_ms", "mutation_ms", "summary_ms", "total_ms",
              "before_nodes", "before_edges", "before_input_associations",
              "after_nodes", "after_edges", "after_input_associations", "before_output_nodes", "after_output_nodes")
VARIANTS = {
    "baseline": [], "siso": ["--rewrite"],
    "siso_and_pass": ["--rewrite", "--and-input-redundancy"],
    "pass": ["--and-input-redundancy"],
    "siso_then_pass": ["--rewrite", "--and-input-redundancy",
                       "--and-input-redundancy-placement=after-siso"],
    "alias": ["--deterministic-event-aliases"],
    "alias_and_pass": ["--deterministic-event-aliases", "--and-input-redundancy"],
    "alias_and_siso": ["--deterministic-event-aliases", "--rewrite"],
    "alias_pass_siso": ["--deterministic-event-aliases", "--and-input-redundancy", "--rewrite"],
    "alias_siso_pass": ["--deterministic-event-aliases", "--rewrite", "--and-input-redundancy",
                        "--and-input-redundancy-placement=after-siso"],
}


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
                  "detector_timings": {}, "pass": {}, "aliases": {}, "rewrite": {}}
        peaks = [number(stage.get("peak_mem_kb")) for stage in stages if isinstance(stage, dict)]
        result["peak_mem_kb"] = max((peak for peak in peaks if peak is not None), default=None)
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
            for key in PASS_KEYS:
                field = f"and_input_redundancy_{key}"
                if field in info:
                    result["pass"][key] = (info[field]
                        if key in {"cleanup_strategy", "analysis_strategy", "placement"}
                        else number(info[field]))
            for key in REWRITE_KEYS:
                if key in info:
                    result["rewrite"][key] = number(info[key])
            for key in ALIAS_KEYS:
                field = f"deterministic_event_aliases_{key}"
                if field in info:
                    result["aliases"][key] = number(info[field])
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


def read_probabilities(path):
    values = {}
    with path.open(encoding="utf-8") as stream:
        for index, line in enumerate(stream, 1):
            if not line.strip():
                continue
            key, separator, text = line.rpartition(":")
            key = key.strip()
            try:
                value = Decimal(text.strip())
            except InvalidOperation as exc:
                raise ValueError(f"invalid probability at {path}:{index}") from exc
            if not separator or not key or not value.is_finite() or key in values:
                raise ValueError(f"invalid or duplicate probability at {path}:{index}")
            values[key] = value
    return values


def compare_probabilities(reference, current):
    missing = sorted(reference.keys() - current.keys())
    extra = sorted(current.keys() - reference.keys())
    # Compare the printed decimal literals exactly. Binary float subtraction can
    # turn a true 1e-8 difference into 1.000000005e-8 at the tolerance boundary.
    deltas = {key: abs(Decimal(str(reference[key])) - Decimal(str(current[key])))
              for key in reference.keys() & current.keys()}
    changed = sorted(key for key, delta in deltas.items() if delta > Decimal("1e-8"))
    return {"status": "match" if not (missing or extra or changed) else "mismatch",
            "tolerance": 1e-8, "reference_query_count": len(reference),
            "query_count": len(current), "missing_keys": len(missing),
            "extra_keys": len(extra), "changed_values": len(changed),
            "max_absolute_difference": float(max(deltas.values(), default=Decimal(0))),
            "missing_key_examples": missing[:8], "extra_key_examples": extra[:8],
            "changed_value_examples": changed[:8]}


def wait_with_timeout(proc, timeout):
    """Block until exit; use a watchdog instead of timeout wait's 50 ms polling."""
    timed_out = threading.Event()

    def expire():
        if proc.poll() is None:
            timed_out.set()
            proc.kill()

    watchdog = threading.Timer(timeout, expire)
    watchdog.daemon = True
    try:
        watchdog.start()
        return_code = proc.wait()
    finally:
        watchdog.cancel()
        if watchdog.ident is not None:
            watchdog.join()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    if timed_out.is_set():
        raise subprocess.TimeoutExpired(proc.args, timeout)
    return return_code


def run_benchmark_case(binary, case_dir, output_root, timeout, variant, repeat, mem_limit_mb=0):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
    run_dir = output_root / case_dir.name / variant / f"run_{repeat:02d}_{stamp}"
    output_dir = run_dir / "output"
    output_dir.mkdir(parents=True)
    # Ordinary debugger counters remain available without verbose FC/WMC
    # profiles. Timed runs exclude the audit's large certificate JSON files.
    cmd = [str(binary), "-F", str((case_dir / "input").resolve()), "-D", str(output_dir),
           "-j1", "--det-opt", *VARIANTS[variant]]
    inherited_soft, inherited_hard = resource.getrlimit(resource.RLIMIT_AS)
    limit_bytes = mem_limit_mb * 1024 * 1024 if mem_limit_mb else inherited_soft
    if mem_limit_mb and inherited_hard != resource.RLIM_INFINITY:
        limit_bytes = min(limit_bytes, inherited_hard)
    record = {"case": case_dir.name, "variant": variant, "repeat": repeat,
              "cmd": cmd, "run_dir": str(run_dir), "status": "ok", "notes": [],
              "timing_method": TIMING_METHOD,
              "timeout_seconds": timeout, "requested_mem_limit_mb": mem_limit_mb,
              "mem_limit_mb": limit_bytes / (1024 * 1024) if mem_limit_mb else 0,
              "effective_address_space_limit_bytes": None if limit_bytes == resource.RLIM_INFINITY else limit_bytes}
    def limit_memory():
        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))
    start = time.perf_counter()
    with (run_dir / "run.log").open("w", encoding="utf-8") as log:
        try:
            # Start the watchdog after Popen, keeping fork/preexec single threaded.
            with subprocess.Popen(cmd, cwd=run_dir, stdout=log, stderr=subprocess.STDOUT,
                                  preexec_fn=limit_memory if mem_limit_mb else None) as proc:
                return_code = wait_with_timeout(proc, timeout)
            record["exit_code"] = return_code
            if return_code != 0:
                record["status"] = "error"
        except subprocess.TimeoutExpired:
            record.update(status="timeout", exit_code=124)
            record["notes"].append(f"run exceeded {timeout} seconds")
        except (OSError, subprocess.SubprocessError) as exc:
            record.update(status="error", exit_code=127)
            record["notes"].append(str(exc))
    record["wall_seconds"] = time.perf_counter() - start
    record["debugger"] = read_debugger(output_dir)
    if record["status"] == "error" and mem_limit_mb:
        with (run_dir / "run.log").open(encoding="utf-8", errors="replace") as stream:
            stream.seek(max(0, (run_dir / "run.log").stat().st_size - 65536))
            tail = stream.read().lower()
        if any(marker in tail for marker in ("bad_alloc", "cannot allocate memory", "out of memory")):
            record["status"] = "memory_limit"
            record["notes"].append(f"Allocation failed under the {mem_limit_mb} MiB address-space cap.")
    record["probabilities_path"] = str(output_dir / "facts.prob")
    debugger = record["debugger"]
    pipeline = debugger.get("pipeline", {})
    pass_stats = debugger.get("pass", {})
    for metric in ("nodes", "edges"):
        record[f"final_{metric}"] = (pass_stats.get(f"after_{metric}")
                if pass_stats.get("placement") == "after-siso" else pipeline.get(f"rewrite_final_{metric}"))
        if record[f"final_{metric}"] is None:
            record[f"final_{metric}"] = pass_stats.get(f"after_{metric}",
                    debugger.get("aliases", {}).get(f"after_{metric}", pipeline.get(f"after_prune_{metric}")))
    rewrite = debugger.get("rewrite", {})
    simple, general = rewrite.get("rewrite_simple_regions"), rewrite.get("rewrite_general_regions")
    record["completed_siso_rewrites"] = (
        simple + general if simple is not None and general is not None
        else rewrite.get("graph_rewrite_rewritten_regions"))
    record["siso_ms"] = rewrite.get("rewrite_ms", rewrite.get("graph_rewrite_total_ms"))
    if record["status"] == "ok" and "--and-input-redundancy" in VARIANTS[variant]:
        if "deleted_input_associations" not in pass_stats:
            record["status"] = "incomplete_metrics"
            record["notes"].append("Enabled pass did not emit its deletion counters.")
    if record["status"] == "ok" and "--deterministic-event-aliases" in VARIANTS[variant]:
        if "merged_aliases" not in debugger.get("aliases", {}):
            record["status"] = "incomplete_metrics"
            record["notes"].append("Enabled event alias pass did not emit its merge counters.")
    return record


def latest_records(records):
    return list({(r["case"], r["variant"], r["repeat"]): r for r in records}.values())


def benchmark_summary(output_root, binary, cases_root, timeout, runs, records, binary_hash, mem_limit_mb=0):
    active = latest_records(records)
    groups = []
    for case, variant in sorted({(r["case"], r["variant"]) for r in active}):
        cell = [r for r in active if r["case"] == case and r["variant"] == variant]
        successful = [r for r in cell if r["status"] == "ok"]
        wall = [r["wall_seconds"] for r in successful]
        group = {"case": case, "variant": variant, "successful_runs": len(successful),
                 "requested_runs": runs, "status_counts": dict(Counter(r["status"] for r in cell)),
                 "wall_seconds_median": statistics.median(wall) if wall else None,
                 "wall_seconds_min": min(wall) if wall else None,
                 "wall_seconds_max": max(wall) if wall else None}
        for key in ("completed_siso_rewrites", "final_nodes", "final_edges"):
            values = [r[key] for r in successful if r.get(key) is not None]
            group[key + "_median"] = statistics.median(values) if values else None
        groups.append(group)
    comparisons = []
    for case in sorted({r["case"] for r in active}):
        cells = {g["variant"]: g for g in groups if g["case"] == case}
        comparison = {"case": case}
        for variant in VARIANTS:
            for reference in ("baseline", "siso", "siso_and_pass", "alias"):
                if variant == reference:
                    continue
                left, right = cells.get(reference), cells.get(variant)
                if left and right and left["successful_runs"] == runs and right["successful_runs"] == runs:
                    comparison[f"{variant}_speedup_vs_{reference}"] = (
                        left["wall_seconds_median"] / right["wall_seconds_median"])
        left, right = cells.get("siso"), cells.get("siso_and_pass")
        if left and right and left["successful_runs"] == runs and right["successful_runs"] == runs:
            a, b = left["completed_siso_rewrites_median"], right["completed_siso_rewrites_median"]
            if a is not None and b is not None:
                comparison["additional_siso_rewrites_delta"] = b - a
        comparisons.append(comparison)
    summary = {"schema": "and-input-redundancy-benchmark-v1", "binary": str(binary),
               "timing_method": TIMING_METHOD,
               "binary_sha256": binary_hash, "cases_root": str(cases_root),
               "timeout_seconds": timeout, "runs": runs, "variant_flags": VARIANTS,
               "mem_limit_mb": mem_limit_mb,
               "memory_limit_scope": "RLIMIT_AS per child; each record preserves its configured cap (0 means unlimited).",
               "resume_semantics": "Latest attempt per case/variant/repeat enters comparisons; earlier failures remain in records as superseded. Changed timeout or memory cap retries failed cells.",
               "proof_dumps_enabled": False, "verbose_profiles_enabled": False,
               "pass_placement": "Selected per variant; before-siso by default, siso_then_pass after-siso.",
               "variant_pass_placements": {"pass": "before-siso", "siso_and_pass": "before-siso",
                                           "siso_then_pass": "after-siso"},
               "remaining_opportunities_scope": "Core deletion fixpoint at the selected placement; a later SISO rewrite may expose more opportunities.",
               "additional_siso_rewrites_delta_semantics": "Signed difference in completed SISO region counts, not identities of newly triggered regions.",
               "bdd_live_nodes_semantics": {"FC_WMC_HYBRID": "Sum after each slow component's formula compilation.",
                                            "FORWARD_COMPILATION": "Whole-graph manager count after formula compilation."},
               "timing_scope": "Child wall time includes detection, mutation, pruning, SISO, inference and ordinary outputs; excludes compilation and Python collection.",
               "records": records, "groups": groups, "comparisons": comparisons}
    summary_tmp = output_root / "summary.json.tmp"
    summary_tmp.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    summary_tmp.replace(output_root / "summary.json")
    columns = ["case", "variant", "repeat", "status", "superseded", "exit_code", "timing_method", "wall_seconds",
               "timeout_seconds", "requested_mem_limit_mb", "mem_limit_mb", "effective_address_space_limit_bytes",
               "final_nodes", "final_edges", "completed_siso_rewrites", "siso_ms", *PIPELINE_KEYS]
    columns += [f"and_input_redundancy_{key}" for key in PASS_KEYS]
    columns += [f"deterministic_event_aliases_{key}" for key in ALIAS_KEYS]
    columns += ["bdd_live_nodes", "bdd_stage_status", "peak_mem_kb", "probability_check", "probability_reference_variant",
                "query_count", "max_absolute_difference", "run_dir", "notes"]
    csv_tmp = output_root / "summary.csv.tmp"
    with csv_tmp.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for record in records:
            row = {key: record.get(key) for key in columns if key in record}
            debugger = record.get("debugger", {})
            row.update(debugger.get("pipeline", {}))
            row.update({f"and_input_redundancy_{key}": value for key, value in debugger.get("pass", {}).items()})
            row.update({f"deterministic_event_aliases_{key}": value
                        for key, value in debugger.get("aliases", {}).items()})
            for key in ("bdd_live_nodes", "bdd_stage_status", "peak_mem_kb"):
                row[key] = debugger.get(key)
            check = record.get("probability_check", {})
            row["probability_check"] = check.get("status")
            row["probability_reference_variant"] = check.get("reference_variant")
            for key in ("query_count", "max_absolute_difference"):
                row[key] = check.get(key)
            row["notes"] = "; ".join(record["notes"])
            writer.writerow(row)
    csv_tmp.replace(output_root / "summary.csv")


def run_benchmark(binary, cases_root, cases, output_root, args, variants):
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    records = []
    summary_path = output_root / "summary.json"
    if args.resume and summary_path.is_file():
        existing = json.loads(summary_path.read_text(encoding="utf-8"))
        if (existing.get("schema") != "and-input-redundancy-benchmark-v1"
                or existing.get("binary_sha256") != binary_hash
                or existing.get("cases_root") != str(cases_root)
                or existing.get("timing_method") != TIMING_METHOD):
            raise SystemExit("Resume requires the same binary, cases root and timing method; use a new output root.")
        records = existing["records"]
    mem_limit_mb = getattr(args, "mem_limit_mb", 0)
    def retryable(record):
        return record["status"] != "ok" and (
            record.get("timeout_seconds") != args.timeout
            or record.get("requested_mem_limit_mb", record.get("mem_limit_mb", 0)) != mem_limit_mb)
    completed = {(r["case"], r["variant"], r["repeat"]) for r in latest_records(records) if not retryable(r)}
    for case_dir in cases:
        previous = [r for r in latest_records(records) if r["case"] == case_dir.name]
        successful = sorted((r for r in previous if r["status"] == "ok"),
                            key=lambda r: list(VARIANTS).index(r["variant"]))
        reference = read_probabilities(Path(successful[0]["probabilities_path"])) if successful else None
        reference_record = successful[0] if successful else None
        failed = {r["variant"] for r in previous
                  if r["status"] not in ("ok", "skipped_after_failure") and not retryable(r)}
        for repeat in range(1, args.runs + 1):
            # Rotate order to spread startup and machine-load effects across variants.
            offset = (repeat - 1) % len(variants)
            for variant in variants[offset:] + variants[:offset]:
                if (case_dir.name, variant, repeat) in completed:
                    continue
                if variant in failed:
                    record = {"case": case_dir.name, "variant": variant, "repeat": repeat,
                              "status": "skipped_after_failure", "wall_seconds": None,
                              "timeout_seconds": args.timeout, "requested_mem_limit_mb": mem_limit_mb,
                              "notes": ["Earlier repeat of this variant failed; no repeated timeout was attempted."]}
                else:
                    record = run_benchmark_case(binary, case_dir, output_root, args.timeout, variant, repeat, mem_limit_mb)
                    if record["status"] == "ok":
                        try:
                            current = read_probabilities(Path(record["probabilities_path"]))
                            if reference is None:
                                reference, reference_record = current, record
                                check = {"status": "reference", "query_count": len(current), "tolerance": 1e-8}
                            else:
                                check = compare_probabilities(reference, current)
                                if check["status"] != "match":
                                    record["status"] = "probability_mismatch"
                            check["reference_variant"] = reference_record["variant"]
                            check["reference_path"] = reference_record["probabilities_path"]
                            record["probability_check"] = check
                        except (OSError, ValueError) as exc:
                            record["status"] = "invalid_probability_output"
                            record["notes"].append(str(exc))
                    if record["status"] != "ok":
                        failed.add(variant)
                    (Path(record["run_dir"]) / "run.meta.json").write_text(
                        json.dumps(record, indent=2) + "\n", encoding="utf-8")
                for old in records:
                    if (old["case"], old["variant"], old["repeat"]) == (case_dir.name, variant, repeat):
                        old["superseded"] = True
                records.append(record)
                benchmark_summary(output_root, binary, cases_root, args.timeout, args.runs, records, binary_hash, mem_limit_mb)
                elapsed = record["wall_seconds"]
                print(f"{case_dir.name} {variant} repeat={repeat}: {record['status']} "
                      f"wall_seconds={elapsed:.3f}" if elapsed is not None
                      else f"{case_dir.name} {variant} repeat={repeat}: {record['status']}", flush=True)
    return int(any(record["status"] != "ok" for record in latest_records(records)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Newly compiled full-only Symbolization executable.")
    parser.add_argument("--cases-root", type=Path, default=Path(__file__).resolve().parent / "symbolization")
    parser.add_argument("--output-root", type=Path, required=True,
                        help="Generated run outputs; use an ignored directory under build/.")
    parser.add_argument("--timeout", type=float, default=300, help="Wall-clock timeout per case, seconds.")
    parser.add_argument("--cases", nargs="+", help="Optional comma/space-separated case names.")
    parser.add_argument("--mode", choices=("audit", "benchmark"), default="audit")
    parser.add_argument("--runs", type=int, default=3, help="Repeats per variant in benchmark mode (default: 3).")
    parser.add_argument("--variants", nargs="+", help="Benchmark variants: " + ",".join(VARIANTS))
    parser.add_argument("--resume", action="store_true", help="Resume or append benchmark cells with the same binary.")
    parser.add_argument("--mem-limit-mb", type=int, default=0,
                        help="Benchmark child address-space cap in MiB; 0 disables it.")
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
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.mem_limit_mb < 0:
        parser.error("--mem-limit-mb must be nonnegative")
    if args.mode == "audit" and (args.variants or args.resume):
        parser.error("--variants and --resume require --mode benchmark")
    output_root.mkdir(parents=True, exist_ok=True)
    if args.mode == "benchmark":
        variants = list(dict.fromkeys(name for value in args.variants for name in re.split(r"[,\s]+", value) if name)) if args.variants else ["baseline", "siso", "siso_and_pass"]
        if not variants or set(variants) - VARIANTS.keys():
            parser.error("unknown or empty benchmark variants")
        result = run_benchmark(binary, cases_root, cases, output_root, args, variants)
        print(f"Wrote {output_root / 'summary.json'} and {output_root / 'summary.csv'}")
        return result
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
