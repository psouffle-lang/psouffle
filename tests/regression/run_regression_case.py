#!/usr/bin/env python3
"""Incremental artifact regression runner."""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

PROB_LINE_RE = re.compile(r"^\s*(.*?)\s*:\s*([+\-]?\d+(?:\.\d+)?(?:[eE][+\-]?\d+)?)\s*$")
CASES_ROOT = Path(__file__).resolve().parent / "cases"


class CaseFailure(RuntimeError):
    """Raised when a regression case fails."""


def format_cmd(cmd: Sequence[str]) -> str:
    return " ".join(cmd)


def run_cmd(
    cmd: Sequence[str],
    cwd: Path,
    *,
    stdin_text: str | None = None,
    timeout: int = 240,
) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        list(cmd),
        cwd=str(cwd),
        input=stdin_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
    if proc.returncode != 0:
        raise CaseFailure(
            f"command failed (exit={proc.returncode})\n"
            f"cwd: {cwd}\n"
            f"cmd: {format_cmd(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return proc


def reset_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def prepare_case_workspace(case_id: str, work_root: Path) -> Path:
    case_src = CASES_ROOT / case_id
    if not case_src.exists():
        raise CaseFailure(f"missing regression case directory: {case_src}")

    case_dir = work_root / case_id
    reset_dir(case_dir)

    for entry in sorted(case_src.iterdir()):
        if entry.name == "generate.py":
            continue
        dst = case_dir / entry.name
        if entry.is_dir():
            shutil.copytree(entry, dst)
        elif entry.is_file():
            shutil.copy2(entry, dst)

    generator = case_src / "generate.py"
    if generator.exists():
        run_cmd(
            [sys.executable, str(generator), "--out-dir", str(case_dir)],
            cwd=case_src,
            timeout=180,
        )

    if not (case_dir / "compute.dl").exists():
        raise CaseFailure(f"case {case_id} did not provide compute.dl")
    if not (case_dir / "input").exists():
        raise CaseFailure(f"case {case_id} did not provide input/")
    return case_dir


def parse_prob_file(path: Path) -> Dict[str, float]:
    if not path.exists():
        raise CaseFailure(f"missing probability output: {path}")
    results: Dict[str, float] = {}
    with path.open("r", encoding="utf-8") as f:
        for idx, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            match = PROB_LINE_RE.match(line)
            if not match:
                raise CaseFailure(f"malformed probability line at {path}:{idx}: {line}")
            results[match.group(1)] = float(match.group(2))
    return results


def normalize_tuple_key(value: str) -> str:
    return re.sub(r"\s+", "", value)


def assert_prob_close(lhs: Path, rhs: Path, *, tol: float = 1e-9, label: str) -> None:
    left = parse_prob_file(lhs)
    right = parse_prob_file(rhs)
    left_keys = set(left.keys())
    right_keys = set(right.keys())
    if left_keys != right_keys:
        raise CaseFailure(
            f"{label}: tuple key mismatch\n"
            f"lhs={lhs}\nrhs={rhs}\n"
            f"missing_in_rhs={sorted(left_keys - right_keys)[:8]}\n"
            f"extra_in_rhs={sorted(right_keys - left_keys)[:8]}"
        )

    diffs: List[str] = []
    for key in sorted(left_keys):
        if not math.isclose(left[key], right[key], rel_tol=0.0, abs_tol=tol):
            diffs.append(f"{key}: lhs={left[key]:.12g} rhs={right[key]:.12g}")
            if len(diffs) >= 8:
                break
    if diffs:
        raise CaseFailure(
            f"{label}: probability mismatch (tol={tol})\n"
            f"lhs={lhs}\nrhs={rhs}\n" + "\n".join(diffs)
        )


def normalize_table_lines(path: Path) -> List[str]:
    return sorted(line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip())


def iter_prob_path(output_dir: Path, iteration: int, suffix: str) -> Path:
    return output_dir / f"fact-iter{iteration}-{suffix}.prob"


def compile_compute(
    *,
    souffle_bin: Path,
    case_dir: Path,
    compile_args: Sequence[str] | None = None,
) -> Tuple[Path, Path]:
    input_dir = case_dir / "input"
    output_dir = case_dir / "output_compile"
    build_dir = case_dir / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    compute_bin = build_dir / "compute"
    cmd = [str(souffle_bin)]
    if compile_args:
        cmd.extend(compile_args)
    cmd.extend(
        [
            "-F",
            str(input_dir),
            "-D",
            str(output_dir),
            str(case_dir / "compute.dl"),
            "-o",
            str(compute_bin),
        ]
    )
    run_cmd(cmd, cwd=case_dir, timeout=300)
    if not compute_bin.exists():
        raise CaseFailure(f"compile did not create binary: {compute_bin}")
    return compute_bin, input_dir


def make_cli_script(turns: Sequence[Sequence[str]]) -> str:
    lines: List[str] = []
    for turn_ops in turns:
        lines.extend(turn_ops)
        lines.append("commit")
    lines.append("q")
    return "\n".join(lines) + "\n"


def run_cli_script(
    *,
    compute_bin: Path,
    input_dir: Path,
    output_dir: Path,
    script_text: str,
    extra_args: Sequence[str] | None = None,
    timeout: int = 240,
) -> subprocess.CompletedProcess[str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(compute_bin), "-F", str(input_dir), "-D", str(output_dir)]
    if extra_args:
        cmd.extend(extra_args)
    write_text(output_dir / "commands.txt", script_text)
    return run_cmd(cmd, cwd=compute_bin.parent, stdin_text=script_text, timeout=timeout)


def run_cli_mode(
    *,
    compute_bin: Path,
    input_dir: Path,
    output_dir: Path,
    mode: str,
    turns: Sequence[Sequence[str]],
    extra_args: Sequence[str] | None = None,
    timeout: int = 240,
) -> subprocess.CompletedProcess[str]:
    return run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=output_dir,
        script_text=make_cli_script(turns),
        extra_args=["--setmode", mode, *(list(extra_args) if extra_args else [])],
        timeout=timeout,
    )


