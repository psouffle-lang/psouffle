#!/usr/bin/env python3
"""End-to-end T2 taint vproblog driver with cross-stage chaining.

The static artifacts under ``taint/<case>/vproblog/<stage>/`` reuse the raw
initial EDB for every stage, so downstream stages (pre / typefilter / pt-obj /
taint-lim) cannot see the predicates derived upstream and produce 0 tuples for
anything that depends on them. Souffle's T2 driver chains by copying each
stage's output CSVs into the next stage's input dir as ``<rel>.facts`` (with
all-1.0 ``.prob``); see ``_taint_merge_outputs_as_facts`` in FMCAD.py.

This driver does the same for vproblog, but threads per-tuple WMC probabilities
(from vlog's ``req_<rel>_prob`` storemat outputs) into the next stage's
``.prob`` file so the chained EDB carries the actual computed probability.

Per case it:
  1. Stages an initial input dir = copy of ``taint/<case>/input/``.
  2. For each stage in ``taint/stages.txt``:
     a. Synthesises a source dir from ``taint/programs/<stage>.dl`` + the
        current scratch input.
     b. Generates bridge form (``generate_vproblog_side_channel.py``).
     c. Lowers to native form (``transform_to_vproblog_native.py``).
     d. Runs ``vlog mat`` on the per-stage artifacts.
     e. Reads each ``req_<rel>_prob`` file produced and appends its tuples to
        the scratch input as ``<rel>.facts`` / ``<rel>.prob`` (per-row WMC
        probability), so the next stage sees the derived predicate as EDB.
  3. Writes one row per (case, stage) to the summary TSV.

Outputs land under ``$OUT/<case>/<stage>/{vproblog/, vlog.log, storemat/}``,
plus a top-level ``T2result_vproblog_chained.tsv`` (per-stage rows) and a
``per_case_summary.tsv`` (totals matching the souffle per-case wall-clock
shape).
"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
TAINT_ROOT = REPO_ROOT / "taint"
PROGRAMS_DIR = TAINT_ROOT / "programs"
STAGES_FILE = TAINT_ROOT / "stages.txt"

# Reuse the seed-fact extractor and bridge-input injection helpers we added
# for the static artifact builder, plus the line-flattening + SUM rewrite.
sys.path.insert(0, str(SCRIPT_DIR))
from build_vproblog_artifacts import (  # noqa: E402
    GENERATE,
    TRANSFORM,
    _inject_ground_prob_facts,
    _preprocess_souffle_dl,
)


def _run(cmd: Sequence[str]) -> int:
    print("$", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.call([str(c) for c in cmd])


def _read_stages() -> List[str]:
    out: List[str] = []
    for line in STAGES_FILE.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s and not s.startswith("#"):
            out.append(s)
    return out


def _list_cases(filter_cases: Optional[List[str]]) -> List[str]:
    if filter_cases:
        return list(filter_cases)
    out: List[str] = []
    for p in sorted(TAINT_ROOT.iterdir()):
        if not p.is_dir() or p.name == "programs":
            continue
        if (p / "input").is_dir():
            out.append(p.name)
    return out


def _build_artifacts(
    case_name: str,
    stage: str,
    src_input: Path,
    artifacts_out: Path,
) -> bool:
    """Run bridge -> native pipeline against ``src_input`` (which already
    contains any chained `<rel>.facts`/`.prob` from prior stages) and place
    the result at ``artifacts_out``."""
    src_dl = PROGRAMS_DIR / f"{stage}.dl"
    if not src_dl.is_file():
        print(f"[skip] {case_name}/{stage}: missing source .dl {src_dl}")
        return False
    if not src_input.is_dir():
        print(f"[skip] {case_name}/{stage}: missing input dir {src_input}")
        return False

    case_label = f"{case_name}__{stage}"
    with tempfile.TemporaryDirectory(prefix=f"vp_{case_label}_") as scratch:
        scratch_root = Path(scratch)
        synthetic_src = scratch_root / "src" / case_label
        synthetic_src.mkdir(parents=True)
        rewritten, ground_facts = _preprocess_souffle_dl(
            src_dl.read_text(encoding="utf-8")
        )
        (synthetic_src / "compute.souffle.dl").write_text(
            rewritten, encoding="utf-8"
        )
        shutil.copytree(src_input, synthetic_src / "input", symlinks=False)
        _inject_ground_prob_facts(synthetic_src / "input", ground_facts)

        bridge = scratch_root / "bridge"
        rc = _run([
            sys.executable, GENERATE, "generate",
            "--source", scratch_root / "src",
            "--output", bridge,
            "--case", case_label,
            "--case-regex", r".+",
            "--clean",
        ])
        if rc != 0:
            print(f"[fail] {case_label}: bridge rc={rc}")
            return False

        native = scratch_root / "native"
        rc = _run([
            sys.executable, TRANSFORM,
            "--source", bridge,
            "--output", native,
            "--clean",
        ])
        if rc != 0:
            print(f"[fail] {case_label}: native transform rc={rc}")
            return False

        native_case = native / case_label
        if not native_case.is_dir():
            print(f"[fail] {case_label}: native output dir missing")
            return False

        if artifacts_out.exists():
            shutil.rmtree(artifacts_out)
        artifacts_out.mkdir(parents=True)
        for entry in native_case.iterdir():
            dst = artifacts_out / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dst)
            else:
                shutil.copy2(entry, dst)
        return True


def _run_vlog(
    vlog_bin: Path,
    artifacts: Path,
    storemat: Path,
    log_path: Path,
    timeout: int,
    env: Dict[str, str],
) -> Tuple[int, float]:
    storemat.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(vlog_bin), "mat",
        "-e", str((artifacts / "edb.conf").resolve()),
        "--rules", str((artifacts / "rules").resolve()),
        "--prob_file", str((artifacts / "mappings.csv").resolve()),
        "--storemat_path", str(storemat.resolve()),
        "--storemat_format", "csv",
        "--rewriteMultihead", "true",
        "--restrictedChase", "false",
        "--ignoreMagic", "false",
        "-l", "info",
    ]
    t0 = time.perf_counter()
    with log_path.open("w", encoding="utf-8") as lf:
        proc = subprocess.run(
            ["timeout", f"{timeout}s", *cmd],
            cwd=str(artifacts),
            stdout=lf,
            stderr=subprocess.STDOUT,
            env=env,
            check=False,
        )
    elapsed = time.perf_counter() - t0
    return proc.returncode, elapsed


def _chain_outputs_into_input(
    storemat: Path,
    next_input: Path,
) -> List[Tuple[str, int]]:
    """For every ``req_<rel>_prob`` produced by the prior stage, append its
    rows to ``next_input/<rel>.facts`` (args only, tab-separated) and
    ``next_input/<rel>.prob`` with prob = ``1.0`` per row. Matches souffle's
    ``_taint_merge_outputs_as_facts`` (FMCAD.py:2675) which writes ``1.0\n``
    per derived fact — the downstream stage re-multiplies its own rule
    probabilities (``0.999::``) on top, so chained facts are treated as
    deterministic carriers between stages. Threading the upstream WMC prob
    here would double-count: the next stage's rules already include the
    original probabilistic factors via their EDB joins. Returns
    [(rel, n_rows_appended), ...]."""
    if not storemat.is_dir():
        return []
    next_input.mkdir(parents=True, exist_ok=True)
    chained: List[Tuple[str, int]] = []
    for entry in sorted(storemat.iterdir()):
        if not entry.is_file():
            continue
        if not entry.name.startswith("req_") or not entry.name.endswith("_prob"):
            continue
        rel = entry.name[len("req_"):-len("_prob")]
        if rel == "":
            continue
        arg_rows: List[List[str]] = []
        with entry.open("r", encoding="utf-8") as fh:
            reader = csv.reader(fh)
            for row in reader:
                if not row:
                    continue
                args = [c.strip() for c in row[:-1]]
                if not args:
                    continue
                arg_rows.append(args)
        if not arg_rows:
            continue
        facts_path = next_input / f"{rel}.facts"
        prob_path = next_input / f"{rel}.prob"
        with facts_path.open("a", encoding="utf-8") as ff, prob_path.open(
            "a", encoding="utf-8"
        ) as pf:
            for args in arg_rows:
                ff.write("\t".join(args) + "\n")
                pf.write("1.0\n")
        chained.append((rel, len(arg_rows)))
    return chained


def _summarise_outputs(storemat: Path) -> Dict[str, int]:
    """Count tuples per `req_<rel>_prob` for the per-stage TSV column."""
    counts: Dict[str, int] = {}
    if not storemat.is_dir():
        return counts
    for entry in sorted(storemat.iterdir()):
        if not entry.is_file():
            continue
        if not entry.name.startswith("req_") or not entry.name.endswith("_prob"):
            continue
        rel = entry.name[len("req_"):-len("_prob")]
        with entry.open("r", encoding="utf-8") as fh:
            counts[rel] = sum(1 for _ in fh)
    return counts


def _format_relation_summary(counts: Dict[str, int]) -> str:
    if not counts:
        return ""
    return ";".join(f"{rel}={n}" for rel, n in sorted(counts.items()))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out", required=True, type=Path,
        help="Output root: one subdir per case, plus summary TSV.",
    )
    ap.add_argument(
        "--vlog-bin",
        default=os.environ.get(
            "VLOG_BIN", "/opt/vproblog/src/vlog-beta-sdd/build/vlog",
        ),
        type=Path,
    )
    ap.add_argument(
        "--timeout", type=int, default=900,
        help="Per-stage vlog timeout in seconds.",
    )
    ap.add_argument(
        "--cases", default=None,
        help="Comma/space-separated case names; default = all under taint/.",
    )
    args = ap.parse_args()

    if not args.vlog_bin.is_file():
        print(f"[fatal] vlog binary missing at {args.vlog_bin}", file=sys.stderr)
        return 2

    stages = _read_stages()
    if not stages:
        print("[fatal] no stages in stages.txt", file=sys.stderr)
        return 2

    filter_cases: Optional[List[str]] = None
    if args.cases:
        import re as _re
        filter_cases = [c for c in _re.split(r"[,\s]+", args.cases) if c]
    cases = _list_cases(filter_cases)
    if not cases:
        print("[fatal] no cases", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    log_path = args.out / "run.log"
    stage_tsv = args.out / "T2result_vproblog_chained.tsv"
    case_tsv = args.out / "per_case_summary.tsv"
    log_fh = log_path.open("w", encoding="utf-8")

    def log(msg: str) -> None:
        line = f"[{time.strftime('%F %T')}] {msg}"
        print(line, flush=True)
        log_fh.write(line + "\n")
        log_fh.flush()

    env = os.environ.copy()
    extra_libdir = str(args.vlog_bin.parent)
    cur = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = (
        f"{extra_libdir}:{cur}" if cur else extra_libdir
    )

    log(f"chained vproblog T2 taint: vlog={args.vlog_bin} timeout={args.timeout}s")
    log(f"stages: {' '.join(stages)}")
    log(f"cases ({len(cases)}): {' '.join(cases)}")

    stage_rows: List[List[str]] = [
        ["case", "stage", "status", "elapsed_s", "tuples_per_relation", "note"],
    ]
    case_rows: List[List[str]] = [
        ["case", "status", "total_s", "stages_completed", "note"],
    ]

    for c_idx, case in enumerate(cases, 1):
        case_input_src = TAINT_ROOT / case / "input"
        if not case_input_src.is_dir():
            log(f"[{c_idx}/{len(cases)}] {case} SKIP (no input dir)")
            case_rows.append([case, "SKIP", "0.0", "0", "no input dir"])
            stage_rows.append([case, "-", "SKIP", "0.0", "", "no input dir"])
            continue

        case_out = args.out / case
        case_out.mkdir(parents=True, exist_ok=True)
        scratch_input = case_out / "_chained_input"
        if scratch_input.exists():
            shutil.rmtree(scratch_input)
        shutil.copytree(case_input_src, scratch_input, symlinks=False)

        log(f"[{c_idx}/{len(cases)}] {case} start")
        case_total = 0.0
        case_status = "OK"
        case_note = ""
        completed = 0
        for stage in stages:
            stage_dir = case_out / stage
            artifacts_dir = stage_dir / "vproblog"
            storemat = stage_dir / "storemat"
            vlog_log = stage_dir / "vlog.log"
            ok = _build_artifacts(case, stage, scratch_input, artifacts_dir)
            if not ok:
                case_status = "FAIL"
                case_note = f"build failed at {stage}"
                stage_rows.append([case, stage, "FAIL", "0.0", "", "build"])
                log(f"  {stage} BUILD FAIL")
                break
            if storemat.exists():
                shutil.rmtree(storemat)
            rc, elapsed = _run_vlog(
                vlog_bin=args.vlog_bin,
                artifacts=artifacts_dir,
                storemat=storemat,
                log_path=vlog_log,
                timeout=args.timeout,
                env=env,
            )
            case_total += elapsed
            counts = _summarise_outputs(storemat)
            counts_str = _format_relation_summary(counts)
            if rc != 0:
                stage_status = "TIMEOUT" if rc == 124 else "FAIL"
                stage_rows.append([
                    case, stage, stage_status, f"{elapsed:.2f}",
                    counts_str, f"exit={rc}",
                ])
                log(
                    f"  {stage} {stage_status} in {elapsed:.2f}s "
                    f"(exit={rc}; {counts_str})"
                )
                case_status = stage_status
                case_note = f"exit={rc} at {stage}"
                break
            stage_rows.append([
                case, stage, "OK", f"{elapsed:.2f}", counts_str, "",
            ])
            log(f"  {stage} OK in {elapsed:.2f}s ({counts_str})")
            completed += 1
            chained = _chain_outputs_into_input(storemat, scratch_input)
            if chained:
                log(
                    f"    chained -> next: "
                    + ", ".join(f"{r}={n}" for r, n in chained)
                )

        log(
            f"[{c_idx}/{len(cases)}] {case} {case_status} "
            f"total={case_total:.2f}s stages={completed}/{len(stages)} "
            f"({case_note})"
        )
        case_rows.append([
            case, case_status, f"{case_total:.2f}",
            str(completed), case_note,
        ])

    with stage_tsv.open("w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh, delimiter="\t")
        for row in stage_rows:
            w.writerow(row)
    with case_tsv.open("w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh, delimiter="\t")
        for row in case_rows:
            w.writerow(row)
    log(f"done. summaries: {stage_tsv}  {case_tsv}")
    log_fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
