#!/usr/bin/env python3
"""One-shot helper: materialize per-case vProbLog artefacts under each
benchmark case directory. NOT wired into FMCAD.py; run by hand once and
commit the outputs.

Per case it produces ``<case>/vproblog/{rules, edb.conf, mappings.csv,
data/<rel>.csv}`` by chaining
``scripts/generate_vproblog_side_channel.py`` (Souffle .dl + input/ ->
bridge form) into ``scripts/transform_to_vproblog_native.py`` (bridge ->
TcP-native t_/agg_/tt_/d_/req_).

Usage::

    python3 scripts/build_vproblog_artifacts.py sc          # side_channel/full/P*
    python3 scripts/build_vproblog_artifacts.py taint       # taint/<case>/<stage>
    python3 scripts/build_vproblog_artifacts.py symbol      # symbolization/<case>
    python3 scripts/build_vproblog_artifacts.py all
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


# Souffle's `V = sum X : { atom(args) }` -> vlog-native `V = sum(X, atom(args))`.
# The transform_to_vproblog_native script then lifts F/I columns inside the
# witness atom. Wildcard `_` arg fillers are replaced by fresh WC_* vars per
# occurrence — vlog treats `_` as a string constant, not a don't-care.
_SOUFFLE_SUM_RE = re.compile(
    r"(?P<lhs>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*sum\s+"
    r"(?P<wit>[A-Za-z_][A-Za-z0-9_]*)\s*:\s*"
    r"\{\s*(?P<atom>[A-Za-z_][A-Za-z0-9_]*\s*\([^{}]*\))\s*\}"
)

# Probabilistic ground facts in the .dl source: `pX::head(args).` (no `:-`).
# Souffle treats these as IDB seed facts; the bridge generator drops them
# (line 327 `if ":-" not in s: return None`), which strands recursive
# predicates that depend on the seed (cipt-cg's ci_reachableM, pt-obj's
# reachableCM). We extract them here and inject as `<rel>.facts`/`.prob`
# into the staged input dir so the bridge picks them up as EDB.
_GROUND_PROB_FACT_RE = re.compile(
    r"^\s*(?P<prob>[0-9]+(?:\.[0-9]+)?)\s*::\s*"
    r"(?P<head>[A-Za-z_][A-Za-z0-9_]*)\s*\((?P<args>[^()]*)\)\s*\.\s*$"
)


def _replace_wildcards(atom_text: str, prefix: str) -> str:
    """Replace each `_` occurring as a top-level argument in atom_text with
    a fresh WC_<prefix>_<idx> variable. Other `_` (inside identifiers, in
    strings) are untouched."""
    open_paren = atom_text.find("(")
    if open_paren < 0:
        return atom_text
    head = atom_text[: open_paren + 1]
    inner = atom_text[open_paren + 1 : atom_text.rfind(")")]
    tail = atom_text[atom_text.rfind(")") :]
    parts: List[str] = []
    depth = 0
    cur: List[str] = []
    for ch in inner:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    out_parts: List[str] = []
    for i, p in enumerate(parts):
        stripped = p.strip()
        if stripped == "_":
            ws = p[: len(p) - len(p.lstrip())]
            te = p[len(p.rstrip()):]
            out_parts.append(f"{ws}WC_{prefix}_{i}{te}")
        else:
            out_parts.append(p)
    return head + ",".join(out_parts) + tail


def _flatten_multiline_rules(text: str) -> str:
    """Join Souffle multi-line rules onto single lines. The bridge
    generator splits on ``\\n`` and looks for ``:-`` per line, so a rule
    written as::

        head(...) :-
            body1(...),
            body2(...).

    silently loses its body. Approach: walk lines, accumulate into a
    buffer until a line ends with ``.`` (outside of strings); emit the
    flattened buffer as one logical line."""
    out: List[str] = []
    buf: List[str] = []
    in_string = False
    for line in text.splitlines():
        s = line.rstrip()
        if not buf and (
            s.lstrip().startswith("//")
            or s.lstrip().startswith(".")
            or not s.strip()
        ):
            out.append(line)
            continue
        buf.append(s)
        # Detect terminating `.` outside of strings. A rule terminates when
        # the trailing non-whitespace char is a `.` not preceded by a digit
        # (which would be a fractional number). Multi-char operators like
        # `=:=` don't appear in Souffle .dl.
        joined = " ".join(buf).strip()
        # Track string state across the joined line
        depth_in_string = False
        for ch in joined:
            if ch == '"':
                depth_in_string = not depth_in_string
        if not depth_in_string and joined.endswith("."):
            out.append(joined)
            buf = []
    if buf:
        out.append(" ".join(buf).strip())
    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


def _preprocess_souffle_dl(text: str) -> Tuple[str, List[Tuple[str, str, str]]]:
    """Rewrite Souffle aggregates the bridge generator can't pass through.
    Currently handles: ``V = sum X : { atom(...) }`` -> ``V = sum(X, atom(...))``
    with `_` wildcard args replaced by fresh per-occurrence WC_* vars.
    Also flattens multi-line rules (the generator's per-line ``:-`` split
    drops bodies otherwise).

    Returns ``(rewritten_text, ground_prob_facts)`` where ground_prob_facts
    is a list of (relation_name, args_text, prob_text) extracted from the
    .dl source — these are emitted as EDB by the caller."""
    text = _flatten_multiline_rules(text)
    out_lines: List[str] = []
    ground_facts: List[Tuple[str, str, str]] = []
    for idx, line in enumerate(text.splitlines()):
        # Probabilistic ground facts (no `:-`) survive the SUM rewrite untouched
        # because they don't match _SOUFFLE_SUM_RE. Strip them here so they
        # don't reach the bridge generator (which would silently drop them).
        m_fact = _GROUND_PROB_FACT_RE.match(line)
        if m_fact is not None and ":-" not in line:
            ground_facts.append(
                (m_fact.group("head"), m_fact.group("args").strip(), m_fact.group("prob"))
            )
            continue
        new_line = line
        for m in list(_SOUFFLE_SUM_RE.finditer(new_line)):
            atom = _replace_wildcards(m.group("atom"), f"sum_{idx}")
            replacement = f"{m.group('lhs')} = sum({m.group('wit')}, {atom})"
            new_line = new_line[: m.start()] + replacement + new_line[m.end():]
        out_lines.append(new_line)
    return (
        "\n".join(out_lines) + ("\n" if text.endswith("\n") else ""),
        ground_facts,
    )


def _inject_ground_prob_facts(
    input_dir: Path, ground_facts: Sequence[Tuple[str, str, str]],
) -> None:
    """Append each ground probabilistic fact to ``<rel>.facts`` and ``<rel>.prob``
    in input_dir so the bridge generator picks them up as ordinary EDB rows."""
    if not ground_facts:
        return
    for rel, args_text, prob_text in ground_facts:
        cols = [c.strip() for c in args_text.split(",")] if args_text else []
        facts_path = input_dir / f"{rel}.facts"
        prob_path = input_dir / f"{rel}.prob"
        with facts_path.open("a", encoding="utf-8") as f:
            f.write("\t".join(cols) + "\n")
        with prob_path.open("a", encoding="utf-8") as f:
            f.write(prob_text + "\n")

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
GENERATE = SCRIPT_DIR / "generate_vproblog_side_channel.py"
TRANSFORM = SCRIPT_DIR / "transform_to_vproblog_native.py"


def _run(cmd: Sequence[str]) -> int:
    print("$", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.call([str(c) for c in cmd])


def _materialize_case(
    case_name: str,
    src_dl: Path,
    src_input: Path,
    out_dir: Path,
) -> bool:
    """Run bridge -> native pipeline. Stages a synthetic source dir holding
    ``compute.souffle.dl`` plus the case input, runs generate + transform,
    moves the native output into ``out_dir/`` and cleans the scratch."""
    if not src_dl.is_file():
        print(f"[skip] {case_name}: missing source .dl {src_dl}")
        return False
    if not src_input.is_dir():
        print(f"[skip] {case_name}: missing input dir {src_input}")
        return False

    with tempfile.TemporaryDirectory(prefix=f"vp_{case_name}_") as scratch:
        scratch_root = Path(scratch)
        synthetic_src = scratch_root / "src" / case_name
        synthetic_src.mkdir(parents=True)
        # generate_vproblog_side_channel reads compute.{souffle,problog}.dl
        # or compute.dl from the case dir, plus input/. Always stage as
        # compute.souffle.dl regardless of the source program's name; the
        # generator's auto-discovery only recognises these canonical names.
        rewritten, ground_facts = _preprocess_souffle_dl(
            src_dl.read_text(encoding="utf-8")
        )
        (synthetic_src / "compute.souffle.dl").write_text(
            rewritten, encoding="utf-8"
        )
        shutil.copytree(src_input, synthetic_src / "input", symlinks=False)
        _inject_ground_prob_facts(synthetic_src / "input", ground_facts)
        if ground_facts:
            print(
                f"[ground] {case_name}: injected "
                + ", ".join(
                    f"{rel}({args})={prob}"
                    for rel, args, prob in ground_facts
                )
            )

        bridge = scratch_root / "bridge"
        rc = _run([
            sys.executable, GENERATE, "generate",
            "--source", scratch_root / "src",
            "--output", bridge,
            "--case", case_name,
            "--case-regex", r".+",
            "--clean",
        ])
        if rc != 0:
            print(f"[fail] {case_name}: bridge generate rc={rc}")
            return False

        native = scratch_root / "native"
        rc = _run([
            sys.executable, TRANSFORM,
            "--source", bridge,
            "--output", native,
            "--clean",
        ])
        if rc != 0:
            print(f"[fail] {case_name}: native transform rc={rc}")
            return False

        native_case = native / case_name
        if not native_case.is_dir():
            print(f"[fail] {case_name}: native output dir missing")
            return False

        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True)
        for entry in native_case.iterdir():
            dst = out_dir / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dst)
            else:
                shutil.copy2(entry, dst)
        print(f"[ok] {case_name}: -> {out_dir}")
        return True


def materialize_sc(filter_cases: Optional[List[str]]) -> int:
    base = REPO_ROOT / "side_channel" / "full"
    n_ok = 0
    for case_dir in sorted(base.iterdir(), key=lambda p: (len(p.name), p.name)):
        if not case_dir.is_dir() or not case_dir.name.startswith("P"):
            continue
        if filter_cases and case_dir.name not in filter_cases:
            continue
        ok = _materialize_case(
            case_name=case_dir.name,
            src_dl=case_dir / "compute.souffle.dl",
            src_input=case_dir / "input",
            out_dir=case_dir / "vproblog",
        )
        if ok:
            n_ok += 1
    return n_ok


def _vlog_run_stage(
    vlog_bin: Path,
    artifact_dir: Path,
    storemat_dir: Path,
    log_path: Path,
    timeout: int,
) -> int:
    """Run `vlog mat` on a freshly materialised stage artifact so its
    req_<rel>_prob outputs can be chained into the next stage's input."""
    storemat_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(vlog_bin), "mat",
        "-e", str((artifact_dir / "edb.conf").resolve()),
        "--rules", str((artifact_dir / "rules").resolve()),
        "--prob_file", str((artifact_dir / "mappings.csv").resolve()),
        "--storemat_path", str(storemat_dir.resolve()),
        "--storemat_format", "csv",
        "--rewriteMultihead", "true",
        "--restrictedChase", "false",
        "--ignoreMagic", "false",
        "-l", "info",
    ]
    env = os.environ.copy()
    extra = str(vlog_bin.parent)
    cur = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{extra}:{cur}" if cur else extra
    print("$", " ".join(cmd), flush=True)
    with log_path.open("w", encoding="utf-8") as lf:
        proc = subprocess.run(
            ["timeout", f"{timeout}s", *cmd],
            stdout=lf, stderr=subprocess.STDOUT, env=env,
        )
    return proc.returncode