def assert_stdout_contains(stdout: str, needle: str, *, label: str) -> None:
    if needle not in stdout:
        raise CaseFailure(f"{label}: missing stdout substring\nexpected={needle}\nstdout:\n{stdout}")


def assert_stdout_not_contains(stdout: str, needle: str, *, label: str) -> None:
    if needle in stdout:
        raise CaseFailure(f"{label}: unexpected stdout substring\nneedle={needle}\nstdout:\n{stdout}")


def assert_path_exists(path: Path, *, label: str) -> None:
    if not path.exists():
        raise CaseFailure(f"{label}: missing expected path {path}")


def assert_path_missing(path: Path, *, label: str) -> None:
    if path.exists():
        raise CaseFailure(f"{label}: unexpected path present {path}")


def assert_glob_nonempty(base_dir: Path, pattern: str, *, label: str) -> None:
    if not sorted(base_dir.glob(pattern)):
        raise CaseFailure(f"{label}: expected files matching {base_dir / pattern}")


def assert_glob_empty(base_dir: Path, pattern: str, *, label: str) -> None:
    matches = sorted(base_dir.glob(pattern))
    if matches:
        raise CaseFailure(f"{label}: unexpected files matching {base_dir / pattern}: {matches}")


def assert_dot_body_contains(path: Path, needle: str, *, label: str) -> None:
    if not path.exists():
        raise CaseFailure(f"{label}: missing expected path {path}")
    body = path.read_text(encoding="utf-8").split("subgraph cluster_legend", 1)[0]
    if needle not in body:
        raise CaseFailure(f"{label}: missing DOT body substring\nexpected={needle}\npath={path}\nbody:\n{body}")


def assert_dot_colored_bridge(path: Path, source: str, target: str, color: str, *, label: str) -> None:
    if not path.exists():
        raise CaseFailure(f"{label}: missing expected path {path}")
    body = path.read_text(encoding="utf-8").split("subgraph cluster_legend", 1)[0]
    source_re = re.compile(
        rf'"{re.escape(source)}"\s*->\s*"(edge_node_[^"]+)"\s*\[color="{re.escape(color)}"'
    )
    edge_nodes = [match.group(1) for match in source_re.finditer(body)]
    for edge_node in edge_nodes:
        target_re = re.compile(
            rf'"{re.escape(edge_node)}"\s*->\s*"{re.escape(target)}"\s*\[color="{re.escape(color)}"'
        )
        if target_re.search(body):
            return
    raise CaseFailure(
        f"{label}: missing colored DOT bridge {source} -> * -> {target} color={color}\n"
        f"path={path}\nbody:\n{body}"
    )


def load_single_json_match(base_dir: Path, pattern: str, *, label: str) -> object:
    matches = sorted(base_dir.glob(pattern))
    if len(matches) != 1:
        raise CaseFailure(f"{label}: expected one file matching {base_dir / pattern}, got {len(matches)}")
    with matches[0].open("r", encoding="utf-8") as f:
        return json.load(f)


def iter_derivation_edges(doc: object):
    if not isinstance(doc, dict):
        return
    for edge in doc.get("rules", []):
        yield edge
    delta = doc.get("delta", {})
    if isinstance(delta, dict):
        for side in ("insert", "delete"):
            payload = delta.get(side, {})
            if isinstance(payload, dict):
                for edge in payload.get("edges", []):
                    yield edge


def assert_negated_bodies_absent(doc: object, names: Sequence[str], *, label: str) -> None:
    forbidden = set(names)
    hits: List[str] = []
    for edge in iter_derivation_edges(doc):
        if not isinstance(edge, dict):
            continue
        for body in edge.get("bodies", []):
            if isinstance(body, dict) and body.get("negation") is True and body.get("name") in forbidden:
                hits.append(f"{edge.get('head')} <- !{body.get('name')}")
    if hits:
        raise CaseFailure(f"{label}: absent negated tuples were materialized\n" + "\n".join(hits[:8]))


