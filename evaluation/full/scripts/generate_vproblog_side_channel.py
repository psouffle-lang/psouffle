#!/usr/bin/env python3
"""Convert side-channel benchmark cases into vProbLog scenario folders.

Expected source case layout (default: side_channel_full/P*):
  <case>/
    compute.dl (or compute.souffle.dl / compute.lightmagic.dl / compute.problog.dl
                or an already lowered vProbLog-style rules file)
    input/<relation>.facts
    input/<relation>.prob

Generated layout per case:
  <output>/<case>/
    rules
    edb.conf
    mappings.csv
    data/<relation>.csv
    metadata.json

The key point is that many vProbLog rule files use source EDB predicates of the form
`esrc_<rel>(..., FID)`, where the final `FID` (for example `ff123`) is looked up in
`mappings.csv`. This script supports both styles:

1. `bridge` style
   Rules use the original relation name/arity and we generate helper rules
   `<rel>(...) :- esrc_<rel>(..., FID).`

2. `esrc` style
   Rules already directly reference `esrc_<rel>(..., FID)`, so we emit `esrc_*` CSVs
   and do NOT append bridge rules.

Default mode is `auto`, which inspects the rules file.

Phase handling for vProbLog:
- If source rules already contain explicit phase comments (`// input`, `// final`,
  `// firstphaseEdb`, ...), they are preserved.
- Otherwise, all normalized rules are emitted under `input`, and `.output` predicates
  are appended as minimal `final` anchor rules so WMC runs on the intended outputs.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# Optional: when --seed is given we regenerate the source case's
# compute.dl + probability-oopsla.txt + input/ from the SMT, using the
# same seeding scheme as side_channel_full_evaluation.py (seed + case_num).
# These imports are guarded because the side_channel_benchmark package
# isn't always on sys.path in environments that only want the vProbLog
# transform.
try:
    from side_channel_benchmark.tools import batch_convert as _sc_batch  # noqa: E402
    from side_channel_benchmark.tools import program_builder as _sc_program  # noqa: E402
    _SC_AVAILABLE = True
except Exception:  # noqa: BLE001
    _sc_batch = None
    _sc_program = None
    _SC_AVAILABLE = False

RULE_CANDIDATES = [
    "compute.lightmagic.dl",
    "compute.souffle.dl",
    "compute.dl",
    "compute.problog.dl",
]
MANIFEST_NAME = "cases_manifest.tsv"

PROB_PREFIX_RE = re.compile(
    r"^\s*[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?::\s*(?P<rest>.+)$"
)
CASE_DEFAULT_RE = re.compile(r"^P\d+$", re.IGNORECASE)
ESRC_PRED_RE = re.compile(r"\besrc_([A-Za-z_][A-Za-z0-9_]*)\s*\(")
OUTPUT_DECL_RE = re.compile(r"^\s*\.output\s+([A-Za-z_][A-Za-z0-9_]*)\b")
DECL_RE = re.compile(r"^\s*\.decl\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)")
HEAD_PRED_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(")
PHASE_COMMENT_RE = re.compile(r"^\s*//\s*([A-Za-z_][A-Za-z0-9_]*)\b")

PHASE_NAMES = (
    "input",
    "firstphaseEdb",
    "secondphaseEdb",
    "firstphasenonRecIdb",
    "firstphaseRecIdb",
    "secondphaseIdb",
    "final",
)
PHASE_NAME_BY_KEY = {name.lower(): name for name in PHASE_NAMES}


@dataclass(frozen=True)
class Case:
    name: str
    directory: Path


@dataclass(frozen=True)
class RulesProfile:
    uses_esrc_predicates: bool
    esrc_predicates: Tuple[str, ...]


@dataclass(frozen=True)
class RulesBuildResult:
    kept_rules: int
    rules_profile: RulesProfile
    output_predicates: Tuple[str, ...]
    final_rules_marked: int
    predicate_arities: Tuple[Tuple[str, int], ...]
    final_head_predicates: Tuple[str, ...]
    has_explicit_phases: bool


@dataclass
class CaseStats:
    case_name: str
    source_dir: Path
    output_dir: Path
    rules_source: Path
    relations: int
    rows_total: int
    rows_probabilistic: int
    rows_deterministic: int
    mapping_entries: int
    missing_probs: int
    edb_mode: str
    bridge_rules_added: int


def ensure_empty_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def discover_cases(
    source_root: Path,
    case_names: Optional[Sequence[str]],
    recursive: bool,
    case_name_regex: re.Pattern[str],
) -> List[Case]:
    if case_names:
        out: List[Case] = []
        for name in case_names:
            case_dir = source_root / name
            if not case_dir.is_dir():
                raise FileNotFoundError(f"Case directory not found: {case_dir}")
            out.append(Case(name=name, directory=case_dir))
        return out

    candidates: List[Path]
    if recursive:
        candidates = sorted(
            [
                p
                for p in source_root.rglob("*")
                if p.is_dir() and (p / "input").is_dir()
            ],
            key=lambda p: str(p),
        )
    else:
        candidates = sorted(
            [p for p in source_root.iterdir() if p.is_dir()],
            key=lambda p: p.name,
        )

    out = []
    for case_dir in candidates:
        if not case_name_regex.match(case_dir.name):
            continue
        if not (case_dir / "input").is_dir():
            continue
        out.append(Case(name=case_dir.name, directory=case_dir))

    return out


def choose_rules_source(case_dir: Path, override: Optional[str]) -> Path:
    if override:
        p = case_dir / override
        if p.is_file():
            return p
        if Path(override).is_file():
            return Path(override)
        raise FileNotFoundError(f"Rules source not found: {override}")

    for name in RULE_CANDIDATES:
        p = case_dir / name
        if p.is_file():
            return p
    raise FileNotFoundError(f"No rules file found in {case_dir}")


def _strip_prob_prefix(head_text: str) -> str:
    """Return the head atom without the optional ``p::`` annotation.

    The annotation itself is intentionally preserved in the emitted rule line
    so that the patched vProbLog parser can apply ProbLog's per-grounding
    distribution semantics directly; this helper is only used internally for
    predicate-name/arity inference.
    """
    m = PROB_PREFIX_RE.match(head_text.strip())
    if m is not None:
        return m.group("rest").strip()
    return head_text.strip()


# Predicate names we must NOT capitalise when normalising rule text. These
# are atom heads (pred(...)) and function-term names, which must stay in
# their original case so the head/body atoms still identify the right
# predicate. Anything INSIDE the parens is an argument list where we need
# to capitalise identifiers that look like variables.
_ATOM_PRED_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)(\s*\()")
_VAR_CANDIDATE_RE = re.compile(r"\b([a-z_][A-Za-z0-9_]*)\b")


def _uppercase_args(text: str) -> str:
    """Rewrite `pred(a, b, c)` -> `pred(A, B, C)` for every atom — vlog's
    parser treats lowercase-leading identifiers as CONSTANTS, so Souffle-
    style variables like `c`, `v`, `m` silently turn into new dict entries
    and the join evaluates to empty (no tuple matches those constants).

    We scan the text left-to-right and for each `<pred>(<args>)` segment
    only uppercase the leading char of each comma-separated argument if
    the argument looks like a plain identifier. Numbers, strings,
    `_`-only anonymous variables, and nested function terms are all left
    alone.
    """
    out: List[str] = []
    i = 0
    while i < len(text):
        m = _ATOM_PRED_RE.match(text, i)
        if m is None:
            out.append(text[i])
            i += 1
            continue
        pred = m.group(1)
        # Copy the pred name + opening paren verbatim.
        out.append(pred + m.group(2))
        i = m.end()
        # Find the matching close paren, tracking nested parens.
        depth = 1
        start = i
        while i < len(text) and depth > 0:
            ch = text[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        args_text = text[start:i]
        # Rewrite args_text. Split on top-level commas to handle nested
        # function terms (like conj(F0, F1)).
        parts: List[str] = []
        buf: List[str] = []
        d = 0
        for ch in args_text:
            if ch == "(":
                d += 1
                buf.append(ch)
            elif ch == ")":
                d -= 1
                buf.append(ch)
            elif ch == "," and d == 0:
                parts.append("".join(buf))
                buf = []
            else:
                buf.append(ch)
        if buf:
            parts.append("".join(buf))

        def fix_arg(arg: str) -> str:
            raw = arg.strip()
            prefix_ws = arg[: len(arg) - len(arg.lstrip())]
            suffix_ws = arg[len(arg.rstrip()):]
            if not raw:
                return arg
            # Recursive: the arg itself may be a nested atom/function term.
            if "(" in raw:
                return prefix_ws + _uppercase_args(raw) + suffix_ws
            # Anonymous var `_` or `_name`: leave alone.
            if raw.startswith("_"):
                return arg
            # Numeric / negative-numeric constant: leave.
            if raw[0].isdigit() or (raw[0] == "-" and len(raw) > 1 and raw[1].isdigit()):
                return arg
            # Quoted string: leave.
            if raw.startswith('"'):
                return arg
            # Identifier starting lowercase: uppercase first char.
            if raw[0].islower():
                return prefix_ws + raw[0].upper() + raw[1:] + suffix_ws
            return arg

        new_parts = [fix_arg(p) for p in parts]
        out.append(",".join(new_parts))
        # Consume the closing `)` we stopped at.
        if i < len(text):
            out.append(text[i])
            i += 1
    return "".join(out)


def _normalize_rule_line(line: str) -> Optional[str]:
    s = line.strip()
    if not s:
        return None
    if s.startswith("//"):
        return s
    if s.startswith("."):
        return None
    if ":-" not in s:
        return None

    head, body = s.split(":-", 1)
    # Preserve the probability prefix verbatim — the current vProbLog build
    # accepts ``p::head :- body.`` and mints a fresh random variable per
    # grounding, matching Souffle's semantics for the same probabilistic rule.
    head = head.strip()
    body = body.strip()

    # vProbLog parser expects ~ negation.
    body = body.replace("\\+", "~")
    body = re.sub(r"!(?=\s*[\w(])", "~", body)

    # Uppercase variable names (lowercase-leading identifiers inside atom
    # arg lists). vlog's parser treats lowercase identifiers as CONSTANTS —
    # leaving Souffle-style variables like `c`, `v`, `o`, `m` as-is causes
    # every join involving them to produce 0 tuples.
    head = _uppercase_args(head)
    body = _uppercase_args(body)
    # Also uppercase bare `VAR <op> ...` constraints in the body where the
    # operator is `!=`, `==`, or single `=`. Souffle's grouped equalities
    # like `(int_res0 = 0)` and arithmetic bindings like `(tmp = (X band Y))`
    # leave the LHS lowercase otherwise, and vlog would silently treat the
    # identifier as a string constant — every join referencing the captured
    # value evaluates to 0 tuples. The negative lookahead on `=` keeps `==`
    # / `!=` / `>=` / `<=` from double-firing.
    body = re.sub(
        r"(?<![A-Za-z0-9_])([a-z_][A-Za-z0-9_]*)\s*(!=|==|=(?!=))",
        lambda m: m.group(1)[0].upper() + m.group(1)[1:] + " " + m.group(2),
        body,
    )
    # Same treatment when the variable is on the RHS of `<op>`: e.g.
    # `(0 = int_res0)` or `(Times - 1) = times_new`.
    body = re.sub(
        r"(!=|==|=(?!=)|<=?|>=?)\s*([a-z_][A-Za-z0-9_]*)(?![A-Za-z0-9_(])",
        lambda m: m.group(1) + " " + m.group(2)[0].upper() + m.group(2)[1:],
        body,
    )

    normalized = f"{head} :- {body}"
    if not normalized.endswith("."):
        normalized += "."
    return normalized


def _head_predicate(normalized_rule: str) -> Optional[str]:
    if normalized_rule.startswith("//"):
        return None
    head = normalized_rule.split(":-", 1)[0].strip()
    head = _strip_prob_prefix(head)
    m = HEAD_PRED_RE.match(head)
    if m is None:
        return None
    return m.group(1)


def _extract_phase_marker(line: str) -> Optional[str]:
    m = PHASE_COMMENT_RE.match(line)
    if m is None:
        return None
    return PHASE_NAME_BY_KEY.get(m.group(1).lower())


def _split_top_level_args(text: str) -> List[str]:
    parts: List[str] = []
    cur: List[str] = []
    depth = 0
    for ch in text:
        if ch == "(":
            depth += 1
            cur.append(ch)
            continue
        if ch == ")":
            if depth > 0:
                depth -= 1
            cur.append(ch)
            continue
        if ch == "," and depth == 0:
            part = "".join(cur).strip()
            if part != "":
                parts.append(part)
            cur = []
            continue
        cur.append(ch)
    last = "".join(cur).strip()
    if last != "":
        parts.append(last)
    return parts


def _find_matching_paren(text: str, start_idx: int) -> int:
    depth = 0
    for idx in range(start_idx, len(text)):
        ch = text[idx]
        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth -= 1
            if depth == 0:
                return idx
    return -1


def _head_predicate_and_arity(normalized_rule: str) -> Tuple[Optional[str], Optional[int]]:
    head_pred = _head_predicate(normalized_rule)
    if head_pred is None:
        return None, None
    head = normalized_rule.split(":-", 1)[0].strip()
    head = _strip_prob_prefix(head)
    open_idx = head.find("(")
    if open_idx < 0:
        return head_pred, 0
    close_idx = _find_matching_paren(head, open_idx)
    if close_idx < 0:
        return head_pred, None
    inner = head[open_idx + 1 : close_idx].strip()
    if inner == "":
        return head_pred, 0
    return head_pred, len(_split_top_level_args(inner))


def build_rules_file(source_rules: Path, output_rules: Path) -> RulesBuildResult:
    # (normalized rule text, current phase name, head predicate)
    normalized_rules: List[Tuple[str, str, Optional[str]]] = []
    kept_rules = 0
    esrc_predicates: Set[str] = set()
    output_predicates: Set[str] = set()
    predicate_arities: Dict[str, int] = {}
    final_head_predicates: Set[str] = set()
    final_rules_marked = 0
    has_explicit_phases = False
    current_phase = "input"

    with source_rules.open("r", encoding="utf-8") as f:
        for raw in f:
            phase_marker = _extract_phase_marker(raw)
            if phase_marker is not None:
                current_phase = phase_marker
                has_explicit_phases = True
                continue

            out_decl = OUTPUT_DECL_RE.match(raw)
            if out_decl is not None:
                output_predicates.add(out_decl.group(1))

            decl = DECL_RE.match(raw)
            if decl is not None:
                pred = decl.group(1)
                args_blob = decl.group(2).strip()
                arity = 0 if args_blob == "" else len([x for x in args_blob.split(",") if x.strip() != ""])
                predicate_arities.setdefault(pred, arity)

            normalized = _normalize_rule_line(raw)
            if normalized is None or normalized.startswith("//"):
                continue

            head_pred, head_arity = _head_predicate_and_arity(normalized)
            if head_pred is not None and head_arity is not None:
                predicate_arities.setdefault(head_pred, head_arity)

            normalized_rules.append((normalized, current_phase, head_pred))
            kept_rules += 1

            for match in ESRC_PRED_RE.finditer(normalized):
                esrc_predicates.add(match.group(1))

            if current_phase == "final":
                final_rules_marked += 1
                if head_pred is not None:
                    final_head_predicates.add(head_pred)

    lines_out: List[str] = []
    if normalized_rules:
        if has_explicit_phases:
            written_phase: Optional[str] = None
            for normalized, phase, _ in normalized_rules:
                if phase != written_phase:
                    lines_out.append(f"// {phase}")
                    written_phase = phase
                lines_out.append(normalized)
        else:
            lines_out.append("// input")
            for normalized, _, _ in normalized_rules:
                lines_out.append(normalized)

    output_rules.write_text(
        "\n".join(lines_out) + ("\n" if lines_out else ""),
        encoding="utf-8",
    )
    return RulesBuildResult(
        kept_rules=kept_rules,
        rules_profile=RulesProfile(
            uses_esrc_predicates=bool(esrc_predicates),
            esrc_predicates=tuple(sorted(esrc_predicates)),
        ),
        output_predicates=tuple(sorted(output_predicates)),
        final_rules_marked=final_rules_marked,
        predicate_arities=tuple(sorted(predicate_arities.items(), key=lambda kv: kv[0])),
        final_head_predicates=tuple(sorted(final_head_predicates)),
        has_explicit_phases=has_explicit_phases,
    )


def _read_nonempty_lines(path: Path) -> List[str]:
    rows: List[str] = []
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n\r")
            if line.strip() == "":
                continue
            rows.append(line)
    return rows


def _split_fact_columns(fact_line: str) -> List[str]:
    if "\t" in fact_line:
        return [c.strip() for c in fact_line.split("\t")]
    if "," in fact_line:
        return [c.strip() for c in fact_line.split(",")]
    return [fact_line.strip()]


def choose_edb_mode(
    requested_mode: str,
    rules_profile: RulesProfile,
    always_id: bool,
    inline_prob_id: bool,
) -> str:
    if requested_mode != "auto":
        return requested_mode

    if rules_profile.uses_esrc_predicates:
        return "esrc"

    if always_id or inline_prob_id:
        return "bridge"

    # Most probabilistic benchmark cases have separate .prob files and require ff* ids.
    # When the rules are not already esrc_* style, fall back to bridge mode.
    return "bridge"


def write_data_and_mapping(
    input_dir: Path,
    out_data_dir: Path,
    mapping_file: Path,
    id_prefix: str,
    edb_mode: str,
    skip_empty_relations: bool,
) -> Tuple[List[str], List[Tuple[str, int]], int, int, int, int, int]:
    facts_files = sorted(input_dir.glob("*.facts"), key=lambda p: p.name)
    if not facts_files:
        raise FileNotFoundError(f"No .facts files in {input_dir}")

    out_data_dir.mkdir(parents=True, exist_ok=True)

    mapping_rows: List[Tuple[str, str]] = []
    relation_names: List[str] = []
    bridge_specs: List[Tuple[str, int]] = []
    rows_total = 0
    rows_prob = 0
    rows_det = 0
    missing_probs = 0
    next_id = 0

    use_source_ids = edb_mode in {"bridge", "esrc"}
    emit_bridge_rules = edb_mode == "bridge"

    for facts_path in facts_files:
        rel = facts_path.stem
        prob_path = input_dir / f"{rel}.prob"
        if not prob_path.is_file():
            raise FileNotFoundError(f"Missing probability file: {prob_path}")

        fact_lines = _read_nonempty_lines(facts_path)
        prob_lines = _read_nonempty_lines(prob_path)
        if len(fact_lines) != len(prob_lines):
            raise ValueError(
                f"Line mismatch for {rel}: facts={len(fact_lines)} prob={len(prob_lines)}"
            )

        if len(fact_lines) == 0 and skip_empty_relations:
            continue

        out_rel = f"esrc_{rel}" if use_source_ids else rel
        relation_names.append(out_rel)
        if emit_bridge_rules and fact_lines:
            arity = len(_split_fact_columns(fact_lines[0]))
            bridge_specs.append((rel, arity))

        out_csv_path = out_data_dir / f"{out_rel}.csv"
        with out_csv_path.open("w", encoding="utf-8", newline="") as out_f:
            writer = csv.writer(out_f)
            for fact_line, prob_line in zip(fact_lines, prob_lines):
                cols = _split_fact_columns(fact_line)
                prob = prob_line.strip()
                if prob == "":
                    missing_probs += 1
                    prob = "1.0"

                if use_source_ids:
                    fid = f"{id_prefix}{next_id}"
                    next_id += 1
                    writer.writerow([*cols, fid])
                    mapping_rows.append((fid, prob))
                    rows_prob += 1
                else:
                    writer.writerow(cols)
                    rows_det += 1
                rows_total += 1

    with mapping_file.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        for fid, prob in mapping_rows:
            writer.writerow([fid, prob])

    return (
        relation_names,
        bridge_specs,
        rows_total,
        rows_prob,
        rows_det,
        len(mapping_rows),
        missing_probs,
    )


def _build_bridge_rule(rel: str, arity: int) -> str:
    vars_ = [f"V{i}" for i in range(arity)]
    head = f"{rel}({','.join(vars_)})"
    body = f"esrc_{rel}({','.join([*vars_, 'FID'])})"
    return f"{head} :- {body}."


def append_bridge_rules(
    rules_path: Path,
    bridge_specs: Sequence[Tuple[str, int]],
    force_input_phase: bool,
) -> int:
    if not bridge_specs:
        return 0
    emitted: List[str] = []
    seen = set()
    for rel, arity in sorted(bridge_specs, key=lambda x: x[0]):
        key = (rel, arity)
        if key in seen:
            continue
        seen.add(key)
        emitted.append(_build_bridge_rule(rel, arity))
    if not emitted:
        return 0
    with rules_path.open("a", encoding="utf-8") as f:
        if rules_path.stat().st_size > 0:
            f.write("\n")
        if force_input_phase:
            f.write("// input\n")
        f.write("\n".join(emitted))
        f.write("\n")
    return len(emitted)


def _build_final_anchor_rule(predicate: str, arity: int) -> str:
    if arity <= 0:
        return f"{predicate}() :- {predicate}()."
    vars_ = [f"V{i}" for i in range(arity)]
    args = ",".join(vars_)
    return f"{predicate}({args}) :- {predicate}({args})."


def append_output_final_rules(
    rules_path: Path,
    output_predicates: Sequence[str],
    predicate_arities: Dict[str, int],
    existing_final_heads: Set[str],
) -> int:
    if not output_predicates:
        return 0

    emitted: List[str] = []
    for pred in sorted(output_predicates):
        if pred in existing_final_heads:
            continue
        arity = predicate_arities.get(pred)
        if arity is None:
            continue
        emitted.append(_build_final_anchor_rule(pred, arity))

    if not emitted:
        return 0

    with rules_path.open("a", encoding="utf-8") as f:
        if rules_path.stat().st_size > 0:
            f.write("\n")
        f.write("// final\n")
        f.write("\n".join(emitted))
        f.write("\n")
    return len(emitted)


def write_edb_conf(path: Path, relation_names: Iterable[str]) -> None:
    lines: List[str] = []
    for i, rel in enumerate(sorted(relation_names)):
        lines.extend(
            [
                f"EDB{i}_predname={rel}",
                f"EDB{i}_type=INMEMORY",
                f"EDB{i}_param0=data",
                f"EDB{i}_param1={rel}",
            ]
        )
    path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def write_case(
    case: Case,
    output_root: Path,
    rules_override: Optional[str],
    id_prefix: str,
    requested_edb_mode: str,
    always_id: bool,
    inline_prob_id: bool,
    skip_empty_relations: bool,
) -> CaseStats:
    input_dir = case.directory / "input"
    if not input_dir.is_dir():
        raise FileNotFoundError(f"Missing input directory: {input_dir}")

    rules_source = choose_rules_source(case.directory, rules_override)

    case_out = output_root / case.name
    ensure_empty_dir(case_out)
    data_out = case_out / "data"

    rules_path = case_out / "rules"
    rules_build = build_rules_file(rules_source, rules_path)
    kept_rules = rules_build.kept_rules
    rules_profile = rules_build.rules_profile
    output_predicates = rules_build.output_predicates
    final_rules_marked = rules_build.final_rules_marked
    predicate_arities = dict(rules_build.predicate_arities)
    existing_final_heads = set(rules_build.final_head_predicates)
    edb_mode = choose_edb_mode(
        requested_mode=requested_edb_mode,
        rules_profile=rules_profile,
        always_id=always_id,
        inline_prob_id=inline_prob_id,
    )

    mappings_path = case_out / "mappings.csv"
    (
        relation_names,
        bridge_specs,
        rows_total,
        rows_prob,
        rows_det,
        mapping_entries,
        missing_probs,
    ) = write_data_and_mapping(
        input_dir=input_dir,
        out_data_dir=data_out,
        mapping_file=mappings_path,
        id_prefix=id_prefix,
        edb_mode=edb_mode,
        skip_empty_relations=skip_empty_relations,
    )

    bridge_rule_count = 0
    if edb_mode == "bridge":
        bridge_rule_count = append_bridge_rules(
            rules_path,
            bridge_specs,
            force_input_phase=rules_build.has_explicit_phases,
        )
        kept_rules += bridge_rule_count

    final_anchor_rule_count = append_output_final_rules(
        rules_path=rules_path,
        output_predicates=output_predicates,
        predicate_arities=predicate_arities,
        existing_final_heads=existing_final_heads,
    )
    kept_rules += final_anchor_rule_count
    final_rules_marked += final_anchor_rule_count

    output_preds_missing_arity = [
        pred for pred in output_predicates if pred not in predicate_arities
    ]

    write_edb_conf(case_out / "edb.conf", relation_names)

    metadata = {
        "case_name": case.name,
        "source_directory": str(case.directory),
        "rules_source": str(rules_source),
        "kept_rules": kept_rules,
        "relations": len(relation_names),
        "rows_total": rows_total,
        "rows_probabilistic": rows_prob,
        "rows_deterministic": rows_det,
        "mapping_entries": mapping_entries,
        "missing_probs": missing_probs,
        "edb_mode": edb_mode,
        "bridge_rules_added": bridge_rule_count,
        "final_anchor_rules_added": final_anchor_rule_count,
        "rules_uses_esrc_predicates": rules_profile.uses_esrc_predicates,
        "rules_esrc_predicates": list(rules_profile.esrc_predicates),
        "rules_has_explicit_phases": rules_build.has_explicit_phases,
        "rules_output_predicates": list(output_predicates),
        "rules_output_predicates_missing_arity": output_preds_missing_arity,
        "rules_final_marked": final_rules_marked,
    }
    (case_out / "metadata.json").write_text(
        json.dumps(metadata, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )

    return CaseStats(
        case_name=case.name,
        source_dir=case.directory,
        output_dir=case_out,
        rules_source=rules_source,
        relations=len(relation_names),
        rows_total=rows_total,
        rows_probabilistic=rows_prob,
        rows_deterministic=rows_det,
        mapping_entries=mapping_entries,
        missing_probs=missing_probs,
        edb_mode=edb_mode,
        bridge_rules_added=bridge_rule_count,
    )


def write_manifest(output_root: Path, stats: Sequence[CaseStats]) -> Path:
    path = output_root / MANIFEST_NAME
    lines = [
        "case\toutput_dir\tsource_dir\trules_source\trelations\trows_total\trows_probabilistic\trows_deterministic\tmapping_entries\tmissing_probs\tedb_mode\tbridge_rules_added"
    ]
    for st in stats:
        lines.append(
            "\t".join(
                [
                    st.case_name,
                    str(st.output_dir),
                    str(st.source_dir),
                    str(st.rules_source),
                    str(st.relations),
                    str(st.rows_total),
                    str(st.rows_probabilistic),
                    str(st.rows_deterministic),
                    str(st.mapping_entries),
                    str(st.missing_probs),
                    st.edb_mode,
                    str(st.bridge_rules_added),
                ]
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


_CASE_NUM_RE = re.compile(r"(\d+)")


def _case_num_from_name(name: str) -> Optional[int]:
    """Extract the numeric suffix from a case name like 'P12' -> 12."""
    m = _CASE_NUM_RE.search(name)
    if m is None:
        return None
    return int(m.group(1))


# Mirror side_channel_full_evaluation.FORCE_TRIMMED_CASES so that seeded
# regen here produces bit-identical compute.dl + probability-oopsla.txt to
# what rebuttal.py / RQ2 pipelines generate. These case numbers have SMT
# sources whose "full" rule set is too heavy to materialize in a reasonable
# budget, so the upstream evaluator silently downgrades them to "trimmed".
_FORCE_TRIMMED_CASES = {1, 3}


def _effective_rule_set(case_num: int, rule_set: str) -> str:
    """Apply the FORCE_TRIMMED override just like side_channel_full_evaluation."""
    if case_num in _FORCE_TRIMMED_CASES and rule_set == "full":
        return "trimmed"
    return rule_set


def _regen_source_for_case(
    source_case: Path,
    case_num: int,
    seed: int,
    rule_set: str,
    force: bool,
) -> None:
    """Regenerate compute.dl + probability-oopsla.txt + input/ in
    `source_case` using (seed + case_num) as the RNG seed. Mirrors the
    side_channel_full_evaluation.py generate_case logic so that Souffle
    and vProbLog pipelines reading from the same source dir see the
    same rule probs and fact probs.
    """
    if not _SC_AVAILABLE:
        raise RuntimeError(
            "--seed requires the side_channel_benchmark package to be "
            "importable from the repo root. Run from the repo root or "
            "set PYTHONPATH accordingly."
        )
    effective = _effective_rule_set(case_num, rule_set)
    compute_dl = source_case / "compute.dl"
    prob_file = source_case / "probability-oopsla.txt"
    if not force and compute_dl.is_file() and prob_file.is_file():
        # Still rerun process_case so input/*.prob tracks compute.dl.
        _sc_program.process_case(source_case, rule_set=effective)
        return

    smt_path = _sc_batch.choose_compute_file(source_case)
    if smt_path is None:
        candidate = source_case / "compute.smt2"
        if candidate.exists():
            smt_path = candidate
    if smt_path is None or not smt_path.exists():
        raise FileNotFoundError(
            f"[{source_case.name}] No compute.smt2 under {source_case}; "
            f"cannot regenerate with --seed"
        )

    seed_val = seed + case_num
    try:
        _sc_batch.random.seed(seed_val)
    except Exception:  # noqa: BLE001
        pass

    souffle_text, prob_text = _sc_batch.convert_smt_to_souffle(
        smt_path.read_text(encoding="utf-8"),
        rule_set=effective,
    )
    compute_dl.write_text(souffle_text, encoding="utf-8")
    prob_file.write_text(prob_text, encoding="utf-8")
    dst_smt = source_case / "compute.smt2"
    if smt_path.resolve() != dst_smt.resolve():
        try:
            shutil.copy2(smt_path, dst_smt)
        except Exception:  # noqa: BLE001
            shutil.copyfile(smt_path, dst_smt)

    # Fill in input/*.facts and input/*.prob from the freshly written
    # compute.dl + probability-oopsla.txt.
    _sc_program.process_case(source_case, rule_set=effective)


def cmd_generate(args: argparse.Namespace) -> int:
    source_root = Path(args.source).resolve()
    output_root = Path(args.output).resolve()
    if not source_root.is_dir():
        print(f"ERROR: source directory not found: {source_root}")
        return 2

    if args.clean and output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    case_regex = re.compile(args.case_regex, re.IGNORECASE)
    cases = discover_cases(
        source_root=source_root,
        case_names=args.case,
        recursive=args.recursive,
        case_name_regex=case_regex,
    )
    if not cases:
        print("ERROR: no cases selected")
        return 2

    print(f"[generate] source={source_root}")
    print(f"[generate] output={output_root}")
    print(f"[generate] cases={len(cases)}")
    print(f"[generate] edb_mode={args.edb_mode}")
    if args.seed is not None:
        print(f"[generate] seed={args.seed} rule_set={args.rule_set} "
              f"force_smt={args.force_smt}")

    stats: List[CaseStats] = []
    for i, case in enumerate(sorted(cases, key=lambda c: c.name), start=1):
        print(f"[generate] [{i}/{len(cases)}] {case.name}")

        if args.seed is not None:
            case_num = _case_num_from_name(case.name)
            if case_num is None:
                print(f"[generate]     WARN: cannot derive case_num from "
                      f"'{case.name}'; skipping seeded regen")
            else:
                try:
                    _regen_source_for_case(
                        source_case=case.directory,
                        case_num=case_num,
                        seed=args.seed,
                        rule_set=args.rule_set,
                        force=args.force_smt,
                    )
                    eff = _effective_rule_set(case_num, args.rule_set)
                    note = ""
                    if eff != args.rule_set:
                        note = f" (FORCE_TRIMMED: {args.rule_set}->{eff})"
                    print(f"[generate]     seeded regen OK "
                          f"(seed={args.seed + case_num}, rule_set={eff}){note}")
                except Exception as exc:  # noqa: BLE001
                    print(f"[generate]     seeded regen FAILED: {exc}")
                    return 3
        st = write_case(
            case=case,
            output_root=output_root,
            rules_override=args.rules_source,
            id_prefix=args.id_prefix,
            requested_edb_mode=args.edb_mode,
            always_id=args.always_id,
            inline_prob_id=args.inline_prob_id,
            skip_empty_relations=not args.keep_empty_relations,
        )
        print(
            f"[generate]     mode={st.edb_mode} relations={st.relations} rows={st.rows_total} mapping_entries={st.mapping_entries} bridge_rules={st.bridge_rules_added}"
        )
        stats.append(st)

    manifest = write_manifest(output_root, stats)
    total_rows = sum(s.rows_total for s in stats)
    total_prob = sum(s.rows_probabilistic for s in stats)
    total_det = sum(s.rows_deterministic for s in stats)
    print("[generate] done")
    print(f"[generate] manifest={manifest}")
    print(
        f"[generate] summary: rows_total={total_rows}, probabilistic={total_prob}, deterministic={total_det}"
    )
    return 0


def _load_mapping(path: Path) -> Dict[str, str]:
    mapping: Dict[str, str] = {}
    if not path.is_file():
        return mapping
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split(",", 1)
            if len(parts) != 2:
                continue
            mapping[parts[0].strip()] = parts[1].strip()
    return mapping


def cmd_validate(args: argparse.Namespace) -> int:
    output_root = Path(args.output).resolve()
    if not output_root.is_dir():
        print(f"ERROR: output directory not found: {output_root}")
        return 2

    case_dirs = sorted(
        [p for p in output_root.iterdir() if p.is_dir() and (p / "data").is_dir()],
        key=lambda p: p.name,
    )
    if not case_dirs:
        print("ERROR: no case directories found")
        return 2

    bad = 0
    checked_rows = 0
    id_like = re.compile(rf"^{re.escape(args.id_prefix)}\d+$")

    for case_dir in case_dirs:
        rules = case_dir / "rules"
        edb = case_dir / "edb.conf"
        mapping = _load_mapping(case_dir / "mappings.csv")
        data_dir = case_dir / "data"
        if not rules.is_file():
            print(f"[validate][ERROR] {case_dir.name}: missing rules")
            bad += 1
        if not edb.is_file():
            print(f"[validate][ERROR] {case_dir.name}: missing edb.conf")
            bad += 1

        for csv_path in sorted(data_dir.glob("*.csv"), key=lambda p: p.name):
            with csv_path.open("r", encoding="utf-8") as f:
                reader = csv.reader(f)
                for row in reader:
                    if not row:
                        continue
                    checked_rows += 1
                    last = row[-1].strip()
                    if id_like.match(last) and last not in mapping:
                        print(
                            f"[validate][ERROR] {case_dir.name}/{csv_path.name}: id '{last}' missing in mappings.csv"
                        )
                        bad += 1
                        break

    if bad:
        print(f"[validate] FAILED bad={bad} checked_rows={checked_rows}")
        return 1
    print(f"[validate] OK cases={len(case_dirs)} checked_rows={checked_rows}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Convert side-channel benchmark cases to vProbLog scenario folders."
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate", help="Generate vProbLog case folders")
    g.add_argument("--source", default=str(REPO_ROOT / "side_channel" / "full"), help="Path to source cases root")
    g.add_argument("--output", default=str(REPO_ROOT / "out" / "side_channel_vproblog"), help="Output root")
    g.add_argument("--clean", action="store_true", help="Delete output root before generation")
    g.add_argument("--case", action="append", help="Specific case name(s), e.g. --case P1 --case P7")
    g.add_argument(
        "--case-regex",
        default=CASE_DEFAULT_RE.pattern,
        help="Regex for auto-discovery when --case is not provided",
    )
    g.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively discover case directories containing input/",
    )
    g.add_argument(
        "--rules-source",
        default=None,
        help="Override rules source file name (relative to case dir) or absolute path",
    )
    g.add_argument("--id-prefix", default="ff", help="Prefix for generated probabilistic fact ids")
    g.add_argument(
        "--edb-mode",
        choices=["auto", "plain", "bridge", "esrc"],
        default="auto",
        help=(
            "How to emit source EDB facts: auto=inspect rules, plain=<rel>(...) only, "
            "bridge=emit esrc_<rel>(...,FID) plus helper rules, esrc=emit esrc_<rel>(...,FID) only"
        ),
    )
    g.add_argument(
        "--always-id",
        action="store_true",
        help="Compatibility flag: in auto mode this nudges plain rules toward bridge mode.",
    )
    g.add_argument(
        "--inline-prob-id",
        action="store_true",
        help="Legacy compatibility flag: same effect as --always-id in auto mode.",
    )
    g.add_argument(
        "--keep-empty-relations",
        action="store_true",
        help="Keep empty CSV relations and include them in edb.conf (not recommended for vProbLog parser).",
    )
    g.add_argument(
        "--seed",
        type=int,
        default=None,
        help=(
            "Regenerate each source case's compute.dl + probability-oopsla.txt "
            "+ input/ from compute.smt2 using (seed + case_num) as the RNG "
            "seed, matching side_channel_full_evaluation.py's seeding. This "
            "makes the vProbLog pipeline's input probabilities identical to "
            "Souffle's for the same --seed. Requires the side_channel_benchmark "
            "package to be importable."
        ),
    )
    g.add_argument(
        "--rule-set",
        default="full",
        choices=["full", "trimmed", "trimmed_plus", "probabilistic_enhanced"],
        help="Rule set passed to sc_batch.convert_smt_to_souffle (only used with --seed).",
    )
    g.add_argument(
        "--force-smt",
        action="store_true",
        help="With --seed: force SMT->compute.dl reconversion even if compute.dl "
             "and probability-oopsla.txt already exist in the source case dir.",
    )
    g.set_defaults(func=cmd_generate)

    v = sub.add_parser("validate", help="Validate generated vProbLog folders")
    v.add_argument("--output", default=str(REPO_ROOT / "out" / "side_channel_vproblog"), help="Output root")
    v.add_argument("--id-prefix", default="ff", help="ID prefix used during generation")
    v.set_defaults(func=cmd_validate)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
