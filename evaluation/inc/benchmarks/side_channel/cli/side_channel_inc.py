#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Incremental Side Channel Benchmark Toolkit

Subcommands:
  - delta    : create delta workloads under <base-dir>/P*/delta/
  - compile  : compile Soufflé with incremental CLI support
  - run      : run Soufflé baseline + incremental CLI using sampled delta files
  - collect  : aggregate incremental-vs-full comparison results
  - clean    : remove generated artifacts under the base directory

This script focuses on Soufflé-only experiments where incremental CLI updates
are compared against full recomputation for the same delta workload.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import random
import shutil
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

SIDE_CHANNEL_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SIDE_CHANNEL_ROOT.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
from benchmarks.side_channel.core.side_channel_common import (
    Logger,
    case_dir,
    case_name,
    compare_prob_maps,
    ensure_dirs,
    parse_cases_spec,
    read_souffle_prob_file,
    time_cmd,
)

DEFAULT_BASE_DIR = SIDE_CHANNEL_ROOT / "runs" / "side_channel_inc"
DEFAULT_CASE_TEMPLATE_DIR = SIDE_CHANNEL_ROOT / "cases"
DEFAULT_REPO_SOUFFLE_BIN = REPO_ROOT.parents[1] / "build" / "src" / "souffle"
DEFAULT_CHANGE_SPEC = "inc0p5=0.005,inc1p0=0.01,inc1p5=0.015"
DEFAULT_CHANGE_CAP = "inc0p5=150,inc1p0=300,inc1p5=450"
DEFAULT_CHANGE_CAP_TOL = 0.10
DEFAULT_GLOBAL_SEED = 42
DEFAULT_MIX_TOTAL_RATIO = 0.01
DEFAULT_MIX_CAP = 300
DEFAULT_MIX_POOL_RATIO = 0.02
DEFAULT_MIX_POOL_CAP_MULT = 2.0
DEFAULT_MIX_DISTRIBUTIONS = "0-1,0.25-0.75,0.5-0.5,0.75-0.25,1-0"
DEFAULT_DELTA_SAMPLES = 5
DEFAULT_DELTA_RUNS = 5
DEFAULT_CASES = list(range(13, 21))
DEFAULT_CLUSTER_ATTEMPTS = 100
DEFAULT_CLUSTER_MAX = 500
DEFAULT_TRANSITIVE_DEPTH = 2
DEFAULT_STAGED_COMPARE_TOL = 1e-5
RUN_MODE_ORDER = (
    "full",
    "inc-naive",
    "inc-regional",
    "inc-full",
    "full-inc-naive",
    "full-inc-regional",
)
ASSIGN_LIKE_RELATIONS = {"binary_constant"}
ASSIGN_REL_EXCLUDE = {"assign", "equal_assign"}
INSERT_POOL_MANIFEST_NAME = "delta_insert_pool.jsonl"


def case_seed_value(case_id: Any) -> int:
    name = case_name(case_id)
    match = re.fullmatch(r"([A-Za-z]+)(\d+)", name)
    if match:
        prefix = sum((idx + 1) * ord(ch) for idx, ch in enumerate(match.group(1)))
        return prefix * 1000 + int(match.group(2))
    return int(case_id)


@dataclass
class DeltaFile:
    label: str
    sample: str
    path: Path


def _parse_delta_name(path: Path) -> Tuple[str, str]:
    stem = path.stem
    if "_" in stem:
        label, sample = stem.split("_", 1)
    else:
        label, sample = stem, "1"
    label = label.strip()
    sample = sample.strip()
    if not label:
        label = "delta"
    if not sample:
        sample = "1"
    return label, sample


def safe_relpath(path: Path, base: Path) -> str:
    try:
        return str(path.relative_to(base))
    except ValueError:
        return str(path)


def select_delta_files(
        case_path: Path,
        labels: Iterable[str],
        samples_per_label: int,
        shuffle: bool,
        seed: Optional[int],
        override_root: Optional[Path] = None,
        logger: Optional[Logger] = None,
) -> List[DeltaFile]:
    local_delta = case_path / "delta"
    src_delta = override_root / case_path.name / "delta" if override_root else None
    labels_list = list(labels)

    def has_txt(path: Path) -> bool:
        return path.exists() and any(path.glob("*.txt"))

    if not has_txt(local_delta) and src_delta and src_delta.exists():
        ensure_dirs(local_delta)
        shutil.copytree(src_delta, local_delta, dirs_exist_ok=True)
        if logger:
            logger.info(
                f"[{case_path.name}] Copied delta files into {local_delta.relative_to(case_path)} "
                f"from {safe_relpath(src_delta, case_path.parent)}"
            )

    if has_txt(local_delta):
        delta_dir = local_delta
    elif src_delta and src_delta.exists():
        delta_dir = src_delta
    else:
        return []

    rng = random.Random(seed if seed is not None else DEFAULT_GLOBAL_SEED)
    selected: List[DeltaFile] = []
    if logger and logger.verbose:
        logger.debug(
            f"[{case_path.name}] Delta selection: labels={labels_list} "
            f"samples_per_label={samples_per_label} shuffle={shuffle} seed={seed} "
            f"delta_dir={safe_relpath(delta_dir, case_path.parent)}"
        )
    for raw_label in labels_list:
        label = raw_label.strip()
        if not label:
            continue
        matches = [
            path for path in sorted(delta_dir.glob(f"{label}*.txt"))
            if path.stem == label or path.stem.startswith(f"{label}_")
        ]
        if not matches:
            continue
        if shuffle:
            rng.shuffle(matches)
        for path in matches[:samples_per_label]:
            l, sample = _parse_delta_name(path)
            selected.append(DeltaFile(label=l, sample=sample, path=path))
        if logger and logger.verbose:
            picked = [
                safe_relpath(path, case_path)
                for path in matches[:samples_per_label]
            ]
            logger.debug(f"[{case_path.name}] Delta label={label} matches={len(matches)} picked={picked}")
    if logger and logger.verbose:
        chosen = [f"{safe_relpath(d.path, case_path)}[{d.label}:{d.sample}]" for d in selected]
        logger.debug(f"[{case_path.name}] Delta selected={chosen if chosen else 'none'}")
    return selected


# -----------------------------
# Delta generation helpers
# -----------------------------


def _parse_change_spec(spec: str) -> List[Tuple[str, float]]:
    pairs: List[Tuple[str, float]] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "=" in part:
            label, raw_val = part.split("=", 1)
        else:
            label, raw_val = part, ""
        label = label.strip()
        if not label:
            continue
        try:
            val = float(raw_val) if raw_val else 0.0
        except ValueError:
            continue
        pairs.append((label, val))
    return pairs


def _parse_change_caps(spec: str) -> Dict[str, int]:
    caps: Dict[str, int] = {}
    if not spec:
        return caps
    for part in spec.split(","):
        part = part.strip()
        if not part or "=" not in part:
            continue
        label, raw_val = part.split("=", 1)
        label = label.strip()
        raw_val = raw_val.strip()
        if not label or not raw_val:
            continue
        try:
            cap = int(float(raw_val))
        except ValueError:
            continue
        if cap > 0:
            caps[label] = cap
    return caps


def _parse_mix_distributions(spec: str) -> List[Tuple[float, float]]:
    ratios: List[Tuple[float, float]] = []
    for part in spec.split(","):
        part = part.strip()
        if not part or "-" not in part:
            continue
        left, right = part.split("-", 1)
        try:
            del_val = float(left.strip())
            ins_val = float(right.strip())
        except ValueError:
            continue
        if del_val < 0 or ins_val < 0:
            continue
        if del_val > 1.0 or ins_val > 1.0:
            del_ratio = del_val / 100.0
            ins_ratio = ins_val / 100.0
        else:
            del_ratio = del_val
            ins_ratio = ins_val
        total = del_ratio + ins_ratio
        if total <= 0:
            continue
        ratios.append((del_ratio / total, ins_ratio / total))
    return ratios


def _read_facts(path: Path) -> List[Tuple[str, ...]]:
    facts: List[Tuple[str, ...]] = []
    if not path.exists():
        return facts
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.strip().split("\t")
        if parts and any(p for p in parts):
            facts.append(tuple(parts))
    return facts


def _read_probabilities(path: Path, default_len: int) -> List[float]:
    if not path.exists():
        return [1.0] * max(1, default_len)
    vals: List[float] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            vals.append(float(line))
        except ValueError:
            continue
    if not vals:
        vals = [1.0] * max(1, default_len)
    return vals


def _load_all_facts(input_dir: Path) -> Tuple[List[Tuple[str, Tuple[str, ...]]], Dict[Tuple[str, str], float]]:
    """Return list of (relation, fact tuple) and per-fact probability lookup."""
    all_facts: List[Tuple[str, Tuple[str, ...]]] = []
    prob_lookup: Dict[Tuple[str, str], float] = {}
    for fact_path in sorted(input_dir.glob("*.facts")):
        rel = fact_path.stem.replace(".facts", "")
        facts = _read_facts(fact_path)
        probs = _read_probabilities(fact_path.with_suffix(".prob"), len(facts))
        for idx, fact in enumerate(facts):
            prob = probs[idx] if idx < len(probs) else 1.0
            all_facts.append((rel, fact))
            prob_lookup[(rel, ", ".join(fact))] = prob
    return all_facts, prob_lookup


def _build_fact_index(all_facts: List[Tuple[str, Tuple[str, ...]]]) -> Dict[int, List[int]]:
    id_to_indices: Dict[int, List[int]] = {}
    for idx, (_, fact) in enumerate(all_facts):
        for item in fact:
            try:
                key = int(item)
            except ValueError:
                continue
            id_to_indices.setdefault(key, []).append(idx)
    return id_to_indices


# -----------------------------
# Fact graph helpers
# -----------------------------


def _collect_assign_edges(input_dir: Path) -> List[Tuple[int, int]]:
    edges: List[Tuple[int, int]] = []
    for fact_path in sorted(input_dir.glob("*.facts")):
        stem = fact_path.stem
        if not _is_assign_like_relation(stem):
            continue
        for fact in _read_facts(fact_path):
            if len(fact) < 2:
                continue
            try:
                to_id = int(fact[0])
                from_id = int(fact[1])
            except ValueError:
                continue
            edges.append((to_id, from_id))
    return edges


def _collect_assign_edges_from_facts(facts: List[Tuple[str, Tuple[str, ...]]]) -> List[Tuple[int, int]]:
    edges: List[Tuple[int, int]] = []
    for rel, fact in facts:
        if not _is_assign_like_relation(rel):
            continue
        if len(fact) < 2:
            continue
        try:
            to_id = int(fact[0])
            from_id = int(fact[1])
        except ValueError:
            continue
        edges.append((to_id, from_id))
    return edges


def _is_assign_like_relation(name: str) -> bool:
    if name in ASSIGN_REL_EXCLUDE:
        return False
    return "assign" in name or name in ASSIGN_LIKE_RELATIONS


def _write_facts(path: Path, facts: List[Tuple[str, ...]]) -> None:
    data = "\n".join("\t".join(items) for items in facts)
    if data:
        data += "\n"
    path.write_text(data, encoding="utf-8")


def _write_probabilities(path: Path, probs: List[float]) -> None:
    data = "\n".join(str(prob) for prob in probs)
    if data:
        data += "\n"
    path.write_text(data, encoding="utf-8")


def _remove_facts(input_dir: Path, deletions: List[Tuple[str, Tuple[str, ...]]], logger: Logger, label: str) -> int:
    if not deletions:
        return 0
    counts = Counter(deletions)
    removed = 0
    for fact_path in sorted(input_dir.glob("*.facts")):
        rel = fact_path.stem
        if all(count == 0 for (rel_name, _), count in counts.items() if rel_name == rel):
            continue
        facts = _read_facts(fact_path)
        if not facts:
            continue
        probs = _read_probabilities(fact_path.with_suffix(".prob"), len(facts))
        new_facts: List[Tuple[str, ...]] = []
        new_probs: List[float] = []
        for idx, fact in enumerate(facts):
            key = (rel, fact)
            if counts.get(key, 0) > 0:
                counts[key] -= 1
                removed += 1
                continue
            new_facts.append(fact)
            new_probs.append(probs[idx] if idx < len(probs) else 1.0)
        if len(new_facts) != len(facts):
            _write_facts(fact_path, new_facts)
            prob_path = fact_path.with_suffix(".prob")
            if prob_path.exists():
                _write_probabilities(prob_path, new_probs)
    leftover = sum(counts.values())
    if leftover > 0:
        logger.warn(f"[{label}] Requested deletion count exceeds available facts by {leftover}")
    return removed


def _write_insert_pool_manifest(
        input_dir: Path,
        entries: List[Tuple[str, Tuple[str, ...]]],
        prob_lookup: Dict[Tuple[str, str], float],
        logger: Optional[Logger] = None,
) -> None:
    if not entries:
        return
    path = input_dir / INSERT_POOL_MANIFEST_NAME
    ensure_dirs(path.parent)
    with path.open("w", encoding="utf-8") as handle:
        for rel, fact in entries:
            arglist = ", ".join(fact)
            prob = prob_lookup.get((rel, arglist), 1.0)
            handle.write(json.dumps({"rel": rel, "fact": list(fact), "prob": prob}) + "\n")
    if logger and logger.verbose:
        logger.debug(
            f"[{input_dir.parent.name}] Recorded {len(entries)} insert-pool facts "
            f"to {safe_relpath(path, input_dir.parent)}"
        )