def case_dred_mix_naive_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("dred_mix_naive_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [
        [],
        ["insert 0.35::bridge(1,3)", "delete edge(2,4)"],
        ["insert 0.41::edge(3,6)"],
    ]

    out_inc = case_dir / "out_inc_naive"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(compute_bin=compute_bin, input_dir=input_dir, output_dir=out_inc, mode="inc-naive", turns=turns)
    run_cli_mode(compute_bin=compute_bin, input_dir=input_dir, output_dir=out_full, mode="full", turns=turns)

    for iteration in (1, 2, 3):
        assert_prob_close(
            iter_prob_path(out_inc, iteration, "inc-naive"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"dred_mix iter={iteration}",
        )


def case_dred_hub_rederive_naive_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("dred_hub_rederive_naive_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    delete_ops = [f"delete edge({i},40)" for i in range(1, 7)]
    turns = [
        [],
        delete_ops + ["delete edge(6,7)"],
        ["insert 0.73::edge(6,7)", "insert 0.67::edge(17,40)", "insert 0.55::edge(12,30)"],
    ]

    out_inc = case_dir / "out_inc_naive"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_inc,
        mode="inc-naive",
        turns=turns,
        timeout=300,
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
        timeout=300,
    )

    for iteration in (1, 2, 3):
        assert_prob_close(
            iter_prob_path(out_inc, iteration, "inc-naive"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"dred_hub_rederive iter={iteration}",
        )


def case_deterministic_inc_naive_combo_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("deterministic_inc_naive_combo_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [
        [],
        ["delete trust(1,2)", "delete trust(2,3)"],
        ["insert trust(1,2)", "insert trust(2,3)", "delete chance(1,4)"],
    ]

    out_inc = case_dir / "out_inc_naive"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_inc,
        mode="inc-naive",
        turns=turns,
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    for iteration in (1, 2, 3):
        assert_prob_close(
            iter_prob_path(out_inc, iteration, "inc-naive"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"deterministic_combo iter={iteration}",
        )


def case_deterministic_inc_regional_single_round_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("deterministic_inc_regional_single_round_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [[], ["delete trust(1,5)", "insert 0.52::chance(1,5)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    for iteration in (1, 2):
        assert_prob_close(
            iter_prob_path(out_regional, iteration, "inc-regional"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"deterministic_inc_regional iter={iteration}",
        )


def case_inc_regional_calibration_single_interface(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("inc_regional_calibration_single_interface", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert 0.40::d2(0)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full"
    proc = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
        extra_args=["--profile-stage=inc-regional", "--dump=dot"],
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    assert_prob_close(
        iter_prob_path(out_regional, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="inc_regional_calibration_single_interface iter=1",
    )
    probs = parse_prob_file(iter_prob_path(out_regional, 1, "inc-regional"))
    if not math.isclose(probs.get("o5(0)", -1.0), 0.42, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_calibration_single_interface: expected o5(0)=0.42, got {probs}")
    for needle in (
        "[inc-regional-calibration]",
        "head=o2(0)",
        "anchor=node:i2(0)",
        "old_weight=0.5",
        "calibrated_weight=0.7",
        "target_prob=0.7",
    ):
        assert_stdout_contains(proc.stdout, needle, label="calibration profile detail")
    assert_path_exists(
        out_regional / "inc-region-1.dot",
        label="calibration region dot",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"i2(0)" [shape=box, penwidth=2, color="#f2c744"]',
        label="calibration mergeable node anchor",
    )
    assert_dot_colored_bridge(
        out_regional / "inc-region-1.dot",
        "i2(0)",
        "o2(0)",
        "red",
        label="calibration mergeable anchor path",
    )
    assert_glob_empty(out_regional, "inc-region-step-*.dot", label="calibration intermediate region dot")
    assert_glob_empty(out_regional, "region-*.txt", label="calibration region text dump")


def case_inc_regional_shared_delta_join_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("inc_regional_shared_delta_join_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert 0.40::dshared(0)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full"
    proc = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
        extra_args=["--profile-stage=inc-regional", "--dump=dot"],
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    assert_prob_close(
        iter_prob_path(out_regional, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="inc_regional_shared_delta_join iter=1",
    )
    probs = parse_prob_file(iter_prob_path(out_regional, 1, "inc-regional"))
    if not math.isclose(probs.get("o5(0)", -1.0), 0.58, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_shared_delta_join: expected o5(0)=0.58, got {probs}")
    assert_stdout_contains(proc.stdout, "overlap closure reason=preplan", label="shared-delta join overlap profile")
    assert_stdout_contains(proc.stdout, "overlap_closure_dependent=1", label="shared-delta join overlap profile")
    assert_path_exists(
        out_regional / "inc-region-1.dot",
        label="shared-delta join region dot",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o4(0)" [shape=ellipse, penwidth=2, color="#1f77b4"]',
        label="shared-delta join final region includes o4",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o5(0)" [shape=ellipse, penwidth=2, color="#1f77b4"]',
        label="shared-delta join final region includes o5",
    )
    assert_glob_empty(out_regional, "inc-region-step-*.dot", label="shared-delta join intermediate region dot")
    assert_glob_empty(out_regional, "region-*.txt", label="shared-delta join region text dump")


def case_inc_regional_independent_overlap_skip_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("inc_regional_independent_overlap_skip_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert 0.40::d1(0)", "insert 0.30::d2(0)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full"
    proc = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
        extra_args=["--profile-stage=inc-regional", "--dump=dot"],
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    assert_prob_close(
        iter_prob_path(out_regional, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="inc_regional_independent_overlap_skip iter=1",
    )
    probs = parse_prob_file(iter_prob_path(out_regional, 1, "inc-regional"))
    if not math.isclose(probs.get("o5(0)", -1.0), 0.76, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_independent_overlap_skip: expected o5(0)=0.76, got {probs}")
    if not math.isclose(probs.get("o6(0)", -1.0), 0.65, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_independent_overlap_skip: expected o6(0)=0.65, got {probs}")
    assert_stdout_contains(
        proc.stdout,
        "overlap closure skip reason=independent_anchors boundary_nodes=2",
        label="independent overlap skip profile",
    )
    assert_stdout_not_contains(
        proc.stdout,
        "overlap closure reason=preplan",
        label="independent overlap skip profile",
    )
    assert_path_exists(
        out_regional / "inc-region-1.dot",
        label="independent overlap skip region dot",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o3(0)" [shape=ellipse, penwidth=2, color="#ff7f0e"]',
        label="independent overlap skip left boundary",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o4(0)" [shape=ellipse, penwidth=2, color="#ff7f0e"]',
        label="independent overlap skip right boundary",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o5(0)" [shape=ellipse, penwidth=1, color="black"]',
        label="independent overlap skip left downstream output",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"o6(0)" [shape=ellipse, penwidth=1, color="black"]',
        label="independent overlap skip right downstream output",
    )
    assert_dot_colored_bridge(
        out_regional / "inc-region-1.dot",
        "i1(0)",
        "o3(0)",
        "red",
        label="independent overlap skip left mergeable anchor path",
    )
    assert_dot_colored_bridge(
        out_regional / "inc-region-1.dot",
        "i2(0)",
        "o4(0)",
        "red",
        label="independent overlap skip right mergeable anchor path",
    )
    assert_glob_empty(out_regional, "inc-region-step-*.dot", label="independent overlap skip intermediate region dot")
    assert_glob_empty(out_regional, "region-*.txt", label="independent overlap skip region text dump")


def case_inc_regional_deterministic_chain_anchor(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("inc_regional_deterministic_chain_anchor", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert 0.20::d(2)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full"
    proc = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
        extra_args=["--profile-stage=inc-regional", "--dump=dot"],
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    assert_prob_close(
        iter_prob_path(out_regional, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="inc_regional_deterministic_chain_anchor iter=1",
    )
    probs = parse_prob_file(iter_prob_path(out_regional, 1, "inc-regional"))
    if not math.isclose(probs.get("sink(2)", -1.0), 0.68, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_deterministic_chain_anchor: expected sink(2)=0.68, got {probs}")
    assert_stdout_contains(
        proc.stdout,
        "boundary head anchors: raw(2) anchors=node:i(0)",
        label="deterministic chain anchor profile",
    )
    assert_stdout_contains(
        proc.stdout,
        "[inc-regional-final] region_nodes=2 dr_nodes=3 ratio=0.667",
        label="deterministic chain anchor final region",
    )
    assert_dot_body_contains(
        out_regional / "inc-region-1.dot",
        '"i(0)" [shape=box, penwidth=2, color="#f2c744"]',
        label="deterministic chain node anchor",
    )
    assert_glob_empty(out_regional, "inc-region-step-*.dot", label="deterministic chain intermediate region dot")
    assert_glob_empty(out_regional, "region-*.txt", label="deterministic chain region text dump")


def case_inc_regional_deterministic_join_anchor_reject(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("inc_regional_deterministic_join_anchor_reject", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert 0.20::d(0)"]]

    out_regional = case_dir / "out_inc_regional"
    out_full = case_dir / "out_full"
    proc = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional,
        mode="inc-regional",
        turns=turns,
        extra_args=["--profile-stage=inc-regional", "--dump=dot"],
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    assert_prob_close(
        iter_prob_path(out_regional, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="inc_regional_deterministic_join_anchor_reject iter=1",
    )
    probs = parse_prob_file(iter_prob_path(out_regional, 1, "inc-regional"))
    if not math.isclose(probs.get("sink(0)", -1.0), 0.60, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"inc_regional_deterministic_join_anchor_reject: expected sink(0)=0.60, got {probs}")
    assert_stdout_contains(
        proc.stdout,
        "anchor-check head=head(0) candidate=node:a(0) result=NO reason=path_node_in_region",
        label="deterministic join changed sibling rejection",
    )
    assert_stdout_contains(
        proc.stdout,
        "boundary head missing anchor: head(0)",
        label="deterministic join has no usable anchor after changed sibling rejection",
    )


def case_deterministic_inc_regional_multiturn_state_machine(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("deterministic_inc_regional_multiturn_state_machine", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [
        ["insert srcA(1) 1.0"],
        ["delete srcA(1)", "insert srcD(1) 1.0"],
        ["insert srcA(1) 1.0"],
    ]
    fallback_inc = (
        "[cli] fc-state=R requested=SEM-INC+INC-REGIONAL+WMC-INC-REGIONAL "
        "fallback=SEM-INC+INC-NAIVE+WMC-INC-NAIVE"
    )

    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
        timeout=300,
    )

    out_regional_rrr = case_dir / "out_regional_rrr"
    proc_regional_rrr = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional_rrr,
        mode="inc-regional",
        turns=turns,
        extra_args=["--verbose"],
        timeout=300,
    )
    assert_prob_close(
        iter_prob_path(out_regional_rrr, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="regional->regional->regional iter=1",
    )
    assert_prob_close(
        iter_prob_path(out_regional_rrr, 2, "inc-naive"),
        iter_prob_path(out_full, 2, "full"),
        label="regional->regional->regional iter=2 fallback",
    )
    assert_prob_close(
        iter_prob_path(out_regional_rrr, 3, "inc-regional"),
        iter_prob_path(out_full, 3, "full"),
        label="regional->regional->regional iter=3 re-entry",
    )
    assert_path_exists(out_regional_rrr / "fact-iter2-inc-naive.prob", label="regional turn2 fallback")
    assert_path_missing(out_regional_rrr / "fact-iter2-inc-regional.prob", label="regional turn2 not regional")
    assert_stdout_contains(proc_regional_rrr.stdout, fallback_inc, label="regional fallback log")


def case_deterministic_inc_regional_multiturn_degenerate(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("deterministic_inc_regional_multiturn_degenerate", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [["insert srcA(1) 1.0"], ["insert srcD(1) 1.0"]]

    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
        timeout=240,
    )

    out_regional_rr = case_dir / "out_regional_rr"
    proc_regional_rr = run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_regional_rr,
        mode="inc-regional",
        turns=turns,
        timeout=240,
    )
    assert_prob_close(
        iter_prob_path(out_regional_rr, 1, "inc-regional"),
        iter_prob_path(out_full, 1, "full"),
        label="degenerate regional iter=1",
    )
    assert_prob_close(
        iter_prob_path(out_regional_rr, 2, "inc-regional"),
        iter_prob_path(out_full, 2, "full"),
        label="degenerate regional iter=2",
    )
    assert_stdout_not_contains(proc_regional_rr.stdout, "[cli] fc-state=", label="degenerate regional stays regional")
    assert_path_exists(out_regional_rr / "fact-iter2-inc-regional.prob", label="degenerate regional artifact")


def case_deterministic_recursive_derivation_guard_vs_full(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("deterministic_recursive_derivation_guard_vs_full", work_root)
    compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=case_dir)
    turns = [[], ["delete edge(1,2)"], ["delete edge(1,3)"]]

    out_inc = case_dir / "out_inc_naive"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_inc,
        mode="inc-naive",
        turns=turns,
    )
    run_cli_mode(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=out_full,
        mode="full",
        turns=turns,
    )

    for iteration in (1, 2, 3):
        assert_prob_close(
            iter_prob_path(out_inc, iteration, "inc-naive"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"deterministic_recursive_derivation_guard iter={iteration}",
        )


def case_negated_absent_tuple_universe(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("negated_absent_tuple_universe", work_root)
    compute_bin, input_dir = compile_compute(
        souffle_bin=souffle_bin,
        case_dir=case_dir,
        compile_args=["--dump=json"],
    )
    turns = [[], ["insert 0.70::src(3)"]]

    out_inc = case_dir / "out_inc_naive"
    out_full = case_dir / "out_full_hard"
    run_cli_mode(compute_bin=compute_bin, input_dir=input_dir, output_dir=out_inc, mode="inc-naive", turns=turns)
    run_cli_mode(compute_bin=compute_bin, input_dir=input_dir, output_dir=out_full, mode="full", turns=turns)

    for iteration in (1, 2):
        assert_prob_close(
            iter_prob_path(out_inc, iteration, "inc-naive"),
            iter_prob_path(out_full, iteration, "full"),
            label=f"negated_absent_tuple iter={iteration}",
        )

    inc_iter2 = parse_prob_file(iter_prob_path(out_inc, 2, "inc-naive"))
    if not math.isclose(inc_iter2.get(normalize_tuple_key("allow(1)"), -1.0), 0.4, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"negated_absent_tuple: allow(1) probability mismatch: {inc_iter2}")
    if not math.isclose(inc_iter2.get(normalize_tuple_key("allow(3)"), -1.0), 0.7, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"negated_absent_tuple: allow(3) probability mismatch: {inc_iter2}")
    blocked = inc_iter2.get(normalize_tuple_key("allow(2)"), 0.0)
    if not math.isclose(blocked, 0.0, rel_tol=0.0, abs_tol=1e-9):
        raise CaseFailure(f"negated_absent_tuple: blocked tuple allow(2) is non-zero: {inc_iter2}")

    assert_negated_bodies_absent(
        load_single_json_match(out_full, "derivation-full-after-prune1-*.json", label="full iter1 json"),
        ["block(1)"],
        label="full iter1",
    )
    assert_negated_bodies_absent(
        load_single_json_match(out_inc, "derivation-inc-after-prune2-*.json", label="inc iter2 json"),
        ["block(3)"],
        label="inc iter2",
    )


def case_nonrecursive_mixed_timestamp_views(souffle_bin: Path, work_root: Path) -> None:
    case_src = CASES_ROOT / "nonrecursive_mixed_timestamp_views"
    case_dir = work_root / "nonrecursive_mixed_timestamp_views"
    reset_dir(case_dir)
    shutil.copytree(case_src, case_dir, dirs_exist_ok=True)
    turns = [["delete assign(3, 4)", "insert assign(2, 4) 1.0"]]

    for variant in ("det", "prob"):
        variant_dir = case_dir / variant
        compute_bin, input_dir = compile_compute(souffle_bin=souffle_bin, case_dir=variant_dir)
        out_inc = variant_dir / "out_inc_naive"
        out_full = variant_dir / "out_full_hard"

        run_cli_mode(
            compute_bin=compute_bin,
            input_dir=input_dir,
            output_dir=out_inc,
            mode="inc-naive",
            turns=turns,
            timeout=180,
        )
        run_cli_mode(
            compute_bin=compute_bin,
            input_dir=input_dir,
            output_dir=out_full,
            mode="full",
            turns=turns,
            timeout=180,
        )

        assert_prob_close(out_inc / "facts.prob", out_full / "facts.prob", label=f"timestamp {variant} final")

        inc_iter = parse_prob_file(iter_prob_path(out_inc, 1, "inc-naive"))
        full_iter = parse_prob_file(iter_prob_path(out_full, 1, "full"))
        key = normalize_tuple_key("KEY_IND(1)")
        if key in inc_iter:
            raise CaseFailure(f"timestamp {variant}: spurious KEY_IND(1) remained in inc output")
        if key in full_iter:
            raise CaseFailure(f"timestamp {variant}: unexpected KEY_IND(1) appeared in full output")

        if normalize_table_lines(out_inc / "KEY_IND.csv") != normalize_table_lines(out_full / "KEY_IND.csv"):
            raise CaseFailure(
                f"timestamp {variant}: KEY_IND.csv mismatch\n"
                f"inc={out_inc / 'KEY_IND.csv'}\nfull={out_full / 'KEY_IND.csv'}"
            )


def case_canonical_online_cli_surface(souffle_bin: Path, work_root: Path) -> None:
    case_dir = prepare_case_workspace("canonical_online_cli_surface", work_root)
    compute_bin, input_dir = compile_compute(
        souffle_bin=souffle_bin,
        case_dir=case_dir,
        compile_args=[
            "--setmode=inc-regional",
            "--dump=json",
            "--profile-stage=inc,wmc",
        ],
    )

    output_dir = case_dir / "out_canonical_cli"
    cli_script = "\n".join(
        [
            "show config",
            "set dump stat",
            "set profile-stage fc",
            "unset profile-stage inc",
            "setmode full",
            "show config",
            "commit",
            "q",
        ]
    ) + "\n"
    proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=output_dir,
        script_text=cli_script,
        timeout=240,
    )

    assert_stdout_contains(
        proc.stdout,
        "mode=inc-regional dumps=json profile-stages=inc,wmc",
        label="canonical online cli initial config",
    )
    assert_stdout_contains(
        proc.stdout,
        "mode=full dumps=json,stat profile-stages=fc,wmc",
        label="canonical online cli updated config",
    )
    assert_stdout_not_contains(proc.stdout, "PARSED ", label="canonical online cli hides parser trace")
    assert_stdout_not_contains(proc.stdout, "[Debug]", label="canonical online cli hides graph debug trace")
    assert_stdout_not_contains(proc.stdout, "[CUDD]", label="canonical online cli hides CUDD trace")
    assert_stdout_not_contains(proc.stdout, "[pipeline]", label="canonical online cli hides pipeline trace")
    assert_stdout_not_contains(proc.stdout, "reading:", label="canonical online cli hides input trace")
    assert_stdout_not_contains(proc.stdout, "[det-analysis]", label="canonical online cli hides det trace")
    assert_stdout_not_contains(proc.stdout, "[inc-iter", label="canonical online cli hides turn trace")
    assert_stdout_not_contains(proc.stdout, "[applyDelta", label="canonical online cli hides graph delta trace")
    assert_stdout_not_contains(proc.stdout, " took ", label="canonical online cli hides timer trace")
    assert_stdout_not_contains(proc.stdout, "Inserting tuple:", label="canonical online cli hides staging trace")
    assert_stdout_not_contains(
        proc.stdout,
        "Incremental Souffle CLI (Callback Version)",
        label="canonical online cli hides non-interactive banner",
    )
    assert_path_exists(output_dir / "fact-iter1-full.prob", label="canonical online cli full artifact")
    assert_glob_nonempty(output_dir, "derivation-full-after-prune1-*.json", label="canonical online cli json dump")
    assert_glob_nonempty(output_dir, "graph-*.json", label="canonical online cli graph stats")

    quiet_regional_output_dir = case_dir / "out_canonical_cli_quiet_regional"
    quiet_regional_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=quiet_regional_output_dir,
        script_text="insert edge(10,20)\ncommit\nq\n",
        timeout=240,
    )
    for noisy in [
        "reading:",
        "[det-analysis]",
        "[inc-iter",
        "[applyDelta",
        "[prune-inc",
        "[buildFormulasCyclewise]",
        "[inc-analyze]",
        "[inc-regional",
        "[least-parents]",
        "[Info] Attached evidence",
        " took ",
        "Inserting tuple:",
        "Incremental Souffle CLI (Callback Version)",
    ]:
        assert_stdout_not_contains(
            quiet_regional_proc.stdout,
            noisy,
            label=f"canonical online cli quiet regional hides {noisy}",
        )
    assert_path_exists(
        quiet_regional_output_dir / "fact-iter1-inc-regional.prob",
        label="canonical online cli quiet regional artifact",
    )
    assert_glob_empty(
        quiet_regional_output_dir,
        "initial-input-relations-iter*.txt",
        label="canonical online cli default output omits initial input relation dumps",
    )
    assert_path_missing(
        quiet_regional_output_dir / "det-relations.txt",
        label="canonical online cli default output omits det-relations.txt",
    )
    assert_path_missing(
        quiet_regional_output_dir / "det-scc.txt",
        label="canonical online cli default output omits det-scc.txt",
    )

    help_output_dir = case_dir / "out_canonical_cli_help"
    help_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=help_output_dir,
        script_text="help\nq\n",
        timeout=240,
    )
    if any(line.strip().startswith("dump") for line in help_proc.stdout.splitlines()):
        raise CaseFailure(f"canonical online cli help exposes removed dump command\nstdout:\n{help_proc.stdout}")

    removed_dump_output_dir = case_dir / "out_canonical_cli_removed_dump"
    removed_dump_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=removed_dump_output_dir,
        script_text="dump\nq\n",
        timeout=240,
    )
    assert_stdout_contains(
        removed_dump_proc.stdout,
        "Unknown command: dump",
        label="canonical online cli rejects removed dump command",
    )

    startup_only_dump_output_dir = case_dir / "out_canonical_cli_startup_only_dump"
    startup_only_dump_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=startup_only_dump_output_dir,
        script_text="show config\nunset dump json-before-graph\nshow config\nq\n",
        extra_args=["--dump=json-before-graph"],
        timeout=240,
    )
    assert_path_exists(
        startup_only_dump_output_dir / "derivation-before-graph.json",
        label="canonical online cli startup json-before-graph artifact",
    )
    assert_stdout_contains(
        startup_only_dump_proc.stdout,
        "mode=inc-regional dumps=json,json-before-graph profile-stages=inc,wmc",
        label="canonical online cli shows startup-only json-before-graph",
    )
    assert_stdout_contains(
        startup_only_dump_proc.stdout,
        "dump json-before-graph is only evaluated during startup graph construction",
        label="canonical online cli rejects mutable json-before-graph unset",
    )
    assert_stdout_contains(
        startup_only_dump_proc.stdout,
        "mode=inc-regional dumps=json,json-before-graph profile-stages=inc,wmc",
        label="canonical online cli preserves startup-only dump config after unset rejection",
    )

    arity_output_dir = case_dir / "out_canonical_cli_arity"
    arity_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=arity_output_dir,
        script_text="insert edge(10,20)\ndelete edge(10)\nlist\nq\n",
        timeout=240,
    )
    assert_stdout_not_contains(
        arity_proc.stdout,
        "Overlapped insertion and deletion removed.",
        label="canonical online cli ignores mismatched-arity overlap",
    )
    assert_stdout_contains(
        arity_proc.stdout,
        "1. insert 1::edge(10, 20)",
        label="canonical online cli keeps mismatched insert pending",
    )
    assert_stdout_contains(
        arity_proc.stdout,
        "2. delete edge(10)",
        label="canonical online cli keeps mismatched delete pending",
    )

    arity_reverse_output_dir = case_dir / "out_canonical_cli_arity_reverse"
    arity_reverse_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=arity_reverse_output_dir,
        script_text="insert edge(10)\ndelete edge(10,20)\nlist\nq\n",
        timeout=240,
    )
    assert_stdout_not_contains(
        arity_reverse_proc.stdout,
        "Overlapped insertion and deletion removed.",
        label="canonical online cli ignores reverse mismatched-arity overlap",
    )
    assert_stdout_contains(
        arity_reverse_proc.stdout,
        "1. insert 1::edge(10)",
        label="canonical online cli keeps reverse mismatched insert pending",
    )
    assert_stdout_contains(
        arity_reverse_proc.stdout,
        "2. delete edge(10, 20)",
        label="canonical online cli keeps reverse mismatched delete pending",
    )

    overlap_output_dir = case_dir / "out_canonical_cli_overlap"
    overlap_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=overlap_output_dir,
        script_text="insert edge(10,20)\ndelete edge(10,20)\nlist\nq\n",
        timeout=240,
    )
    assert_stdout_not_contains(
        overlap_proc.stdout,
        "Overlapped insertion and deletion removed.",
        label="canonical online cli hides overlap trace by default",
    )
    assert_stdout_contains(
        overlap_proc.stdout,
        "No pending operations.",
        label="canonical online cli exact overlap clears pending operations",
    )

    verbose_output_dir = case_dir / "out_canonical_cli_verbose"
    verbose_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=verbose_output_dir,
        script_text="insert edge(10,20)\ndelete edge(10,20)\nlist\nq\n",
        extra_args=["--verbose"],
        timeout=240,
    )
    assert_stdout_contains(
        verbose_proc.stdout,
        "[pipeline] recorded ruleapps:",
        label="canonical online cli verbose pipeline trace",
    )
    assert_stdout_contains(
        verbose_proc.stdout,
        "Overlapped insertion and deletion removed.",
        label="canonical online cli verbose overlap trace",
    )
    assert_stdout_contains(
        verbose_proc.stdout,
        "No pending operations.",
        label="canonical online cli verbose exact overlap clears pending operations",
    )

    verbose_regional_output_dir = case_dir / "out_canonical_cli_verbose_regional"
    verbose_regional_proc = run_cli_script(
        compute_bin=compute_bin,
        input_dir=input_dir,
        output_dir=verbose_regional_output_dir,
        script_text="insert edge(10,20)\ncommit\nq\n",
        extra_args=["--verbose"],
        timeout=240,
    )
    assert_stdout_contains(
        verbose_regional_proc.stdout,
        "[inc-analyze] timing(ms):",
        label="canonical online cli verbose regional analysis trace",
    )


CASES = {
    "dred_mix_naive_vs_full": case_dred_mix_naive_vs_full,
    "dred_hub_rederive_naive_vs_full": case_dred_hub_rederive_naive_vs_full,
    "deterministic_inc_naive_combo_vs_full": case_deterministic_inc_naive_combo_vs_full,
    "deterministic_inc_regional_single_round_vs_full": case_deterministic_inc_regional_single_round_vs_full,
    "inc_regional_calibration_single_interface": case_inc_regional_calibration_single_interface,
    "inc_regional_shared_delta_join_vs_full": case_inc_regional_shared_delta_join_vs_full,
    "inc_regional_independent_overlap_skip_vs_full": case_inc_regional_independent_overlap_skip_vs_full,
    "inc_regional_deterministic_chain_anchor": case_inc_regional_deterministic_chain_anchor,
    "inc_regional_deterministic_join_anchor_reject": case_inc_regional_deterministic_join_anchor_reject,
    "deterministic_inc_regional_multiturn_state_machine": case_deterministic_inc_regional_multiturn_state_machine,
    "deterministic_inc_regional_multiturn_degenerate": case_deterministic_inc_regional_multiturn_degenerate,
    "deterministic_recursive_derivation_guard_vs_full": case_deterministic_recursive_derivation_guard_vs_full,
    "negated_absent_tuple_universe": case_negated_absent_tuple_universe,
    "nonrecursive_mixed_timestamp_views": case_nonrecursive_mixed_timestamp_views,
    "canonical_online_cli_surface": case_canonical_online_cli_surface,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run one incremental artifact regression case")
    parser.add_argument("--case", required=True, choices=sorted(CASES.keys()))
    parser.add_argument("--souffle-bin", required=True, help="Path to repo-built souffle binary")
    parser.add_argument("--work-root", required=True, help="Directory for per-case temporary work")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    souffle_bin = Path(args.souffle_bin).resolve()
    work_root = Path(args.work_root).resolve()
    work_root.mkdir(parents=True, exist_ok=True)

    if not souffle_bin.is_file():
        print(f"error: souffle binary path is not a file: {souffle_bin}", file=sys.stderr)
        return 2

    try:
        CASES[args.case](souffle_bin, work_root)
    except CaseFailure as err:
        print(f"[regression:{args.case}] FAIL\n{err}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as err:
        print(f"[regression:{args.case}] TIMEOUT: {err}", file=sys.stderr)
        return 1

    print(f"[regression:{args.case}] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