def _chain_storemat_into_input(
    storemat_dir: Path, next_input: Path,
) -> List[Tuple[str, int]]:
    """Append every req_<rel>_prob row to next_input/<rel>.facts +
    .prob (prob=1.0). Mirrors souffle's _taint_merge_outputs_as_facts —
    chained facts are deterministic carriers; downstream rules re-multiply
    their own probabilistic factors. Returns [(rel, n_appended), ...]."""
    if not storemat_dir.is_dir():
        return []
    next_input.mkdir(parents=True, exist_ok=True)
    chained: List[Tuple[str, int]] = []
    for entry in sorted(storemat_dir.iterdir()):
        if not entry.is_file():
            continue
        if not entry.name.startswith("req_") or not entry.name.endswith("_prob"):
            continue
        rel = entry.name[len("req_"):-len("_prob")]
        if not rel:
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
        with (next_input / f"{rel}.facts").open("a", encoding="utf-8") as ff, \
             (next_input / f"{rel}.prob").open("a", encoding="utf-8") as pf:
            for args in arg_rows:
                ff.write("\t".join(args) + "\n")
                pf.write("1.0\n")
        chained.append((rel, len(arg_rows)))
    return chained


def materialize_taint(
    filter_cases: Optional[List[str]],
    vlog_bin: Optional[Path] = None,
    chain_timeout: int = 1800,
) -> int:
    """Materialise taint per-stage artifacts under taint/<case>/vproblog/<stage>/.

    If ``vlog_bin`` is provided (default), each stage is built off a per-case
    scratch input that accumulates upstream stages' req_<rel>_prob outputs as
    chained <rel>.facts/.prob — so by the time taint-lim is built its
    input already contains pt/fpt/reachableCM/label*/sink* etc. as ordinary
    EDB. The bridge generator then emits esrc_<rel> + the matching first-phase
    rule, and the committed artefact is self-sufficient at runtime (FMCAD.py
    just runs vlog mat per stage; no chaining at runtime).

    If ``vlog_bin`` is None we fall back to the legacy behaviour (each stage
    built off the raw initial input). Downstream stages then derive nothing
    because their EDBs are missing the upstream-derived predicates — taint-lim
    emits 0 req_*_prob. Use only when no vlog binary is available; the result
    is wrong but the directory layout matches.
    """
    base = REPO_ROOT / "taint"
    stages_path = base / "stages.txt"
    if not stages_path.is_file():
        print("[skip] taint: stages.txt missing")
        return 0
    stages = [
        line.strip()
        for line in stages_path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    chained = vlog_bin is not None
    if chained and not vlog_bin.is_file():
        print(f"[fatal] --vlog-bin {vlog_bin} not found; aborting taint chain")
        return 0
    if not chained:
        print(
            "[warn] --vlog-bin not set; building taint stages off initial "
            "input only — downstream stages (taint-lim) will produce 0 prob "
            "output at runtime."
        )

    n_ok = 0
    for case_dir in sorted(base.iterdir(), key=lambda p: p.name):
        if not case_dir.is_dir() or case_dir.name in {"programs"}:
            continue
        if not (case_dir / "input").is_dir():
            continue
        if filter_cases and case_dir.name not in filter_cases:
            continue

        with tempfile.TemporaryDirectory(prefix=f"vp_chain_{case_dir.name}_") as scratch:
            scratch_root = Path(scratch)
            if chained:
                scratch_input = scratch_root / "input"
                shutil.copytree(case_dir / "input", scratch_input, symlinks=False)
            else:
                scratch_input = case_dir / "input"

            for stage in stages:
                stage_dl = base / "programs" / f"{stage}.dl"
                if not stage_dl.is_file():
                    print(f"[skip] {case_dir.name}/{stage}: program missing {stage_dl}")
                    continue

                out_dir = case_dir / "vproblog" / stage
                ok = _materialize_case(
                    case_name=f"{case_dir.name}__{stage}",
                    src_dl=stage_dl,
                    src_input=scratch_input,
                    out_dir=out_dir,
                )
                if not ok:
                    print(f"[fail] {case_dir.name}/{stage}: build failed; aborting case chain")
                    break
                n_ok += 1

                if not chained:
                    continue

                stage_storemat = scratch_root / "storemat" / stage
                stage_log = scratch_root / f"vlog_{stage}.log"
                rc = _vlog_run_stage(
                    vlog_bin=vlog_bin,
                    artifact_dir=out_dir,
                    storemat_dir=stage_storemat,
                    log_path=stage_log,
                    timeout=chain_timeout,
                )
                if rc != 0:
                    print(
                        f"[fail] {case_dir.name}/{stage}: vlog mat rc={rc} "
                        f"(see {stage_log}); downstream chain aborted for case"
                    )
                    break
                rows = _chain_storemat_into_input(stage_storemat, scratch_input)
                if rows:
                    print(
                        f"[chain] {case_dir.name}/{stage}: -> "
                        + ", ".join(f"{r}={n}" for r, n in rows)
                    )
                else:
                    print(f"[chain] {case_dir.name}/{stage}: no req_*_prob outputs")
    return n_ok


def materialize_symbol(filter_cases: Optional[List[str]]) -> int:
    """Symbolization shares one source program (symbolization.dl) across cases.
    This program uses Souffle's `Points = sum X : { ... }` aggregate that the
    bridge generator does NOT rewrite, so the rules file produced here will
    NOT survive vlog's parser without a SUM rewriter. We materialize the
    bridge+native artifacts anyway so the directory layout matches sc/taint;
    callers that need a working program have to rewrite SUM separately.
    """
    base = REPO_ROOT / "symbolization"
    src_dl = base / "symbolization.dl"
    if not src_dl.is_file():
        print(f"[skip] symbolization: source .dl missing at {src_dl}")
        return 0
    n_ok = 0
    for case_dir in sorted(base.iterdir(), key=lambda p: p.name):
        if not case_dir.is_dir():
            continue
        if not (case_dir / "input").is_dir():
            continue
        if filter_cases and case_dir.name not in filter_cases:
            continue
        ok = _materialize_case(
            case_name=case_dir.name,
            src_dl=src_dl,
            src_input=case_dir / "input",
            out_dir=case_dir / "vproblog",
        )
        if ok:
            n_ok += 1
    return n_ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "family",
        choices=("sc", "taint", "symbol", "all"),
        help="Which benchmark family to materialize.",
    )
    ap.add_argument(
        "--cases",
        default=None,
        help="Comma-separated list of case names to limit to "
        "(e.g. 'P1,P5' for sc; 'and-roc' for taint).",
    )
    ap.add_argument(
        "--vlog-bin",
        default=os.environ.get(
            "VLOG_BIN", "/opt/vproblog/src/vlog-beta-sdd/build/vlog"
        ),
        help="vlog binary used to run each taint stage between materializations "
        "so its req_<rel>_prob outputs can be chained into the next stage's "
        "EDB. Pass empty string to disable taint chaining (downstream stages "
        "will then produce 0 prob output).",
    )
    ap.add_argument(
        "--chain-timeout",
        type=int,
        default=1800,
        help="Per-stage vlog mat timeout in seconds during taint chaining.",
    )
    args = ap.parse_args()
    flt: Optional[List[str]]
    flt = None
    if args.cases:
        flt = [c.strip() for c in args.cases.split(",") if c.strip()]

    vlog_bin: Optional[Path]
    vlog_bin = Path(args.vlog_bin) if args.vlog_bin else None

    total = 0
    if args.family in ("sc", "all"):
        total += materialize_sc(flt)
    if args.family in ("taint", "all"):
        total += materialize_taint(
            flt, vlog_bin=vlog_bin, chain_timeout=args.chain_timeout,
        )
    if args.family in ("symbol", "all"):
        total += materialize_symbol(flt)
    print(f"[done] materialized {total} case dirs")
    return 0 if total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