def _load_insert_pool_manifest(
        input_dir: Path,
        logger: Optional[Logger] = None,
) -> Tuple[List[Tuple[str, Tuple[str, ...]]], Dict[Tuple[str, str], float]]:
    path = input_dir / INSERT_POOL_MANIFEST_NAME
    if not path.exists():
        return [], {}
    entries: List[Tuple[str, Tuple[str, ...]]] = []
    prob_lookup: Dict[Tuple[str, str], float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        rel = payload.get("rel")
        fact = payload.get("fact")
        if not rel or not isinstance(fact, list):
            continue
        items = tuple(str(item) for item in fact)
        entries.append((str(rel), items))
        arglist = ", ".join(items)
        prob = payload.get("prob", 1.0)
        try:
            prob_val = float(prob)
        except (TypeError, ValueError):
            prob_val = 1.0
        prob_lookup[(str(rel), arglist)] = prob_val
    if logger and logger.verbose:
        logger.debug(
            f"[{input_dir.parent.name}] Loaded {len(entries)} insert-pool facts "
            f"from {safe_relpath(path, input_dir.parent)}"
        )
    return entries, prob_lookup


def _delta_count(total: int, spec_val: float) -> int:
    if total <= 0:
        return 0
    if spec_val < 1.0:
        return max(1, int(total * spec_val))
    return min(total, int(spec_val))


def _delta_target(total: int, spec_val: float, cap: Optional[int]) -> int:
    count = _delta_count(total, spec_val)
    if cap is None or cap <= 0:
        return count
    return min(count, cap)


def _format_mix_label(del_ratio: float, ins_ratio: float) -> str:
    del_pct = int(round(del_ratio * 100))
    ins_pct = int(round(ins_ratio * 100))
    return f"mix-d{del_pct}-i{ins_pct}"


def write_delta_file(path: Path, deletions: List[Tuple[str, Tuple[str, ...]]], prob_lookup: Dict[Tuple[str, str], float]) -> None:
    lines: List[str] = []
    for rel, fact in deletions:
        arglist = ", ".join(fact)
        lines.append(f"delete {rel}({arglist})")
    lines.append("commit")
    for rel, fact in reversed(deletions):
        arglist = ", ".join(fact)
        prob = prob_lookup.get((rel, arglist), 1.0)
        lines.append(f"insert {rel}({arglist}) {prob}")
    lines.append("commit")
    lines.append("q")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_delta_file_mixed(
        path: Path,
        deletions: List[Tuple[str, Tuple[str, ...]]],
        insertions: List[Tuple[str, Tuple[str, ...]]],
        prob_lookup: Dict[Tuple[str, str], float],
        rng: random.Random,
) -> None:
    ops: List[Tuple[str, str, Tuple[str, ...], Optional[float]]] = []
    for rel, fact in deletions:
        ops.append(("delete", rel, fact, None))
    for rel, fact in insertions:
        arglist = ", ".join(fact)
        prob = prob_lookup.get((rel, arglist), 1.0)
        ops.append(("insert", rel, fact, prob))
    rng.shuffle(ops)
    lines: List[str] = []
    for op, rel, fact, prob in ops:
        arglist = ", ".join(fact)
        if op == "delete":
            lines.append(f"delete {rel}({arglist})")
        else:
            lines.append(f"insert {rel}({arglist}) {prob}")
    lines.append("commit")
    lines.append("q")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _select_deletions(
        *,
        all_facts: List[Tuple[str, Tuple[str, ...]]],
        id_to_indices: Dict[int, List[int]],
        assign_edges: List[Tuple[int, int]],
        target: int,
        mode: str,
        tol: float,
        transitive_depth: int,
        max_clusters_override: Optional[int],
        rng: random.Random,
        logger: Logger,
        label: str,
) -> Tuple[List[Tuple[str, Tuple[str, ...]]], Dict[str, Any]]:
    info: Dict[str, Any] = {"label": label, "mode": mode, "target": target, "truncated": False, "attempts": 0}
    if target <= 0:
        return [], info
    total = len(all_facts)
    if target > total:
        target = total

    def fallback_random(reason: str, from_mode: str) -> Tuple[List[Tuple[str, Tuple[str, ...]]], Dict[str, Any]]:
        logger.warn(
            f"[{label}] {reason} Falling back to random deletion sampling (target={target})."
        )
        deletions_local = rng.sample(all_facts, target)
        info["mode"] = "random"
        info["fallback"] = "random"
        info["fallback_from_mode"] = from_mode
        info["fallback_reason"] = reason
        info["lower"] = target
        info["upper"] = target
        info["clusters"] = None
        info["cluster_max"] = None
        info["size"] = len(deletions_local)
        return deletions_local, info

    if mode == "random":
        deletions = rng.sample(all_facts, target)
        info["lower"] = target
        info["upper"] = target
        info["size"] = len(deletions)
        return deletions, info

    id_keys = list(id_to_indices.keys())
    if mode in ("assign", "assign-transitive") and not assign_edges:
        logger.warn(f"[{label}] No assign edges available; falling back to id clustering")
        mode = "id"
        info["mode"] = mode
    if mode == "id" and not id_keys:
        return fallback_random(
            "No numeric IDs found; cannot perform clustered deletions.",
            from_mode=mode,
        )

    lower = max(1, int(round(target * (1.0 - tol))))
    upper = max(lower, int(round(target * (1.0 + tol))))
    best_indices: Optional[List[int]] = None
    best_diff: Optional[int] = None
    best_size: Optional[int] = None
    best_clusters = 0
    attempts = 0
    if max_clusters_override is None:
        max_clusters = max(1, min(DEFAULT_CLUSTER_MAX, target))
    else:
        max_clusters = max(1, min(max_clusters_override, target))

    id_to_edge_idxs: Optional[Dict[int, List[int]]] = None
    depth_limit = max(0, int(transitive_depth))
    if mode == "assign-transitive":
        id_to_edge_idxs = {}
        for idx, edge in enumerate(assign_edges):
            id_to_edge_idxs.setdefault(edge[0], []).append(idx)
            id_to_edge_idxs.setdefault(edge[1], []).append(idx)

    def sample_cluster() -> Optional[Iterable[int]]:
        if mode == "assign":
            edge = rng.choice(assign_edges)
            indices: set[int] = set()
            for node_id in edge:
                indices.update(id_to_indices.get(node_id, []))
            return indices
        node_id = rng.choice(id_keys)
        return id_to_indices.get(node_id, [])

    for _ in range(DEFAULT_CLUSTER_ATTEMPTS):
        attempts += 1
        indices: set[int] = set()
        clusters = 0
        stalls = 0
        if mode == "assign-transitive":
            selected_ids: set[int] = set()
            used_edge_idxs: set[int] = set()
            edge_count = len(assign_edges)
            assert id_to_edge_idxs is not None

            def pick_unused_edge() -> Optional[int]:
                if len(used_edge_idxs) >= edge_count:
                    return None
                for _ in range(edge_count):
                    idx = rng.randrange(edge_count)
                    if idx in used_edge_idxs:
                        continue
                    edge = assign_edges[idx]
                    if edge[0] in selected_ids and edge[1] in selected_ids:
                        continue
                    return idx
                for idx in range(edge_count):
                    if idx in used_edge_idxs:
                        continue
                    edge = assign_edges[idx]
                    if edge[0] in selected_ids and edge[1] in selected_ids:
                        continue
                    return idx
                return None

            def add_edge(idx: int) -> bool:
                used_edge_idxs.add(idx)
                before = len(indices)
                edge = assign_edges[idx]
                for node_id in edge:
                    selected_ids.add(node_id)
                    indices.update(id_to_indices.get(node_id, []))
                return len(indices) > before

            while len(indices) < lower and clusters < max_clusters:
                seed_idx = pick_unused_edge()
                if seed_idx is None:
                    stalls += 1
                    if stalls >= max_clusters:
                        break
                    continue
                stalls = 0
                clusters += 1
                edge = assign_edges[seed_idx]
                new_nodes = [node_id for node_id in edge if node_id not in selected_ids]
                add_edge(seed_idx)
                if len(indices) >= upper:
                    break
                if depth_limit <= 0:
                    continue
                frontier_ids: set[int] = set(new_nodes if new_nodes else edge)
                depth = 0
                while depth < depth_limit and len(indices) < lower and clusters < max_clusters:
                    if not frontier_ids:
                        break
                    candidate_idxs: set[int] = set()
                    for node_id in frontier_ids:
                        for idx in id_to_edge_idxs.get(node_id, []):
                            if idx in used_edge_idxs:
                                continue
                            edge = assign_edges[idx]
                            if edge[0] in selected_ids and edge[1] in selected_ids:
                                continue
                            candidate_idxs.add(idx)
                    if not candidate_idxs:
                        break
                    next_frontier: set[int] = set()
                    while candidate_idxs and len(indices) < lower and clusters < max_clusters:
                        idx = rng.choice(sorted(candidate_idxs))
                        candidate_idxs.remove(idx)
                        edge = assign_edges[idx]
                        new_nodes = [node_id for node_id in edge if node_id not in selected_ids]
                        clusters += 1
                        add_edge(idx)
                        if new_nodes:
                            next_frontier.update(new_nodes)
                        if len(indices) >= upper:
                            break
                    frontier_ids = next_frontier
                    depth += 1
                    if len(indices) >= upper:
                        break
        else:
            while len(indices) < lower and clusters < max_clusters:
                cluster = sample_cluster()
                clusters += 1
                if not cluster:
                    stalls += 1
                    if stalls >= max_clusters:
                        break
                    continue
                before = len(indices)
                indices.update(cluster)
                if len(indices) == before:
                    stalls += 1
                    if stalls >= max_clusters:
                        break
                if len(indices) >= upper:
                    break
        if not indices:
            continue
        size = len(indices)
        diff = abs(size - target)
        if best_diff is None or diff < best_diff:
            best_diff = diff
            best_size = size
        if lower <= size <= upper:
            best_indices = sorted(indices)
            best_clusters = clusters
            best_diff = diff
            best_size = size
            break

    info["attempts"] = attempts
    info["lower"] = lower
    info["upper"] = upper
    info["clusters"] = best_clusters
    info["cluster_max"] = max_clusters

    if best_indices is None:
        best_desc = "n/a" if best_size is None else str(best_size)
        return fallback_random(
            f"Failed to build clustered deletions within target range "
            f"(target={target}, range=[{lower}, {upper}], best={best_desc}).",
            from_mode=mode,
        )

    if len(best_indices) > target:
        best_indices = rng.sample(best_indices, target)
        info["truncated"] = True

    deletions = [all_facts[idx] for idx in best_indices]
    info["size"] = len(deletions)
    return deletions, info


def generate_deltas_for_case(
        case_num: int,
        base_dir: Path,
        labels_and_sizes: List[Tuple[str, float]],
        sets: int,
        seed: Optional[int],
        cleanup: bool,
        logger: Logger,
        change_caps: Optional[Dict[str, int]] = None,
        cluster_mode: str = "assign",
        cap_tol: float = DEFAULT_CHANGE_CAP_TOL,
        transitive_depth: int = DEFAULT_TRANSITIVE_DEPTH,
) -> None:
    name = case_name(case_num)
    cdir = case_dir(base_dir, case_num)
    input_dir = cdir / "input"
    delta_dir = cdir / "delta"
    if not input_dir.exists():
        logger.warn(f"[{name}] Missing input/; workspace initialization failed")
        return
    if cleanup and delta_dir.exists():
        shutil.rmtree(delta_dir)
    ensure_dirs(delta_dir)

    all_facts, prob_lookup = _load_all_facts(input_dir)
    if not all_facts:
        logger.warn(f"[{name}] No facts found under {input_dir}; skipping")
        return

    change_caps = change_caps or {}
    id_to_indices = _build_fact_index(all_facts)
    assign_edges = _collect_assign_edges(input_dir)
    total_facts = len(all_facts)
    if logger.verbose:
        logger.debug(
            f"[{name}] Delta pool: facts={total_facts} id_keys={len(id_to_indices)} "
            f"assign_edges={len(assign_edges)} change_caps={change_caps if change_caps else 'n/a'}"
        )
    case_seed = case_seed_value(case_num)
    for label, size_val in labels_and_sizes:
        if not label:
            continue
        cap = change_caps.get(label)
        target = _delta_target(total_facts, size_val, cap)
        if target <= 0:
            continue
        for idx in range(sets):
            rng = random.Random((seed if seed is not None else DEFAULT_GLOBAL_SEED) + case_seed * 1000 + idx)
            try:
                deletions, info = _select_deletions(
                    all_facts=all_facts,
                    id_to_indices=id_to_indices,
                    assign_edges=assign_edges,
                    target=target,
                    mode=cluster_mode,
                    tol=cap_tol,
                    transitive_depth=transitive_depth,
                    max_clusters_override=None,
                    rng=rng,
                    logger=logger,
                    label=f"{name}:{label}",
                )
            except RuntimeError as exc:
                logger.error(str(exc))
                raise
            suffix = f"_{idx + 1}" if sets > 1 else ""
            out_path = delta_dir / f"{label}{suffix}.txt"
            write_delta_file(out_path, deletions, prob_lookup)
            logger.info(
                f"[{name}] Wrote delta {out_path.relative_to(cdir)} "
                f"(delete {len(deletions)} facts, target={target}, cap={cap if cap else 'n/a'}, "
                f"mode={info.get('mode')}, attempts={info.get('attempts')}, clusters={info.get('clusters')}, "
                f"cluster_max={info.get('cluster_max')}, truncated={info.get('truncated')})"
            )
            if logger.verbose:
                lower = info.get("lower")
                upper = info.get("upper")
                range_str = f"[{lower}, {upper}]" if lower is not None and upper is not None else "n/a"
                logger.debug(
                    f"[{name}] Delta {label}{suffix}: target={target} range={range_str} "
                    f"size={len(deletions)} mode={info.get('mode')} attempts={info.get('attempts')} "
                    f"clusters={info.get('clusters')}/{info.get('cluster_max')} truncated={info.get('truncated')}"
                )


def generate_mixed_deltas_for_case(
        case_num: int,
        base_dir: Path,
        sets: int,
        seed: Optional[int],
        cleanup: bool,
        logger: Logger,
        cluster_mode: str,
        cap_tol: float,
        transitive_depth: int,
        mix_ratio: float,
        mix_cap: int,
        pool_ratio: float,
        pool_cap_mult: float,
        mix_distributions: List[Tuple[float, float]],
) -> None:
    name = case_name(case_num)
    cdir = case_dir(base_dir, case_num)
    input_dir = cdir / "input"
    delta_dir = cdir / "delta"
    if not input_dir.exists():
        logger.warn(f"[{name}] Missing input/; workspace initialization failed")
        return
    if cleanup and delta_dir.exists():
        shutil.rmtree(delta_dir)
    ensure_dirs(delta_dir)

    base_seed = seed if seed is not None else DEFAULT_GLOBAL_SEED
    case_seed = case_seed_value(case_num)
    existing_pool, existing_pool_probs = _load_insert_pool_manifest(input_dir, logger)
    all_facts, prob_lookup = _load_all_facts(input_dir)
    if not all_facts and not existing_pool:
        logger.warn(f"[{name}] No facts found under {input_dir}; skipping")
        return

    pool_facts: List[Tuple[str, Tuple[str, ...]]] = []
    pool_probs: Dict[Tuple[str, str], float] = {}
    if existing_pool:
        pool_facts = existing_pool
        pool_probs = existing_pool_probs
        remaining_facts = all_facts
        total_facts = len(remaining_facts) + len(pool_facts)
        logger.info(
            f"[{name}] Reusing insert pool from {INSERT_POOL_MANIFEST_NAME} "
            f"(pool={len(pool_facts)} remaining={len(remaining_facts)})"
        )
    else:
        total_facts = len(all_facts)
        if total_facts == 0:
            logger.warn(f"[{name}] No facts found under {input_dir}; skipping")
            return
        pool_cap = int(round(mix_cap * pool_cap_mult)) if pool_cap_mult > 0 else mix_cap
        pool_target = _delta_target(total_facts, pool_ratio, pool_cap)
        rng_pool = random.Random(base_seed + case_seed * 1000 + 17)
        id_to_indices = _build_fact_index(all_facts)
        assign_edges = _collect_assign_edges(input_dir)
        pool_facts, info = _select_deletions(
            all_facts=all_facts,
            id_to_indices=id_to_indices,
            assign_edges=assign_edges,
            target=pool_target,
            mode=cluster_mode,
            tol=cap_tol,
            transitive_depth=transitive_depth,
            max_clusters_override=None,
            rng=rng_pool,
            logger=logger,
            label=f"{name}:insert-pool",
        )
        removed = _remove_facts(input_dir, pool_facts, logger, name)
        for rel, fact in pool_facts:
            arglist = ", ".join(fact)
            pool_probs[(rel, arglist)] = prob_lookup.get((rel, arglist), 1.0)
        _write_insert_pool_manifest(input_dir, pool_facts, pool_probs, logger)
        logger.info(
            f"[{name}] Pre-pruned insert pool "
            f"(removed={removed} target={pool_target} mode={info.get('mode')} "
            f"attempts={info.get('attempts')} clusters={info.get('clusters')}/"
            f"{info.get('cluster_max')} truncated={info.get('truncated')})"
        )
        remaining_facts, _ = _load_all_facts(input_dir)
        total_facts = len(remaining_facts) + len(pool_facts)

    if not remaining_facts:
        logger.warn(f"[{name}] No remaining facts after insert pool; skipping")
        return
    if not pool_facts:
        logger.warn(f"[{name}] No insert pool facts available; skipping")
        return

    total_target = _delta_target(total_facts, mix_ratio, mix_cap)
    if total_target <= 0:
        logger.warn(f"[{name}] Total target size <= 0; skipping")
        return

    remaining_id_to_indices = _build_fact_index(remaining_facts)
    remaining_assign_edges = _collect_assign_edges(input_dir)
    pool_id_to_indices = _build_fact_index(pool_facts)
    pool_assign_edges = _collect_assign_edges_from_facts(pool_facts)

    for ratio_idx, (del_ratio, ins_ratio) in enumerate(mix_distributions):
        label = _format_mix_label(del_ratio, ins_ratio)
        for idx in range(sets):
            seed_base = base_seed + case_seed * 1000 + ratio_idx * 100 + idx
            delete_target = int(round(total_target * del_ratio))
            if delete_target < 0:
                delete_target = 0
            insert_target = total_target - delete_target
            if insert_target < 0:
                insert_target = 0

            deletions: List[Tuple[str, Tuple[str, ...]]] = []
            insertions: List[Tuple[str, Tuple[str, ...]]] = []
            info_del: Dict[str, Any] = {"mode": cluster_mode, "attempts": 0, "clusters": None, "cluster_max": None, "truncated": False}
            info_ins: Dict[str, Any] = {"mode": cluster_mode, "attempts": 0, "clusters": None, "cluster_max": None, "truncated": False}

            if delete_target > 0:
                deletions, info_del = _select_deletions(
                    all_facts=remaining_facts,
                    id_to_indices=remaining_id_to_indices,
                    assign_edges=remaining_assign_edges,
                    target=delete_target,
                    mode=cluster_mode,
                    tol=cap_tol,
                    transitive_depth=transitive_depth,
                    max_clusters_override=None,
                    rng=random.Random(seed_base + 1),
                    logger=logger,
                    label=f"{name}:{label}:delete",
                )
            if insert_target > 0:
                insertions, info_ins = _select_deletions(
                    all_facts=pool_facts,
                    id_to_indices=pool_id_to_indices,
                    assign_edges=pool_assign_edges,
                    target=insert_target,
                    mode=cluster_mode,
                    tol=cap_tol,
                    transitive_depth=transitive_depth,
                    max_clusters_override=None,
                    rng=random.Random(seed_base + 2),
                    logger=logger,
                    label=f"{name}:{label}:insert",
                )

            suffix = f"_{idx + 1}" if sets > 1 else ""
            out_path = delta_dir / f"{label}{suffix}.txt"
            write_delta_file_mixed(
                out_path,
                deletions,
                insertions,
                pool_probs,
                random.Random(seed_base + 3),
            )
            logger.info(
                f"[{name}] Wrote mix delta {out_path.relative_to(cdir)} "
                f"(delete {len(deletions)}/{delete_target} insert {len(insertions)}/{insert_target} "
                f"total={len(deletions) + len(insertions)} cap={mix_cap} mode={cluster_mode} "
                f"del_clusters={info_del.get('clusters')} ins_clusters={info_ins.get('clusters')})"
            )
            if logger.verbose:
                logger.debug(
                    f"[{name}] Mix delta {label}{suffix}: delete_target={delete_target} "
                    f"insert_target={insert_target} del_attempts={info_del.get('attempts')} "
                    f"ins_attempts={info_ins.get('attempts')} del_truncated={info_del.get('truncated')} "
                    f"ins_truncated={info_ins.get('truncated')}"
                )


def generate_alpha_grid_deltas_for_case(
        case_num: int,
        base_dir: Path,
        labels_and_sizes: List[Tuple[str, float]],
        change_caps: Dict[str, int],
        seed: Optional[int],
        cleanup: bool,
        logger: Logger,
        cluster_mode: str,
        cap_tol: float,
        transitive_depth: int,
        pool_ratio: float,
        pool_cap_mult: float,
        mix_distributions: List[Tuple[float, float]],
) -> None:
    name = case_name(case_num)
    cdir = case_dir(base_dir, case_num)
    input_dir = cdir / "input"
    delta_dir = cdir / "delta"
    if not input_dir.exists():
        logger.warn(f"[{name}] Missing input/; workspace initialization failed")
        return
    if cleanup and delta_dir.exists():
        shutil.rmtree(delta_dir)
    ensure_dirs(delta_dir)

    base_seed = seed if seed is not None else DEFAULT_GLOBAL_SEED
    case_seed = case_seed_value(case_num)
    existing_pool, existing_pool_probs = _load_insert_pool_manifest(input_dir, logger)
    all_facts, prob_lookup = _load_all_facts(input_dir)
    if not all_facts and not existing_pool:
        logger.warn(f"[{name}] No facts found under {input_dir}; skipping")
        return

    pool_facts: List[Tuple[str, Tuple[str, ...]]] = []
    pool_probs: Dict[Tuple[str, str], float] = {}
    if existing_pool:
        pool_facts = existing_pool
        pool_probs = existing_pool_probs
        remaining_facts = all_facts
        total_facts = len(remaining_facts) + len(pool_facts)
        logger.info(
            f"[{name}] Reusing insert pool from {INSERT_POOL_MANIFEST_NAME} "
            f"(pool={len(pool_facts)} remaining={len(remaining_facts)})"
        )
    else:
        total_facts = len(all_facts)
        if total_facts == 0:
            logger.warn(f"[{name}] No facts found under {input_dir}; skipping")
            return
        targets = [
            _delta_target(total_facts, size_val, change_caps.get(label))
            for label, size_val in labels_and_sizes
            if label
        ]
        max_target = max(targets, default=0)
        max_insert_ratio = max((ins_ratio for _, ins_ratio in mix_distributions), default=0.0)
        required_pool = int(math.ceil(max_target * max_insert_ratio))
        pool_cap = int(round(max_target * pool_cap_mult)) if pool_cap_mult > 0 else max_target
        pool_target = max(required_pool, _delta_target(total_facts, pool_ratio, pool_cap))
        if pool_target <= 0:
            logger.warn(f"[{name}] Insert-pool target <= 0; skipping")
            return
        rng_pool = random.Random(base_seed + case_seed * 1000 + 17)
        id_to_indices = _build_fact_index(all_facts)
        assign_edges = _collect_assign_edges(input_dir)
        pool_facts, info = _select_deletions(
            all_facts=all_facts,
            id_to_indices=id_to_indices,
            assign_edges=assign_edges,
            target=pool_target,
            mode=cluster_mode,
            tol=cap_tol,
            transitive_depth=transitive_depth,
            max_clusters_override=None,
            rng=rng_pool,
            logger=logger,
            label=f"{name}:insert-pool",
        )
        removed = _remove_facts(input_dir, pool_facts, logger, name)
        for rel, fact in pool_facts:
            arglist = ", ".join(fact)
            pool_probs[(rel, arglist)] = prob_lookup.get((rel, arglist), 1.0)
        _write_insert_pool_manifest(input_dir, pool_facts, pool_probs, logger)
        logger.info(
            f"[{name}] Pre-pruned alpha-grid insert pool "
            f"(removed={removed} target={pool_target} required={required_pool} mode={info.get('mode')} "
            f"attempts={info.get('attempts')} clusters={info.get('clusters')}/"
            f"{info.get('cluster_max')} truncated={info.get('truncated')})"
        )
        remaining_facts, _ = _load_all_facts(input_dir)
        total_facts = len(remaining_facts) + len(pool_facts)

    if not remaining_facts:
        logger.warn(f"[{name}] No remaining facts after insert pool; skipping")
        return
    if not pool_facts:
        logger.warn(f"[{name}] No insert pool facts available; skipping")
        return

    remaining_id_to_indices = _build_fact_index(remaining_facts)
    remaining_assign_edges = _collect_assign_edges(input_dir)
    pool_id_to_indices = _build_fact_index(pool_facts)
    pool_assign_edges = _collect_assign_edges_from_facts(pool_facts)

    for label_idx, (label, size_val) in enumerate(labels_and_sizes):
        if not label:
            continue
        cap = change_caps.get(label)
        total_target = _delta_target(total_facts, size_val, cap)
        if total_target <= 0:
            continue
        for ratio_idx, (del_ratio, ins_ratio) in enumerate(mix_distributions):
            sample_idx = ratio_idx + 1
            seed_base = base_seed + case_seed * 1000 + label_idx * 1000 + ratio_idx * 100
            delete_target = int(round(total_target * del_ratio))
            insert_target = total_target - delete_target
            if insert_target > len(pool_facts):
                logger.warn(
                    f"[{name}] {label}_{sample_idx} insert target {insert_target} exceeds "
                    f"pool size {len(pool_facts)}; truncating"
                )
                insert_target = len(pool_facts)
            deletions: List[Tuple[str, Tuple[str, ...]]] = []
            insertions: List[Tuple[str, Tuple[str, ...]]] = []
            info_del: Dict[str, Any] = {"mode": cluster_mode, "attempts": 0, "clusters": None, "cluster_max": None, "truncated": False}
            info_ins: Dict[str, Any] = {"mode": cluster_mode, "attempts": 0, "clusters": None, "cluster_max": None, "truncated": False}

            if delete_target > 0:
                deletions, info_del = _select_deletions(
                    all_facts=remaining_facts,
                    id_to_indices=remaining_id_to_indices,
                    assign_edges=remaining_assign_edges,
                    target=delete_target,
                    mode=cluster_mode,
                    tol=0.0,
                    transitive_depth=transitive_depth,
                    max_clusters_override=None,
                    rng=random.Random(seed_base + 1),
                    logger=logger,
                    label=f"{name}:{label}_{sample_idx}:delete",
                )
            if insert_target > 0:
                insertions, info_ins = _select_deletions(
                    all_facts=pool_facts,
                    id_to_indices=pool_id_to_indices,
                    assign_edges=pool_assign_edges,
                    target=insert_target,
                    mode=cluster_mode,
                    tol=0.0,
                    transitive_depth=transitive_depth,
                    max_clusters_override=None,
                    rng=random.Random(seed_base + 2),
                    logger=logger,
                    label=f"{name}:{label}_{sample_idx}:insert",
                )

            out_path = delta_dir / f"{label}_{sample_idx}.txt"
            write_delta_file_mixed(
                out_path,
                deletions,
                insertions,
                pool_probs,
                random.Random(seed_base + 3),
            )
            alpha = float(len(deletions)) / float(len(deletions) + len(insertions)) if deletions or insertions else 0.0
            logger.info(
                f"[{name}] Wrote alpha-grid delta {out_path.relative_to(cdir)} "
                f"(alpha={alpha:.2f} delete {len(deletions)}/{delete_target} "
                f"insert {len(insertions)}/{insert_target} total={len(deletions) + len(insertions)} "
                f"target={total_target} cap={cap if cap else 'n/a'} "
                f"del_mode={info_del.get('mode')} ins_mode={info_ins.get('mode')})"
            )


# -----------------------------
# Compile
# -----------------------------


@dataclass
class CmpCfg:
    base_dir: Path
    cases: List[int]
    timeout: int
    souffle_bin: str
    souffle_args: List[str]
    logger: Logger


def _resolve_exec_path(token: str) -> Optional[Path]:
    if not token:
        return None
    if "/" in token:
        candidate = Path(token).expanduser()
        if not candidate.is_absolute():
            candidate = (Path.cwd() / candidate).resolve()
        if candidate.exists() and candidate.is_file():
            return candidate
        return None
    resolved = shutil.which(token)
    return Path(resolved).resolve() if resolved else None


def resolve_souffle_bin(requested: Optional[str], logger: Logger) -> Path:
    requested_val = (requested or "").strip()
    env_val = os.environ.get("SOUFFLE_BIN", "").strip()
    source = ""
    resolved: Optional[Path] = None

    if requested_val:
        source = "--souffle-bin"
        resolved = _resolve_exec_path(requested_val)
        if resolved is None:
            logger.error(f"Invalid --souffle-bin: {requested_val}")
            sys.exit(2)
    elif env_val:
        source = "SOUFFLE_BIN"
        resolved = _resolve_exec_path(env_val)
        if resolved is None:
            logger.error(f"Invalid SOUFFLE_BIN: {env_val}")
            sys.exit(2)
    else:
        resolved = _resolve_exec_path(str(DEFAULT_REPO_SOUFFLE_BIN))
        source = "repo-build"
        if resolved is None:
            resolved = _resolve_exec_path("souffle")
            source = "PATH"
            if resolved is None:
                logger.error(
                    "Cannot find Souffle compiler. Expected one of: "
                    f"{DEFAULT_REPO_SOUFFLE_BIN} or PATH 'souffle'."
                )
                sys.exit(2)
            logger.warn(
                f"Repo Souffle binary not found at {DEFAULT_REPO_SOUFFLE_BIN}; "
                "falling back to PATH 'souffle'."
            )

    logger.info(f"Souffle compiler: {resolved} (source={source})")
    return resolved


def detect_souffle_version(souffle_bin: Path, cwd: Path) -> Optional[str]:
    code, _, out, err = time_cmd([str(souffle_bin), "--version"], cwd=cwd, timeout=15)
    if code != 0:
        return None
    merged = f"{out}\n{err}"
    for raw in merged.splitlines():
        line = raw.strip()
        if line.startswith("Version:"):
            return line.split(":", 1)[1].strip()
    return None


def compile_case(n: int, cfg: CmpCfg) -> None:
    cdir = case_dir(cfg.base_dir, n)
    src = cdir / "compute.souffle.dl"
    if not src.exists():
        cfg.logger.warn(f"[{case_name(n)}] Missing {src.name}; skipping compile")
        return
    cmd = [cfg.souffle_bin, "--inc-only", "-F", "input", "-D", "output"]
    if cfg.souffle_args:
        cmd.extend(cfg.souffle_args)
    cmd.extend([str(src), "-o", "compute"])
    if cfg.logger.verbose:
        cfg.logger.debug(f"[{case_name(n)}] Compile cmd: {' '.join(cmd)} (timeout={cfg.timeout}s)")
    cfg.logger.info(f"[{case_name(n)}] Compiling Soufflé…")
    code, elapsed, _, err = time_cmd(cmd, cwd=cdir, timeout=cfg.timeout)
    if code == 0:
        cfg.logger.info(f"[{case_name(n)}] ✔ compile OK in {elapsed:.3f}s → ./compute")
    else:
        cfg.logger.error(f"[{case_name(n)}] ✖ compile failed (exit={code}) in {elapsed:.3f}s")
        if err.strip():
            cfg.logger.error(f"[{case_name(n)}] stderr: {err.strip()[:300]}")


# -----------------------------
# Run helpers
# -----------------------------


def _iter_suffixes(mode: str) -> List[str]:
    if mode == "inc-naive":
        return ["inc-naive", "inc"]
    if mode == "inc-regional":
        return ["inc-regional"]
    if mode == "inc-full":
        return ["full"]
    if mode == "full-inc-naive":
        return ["inc-naive", "inc"]
    if mode == "full-inc-regional":
        return ["inc-regional"]
    if mode == "full":
        return ["full"]
    return [mode]


def _mode_setmode_command(mode: str) -> str:
    if mode == "inc-full":
        return "sem=inc fc=full"
    if mode == "full-inc-naive":
        return "sem=full fc=inc-naive"
    if mode == "full-inc-regional":
        return "sem=full fc=inc-regional"
    return mode


def _mode_launch_setmode(mode: str) -> str:
    if mode in {"full", "full-inc-naive", "full-inc-regional"}:
        return "full"
    if mode == "inc-regional":
        return "inc-regional"
    return "inc-naive"


def _iter_output_paths(out_dir: Path, mode: str) -> List[Path]:
    patterns = [f"fact-iter*-{suffix}.prob" for suffix in _iter_suffixes(mode)]
    seen: Dict[str, Path] = {}
    for pattern in patterns:
        for path in out_dir.glob(pattern):
            seen[str(path)] = path
    return sorted(seen.values(), key=lambda p: p.name)


def remove_iter_outputs(out_dir: Path, mode: str) -> None:
    for path in _iter_output_paths(out_dir, mode):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def rename_iter_outputs(out_dir: Path, mode: str, tag: str) -> Dict[str, Path]:
    renamed: Dict[str, Path] = {}
    for src in _iter_output_paths(out_dir, mode):
        name = src.name
        if mode in {"inc-naive", "full-inc-naive"}:
            name = name.replace("-inc.prob", "-inc-naive.prob")
        dest = out_dir / f"{tag}-{name}"
        try:
            shutil.move(str(src), str(dest))
        except Exception:
            shutil.copy2(src, dest)
            src.unlink(missing_ok=True)  # type: ignore[attr-defined]
        renamed[src.name] = dest
    return renamed


def run_baseline(
        case_dir_path: Path,
        timeout: int,
        logger: Logger,
        output_dir: Path,
        mode: str = "full",
        run_args: Optional[List[str]] = None,
) -> Tuple[Path, float]:
    exe = case_dir_path / "compute"
    if not exe.exists():
        raise FileNotFoundError(f"Soufflé binary missing for {case_dir_path.name}")
    ensure_dirs(output_dir)
    out_dir_rel = safe_relpath(output_dir, case_dir_path)
    log_path = output_dir / f"log_{case_dir_path.name}_baseline_{mode}"
    log_rel = safe_relpath(log_path, case_dir_path)
    setmode_cmd = _mode_setmode_command(mode)
    launch_mode = _mode_launch_setmode(mode)
    cmd = [
        f"./{exe.name}",
        "-F", "input",
        "-D", out_dir_rel,
        "--setmode", launch_mode,
        "--logfile", log_rel,
    ]
    if run_args:
        cmd.extend(run_args)
    if logger.verbose:
        logger.debug(f"[{case_dir_path.name}] Baseline cmd: {' '.join(cmd)} (timeout={timeout}s)")
    logger.info(f"[{case_dir_path.name}] Running baseline Soufflé ({mode})…")
    feed = f"setmode {setmode_cmd}\nq\n"
    code, elapsed, _, err = time_cmd(cmd, cwd=case_dir_path, timeout=timeout, feed=feed)
    if code != 0:
        msg = f"baseline run failed (exit={code}) in {elapsed:.3f}s"
        if err.strip():
            msg += f". stderr: {err.strip()[:200]}"
        raise RuntimeError(msg)
    out_dir = output_dir
    facts = out_dir / "facts.prob"
    ensure_dirs(out_dir)
    dst = out_dir / "facts.baseline.prob"
    if facts.exists():
        try:
            shutil.move(str(facts), str(dst))
        except Exception:
            shutil.copy2(facts, dst)
            facts.unlink(missing_ok=True)  # type: ignore[attr-defined]
    return dst, elapsed


@dataclass
class CliRunResult:
    mode: str
    label: str
    sample: str
    exit_code: int
    elapsed: float
    stderr: str
    stdout: str
    iter_paths: Dict[str, Path]
    materialized_final: bool = False
    materialized_input: Optional[Path] = None
    materialized_stats: Optional[Dict[str, Any]] = None


def run_cli_for_delta(
        case_dir_path: Path,
        delta: DeltaFile,
        mode: str,
        timeout: int,
        output_dir: Path,
        run_args: Optional[List[str]] = None,
        logger: Optional[Logger] = None,
) -> CliRunResult:
    exe = case_dir_path / "compute"
    ensure_dirs(output_dir)
    out_dir_rel = safe_relpath(output_dir, case_dir_path)
    log_path = output_dir / f"log_{case_dir_path.name}_{delta.label}_{delta.sample}_{mode}"
    log_rel = safe_relpath(log_path, case_dir_path)
    setmode_cmd = _mode_setmode_command(mode)
    launch_mode = _mode_launch_setmode(mode)
    cmd = [
        f"./{exe.name}",
        "-F", "input",
        "-D", out_dir_rel,
        "--setmode", launch_mode,
        "--logfile", log_rel,
    ]
    if run_args:
        cmd.extend(run_args)
    feed = f"setmode {setmode_cmd}\n" + delta.path.read_text(encoding="utf-8")
    if logger and logger.verbose:
        feed_lines = len(feed.splitlines())
        logger.debug(
            f"[{case_dir_path.name}] Delta {delta.label}-{delta.sample} mode={mode} "
            f"cmd: {' '.join(cmd)} feed_lines={feed_lines} "
            f"delta_file={safe_relpath(delta.path, case_dir_path)}"
        )
    out_dir = output_dir
    ensure_dirs(out_dir)
    remove_iter_outputs(out_dir, mode)
    code, elapsed, out, err = time_cmd(cmd, cwd=case_dir_path, timeout=timeout, feed=feed)
    if logger and code == 124:
        logger.warn(
            f"[{case_dir_path.name}] Delta {delta.label}-{delta.sample} mode={mode} "
            f"timed out after {timeout}s"
        )
    tag = f"delta-{delta.label}-{delta.sample}-{mode}"
    renamed = rename_iter_outputs(out_dir, mode, tag)
    return CliRunResult(
        mode=mode,
        label=delta.label,
        sample=delta.sample,
        exit_code=code,
        elapsed=elapsed,
        stderr=err,
        stdout=out,
        iter_paths=renamed,
    )


_DELTA_OP_RE = re.compile(r"^(delete|insert)\s+([A-Za-z0-9_]+)\((.*)\)(?:\s+(.+))?$")


def _parse_delta_operation(line: str) -> Optional[Tuple[str, str, Tuple[str, ...], Optional[str]]]:
    stripped = line.strip()
    if not stripped or stripped == "commit" or stripped == "q":
        return None
    match = _DELTA_OP_RE.match(stripped)
    if not match:
        return None
    op = match.group(1)
    relation = match.group(2)
    args = tuple(part.strip() for part in match.group(3).split(",") if part.strip())
    prob = match.group(4).strip() if op == "insert" and match.group(4) else None
    return op, relation, args, prob


def _reset_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    ensure_dirs(path)


def _relation_rows(input_dir: Path, relation: str) -> Tuple[List[Tuple[str, ...]], List[str], bool]:
    facts_path = input_dir / f"{relation}.facts"
    facts = _read_facts(facts_path)
    prob_path = facts_path.with_suffix(".prob")
    has_prob = prob_path.exists()
    probs = [str(prob) for prob in _read_probabilities(prob_path, len(facts))]
    if len(probs) < len(facts):
        probs.extend(["1.0"] * (len(facts) - len(probs)))
    return facts, probs[:len(facts)], has_prob


def materialize_delta_input(
        base_input_dir: Path,
        delta_path: Path,
        output_input_dir: Path,
        logger: Optional[Logger] = None,
) -> Dict[str, Any]:
    _reset_dir(output_input_dir)
    shutil.copytree(base_input_dir, output_input_dir, dirs_exist_ok=True)

    cache: Dict[str, Tuple[List[Tuple[str, ...]], List[str], bool]] = {}
    touched: set[str] = set()
    stats: Dict[str, Any] = {
        "deletes": 0,
        "inserts": 0,
        "delete_missing": 0,
        "insert_existing": 0,
        "relations": {},
    }

    def rows_for(relation: str) -> Tuple[List[Tuple[str, ...]], List[str], bool]:
        if relation not in cache:
            cache[relation] = _relation_rows(output_input_dir, relation)
        return cache[relation]

    for raw in delta_path.read_text(encoding="utf-8").splitlines():
        parsed = _parse_delta_operation(raw)
        if parsed is None:
            continue
        op, relation, fact, prob = parsed
        facts, probs, has_prob = rows_for(relation)
        touched.add(relation)
        rel_stats = stats["relations"].setdefault(
            relation,
            {"deletes": 0, "inserts": 0, "delete_missing": 0, "insert_existing": 0},
        )
        if op == "delete":
            stats["deletes"] += 1
            rel_stats["deletes"] += 1
            try:
                idx = facts.index(fact)
            except ValueError:
                stats["delete_missing"] += 1
                rel_stats["delete_missing"] += 1
                continue
            facts.pop(idx)
            if idx < len(probs):
                probs.pop(idx)
            continue

        stats["inserts"] += 1
        rel_stats["inserts"] += 1
        if fact in facts:
            stats["insert_existing"] += 1
            rel_stats["insert_existing"] += 1
            continue
        facts.append(fact)
        probs.append(prob or "1.0")
        has_prob = True
        cache[relation] = (facts, probs, has_prob)

    for relation in sorted(touched):
        facts, probs, has_prob = rows_for(relation)
        facts_path = output_input_dir / f"{relation}.facts"
        _write_facts(facts_path, facts)
        if has_prob or probs:
            _write_probabilities(facts_path.with_suffix(".prob"), probs)

    if logger and (stats["delete_missing"] or stats["insert_existing"]) and logger.verbose:
        logger.debug(
            f"Materialized delta input {safe_relpath(delta_path, base_input_dir.parent)}: "
            f"delete_missing={stats['delete_missing']} insert_existing={stats['insert_existing']}"
        )
    return stats


def run_materialized_full_for_delta(
        case_dir_path: Path,
        delta: DeltaFile,
        timeout: int,
        output_dir: Path,
        run_args: Optional[List[str]] = None,
        logger: Optional[Logger] = None,
) -> CliRunResult:
    exe = case_dir_path / "compute"
    ensure_dirs(output_dir)
    input_dir = output_dir / "materialized-input"
    stats = materialize_delta_input(case_dir_path / "input", delta.path, input_dir, logger)
    out_dir_rel = safe_relpath(output_dir, case_dir_path)
    input_dir_rel = safe_relpath(input_dir, case_dir_path)
    log_path = output_dir / f"log_{case_dir_path.name}_{delta.label}_{delta.sample}_full"
    log_rel = safe_relpath(log_path, case_dir_path)
    cmd = [
        f"./{exe.name}",
        "-F", input_dir_rel,
        "-D", out_dir_rel,
        "--setmode", "full",
        "--logfile", log_rel,
    ]
    if run_args:
        cmd.extend(run_args)
    if logger and logger.verbose:
        logger.debug(
            f"[{case_dir_path.name}] Delta {delta.label}-{delta.sample} mode=full "
            f"materialized cmd: {' '.join(cmd)} input={input_dir_rel}"
        )
    code, elapsed, out, err = time_cmd(cmd, cwd=case_dir_path, timeout=timeout, feed="setmode full\nq\n")
    if logger and code == 124:
        logger.warn(
            f"[{case_dir_path.name}] Delta {delta.label}-{delta.sample} mode=full "
            f"materialized run timed out after {timeout}s"
        )
    tag = f"delta-{delta.label}-{delta.sample}-full"
    facts = output_dir / "facts.prob"
    iter_name = "fact-iter1-full.prob"
    iter_paths: Dict[str, Path] = {}
    if facts.exists():
        dest = output_dir / f"{tag}-{iter_name}"
        try:
            shutil.move(str(facts), str(dest))
        except Exception:
            shutil.copy2(facts, dest)
            facts.unlink(missing_ok=True)  # type: ignore[attr-defined]
        iter_paths[iter_name] = dest
    return CliRunResult(
        mode="full",
        label=delta.label,
        sample=delta.sample,
        exit_code=code,
        elapsed=elapsed,
        stderr=err,
        stdout=out,
        iter_paths=iter_paths,
        materialized_final=True,
        materialized_input=input_dir,
        materialized_stats=stats,
    )


def first_iter_path(result: CliRunResult) -> Optional[Path]:
    for fname, path in result.iter_paths.items():
        if "iter1" in fname:
            return path
    return None


def read_iter_maps(result: CliRunResult) -> Dict[int, Dict[str, float]]:
    """Load all iter probability maps keyed by iteration number."""
    maps: Dict[int, Dict[str, float]] = {}
    for fname, path in result.iter_paths.items():
        if "iter" not in fname:
            continue
        try:
            # extract first number after "iter"
            stem = fname
            if "iter" in stem:
                tail = stem.split("iter", 1)[1]
                num = ""
                for ch in tail:
                    if ch.isdigit():
                        num += ch
                    else:
                        break
                if num:
                    maps[int(num)] = read_souffle_prob_file(path)
        except Exception:
            continue
    return maps


def read_initial_map(output_dir: Path) -> Optional[Dict[str, float]]:
    """Load the initial (turn-1) probability map if facts.prob exists."""
    path = output_dir / "facts.prob"
    if not path.exists():
        return None
    try:
        return read_souffle_prob_file(path)
    except Exception:
        return None


def compare_maps(label: str, a: Optional[Dict[str, float]], b: Optional[Dict[str, float]], tol: float = 1e-6) -> Dict[str, Any]:
    if a is None or b is None:
        missing = "lhs" if a is None else "rhs"
        return {"label": label, "ok": False, "mismatches": None, "max_abs_delta": None, "reason": f"{missing} missing"}
    ok, mismatches, max_delta = compare_prob_maps(a, b, tol=tol)
    return {
        "label": label,
        "ok": ok,
        "mismatches": mismatches,
        "max_abs_delta": max_delta,
        **({} if ok else {"reason": ""}),
    }


def _find_latest_log(case_dir: Path, delta: DeltaFile, mode: str, output_dir: Path) -> Optional[Path]:
    pattern = f"log_{case_dir.name}_{delta.label}_{delta.sample}_{mode}*.json"
    logs = []
    if output_dir.exists():
        logs.extend(output_dir.glob(pattern))
    if not logs:
        logs.extend(case_dir.glob(pattern))
    logs = sorted(logs, key=lambda p: p.stat().st_mtime, reverse=True)
    return logs[0] if logs else None


def _parse_inc_stdout(stdout: str) -> Dict[str, Any]:
    if not stdout:
        return {}
    stats: Dict[int, Dict[str, Dict[str, Any]]] = {}
    inc_analyze_entries: List[Dict[str, Any]] = []
    dep_graph_entries: List[Dict[str, Any]] = []
    inc_naive_profile_entries: List[Dict[str, Any]] = []
    fc_profile_entries: List[Dict[str, Any]] = []
    region_touches_entries: List[Dict[str, Any]] = []
    dag_fast_path_entries: List[Dict[str, Any]] = []
    inc_regional_plan_entries: List[Dict[str, Any]] = []
    inc_regional_timing_entries: List[Dict[str, Any]] = []
    inc_regional_rebuild_entries: List[Dict[str, Any]] = []
    inc_regional_final_entries: List[Dict[str, Any]] = []
    inc_regional_insert_profile_entries: List[Dict[str, Any]] = []
    inc_regional_calibrate_profile_entries: List[Dict[str, Any]] = []
    inc_regional_analyze_entries: List[Dict[str, Any]] = []
    inc_regional_expand_events: List[Dict[str, Any]] = []
    inc_regional_overlap_entries: List[Dict[str, Any]] = []
    inc_delete_profile_entries: List[Dict[str, Any]] = []
    wmc_profile_entries: List[Dict[str, Any]] = []
    apply_delta_ops_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) apply_delta_ops: "
        r"delTuples=(\d+) delRuleApps=(\d+) delFacts=(\d+) "
        r"insTuples=(\d+) insRuleApps=(\d+) insFacts=(\d+)$"
    )
    apply_delta_view_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) apply_delta_view: "
        r"insNodes=(\d+) insEdges=(\d+) delNodes=(\d+) delEdges=(\d+)$"
    )
    apply_delta_graph_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) apply_delta_graph: "
        r"totalNodes=(\d+) totalEdges=(\d+)$"
    )
    pruned_delta_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) pruned_delta: "
        r"insNodes=(\d+) insEdges=(\d+) delNodes=(\d+) delEdges=(\d+) "
        r"totalNodes=(\d+) totalEdges=(\d+)$"
    )
    pruned_ratio_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) pruned_delta_ratio: "
        r"insNodes=([^ ]+) insEdges=([^ ]+) delNodes=([^ ]+) delEdges=([^ ]+)$"
    )
    reach_ratio_re = re.compile(
        r"^\[inc-iter (\d+)\] mode=(\S+) deltaReach_ratio: "
        r"insNodes=([^ ]+) insEdges=([^ ]+) reachNodes=(\d+) reachEdges=(\d+)$"
    )
    inc_analyze_re = re.compile(r"^\[inc-analyze\]\s+(.+)$")
    inc_naive_profile_re = re.compile(r"^\[inc-naive-profile\]\s+(.+)$")
    fc_profile_re = re.compile(r"^\[fc-profile\]\s+(.+)$")
    dep_graph_re = re.compile(r"^\[dep-graph\]\s+(.+)$")
    inc_regional_plan_re = re.compile(r"^\[inc-regional-plan\]\s+(.+)$")
    inc_regional_timing_re = re.compile(r"^\[inc-regional\]\s+timing\(ms\):\s+(.+)$")
    inc_regional_rebuild_re = re.compile(r"^\[inc-regional rebuild\]\s+timing\(ms\):\s+(.+)$")
    inc_regional_final_re = re.compile(r"^\[inc-regional-final\]\s+(.+)$")
    inc_regional_insert_profile_re = re.compile(r"^\[inc-regional-insert-profile\]\s+(.+)$")
    inc_regional_calibrate_profile_re = re.compile(r"^\[inc-regional-calibrate-profile\]\s+(.+)$")
    inc_regional_analyze_re = re.compile(
        r"^\[inc-regional\]\s+analyze\s+region_nodes=(\d+)\s+region_edges=(\d+)\s+"
        r"dr_nodes=(\d+)\s+dr_edges=(\d+)$"
    )
    inc_delete_profile_re = re.compile(r"^\[inc-delete-profile\]\s+(.+)$")
    wmc_profile_re = re.compile(r"^\[wmc-profile\]\s+(.+)$")
    region_touches_re = re.compile(r"^\[inc-regional\]\s+regionTouchesCycle\s+(\w+)=(\d+)$")
    dag_fast_path_re = re.compile(r"^\[inc-regional\]\s+dag_fast_path=(\d+)\s+dag_build_ms=([0-9.]+)$")
    region_close_inputs_re = re.compile(
        r"^\[inc-regional\]\s+close region inputs reason=([^\s]+)\s+added_nodes=(\d+)\s+added_edges=(\d+)\b"
    )
    region_overlap_re = re.compile(
        r"^\[inc-regional\]\s+overlap closure reason=([^\s]+)\s+added_nodes=(\d+)\b"
    )
    region_overlap_forward_re = re.compile(
        r"^\[inc-regional\]\s+overlap closure forward-expand reason=([^\s]+)\s+"
        r"added_nodes=(\d+)\s+added_edges=(\d+)\b"
    )
    region_overlap_multi_re = re.compile(
        r"^\[inc-regional\]\s+overlap\s+multi_nodes=(\d+)\s+multi_nodes_trivial=(\d+)\s+multi_nodes_total=(\d+)$"
    )
    region_expand_missing_anchor_re = re.compile(
        r"^\[inc-regional\]\s+expanded region after missing anchors:\s+"
        r"attempt=(\d+)\s+added_nodes=(\d+)\s+added_edges=(\d+)\s+failed=(\d+)"
    )
    region_boundary_empty_re = re.compile(
        r"^\[inc-regional\]\s+boundary empty but region misses delta-reachable nodes; "
        r"expanding region to delta-reachable$"
    )
    region_near_full_re = re.compile(
        r"^\[inc-regional\]\s+expand region to delta-reach \(near-full ratio=([0-9.]+)\)$"
    )

    def _slot(iteration: int, mode: str) -> Dict[str, Any]:
        return stats.setdefault(iteration, {}).setdefault(mode, {})

    for line in stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        m = apply_delta_ops_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["apply_delta_ops"] = {
                "delTuples": int(m.group(3)),
                "delRuleApps": int(m.group(4)),
                "delFacts": int(m.group(5)),
                "insTuples": int(m.group(6)),
                "insRuleApps": int(m.group(7)),
                "insFacts": int(m.group(8)),
            }
            continue
        m = apply_delta_view_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["apply_delta_view"] = {
                "insNodes": int(m.group(3)),
                "insEdges": int(m.group(4)),
                "delNodes": int(m.group(5)),
                "delEdges": int(m.group(6)),
            }
            continue
        m = apply_delta_graph_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["apply_delta_graph"] = {
                "totalNodes": int(m.group(3)),
                "totalEdges": int(m.group(4)),
            }
            continue
        m = pruned_delta_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["pruned_delta"] = {
                "insNodes": int(m.group(3)),
                "insEdges": int(m.group(4)),
                "delNodes": int(m.group(5)),
                "delEdges": int(m.group(6)),
                "totalNodes": int(m.group(7)),
                "totalEdges": int(m.group(8)),
            }
            continue
        m = pruned_ratio_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["pruned_delta_ratio"] = {
                "insNodes": m.group(3),
                "insEdges": m.group(4),
                "delNodes": m.group(5),
                "delEdges": m.group(6),
            }
            continue
        m = reach_ratio_re.match(line)
        if m:
            iteration = int(m.group(1))
            mode = m.group(2)
            slot = _slot(iteration, mode)
            slot["deltaReach_ratio"] = {
                "insNodes": m.group(3),
                "insEdges": m.group(4),
                "reachNodes": int(m.group(5)),
                "reachEdges": int(m.group(6)),
            }
            continue
        m = inc_analyze_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_analyze_entries.append(entry)
            continue
        m = inc_naive_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_naive_profile_entries.append(entry)
            continue
        m = fc_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                fc_profile_entries.append(entry)
            continue
        m = dep_graph_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                dep_graph_entries.append(entry)
            continue
        m = inc_regional_plan_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_plan_entries.append(entry)
            continue
        m = inc_regional_timing_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_timing_entries.append(entry)
            continue
        m = inc_regional_rebuild_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_rebuild_entries.append(entry)
            continue
        m = inc_regional_final_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_final_entries.append(entry)
            continue
        m = inc_regional_insert_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_insert_profile_entries.append(entry)
            continue
        m = inc_regional_calibrate_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_regional_calibrate_profile_entries.append(entry)
            continue
        m = inc_regional_analyze_re.match(line)
        if m:
            inc_regional_analyze_entries.append({
                "region_nodes": int(m.group(1)),
                "region_edges": int(m.group(2)),
                "dr_nodes": int(m.group(3)),
                "dr_edges": int(m.group(4)),
            })
            continue
        m = inc_delete_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                inc_delete_profile_entries.append(entry)
            continue
        m = wmc_profile_re.match(line)
        if m:
            payload = m.group(1)
            entry: Dict[str, Any] = {}
            for token in payload.split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    entry[key] = _parse_stat_value(value)
            if entry:
                wmc_profile_entries.append(entry)
            continue
        m = region_touches_re.match(line)
        if m:
            region_touches_entries.append({
                "phase": m.group(1),
                "value": int(m.group(2)),
            })
            continue
        m = dag_fast_path_re.match(line)
        if m:
            dag_fast_path_entries.append({
                "value": int(m.group(1)),
                "build_ms": _parse_stat_value(m.group(2)),
            })
            continue
        m = region_close_inputs_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "close_inputs",
                "reason": m.group(1),
                "added_nodes": int(m.group(2)),
                "added_edges": int(m.group(3)),
            })
            continue
        m = region_overlap_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "overlap_closure",
                "reason": m.group(1),
                "added_nodes": int(m.group(2)),
            })
            continue
        m = region_overlap_forward_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "overlap_forward",
                "reason": m.group(1),
                "added_nodes": int(m.group(2)),
                "added_edges": int(m.group(3)),
            })
            continue
        m = region_overlap_multi_re.match(line)
        if m:
            inc_regional_overlap_entries.append({
                "multi_nodes": int(m.group(1)),
                "multi_nodes_trivial": int(m.group(2)),
                "multi_nodes_total": int(m.group(3)),
            })
            continue
        m = region_expand_missing_anchor_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "expand_missing_anchor",
                "attempt": int(m.group(1)),
                "added_nodes": int(m.group(2)),
                "added_edges": int(m.group(3)),
                "failed": int(m.group(4)),
            })
            continue
        m = region_boundary_empty_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "boundary_empty_expand",
            })
            continue
        m = region_near_full_re.match(line)
        if m:
            inc_regional_expand_events.append({
                "kind": "near_full_expand",
                "ratio": _parse_stat_value(m.group(1)),
            })
            continue
    if not stats:
        out: Dict[str, Any] = {}
        if inc_analyze_entries:
            out["inc_analyze"] = inc_analyze_entries
        if inc_delete_profile_entries:
            out["inc_delete_profile"] = inc_delete_profile_entries
        if wmc_profile_entries:
            out["wmc_profile"] = wmc_profile_entries
        if inc_naive_profile_entries:
            out["inc_naive_profile"] = inc_naive_profile_entries
        if fc_profile_entries:
            out["fc_profile"] = fc_profile_entries
        if dep_graph_entries:
            out["dep_graph"] = dep_graph_entries
        if inc_regional_insert_profile_entries:
            out["inc_regional_insert_profile"] = inc_regional_insert_profile_entries
        if inc_regional_calibrate_profile_entries:
            out["inc_regional_calibrate_profile"] = inc_regional_calibrate_profile_entries
        if inc_regional_overlap_entries:
            out["inc_regional_overlap"] = inc_regional_overlap_entries
        return out
    out: Dict[str, Any] = {}
    for iteration, modes in sorted(stats.items()):
        out[str(iteration)] = modes
    if inc_analyze_entries:
        out["inc_analyze"] = inc_analyze_entries
    if inc_naive_profile_entries:
        out["inc_naive_profile"] = inc_naive_profile_entries
    if fc_profile_entries:
        out["fc_profile"] = fc_profile_entries
    if dep_graph_entries:
        out["dep_graph"] = dep_graph_entries
    if inc_regional_plan_entries:
        out["inc_regional_plan"] = inc_regional_plan_entries
    if inc_regional_timing_entries:
        out["inc_regional_timing"] = inc_regional_timing_entries
    if inc_regional_rebuild_entries:
        out["inc_regional_rebuild"] = inc_regional_rebuild_entries
    if inc_regional_final_entries:
        out["inc_regional_final"] = inc_regional_final_entries
    if inc_regional_insert_profile_entries:
        out["inc_regional_insert_profile"] = inc_regional_insert_profile_entries
    if inc_regional_calibrate_profile_entries:
        out["inc_regional_calibrate_profile"] = inc_regional_calibrate_profile_entries
    if inc_regional_analyze_entries:
        out["inc_regional_analyze"] = inc_regional_analyze_entries
    if inc_regional_overlap_entries:
        out["inc_regional_overlap"] = inc_regional_overlap_entries
    if inc_delete_profile_entries:
        out["inc_delete_profile"] = inc_delete_profile_entries
    if wmc_profile_entries:
        out["wmc_profile"] = wmc_profile_entries
    if region_touches_entries:
        out["region_touches_cycle"] = region_touches_entries
    if dag_fast_path_entries:
        out["dag_fast_path"] = dag_fast_path_entries
    if inc_regional_expand_events:
        out["inc_regional_expands"] = inc_regional_expand_events
    return out


def _parse_stage_log(log_path: Path) -> Dict[str, Any]:
    def _stage_time(stage: Dict[str, Any]) -> Optional[float]:
        """Extract a stage duration; accept seconds or ms keys."""
        if "time_seconds" in stage and stage["time_seconds"] is not None:
            return float(stage["time_seconds"])
        if "time_ms" in stage and stage["time_ms"] is not None:
            return float(stage["time_ms"]) / 1000.0
        if "time" in stage and stage["time"] is not None:
            return float(stage["time"])
        return None

    def _parse_fc_logs(logs: Dict[str, Any]) -> Dict[str, Any]:
        info: Dict[str, Any] = {}
        for line in logs.get("INFO", []) if isinstance(logs, dict) else []:
            line = str(line)
            if "Total rounds" in line:
                try:
                    info["fc_total_rounds"] = int(line.split(":")[1].strip().split()[0])
                except Exception:
                    pass
            elif "Insertion time" in line and "ms" in line:
                try:
                    num = line.split(":")[1].strip().split()[0]
                    info["fc_insertion_ms"] = float(num)
                except Exception:
                    pass
            elif "Variable ordering takes" in line and "ms" in line:
                try:
                    num = line.split()[-2]
                    info["fc_var_order_ms"] = float(num)
                except Exception:
                    pass
        return info

    try:
        data = json.loads(log_path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    turns = data.get("turns", [])
    out_turns = []
    for turn in turns:
        stage_map = {}
        raw_stages = []
        sem_info_obj = {}
        for stage in turn.get("stages", []):
            name = stage.get("name", "") or ""
            t = _stage_time(stage)
            if t is None:
                continue
            stage_map[name] = t
            if "SEMINAIVE" in name:
                stage_map.setdefault("SEM", t)
            if "PRUNING" in name:
                stage_map.setdefault("PRN", t)
            if "FORWARD_COMPILATION" in name:
                stage_map.setdefault("FC", t)
            if "WEIGHTED_MODEL_COUNTING" in name:
                stage_map.setdefault("WMC", t)
            if not sem_info_obj and "SEMINAIVE" in name:
                sem_info_obj = stage.get("info", {}) or {}
            entry = {
                "name": name,
                "time_seconds": t,
                "peak_mem_kb": stage.get("peak_mem_kb"),
            }
            # include non-empty info/logs blobs for transparency
            if stage.get("info"):
                entry["info"] = stage.get("info")
            if stage.get("logs"):
                entry["logs"] = stage.get("logs")
            raw_stages.append({k: v for k, v in entry.items() if v not in (None, "", {})})
        info_obj = {}
        for stage in turn.get("stages", []):
            if "FORWARD_COMPILATION" in stage.get("name", ""):
                info_obj = stage.get("info", {}) or {}
                # parse additional structured fields from logs if available
                fc_log_bits = _parse_fc_logs(stage.get("logs", {}) or {})
                info_obj = {**fc_log_bits, **info_obj}
                break
        sem = (
            stage_map.get("SEMINAIVE_FULL")
            or stage_map.get("SEMINAIVE_INC")
            or stage_map.get("SEM")
        )
        prn = (
            stage_map.get("PRUNING_FULL")
            or stage_map.get("PRUNING_INC")
            or stage_map.get("PRN")
        )
        fc = (
            stage_map.get("FORWARD_COMPILATION_FULL")
            or stage_map.get("FORWARD_COMPILATION_INC")
            or stage_map.get("FC")
        )
        wmc = (
            stage_map.get("WEIGHTED_MODEL_COUNTING_FULL")
            or stage_map.get("WEIGHTED_MODEL_COUNTING_INC")
            or stage_map.get("WMC")
        )
        turn_obj = {
            "index": turn.get("index"),
            "mode": turn.get("mode"),
            "time_seconds": turn.get("time_seconds"),
            "peak_mem_kb": max((s.get("peak_mem_kb", 0) or 0) for s in turn.get("stages", [])) if turn.get("stages") else None,
        }
        turn_obj = {k: v for k, v in turn_obj.items() if v is not None}
        turn_info = turn.get("info") or {}
        if turn_info:
            turn_obj["info"] = turn_info
            for key in (
                "fc_setup_ms",
                "fc_setup_scope",
                "full_inc_old_prune_ms",
                "full_inc_diff_remap_ms",
            ):
                if key in turn_info:
                    turn_obj[key] = turn_info[key]

        stage_block = {k: v for k, v in {
            "SEM": sem,
            "PRN": prn,
            "FC": fc,
            "WMC": wmc,
        }.items() if v is not None}
        if stage_block:
            turn_obj["stages"] = stage_block

        fc_info = {
            "fc_live_nodes": info_obj.get("live_nodes"),
            "fc_dead_nodes": info_obj.get("dead_nodes"),
            "fc_total_nodes": info_obj.get("total_nodes"),
            "fc_memory_mb": info_obj.get("memory_usage_mb"),
            "fc_reorder_runtime": info_obj.get("reordering_runtime"),
            "fc_cache_hit_rate": info_obj.get("cache_hit_rate"),
            "fc_cache_hits": info_obj.get("cache_hits"),
            "fc_cache_lookups": info_obj.get("cache_lookups"),
            "fc_total_rounds": info_obj.get("fc_total_rounds"),
            "fc_insertion_ms": info_obj.get("fc_insertion_ms"),
            "fc_var_order_ms": info_obj.get("fc_var_order_ms"),
        }
        for k, v in fc_info.items():
            if v is not None:
                turn_obj[k] = v

        sem_info = {
            "dred_phase": sem_info_obj.get("dred_phase"),
            "dred_del_ruleapp_recorded": sem_info_obj.get("dred_del_ruleapp_recorded"),
            "dred_del_ruleapp_delta_delta": sem_info_obj.get("dred_del_ruleapp_delta_delta"),
            "dred_del_ruleapp_overdelete": sem_info_obj.get("dred_del_ruleapp_overdelete"),
            "dred_del_complete_scan_calls": sem_info_obj.get("dred_del_complete_scan_calls"),
            "dred_del_complete_scan_elems": sem_info_obj.get("dred_del_complete_scan_elems"),
            "dred_del_delta_tuples": sem_info_obj.get("dred_del_delta_tuples"),
            "dred_del_delta_ruleapps": sem_info_obj.get("dred_del_delta_ruleapps"),
            "dred_del_ruleapp_erases": sem_info_obj.get("dred_del_ruleapp_erases"),
            "dred_del_tuple_deletes": sem_info_obj.get("dred_del_tuple_deletes"),
            "dred_ins_ruleapp_recorded": sem_info_obj.get("dred_ins_ruleapp_recorded"),
            "dred_ins_ruleapp_delta_delta": sem_info_obj.get("dred_ins_ruleapp_delta_delta"),
            "dred_ins_ruleapp_rederive_erases": sem_info_obj.get("dred_ins_ruleapp_rederive_erases"),
            "dred_ins_delta_tuples": sem_info_obj.get("dred_ins_delta_tuples"),
            "dred_ins_delta_ruleapps": sem_info_obj.get("dred_ins_delta_ruleapps"),
            "dred_rederive_delta_tuples": sem_info_obj.get("dred_rederive_delta_tuples"),
            "dred_rederive_delta_ruleapps": sem_info_obj.get("dred_rederive_delta_ruleapps"),
            "dred_ins_ruleapp_merges": sem_info_obj.get("dred_ins_ruleapp_merges"),
            "dred_ins_tuple_inserts": sem_info_obj.get("dred_ins_tuple_inserts"),
            "dred_del_time_total_ns": sem_info_obj.get("dred_del_time_total_ns"),
            "dred_ins_time_total_ns": sem_info_obj.get("dred_ins_time_total_ns"),
            "dred_red_time_total_ns": sem_info_obj.get("dred_red_time_total_ns"),
        }
        for k, v in sem_info.items():
            if v is not None:
                turn_obj[k] = v

        if raw_stages:
            turn_obj["raw_stages"] = raw_stages

        out_turns.append(turn_obj)
    return {"turns": out_turns}


def _parse_stat_value(raw: str) -> Any:
    try:
        return int(raw)
    except ValueError:
        try:
            return float(raw)
        except ValueError:
            return raw


def _parse_dred_lines(output: str) -> Dict[str, List[Dict[str, Any]]]:
    dred: List[Dict[str, Any]] = []
    dred_scc: List[Dict[str, Any]] = []
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("[seminaive-dred-scc]"):
            payload = line[len("[seminaive-dred-scc]"):].strip()
            target = dred_scc
        elif line.startswith("[seminaive-dred]"):
            payload = line[len("[seminaive-dred]"):].strip()
            target = dred
        else:
            continue
        entry: Dict[str, Any] = {}
        for token in payload.split():
            if "=" in token:
                key, value = token.split("=", 1)
                entry[key] = _parse_stat_value(value)
            elif token in ("del", "ins"):
                entry["section"] = token
        if entry:
            target.append(entry)
    return {"dred": dred, "dred_scc": dred_scc}


def _count_commits(path: Path) -> int:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except Exception:
        return 0
    count = 0
    for line in lines:
        if line.strip().lower() == "commit":
            count += 1
    return count


def _count_delta_ops(path: Path) -> Tuple[int, int]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except Exception:
        return 0, 0
    deletes = 0
    inserts = 0
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("delete "):
            deletes += 1
        elif stripped.startswith("insert "):
            inserts += 1
    return deletes, inserts


def _final_iter(maps: Dict[int, Dict[str, float]]) -> Optional[int]:
    return max(maps.keys()) if maps else None


# -----------------------------
# Run cases
# -----------------------------


@dataclass
class RunCfg:
    base_dir: Path
    cases: List[int]
    timeout: int
    delta_timeout_multiplier: float
    labels: List[str]
    samples_per_label: int
    delta_runs: int
    randomize: bool
    seed: Optional[int]
    delta_root: Optional[Path]
    output_dir: str
    run_args: List[str]
    compare_all: bool
    inc_regional_only: bool
    compare_full_inc: bool
    run_modes: List[str]
    materialized_full: bool
    logger: Logger


def _strip_setmode_args(args: List[str], logger: Logger) -> List[str]:
    filtered: List[str] = []
    skipped = False
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--setmode":
            skipped = True
            i += 2
            continue
        if arg.startswith("--setmode="):
            skipped = True
            i += 1
            continue
        filtered.append(arg)
        i += 1
    if skipped:
        logger.warn("Ignoring --setmode in --run-arg because the runner sets modes explicitly")
    return filtered


def run_case(n: int, cfg: RunCfg) -> None:
    name = case_name(n)
    cdir = case_dir(cfg.base_dir, n)
    out_dir = cdir / cfg.output_dir
    ensure_dirs(out_dir)

    delta_files = select_delta_files(
        cdir, cfg.labels, cfg.samples_per_label, cfg.randomize, cfg.seed, cfg.delta_root, cfg.logger
    )
    if not delta_files:
        cfg.logger.warn(f"[{name}] No delta files found matching {cfg.labels}; skipping case")
        return

    run_args = _strip_setmode_args(cfg.run_args, cfg.logger)
    run_mode_set = set(cfg.run_modes)
    run_inc_naive = "inc-naive" in run_mode_set
    run_inc_regional = "inc-regional" in run_mode_set
    run_inc_full = "inc-full" in run_mode_set
    run_full_inc_naive = "full-inc-naive" in run_mode_set
    run_full_inc_regional = "full-inc-regional" in run_mode_set
    if cfg.logger.verbose:
        cfg.logger.debug(
            f"[{name}] Run config: labels={cfg.labels} samples_per_label={cfg.samples_per_label} "
            f"delta_runs={cfg.delta_runs} shuffle={cfg.randomize} seed={cfg.seed} delta_root={cfg.delta_root} "
            f"run_modes={cfg.run_modes} materialized_full={cfg.materialized_full} "
            f"output_dir={safe_relpath(out_dir, cdir)}"
        )
        cfg.logger.debug(f"[{name}] Run args: {run_args}")
        selected = [f"{safe_relpath(d.path, cdir)}[{d.label}:{d.sample}]" for d in delta_files]
        cfg.logger.debug(f"[{name}] Delta files: {selected}")
    baseline_dir = out_dir / "baseline" / "full"
    try:
        baseline_full_path, baseline_elapsed = run_baseline(
            cdir,
            cfg.timeout,
            cfg.logger,
            output_dir=baseline_dir,
            mode="full",
            run_args=run_args,
        )
    except Exception as exc:
        cfg.logger.error(f"[{name}] Baseline failed: {exc}")
        return

    delta_timeout = cfg.timeout
    if cfg.delta_timeout_multiplier > 0 and baseline_elapsed > 0:
        scaled = int(math.ceil(baseline_elapsed * cfg.delta_timeout_multiplier))
        scaled = max(1, scaled)
        if scaled < delta_timeout:
            delta_timeout = scaled
            cfg.logger.info(
                f"[{name}] Per-delta timeout set to {delta_timeout}s "
                f"(baseline {baseline_elapsed:.3f}s × {cfg.delta_timeout_multiplier})"
            )

    baseline_map = read_souffle_prob_file(baseline_full_path)
    cfg.logger.info(
        f"[{name}] Baseline full keys={len(baseline_map)} saved to {baseline_full_path.relative_to(cdir)}"
    )

    for idx, delta in enumerate(delta_files, 1):
        commit_count = _count_commits(delta.path)
        delta_deletes, delta_inserts = _count_delta_ops(delta.path)
        delta_ops = delta_deletes + delta_inserts
        delta_alpha = (float(delta_deletes) / float(delta_ops)) if delta_ops else None
        is_mixed = commit_count <= 1
        delta_root = out_dir / f"delta-{delta.label}" / f"sample-{delta.sample}"
        for run_idx in range(1, cfg.delta_runs + 1):
            run_root = delta_root / f"run-{run_idx}"
            cfg.logger.info(
                f"[{name}] Delta {idx}/{len(delta_files)} run {run_idx}/{cfg.delta_runs} "
                f"→ {safe_relpath(delta.path, cdir)}"
            )
            if cfg.materialized_full and commit_count == 1:
                full_result = run_materialized_full_for_delta(
                    cdir,
                    delta,
                    timeout=delta_timeout,
                    output_dir=run_root / "full",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            else:
                if cfg.materialized_full and commit_count != 1:
                    cfg.logger.warn(
                        f"[{name}] Materialized full supports single-commit deltas; "
                        f"falling back to online full for {delta.label}-{delta.sample} "
                        f"(commits={commit_count})"
                    )
                full_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="full",
                    timeout=delta_timeout,
                    output_dir=run_root / "full",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            inc_naive_result = None
            if run_inc_naive:
                inc_naive_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="inc-naive",
                    timeout=delta_timeout,
                    output_dir=run_root / "inc-naive",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            inc_regional_result = None
            if run_inc_regional:
                inc_regional_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="inc-regional",
                    timeout=delta_timeout,
                    output_dir=run_root / "inc-regional",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            inc_full_result = None
            if run_inc_full:
                inc_full_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="inc-full",
                    timeout=delta_timeout,
                    output_dir=run_root / "inc-full",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            full_inc_naive_result = None
            if run_full_inc_naive:
                full_inc_naive_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="full-inc-naive",
                    timeout=delta_timeout,
                    output_dir=run_root / "full-inc-naive",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            full_inc_regional_result = None
            if run_full_inc_regional:
                full_inc_regional_result = run_cli_for_delta(
                    cdir,
                    delta,
                    mode="full-inc-regional",
                    timeout=delta_timeout,
                    output_dir=run_root / "full-inc-regional",
                    run_args=run_args,
                    logger=cfg.logger,
                )
            inc_naive_maps = read_iter_maps(inc_naive_result) if inc_naive_result else {}
            inc_regional_maps = read_iter_maps(inc_regional_result) if inc_regional_result else {}
            inc_full_maps = read_iter_maps(inc_full_result) if inc_full_result else {}
            full_inc_naive_maps = read_iter_maps(full_inc_naive_result) if full_inc_naive_result else {}
            full_inc_regional_maps = read_iter_maps(full_inc_regional_result) if full_inc_regional_result else {}
            full_maps = read_iter_maps(full_result)
            inc_naive_initial_map = read_initial_map(run_root / "inc-naive") if inc_naive_result else None
            inc_regional_initial_map = read_initial_map(run_root / "inc-regional") if inc_regional_result else None
            inc_full_initial_map = read_initial_map(run_root / "inc-full") if inc_full_result else None
            full_inc_naive_initial_map = read_initial_map(run_root / "full-inc-naive") if full_inc_naive_result else None
            full_inc_regional_initial_map = read_initial_map(run_root / "full-inc-regional") if full_inc_regional_result else None
            full_initial_map = read_initial_map(run_root / "full")
            base_map = baseline_map
            expected_final_iter = 1 if full_result.materialized_final else (commit_count if commit_count > 0 else _final_iter(full_maps))
            expected_iter1 = 1 if commit_count >= 1 else None
            inc_final_map = inc_naive_maps.get(expected_final_iter) if expected_final_iter else None
            full_final_map = full_maps.get(expected_final_iter) if expected_final_iter else None
            inc_regional_final_map = inc_regional_maps.get(expected_final_iter) if expected_final_iter else None
            inc_full_final_map = inc_full_maps.get(expected_final_iter) if expected_final_iter else None
            full_inc_naive_final_map = full_inc_naive_maps.get(expected_final_iter) if expected_final_iter else None
            full_inc_regional_final_map = full_inc_regional_maps.get(expected_final_iter) if expected_final_iter else None

            reason = None
            if full_result.exit_code != 0:
                msg = f"full exit={full_result.exit_code}"
                if full_result.stderr.strip():
                    msg += f" stderr={full_result.stderr.strip()[:120]}"
                reason = msg
            elif run_inc_naive and inc_naive_result and inc_naive_result.exit_code != 0:
                reason = f"inc-naive exit={inc_naive_result.exit_code}"
            elif run_inc_regional and inc_regional_result and inc_regional_result.exit_code != 0:
                reason = f"inc-regional exit={inc_regional_result.exit_code}"
            elif run_inc_full and inc_full_result and inc_full_result.exit_code != 0:
                reason = f"inc-full exit={inc_full_result.exit_code}"
            elif run_full_inc_naive and full_inc_naive_result and full_inc_naive_result.exit_code != 0:
                reason = f"full-inc-naive exit={full_inc_naive_result.exit_code}"
            elif run_full_inc_regional and full_inc_regional_result and full_inc_regional_result.exit_code != 0:
                reason = f"full-inc-regional exit={full_inc_regional_result.exit_code}"
            elif not full_maps:
                reason = "full iter output missing"

            comparisons: List[Dict[str, Any]] = []
            primary_comparisons: List[Dict[str, Any]] = []

            def add_comp(comp: Dict[str, Any], primary: bool = False) -> None:
                if comp.get("reason") == "":
                    comp.pop("reason", None)
                comparisons.append(comp)
                if primary:
                    primary_comparisons.append(comp)

            if is_mixed:
                if not full_result.materialized_final:
                    comp_base_full_initial = compare_maps("base_vs_full_initial", base_map, full_initial_map)
                    add_comp(comp_base_full_initial, primary=True)
            else:
                comp_base_full_final = compare_maps("base_vs_full_final", base_map, full_final_map)
                add_comp(comp_base_full_final, primary=True)

            if run_inc_naive:
                if is_mixed:
                    comp_base_inc_naive_initial = compare_maps("base_vs_inc_naive_initial", base_map, inc_naive_initial_map)
                    add_comp(comp_base_inc_naive_initial, primary=True)
                    comp_inc_naive_full_final = compare_maps("inc_naive_final_vs_full_final", inc_final_map, full_final_map)
                    add_comp(comp_inc_naive_full_final, primary=True)
                else:
                    comp_base_inc_naive_final = compare_maps("base_vs_inc_naive_final", base_map, inc_final_map)
                    add_comp(comp_base_inc_naive_final, primary=True)
                    comp_inc_naive_full_1 = compare_maps(
                        "inc_naive_iter1_vs_full_iter1",
                        inc_naive_maps.get(expected_iter1) if expected_iter1 else None,
                        full_maps.get(expected_iter1) if expected_iter1 else None,
                    )
                    add_comp(comp_inc_naive_full_1, primary=True)
                    comp_inc_naive_full_final = compare_maps("inc_naive_final_vs_full_final", inc_final_map, full_final_map)
                    add_comp(comp_inc_naive_full_final, primary=True)

            if run_inc_regional:
                if is_mixed:
                    comp_base_inc_regional_initial = compare_maps(
                        "base_vs_inc_regional_initial", base_map, inc_regional_initial_map
                    )
                    add_comp(comp_base_inc_regional_initial, primary=True)
                    comp_inc_regional_full_final = compare_maps(
                        "inc_regional_final_vs_full_final", inc_regional_final_map, full_final_map
                    )
                    add_comp(comp_inc_regional_full_final, primary=True)
                else:
                    comp_base_inc_regional_final = compare_maps(
                        "base_vs_inc_regional_final", base_map, inc_regional_final_map
                    )
                    add_comp(comp_base_inc_regional_final, primary=False)
                    comp_inc_regional_full_1 = compare_maps(
                        "inc_regional_iter1_vs_full_iter1",
                        inc_regional_maps.get(expected_iter1) if expected_iter1 else None,
                        full_maps.get(expected_iter1) if expected_iter1 else None,
                    )
                    add_comp(comp_inc_regional_full_1, primary=True)
                    comp_inc_regional_full_final = compare_maps(
                        "inc_regional_final_vs_full_final", inc_regional_final_map, full_final_map
                    )
                    add_comp(comp_inc_regional_full_final, primary=True)
                if run_inc_naive and not is_mixed:
                    comp_inc_naive_regional_1 = compare_maps(
                        "inc_naive_iter1_vs_inc_regional_iter1",
                        inc_naive_maps.get(expected_iter1) if expected_iter1 else None,
                        inc_regional_maps.get(expected_iter1) if expected_iter1 else None,
                    )
                    add_comp(comp_inc_naive_regional_1, primary=False)
            if run_inc_full:
                if is_mixed:
                    comp_base_inc_full_initial = compare_maps(
                        "base_vs_inc_full_initial", base_map, inc_full_initial_map
                    )
                    add_comp(comp_base_inc_full_initial, primary=True)
                else:
                    comp_base_inc_full_final = compare_maps(
                        "base_vs_inc_full_final", base_map, inc_full_final_map
                    )
                    add_comp(comp_base_inc_full_final, primary=False)
                comp_inc_full_full_final = compare_maps(
                    "inc_full_final_vs_full_final",
                    inc_full_final_map,
                    full_final_map,
                )
                add_comp(comp_inc_full_full_final, primary=True)
            if run_full_inc_naive:
                if is_mixed:
                    comp_base_full_inc_naive_initial = compare_maps(
                        "base_vs_full_inc_naive_initial", base_map, full_inc_naive_initial_map
                    )
                    add_comp(comp_base_full_inc_naive_initial, primary=True)
                else:
                    comp_base_full_inc_naive_final = compare_maps(
                        "base_vs_full_inc_naive_final", base_map, full_inc_naive_final_map
                    )
                    add_comp(comp_base_full_inc_naive_final, primary=False)
                comp_full_inc_naive_inc_naive_final = compare_maps(
                    "full_inc_naive_final_vs_inc_naive_final",
                    full_inc_naive_final_map,
                    inc_final_map,
                    tol=DEFAULT_STAGED_COMPARE_TOL,
                )
                add_comp(comp_full_inc_naive_inc_naive_final, primary=True)
            if run_full_inc_regional:
                if is_mixed:
                    comp_base_full_inc_regional_initial = compare_maps(
                        "base_vs_full_inc_regional_initial", base_map, full_inc_regional_initial_map
                    )
                    add_comp(comp_base_full_inc_regional_initial, primary=True)
                else:
                    comp_base_full_inc_regional_final = compare_maps(
                        "base_vs_full_inc_regional_final", base_map, full_inc_regional_final_map
                    )
                    add_comp(comp_base_full_inc_regional_final, primary=False)
                comp_full_inc_regional_inc_regional_final = compare_maps(
                    "full_inc_regional_final_vs_inc_regional_final",
                    full_inc_regional_final_map,
                    inc_regional_final_map,
                    tol=DEFAULT_STAGED_COMPARE_TOL,
                )
                add_comp(comp_full_inc_regional_inc_regional_final, primary=True)
            overall_ok = all(c.get("ok") for c in primary_comparisons if c.get("ok") is not None) and reason is None
            comment = "" if not reason else reason
            max_delta = max((c.get("max_abs_delta") or 0.0 for c in primary_comparisons if c.get("max_abs_delta") is not None), default=0.0)

            meta = {
                "case": name,
                "delta_label": delta.label,
                "delta_sample": delta.sample,
                "delta_run": run_idx,
                "delta_run_total": cfg.delta_runs,
                "delta_file": safe_relpath(delta.path, cdir),
                "delta_commits": commit_count,
                "delta_deletes": delta_deletes,
                "delta_inserts": delta_inserts,
                "delta_alpha": delta_alpha,
                "delta_mixed": is_mixed,
                "baseline": {
                    "path": str(baseline_full_path.relative_to(cdir)),
                    "keys": len(baseline_map),
                    "mode": "full",
                },
                "inc_naive": {
                    "mode": "inc-naive",
                    "exit": inc_naive_result.exit_code if inc_naive_result else None,
                    "elapsed_s": inc_naive_result.elapsed if inc_naive_result else None,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in inc_naive_result.iter_paths.items()} if inc_naive_result else {},
                    "log": None,
                },
                "inc_regional": {
                    "mode": "inc-regional",
                    "exit": inc_regional_result.exit_code if inc_regional_result else None,
                    "elapsed_s": inc_regional_result.elapsed if inc_regional_result else None,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in inc_regional_result.iter_paths.items()} if inc_regional_result else {},
                    "log": None,
                },
                "inc_full": {
                    "mode": "inc-full",
                    "setmode": "sem=inc fc=full",
                    "exit": inc_full_result.exit_code if inc_full_result else None,
                    "elapsed_s": inc_full_result.elapsed if inc_full_result else None,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in inc_full_result.iter_paths.items()} if inc_full_result else {},
                    "log": None,
                },
                "full_inc_naive": {
                    "mode": "full-inc-naive",
                    "setmode": "sem=full fc=inc-naive",
                    "exit": full_inc_naive_result.exit_code if full_inc_naive_result else None,
                    "elapsed_s": full_inc_naive_result.elapsed if full_inc_naive_result else None,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in full_inc_naive_result.iter_paths.items()} if full_inc_naive_result else {},
                    "log": None,
                },
                "full_inc_regional": {
                    "mode": "full-inc-regional",
                    "setmode": "sem=full fc=inc-regional",
                    "exit": full_inc_regional_result.exit_code if full_inc_regional_result else None,
                    "elapsed_s": full_inc_regional_result.elapsed if full_inc_regional_result else None,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in full_inc_regional_result.iter_paths.items()} if full_inc_regional_result else {},
                    "log": None,
                },
                "full": {
                    "exit": full_result.exit_code,
                    "elapsed_s": full_result.elapsed,
                    "iter_paths": {k: str(v.relative_to(cdir)) for k, v in full_result.iter_paths.items()},
                    "log": None,
                    "materialized_final_input": full_result.materialized_final,
                    **(
                        {
                            "materialized_input": safe_relpath(full_result.materialized_input, cdir),
                            "materialized_stats": full_result.materialized_stats,
                        }
                        if full_result.materialized_final and full_result.materialized_input
                        else {}
                    ),
                },
                "compare": {
                    "ok": overall_ok,
                    **({"comment": comment} if comment else {}),
                    "details": comparisons,
                },
            }
            if not run_inc_naive:
                meta["inc_naive"]["skipped"] = True
            if not run_inc_regional:
                meta["inc_regional"]["skipped"] = True
            if not run_inc_full:
                meta["inc_full"]["skipped"] = True
            if not run_full_inc_naive:
                meta["full_inc_naive"]["skipped"] = True
            if not run_full_inc_regional:
                meta["full_inc_regional"]["skipped"] = True

            inc_naive_log_path = None
            if run_inc_naive:
                inc_naive_log_path = _find_latest_log(cdir, delta, mode="inc-naive", output_dir=run_root / "inc-naive")
            inc_regional_log_path = None
            if run_inc_regional:
                inc_regional_log_path = _find_latest_log(cdir, delta, mode="inc-regional", output_dir=run_root / "inc-regional")
            inc_full_log_path = None
            if run_inc_full:
                inc_full_log_path = _find_latest_log(
                    cdir, delta, mode="inc-full", output_dir=run_root / "inc-full"
                )
            full_inc_naive_log_path = None
            if run_full_inc_naive:
                full_inc_naive_log_path = _find_latest_log(
                    cdir, delta, mode="full-inc-naive", output_dir=run_root / "full-inc-naive"
                )
            full_inc_regional_log_path = None
            if run_full_inc_regional:
                full_inc_regional_log_path = _find_latest_log(
                    cdir, delta, mode="full-inc-regional", output_dir=run_root / "full-inc-regional"
                )
            full_log_path = _find_latest_log(cdir, delta, mode="full", output_dir=run_root / "full")
            if inc_naive_log_path and inc_naive_log_path.exists():
                meta["inc_naive"]["log"] = {
                    "path": safe_relpath(inc_naive_log_path, cdir),
                    "stages": _parse_stage_log(inc_naive_log_path),
                }
            if inc_regional_log_path and inc_regional_log_path.exists():
                meta["inc_regional"]["log"] = {
                    "path": safe_relpath(inc_regional_log_path, cdir),
                    "stages": _parse_stage_log(inc_regional_log_path),
                }
            if inc_full_log_path and inc_full_log_path.exists():
                meta["inc_full"]["log"] = {
                    "path": safe_relpath(inc_full_log_path, cdir),
                    "stages": _parse_stage_log(inc_full_log_path),
                }
            if full_inc_naive_log_path and full_inc_naive_log_path.exists():
                meta["full_inc_naive"]["log"] = {
                    "path": safe_relpath(full_inc_naive_log_path, cdir),
                    "stages": _parse_stage_log(full_inc_naive_log_path),
                }
            if full_inc_regional_log_path and full_inc_regional_log_path.exists():
                meta["full_inc_regional"]["log"] = {
                    "path": safe_relpath(full_inc_regional_log_path, cdir),
                    "stages": _parse_stage_log(full_inc_regional_log_path),
                }
            if full_log_path and full_log_path.exists():
                meta["full"]["log"] = {
                    "path": safe_relpath(full_log_path, cdir),
                    "stages": _parse_stage_log(full_log_path),
                }
            inc_naive_stdout_stats = _parse_inc_stdout(inc_naive_result.stdout or "") if inc_naive_result else {}
            if inc_naive_stdout_stats:
                meta["inc_naive"]["stdout_stats"] = inc_naive_stdout_stats
            inc_regional_stdout_stats = _parse_inc_stdout(inc_regional_result.stdout or "") if inc_regional_result else {}
            if inc_regional_stdout_stats:
                meta["inc_regional"]["stdout_stats"] = inc_regional_stdout_stats
            inc_full_stdout_stats = _parse_inc_stdout(inc_full_result.stdout or "") if inc_full_result else {}
            if inc_full_stdout_stats:
                meta["inc_full"]["stdout_stats"] = inc_full_stdout_stats
            full_inc_naive_stdout_stats = _parse_inc_stdout(full_inc_naive_result.stdout or "") if full_inc_naive_result else {}
            if full_inc_naive_stdout_stats:
                meta["full_inc_naive"]["stdout_stats"] = full_inc_naive_stdout_stats
            full_inc_regional_stdout_stats = _parse_inc_stdout(full_inc_regional_result.stdout or "") if full_inc_regional_result else {}
            if full_inc_regional_stdout_stats:
                meta["full_inc_regional"]["stdout_stats"] = full_inc_regional_stdout_stats
            full_stdout_stats = _parse_inc_stdout(full_result.stdout or "")
            if full_stdout_stats:
                meta["full"]["stdout_stats"] = full_stdout_stats
            inc_naive_dred = _parse_dred_lines(inc_naive_result.stdout or "") if inc_naive_result else {"dred": [], "dred_scc": []}
            if inc_naive_dred["dred"] or inc_naive_dred["dred_scc"]:
                meta["inc_naive"]["dred_stats"] = inc_naive_dred
            inc_regional_dred = _parse_dred_lines(inc_regional_result.stdout or "") if inc_regional_result else {"dred": [], "dred_scc": []}
            if inc_regional_dred["dred"] or inc_regional_dred["dred_scc"]:
                meta["inc_regional"]["dred_stats"] = inc_regional_dred
            inc_full_dred = _parse_dred_lines(inc_full_result.stdout or "") if inc_full_result else {"dred": [], "dred_scc": []}
            if inc_full_dred["dred"] or inc_full_dred["dred_scc"]:
                meta["inc_full"]["dred_stats"] = inc_full_dred
            full_inc_naive_dred = _parse_dred_lines(full_inc_naive_result.stdout or "") if full_inc_naive_result else {"dred": [], "dred_scc": []}
            if full_inc_naive_dred["dred"] or full_inc_naive_dred["dred_scc"]:
                meta["full_inc_naive"]["dred_stats"] = full_inc_naive_dred
            full_inc_regional_dred = _parse_dred_lines(full_inc_regional_result.stdout or "") if full_inc_regional_result else {"dred": [], "dred_scc": []}
            if full_inc_regional_dred["dred"] or full_inc_regional_dred["dred_scc"]:
                meta["full_inc_regional"]["dred_stats"] = full_inc_regional_dred
            full_dred = _parse_dred_lines(full_result.stdout or "")
            if full_dred["dred"] or full_dred["dred_scc"]:
                meta["full"]["dred_stats"] = full_dred
            meta_path = run_root / f"delta-{delta.label}-{delta.sample}-run-{run_idx}.json"
            meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
            if overall_ok:
                elapsed_parts = [f"full={full_result.elapsed:.3f}s"]
                if run_inc_naive and inc_naive_result:
                    elapsed_parts.insert(0, f"inc_naive={inc_naive_result.elapsed:.3f}s")
                if run_inc_regional and inc_regional_result:
                    elapsed_parts.insert(1 if run_inc_naive and inc_naive_result else 0, f"inc_regional={inc_regional_result.elapsed:.3f}s")
                if run_inc_full and inc_full_result:
                    elapsed_parts.append(f"inc_full={inc_full_result.elapsed:.3f}s")
                if run_full_inc_naive and full_inc_naive_result:
                    elapsed_parts.append(f"full_inc_naive={full_inc_naive_result.elapsed:.3f}s")
                if run_full_inc_regional and full_inc_regional_result:
                    elapsed_parts.append(f"full_inc_regional={full_inc_regional_result.elapsed:.3f}s")
                cfg.logger.info(
                    f"[{name}] ✓ Delta {delta.label}-{delta.sample} run {run_idx} OK "
                    f"({' '.join(elapsed_parts)} max|Δ|={max_delta:.2e})"
                )
            else:
                msgs = []
                for comp in comparisons:
                    if comp.get("ok"):
                        continue
                    label = comp.get("label", "?")
                    if comp.get("reason"):
                        msgs.append(f"{label}: {comp['reason']}")
                    else:
                        msgs.append(f"{label}: mismatches={comp.get('mismatches')} max|Δ|={comp.get('max_abs_delta'):.2e}")
                if reason:
                    msgs.append(f"overall reason={reason}")
                extra = "; ".join(msgs)
                cfg.logger.warn(f"[{name}] ⚠ Delta {delta.label}-{delta.sample} run {run_idx} {extra}")


# -----------------------------
# Collect
# -----------------------------


def collect_results(base_dir: Path, cases: List[int], output_dir: str, logger: Logger) -> None:
    def _format_time(value: Any) -> str:
        if value is None:
            return ""
        try:
            return f"{float(value):.6f}"
        except (TypeError, ValueError):
            return ""

    rows: List[List[str]] = []
    header = [
        "Case",
        "DeltaLabel",
        "DeltaSample",
        "DeltaRun",
        "IncNaiveTime",
        "FullTime",
        "IncNaiveExit",
        "FullExit",
        "Comment",
        "OK",
        "IncRegionalTime",
        "IncRegionalExit",
        "IncFullTime",
        "IncFullExit",
        "FullIncNaiveTime",
        "FullIncNaiveExit",
        "FullIncRegionalTime",
        "FullIncRegionalExit",
    ]
    for n in cases:
        name = case_name(n)
        cdir = case_dir(base_dir, n)
        out_dir = cdir / output_dir
        if not out_dir.exists():
            logger.warn(f"[{name}] Missing output dir {safe_relpath(out_dir, cdir)}; skipping")
            continue
        for meta_path in sorted(out_dir.rglob("delta-*.json")):
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except Exception:
                continue
            compare = meta.get("compare", {})
            inc_naive = meta.get("inc_naive", {})
            inc_regional = meta.get("inc_regional", {})
            inc_full = meta.get("inc_full", {})
            full_inc_naive = meta.get("full_inc_naive", {})
            full_inc_regional = meta.get("full_inc_regional", {})
            full = meta.get("full", {})
            rows.append([
                name,
                meta.get("delta_label", ""),
                meta.get("delta_sample", ""),
                str(meta.get("delta_run", "")),
                _format_time(inc_naive.get("elapsed_s", inc_naive.get("elapsed"))),
                _format_time(full.get("elapsed_s", full.get("elapsed"))),
                str(inc_naive.get("exit", "")),
                str(full.get("exit", "")),
                compare.get("comment", "") or "",
                "1" if compare.get("ok") else "0",
                _format_time(inc_regional.get("elapsed_s", inc_regional.get("elapsed"))),
                str(inc_regional.get("exit", "")),
                _format_time(inc_full.get("elapsed_s", inc_full.get("elapsed"))),
                str(inc_full.get("exit", "")),
                _format_time(full_inc_naive.get("elapsed_s", full_inc_naive.get("elapsed"))),
                str(full_inc_naive.get("exit", "")),
                _format_time(full_inc_regional.get("elapsed_s", full_inc_regional.get("elapsed"))),
                str(full_inc_regional.get("exit", "")),
            ])
    out_path = base_dir / "results-souffle-inc.tsv"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\t".join(header) + "\n")
        for row in rows:
            f.write("\t".join(row) + "\n")
    logger.info(f"Wrote {out_path.relative_to(base_dir)} (rows={len(rows)})")


# -----------------------------
# CLI commands
# -----------------------------


def ensure_case_workspace(
        n: int,
        case_templates: Path,
        base: Path,
        logger: Logger,
        reset: bool = False,
        copy_delta: bool = True,
) -> None:
    name = case_name(n)
    src = case_templates / name
    dst = case_dir(base, n)
    if not src.is_dir():
        if (dst / "compute.souffle.dl").is_file() and (dst / "input").is_dir():
            logger.info(f"[{name}] Using existing workspace {dst}")
            return
        logger.error(f"[{name}] Case template missing: {src}")
        raise FileNotFoundError(src)

    src_program = src / "compute.souffle.dl"
    src_input = src / "input"
    if not src_program.is_file() or not src_input.is_dir():
        logger.error(f"[{name}] Case template must contain compute.souffle.dl and input/")
        raise FileNotFoundError(src)

    ensure_dirs(dst)

    if reset or not (dst / "compute.souffle.dl").is_file():
        shutil.copy2(src_program, dst / "compute.souffle.dl")

    dst_input = dst / "input"
    if reset and dst_input.exists():
        shutil.rmtree(dst_input)
    if not dst_input.exists():
        shutil.copytree(src_input, dst_input)

    src_delta = src / "delta"
    dst_delta = dst / "delta"
    if reset and dst_delta.exists():
        shutil.rmtree(dst_delta)
    if copy_delta and src_delta.is_dir() and not dst_delta.exists():
        shutil.copytree(src_delta, dst_delta)

    logger.info(f"[{name}] Initialized workspace from {src.relative_to(case_templates)}")


def cmd_delta(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    cases = parse_cases_spec(args.cases, default_cases=DEFAULT_CASES)
    logger = Logger(base, Path(args.log_file) if args.log_file else None, quiet=args.quiet, verbose=args.verbose)
    logger.banner("SideChannel INC: DELTA")

    if not cases:
        logger.error("No cases to generate deltas for.")
        sys.exit(2)

    cluster_mode = args.delta_cluster
    cap_tol = args.change_cap_tol
    transitive_depth = args.delta_transitive_depth
    if transitive_depth < 0:
        logger.error("--delta-transitive-depth must be >= 0")
        sys.exit(2)

    logger.info(f"Cases: {cases} (count={len(cases)})")
    depth_info = f", depth={transitive_depth}" if cluster_mode == "assign-transitive" else ""
    if args.delta_strategy == "mix":
        mix_distributions = _parse_mix_distributions(args.mix_distributions or "")
        if not mix_distributions:
            mix_distributions = _parse_mix_distributions(DEFAULT_MIX_DISTRIBUTIONS)
        if not mix_distributions:
            logger.error("No valid --mix-distributions entries.")
            sys.exit(2)
        logger.info("Delta strategy: mix")
        logger.info(
            f"Mix ratio: {args.mix_ratio} cap={args.mix_cap} "
            f"pool_ratio={args.mix_pool_ratio} pool_cap_mult={args.mix_pool_cap_mult}"
        )
        logger.info(f"Mix distributions: {mix_distributions}")
        logger.info(f"Delta cluster mode: {cluster_mode}, cap_tol={cap_tol}{depth_info}")
        for idx, n in enumerate(cases, 1):
            logger.info(f"[{idx}/{len(cases)}] Deltas for {case_name(n)}")
            ensure_case_workspace(n, DEFAULT_CASE_TEMPLATE_DIR, base, logger, reset=args.cleanup, copy_delta=not args.cleanup)
            generate_mixed_deltas_for_case(
                case_num=n,
                base_dir=base,
                sets=args.sets,
                seed=args.seed,
                cleanup=args.cleanup,
                logger=logger,
                cluster_mode=cluster_mode,
                cap_tol=cap_tol,
                transitive_depth=transitive_depth,
                mix_ratio=args.mix_ratio,
                mix_cap=args.mix_cap,
                pool_ratio=args.mix_pool_ratio,
                pool_cap_mult=args.mix_pool_cap_mult,
                mix_distributions=mix_distributions,
            )
        logger.info("Delta generation: done.")
        return

    if args.delta_strategy == "alpha-grid":
        labels_and_sizes = _parse_change_spec(args.change_spec or DEFAULT_CHANGE_SPEC)
        labels_and_sizes = [(label.strip(), size) for label, size in labels_and_sizes if label.strip()]
        if not labels_and_sizes:
            logger.error("No valid --change-spec entries.")
            sys.exit(2)
        change_caps = _parse_change_caps(args.change_cap or "")
        if not change_caps and not (args.change_cap or "").strip() and cluster_mode == "assign":
            change_caps = _parse_change_caps(DEFAULT_CHANGE_CAP)
        mix_distributions = _parse_mix_distributions(args.mix_distributions or "")
        if not mix_distributions:
            mix_distributions = _parse_mix_distributions(DEFAULT_MIX_DISTRIBUTIONS)
        if not mix_distributions:
            logger.error("No valid --mix-distributions entries.")
            sys.exit(2)
        if args.sets != len(mix_distributions):
            logger.warn(
                f"alpha-grid maps sample index to alpha bucket; using {len(mix_distributions)} "
                f"samples from --mix-distributions, ignoring --sets={args.sets}"
            )
        logger.info("Delta strategy: alpha-grid")
        logger.info(f"Change spec: {labels_and_sizes}")
        logger.info(f"Change cap: {change_caps if change_caps else 'n/a'}")
        logger.info(f"Alpha buckets: {mix_distributions}")
        logger.info(f"Delta cluster mode: {cluster_mode}, cap_tol={cap_tol}{depth_info}")
        for idx, n in enumerate(cases, 1):
            logger.info(f"[{idx}/{len(cases)}] Deltas for {case_name(n)}")
            ensure_case_workspace(n, DEFAULT_CASE_TEMPLATE_DIR, base, logger, reset=args.cleanup, copy_delta=not args.cleanup)
            generate_alpha_grid_deltas_for_case(
                case_num=n,
                base_dir=base,
                labels_and_sizes=labels_and_sizes,
                change_caps=change_caps,
                seed=args.seed,
                cleanup=args.cleanup,
                logger=logger,
                cluster_mode=cluster_mode,
                cap_tol=cap_tol,
                transitive_depth=transitive_depth,
                pool_ratio=args.mix_pool_ratio,
                pool_cap_mult=args.mix_pool_cap_mult,
                mix_distributions=mix_distributions,
            )
        logger.info("Delta generation: done.")
        return

    labels_and_sizes = _parse_change_spec(args.change_spec or DEFAULT_CHANGE_SPEC)
    labels_and_sizes = [(label.strip(), size) for label, size in labels_and_sizes if label.strip()]
    if not labels_and_sizes:
        logger.error("No valid --change-spec entries.")
        sys.exit(2)
    change_caps = _parse_change_caps(args.change_cap or "")
    if not change_caps and not (args.change_cap or "").strip() and cluster_mode == "assign":
        change_caps = _parse_change_caps(DEFAULT_CHANGE_CAP)
    logger.info(f"Change spec: {labels_and_sizes}")
    logger.info(f"Change cap: {change_caps if change_caps else 'n/a'}")
    logger.info(f"Delta cluster mode: {cluster_mode}, cap_tol={cap_tol}{depth_info}")

    for idx, n in enumerate(cases, 1):
        logger.info(f"[{idx}/{len(cases)}] Deltas for {case_name(n)}")
        ensure_case_workspace(n, DEFAULT_CASE_TEMPLATE_DIR, base, logger, reset=args.cleanup, copy_delta=not args.cleanup)
        generate_deltas_for_case(
            case_num=n,
            base_dir=base,
            labels_and_sizes=labels_and_sizes,
            sets=args.sets,
            seed=args.seed,
            cleanup=args.cleanup,
            logger=logger,
            change_caps=change_caps,
            cluster_mode=cluster_mode,
            cap_tol=cap_tol,
            transitive_depth=transitive_depth,
        )
    logger.info("Delta generation: done.")


def cmd_clean(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    cases = parse_cases_spec(args.cases, default_cases=DEFAULT_CASES)
    logger = Logger(base, Path(args.log_file) if args.log_file else None, quiet=args.quiet, verbose=args.verbose)
    logger.banner("SideChannel INC: CLEAN")

    if args.all:
        if base.exists():
            logger.info(f"Removing base directory {base}")
            shutil.rmtree(base)
        else:
            logger.info(f"Base directory {base} is absent; clean has no work")
        return

    if not cases:
        logger.warn("No cases specified/found; nothing removed. Use --all to wipe the entire base dir.")
        return

    for n in cases:
        cdir = case_dir(base, n)
        if cdir.exists():
            logger.info(f"Removing {cdir}")
            shutil.rmtree(cdir)
        else:
            logger.warn(f"{cdir} not found; skipping")
    logger.info("Clean: done.")


def cmd_compile(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    cases = parse_cases_spec(args.cases, default_cases=DEFAULT_CASES)
    logger = Logger(base, Path(args.log_file) if args.log_file else None, quiet=args.quiet, verbose=args.verbose)
    logger.banner("SideChannel INC: COMPILE (Soufflé online)")

    if not cases:
        logger.error("No cases to compile.")
        sys.exit(2)
    logger.info(f"Cases: {cases}  (count={len(cases)})")

    souffle_bin = resolve_souffle_bin(args.souffle_bin, logger)
    version = detect_souffle_version(souffle_bin, base)
    if version:
        logger.info(f"Souffle version: {version}")
    else:
        logger.warn("Could not determine Souffle version from --version output.")

    cfg = CmpCfg(
        base_dir=base,
        cases=cases,
        timeout=args.timeout,
        souffle_bin=str(souffle_bin),
        souffle_args=args.souffle_arg or [],
        logger=logger,
    )
    jobs = max(1, int(args.jobs))
    if jobs == 1 or len(cases) <= 1:
        for idx, n in enumerate(cases, 1):
            logger.info(f"[{idx}/{len(cases)}] Compile {case_name(n)}")
            ensure_case_workspace(n, DEFAULT_CASE_TEMPLATE_DIR, base, logger)
            compile_case(n, cfg)
    else:
        logger.info(f"Parallel compile enabled: jobs={jobs}")
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {}
            for idx, n in enumerate(cases, 1):
                logger.info(f"[{idx}/{len(cases)}] Compile {case_name(n)}")
                ensure_case_workspace(n, DEFAULT_CASE_TEMPLATE_DIR, base, logger)
                futures[pool.submit(compile_case, n, cfg)] = n
            for fut in as_completed(futures):
                n = futures[fut]
                try:
                    fut.result()
                except Exception as exc:
                    logger.error(f"[{case_name(n)}] Compile failed: {exc}")
    logger.info("Compile: done.")


def _default_run_modes(compare_all: bool, inc_regional_only: bool, compare_full_inc: bool = True) -> List[str]:
    modes = ["full"]
    if inc_regional_only:
        modes.append("inc-regional")
        if compare_full_inc:
            modes.extend(["inc-full", "full-inc-regional"])
    elif compare_all:
        modes.extend(["inc-naive", "inc-regional"])
        if compare_full_inc:
            modes.extend(["inc-full", "full-inc-naive", "full-inc-regional"])
    else:
        modes.append("inc-naive")
        if compare_full_inc:
            modes.extend(["inc-full", "full-inc-naive"])
    return modes


def _parse_run_modes(raw: str) -> List[str]:
    requested = [part.strip() for part in raw.split(",") if part.strip()]
    if not requested:
        raise ValueError("--modes cannot be empty")
    invalid = sorted(set(requested) - set(RUN_MODE_ORDER))
    if invalid:
        raise ValueError(f"unknown --modes entries: {', '.join(invalid)}")
    modes = [mode for mode in RUN_MODE_ORDER if mode in requested]
    if "full" not in modes:
        raise ValueError("--modes must include full")
    if "full-inc-naive" in modes and "inc-naive" not in modes:
        raise ValueError("--modes full-inc-naive requires inc-naive for comparison")
    if "full-inc-regional" in modes and "inc-regional" not in modes:
        raise ValueError("--modes full-inc-regional requires inc-regional for comparison")
    return modes


def cmd_run(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    cases = parse_cases_spec(args.cases, default_cases=DEFAULT_CASES)
    logger = Logger(base, Path(args.log_file) if args.log_file else None, quiet=args.quiet, verbose=args.verbose)
    logger.banner("SideChannel INC: RUN")
    compare_all = args.compare_all and not args.inc_regional_only
    if args.modes:
        try:
            run_modes = _parse_run_modes(args.modes)
        except ValueError as exc:
            logger.error(str(exc))
            sys.exit(2)
        logger.info(f"Explicit run modes: {', '.join(run_modes)}")
    else:
        run_modes = _default_run_modes(compare_all, args.inc_regional_only, compare_full_inc=True)
    if args.inc_regional_only and not args.modes:
        logger.info("inc-regional-only: running inc-regional + full.")
        if args.compare_all:
            logger.warn("--compare-all has no extra effect with --inc-regional-only.")
    elif compare_all and not args.modes:
        logger.info("compare-all runs inc-naive and inc-regional explicitly.")
    if args.materialized_full:
        logger.info("Materialized full reference enabled for single-commit deltas.")
    if not cases:
        logger.error("No cases to run.")
        sys.exit(2)

    labels = [lbl.strip() for lbl in args.delta_labels.split(",") if lbl.strip()]
    if not labels:
        logger.error("Provide at least one --delta-labels value (e.g., inc0p5,inc1p0).")
        sys.exit(2)
    if args.delta_runs < 1:
        logger.error("--delta-runs must be >= 1")
        sys.exit(2)

    run_args = args.run_arg or []
    cfg = RunCfg(
        base_dir=base,
        cases=cases,
        timeout=args.timeout,
        delta_timeout_multiplier=args.delta_timeout_multiplier,
        labels=labels,
        samples_per_label=args.delta_samples,
        delta_runs=args.delta_runs,
        randomize=args.delta_shuffle,
        seed=args.delta_seed,
        delta_root=Path(args.delta_root).resolve() if args.delta_root else None,
        output_dir=args.output_dir,
        run_args=run_args,
        compare_all=compare_all,
        inc_regional_only=args.inc_regional_only,
        compare_full_inc=True,
        run_modes=run_modes,
        materialized_full=args.materialized_full,
        logger=logger,
    )
    for idx, n in enumerate(cases, 1):
        logger.info(f"[{idx}/{len(cases)}] Run {case_name(n)}")
        run_case(n, cfg)
    logger.info("Run: done.")


def cmd_collect(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    cases = parse_cases_spec(args.cases, default_cases=DEFAULT_CASES)
    logger = Logger(base, Path(args.log_file) if args.log_file else None, quiet=args.quiet, verbose=args.verbose)
    logger.banner("SideChannel INC: COLLECT")

    if not cases:
        logger.error("No cases to collect.")
        sys.exit(2)

    collect_results(base, cases, args.output_dir, logger)
    logger.info("Collect: done.")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Incremental Side Channel Benchmark Toolkit (Soufflé-only). "
            "All random workflows are seed-controlled; default seed is 42."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--base-dir", default=str(DEFAULT_BASE_DIR), help="Writable output root for run artifacts")
    p.add_argument("--log-file", default=None, help="Operation log path (default: <base-dir>/operation_inc.log)")
    p.add_argument("--quiet", action="store_true", help="Silence console progress (file logging remains)")
    p.add_argument("--verbose", action="store_true", help="Include verbose details in the operation log")
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("delta", help="Generate delta workloads under <base-dir>/P*/delta/")
    d.add_argument("--cases", "--case", dest="cases", default=None, help="Cases to create deltas for")
    d.add_argument("--delta-strategy", choices=["standard", "mix", "alpha-grid"], default="standard", help="Delta strategy (standard uses change-spec/cap; mix creates delete/insert blends; alpha-grid keeps change-spec labels and maps sample index to delete/insert buckets)")
    d.add_argument("--change-spec", default=DEFAULT_CHANGE_SPEC, help="Comma-separated label=value pairs; value<1.0 = ratio of total facts, value>=1 = absolute deletions (e.g., inc0p5=0.005,inc1p0=0.01)")
    d.add_argument("--change-cap", default="", help="Comma-separated label=cap pairs to cap absolute deletions per spec (e.g., inc0p5=150,inc1p0=300)")
    d.add_argument("--change-cap-tol", type=float, default=DEFAULT_CHANGE_CAP_TOL, help="Tolerance ratio when clustering deletions around the target size")
    d.add_argument("--delta-cluster", choices=["assign", "assign-transitive", "id", "random"], default="assign", help="Delta selection strategy (assign uses assignment edges, assign-transitive expands along assign edges, id uses shared IDs)")
    d.add_argument("--delta-transitive-depth", type=int, default=DEFAULT_TRANSITIVE_DEPTH, help="Max transitive depth for assign-transitive clustering (strict)")
    d.add_argument("--mix-ratio", type=float, default=DEFAULT_MIX_TOTAL_RATIO, help="Total change ratio for mix strategy (fraction of total facts)")
    d.add_argument("--mix-cap", type=int, default=DEFAULT_MIX_CAP, help="Absolute cap on total mix changes")
    d.add_argument("--mix-pool-ratio", type=float, default=DEFAULT_MIX_POOL_RATIO, help="Insert-pool ratio for mix strategy (pre-deleted facts)")
    d.add_argument("--mix-pool-cap-mult", type=float, default=DEFAULT_MIX_POOL_CAP_MULT, help="Multiplier for mix cap when capping insert pool size")
    d.add_argument("--mix-distributions", default=DEFAULT_MIX_DISTRIBUTIONS, help="Comma-separated delete-insert ratios (e.g., 0-1,0.25-0.75)")
    d.add_argument("--sets", type=int, default=5, help="Number of samples per change size")
    d.add_argument(
        "--seed",
        type=int,
        default=DEFAULT_GLOBAL_SEED,
        help=(
            "Seed for delta sampling. With unchanged case input state, repeated runs with the same seed "
            "produce identical delta files. In mix mode, existing input/delta_insert_pool.jsonl is reused"
        ),
    )
    d.add_argument("--cleanup", action="store_true", help="Reset selected case workspaces before writing delta files")
    d.set_defaults(func=cmd_delta)

    cl = sub.add_parser("clean", help="Remove generated artifacts")
    cl.add_argument("--cases", "--case", dest="cases", default=None, help="Cases to remove under --base-dir (default: all found cases)")
    cl.add_argument("--all", action="store_true", help="Remove the entire --base-dir directory")
    cl.set_defaults(func=cmd_clean)

    c = sub.add_parser("compile", help="Compile Soufflé with incremental CLI support")
    c.add_argument("--cases", "--case", dest="cases", default=None, help="Cases to compile")
    c.add_argument("--timeout", type=int, default=300, help="Compile timeout (s) per case")
    c.add_argument(
        "--souffle-bin",
        default=None,
        help=(
            "Souffle compiler path/name. Default lookup order: --souffle-bin, "
            "$SOUFFLE_BIN, <repo>/build/src/souffle, then PATH 'souffle'."
        ),
    )
    c.add_argument("--souffle-arg", action="append", help="Extra args passed to 'souffle' (can repeat)")
    c.add_argument("--jobs", type=int, default=1, help="Parallel compile jobs (per-case)")
    c.set_defaults(func=cmd_compile)

    r = sub.add_parser("run", help="Run Soufflé incremental/full comparisons")
    r.add_argument("--cases", "--case", dest="cases", default=None, help="Cases to run")
    r.add_argument("--timeout", type=int, default=600, help="Run timeout (s) per delta workload")
    r.add_argument(
        "--delta-timeout-multiplier",
        type=float,
        default=0.0,
        help="If >0, per-delta timeout is min(--timeout, baseline_elapsed * multiplier)",
    )
    r.add_argument("--delta-labels", default="inc0p5,inc1p0,inc1p5", help="Comma-separated delta file prefixes to sample (e.g., inc0p5,inc1p5)")
    r.add_argument(
        "--delta-samples",
        type=int,
        default=DEFAULT_DELTA_SAMPLES,
        help="Samples per delta label (default: 5; requires files like <label>_1.txt, ...)",
    )
    r.add_argument("--delta-runs", type=int, default=DEFAULT_DELTA_RUNS, help="Repeat each delta run N times")
    r.add_argument("--delta-root", default=None, help="Optional alternate root containing <Case>/delta/*.txt (default: <case>/delta under base-dir)")
    r.add_argument("--delta-shuffle", action="store_true", help="Shuffle available delta files before sampling")
    r.add_argument(
        "--delta-seed",
        type=int,
        default=DEFAULT_GLOBAL_SEED,
        help="Seed used when shuffling delta files (only when --delta-shuffle is enabled)",
    )
    r.add_argument("--output-dir", default="output", help="Output subdir under each case for run artifacts")
    r.add_argument("--run-arg", action="append", help="Extra args passed to './compute' (can repeat)")
    r.add_argument(
        "--modes",
        default="",
        help=(
            "Explicit comma-separated modes to run. Defaults preserve compare flags. "
            "Choices: full,inc-naive,inc-regional,inc-full,full-inc-naive,full-inc-regional"
        ),
    )
    r.add_argument(
        "--materialized-full",
        action="store_true",
        help="For single-commit deltas, run full on a materialized final input instead of online base+delta.",
    )
    r.add_argument("--inc-regional-only", action="store_true", help="Run inc-regional plus full recomputation")
    r.add_argument(
        "--compare-all",
        dest="compare_all",
        action="store_true",
        default=True,
        help="Compare full vs inc-naive vs inc-regional for each iter (default: enabled)",
    )
    r.add_argument(
        "--no-compare-all",
        dest="compare_all",
        action="store_false",
        help="Run inc-naive plus full recomputation",
    )
    r.set_defaults(func=cmd_run)

    k = sub.add_parser("collect", help="Collect incremental comparison results to TSV")
    k.add_argument("--cases", "--case", dest="cases", default=None, help="Cases to collect")
    k.add_argument("--output-dir", default="output", help="Output subdir under each case to scan")
    k.set_defaults(func=cmd_collect)

    return p


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.log_file is None:
        args.log_file = str(Path(args.base_dir) / "operation_inc.log")
    args.func(args)


if __name__ == "__main__":
    main()
