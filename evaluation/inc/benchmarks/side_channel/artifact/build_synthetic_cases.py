#!/usr/bin/env python3
"""Build provisional synthetic A-cases from P13-P20 side-channel cases."""

from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

SIDE_CHANNEL_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SIDE_CHANNEL_ROOT.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.side_channel.core.side_channel_common import (
    case_name,
    ensure_dirs,
    parse_cases_spec,
)

DEFAULT_SOURCE_DIR = SIDE_CHANNEL_ROOT / "cases"
DEFAULT_BASE_DIR = SIDE_CHANNEL_ROOT / "runs" / "side_channel_inc"


@dataclass(frozen=True)
class Component:
    source: str
    fraction: float = 1.0


DEFAULT_SUITE: Dict[str, List[Component]] = {
    "A1": [Component("P13", 0.70), Component("P14", 0.20)],
    "A2": [Component("P14", 0.80), Component("P13", 0.25)],
    "A3": [Component("P15", 0.75)],
    "A4": [Component("P15", 0.60), Component("P16", 0.25)],
    "A5": [Component("P14", 0.70), Component("P15", 0.35)],
    "A6": [Component("P16", 0.75), Component("P13", 0.30)],
    "A7": [Component("P13", 0.50), Component("P16", 0.55)],
    "A8": [Component("P17", 0.75)],
    "A9": [Component("P18", 0.80)],
    "A10": [Component("P14", 0.60), Component("P16", 0.65)],
    "A11": [Component("P19", 0.65)],
}


def parse_decl_schemas(program: Path) -> Dict[str, List[str]]:
    schemas: Dict[str, List[str]] = {}
    decl_re = re.compile(r"^\.decl\s+([A-Za-z0-9_]+)\((.*)\)")
    for raw in program.read_text(encoding="utf-8").splitlines():
        match = decl_re.match(raw.strip())
        if not match:
            continue
        relation = match.group(1)
        args = match.group(2).strip()
        types: List[str] = []
        if args:
            for part in args.split(","):
                if ":" not in part:
                    continue
                types.append(part.rsplit(":", 1)[1].strip())
        schemas[relation] = types
    return schemas


def parse_input_relations(program: Path) -> Set[str]:
    input_re = re.compile(r"^\.input\s+([A-Za-z0-9_]+)\b")
    inputs: Set[str] = set()
    for raw in program.read_text(encoding="utf-8").splitlines():
        match = input_re.match(raw.strip())
        if match:
            inputs.add(match.group(1))
    return inputs


def ensure_input_directives(program: Path, required_inputs: Iterable[str], schemas: Dict[str, List[str]]) -> None:
    lines = program.read_text(encoding="utf-8").splitlines()
    existing = parse_input_relations(program)
    missing = sorted(relation for relation in required_inputs if relation in schemas and relation not in existing)
    if not missing:
        return

    insert_at = 0
    for idx, raw in enumerate(lines):
        if raw.strip().startswith(".input "):
            insert_at = idx + 1
    if insert_at == 0:
        for idx, raw in enumerate(lines):
            if raw.strip().startswith((".output ", ".decl ", ".type ")):
                insert_at = idx + 1

    updated = lines[:insert_at] + [f".input {relation}" for relation in missing] + lines[insert_at:]
    program.write_text("\n".join(updated) + "\n", encoding="utf-8")


def relation_from_path(path: Path) -> str:
    return path.name.rsplit(".", 1)[0]


def read_relation(input_dir: Path, relation: str) -> List[Tuple[Tuple[str, ...], str]]:
    facts_path = input_dir / f"{relation}.facts"
    prob_path = input_dir / f"{relation}.prob"
    facts = facts_path.read_text(encoding="utf-8").splitlines() if facts_path.exists() else []
    probs = prob_path.read_text(encoding="utf-8").splitlines() if prob_path.exists() else []
    if probs and len(probs) != len(facts):
        raise ValueError(f"{prob_path} has {len(probs)} lines for {len(facts)} facts")
    if not probs:
        probs = ["" for _ in facts]
    rows: List[Tuple[Tuple[str, ...], str]] = []
    for fact, prob in zip(facts, probs):
        if fact.strip():
            rows.append((tuple(fact.split("\t")), prob.strip()))
    return rows


def s_positions(schema: Sequence[str]) -> List[int]:
    return [idx for idx, typ in enumerate(schema) if typ == "s"]


def collect_s_ids(source_dir: Path, schemas: Dict[str, List[str]]) -> Set[int]:
    ids: Set[int] = set()
    for facts_path in (source_dir / "input").glob("*.facts"):
        relation = relation_from_path(facts_path)
        positions = s_positions(schemas.get(relation, []))
        if not positions:
            continue
        for raw in facts_path.read_text(encoding="utf-8").splitlines():
            if not raw.strip():
                continue
            fields = raw.split("\t")
            for pos in positions:
                if pos < len(fields):
                    ids.add(int(fields[pos]))
    return ids


def build_adjacency(source_dir: Path, schemas: Dict[str, List[str]]) -> Dict[int, Set[int]]:
    adjacency: Dict[int, Set[int]] = defaultdict(set)
    for facts_path in (source_dir / "input").glob("*.facts"):
        relation = relation_from_path(facts_path)
        positions = s_positions(schemas.get(relation, []))
        if len(positions) < 2:
            continue
        for raw in facts_path.read_text(encoding="utf-8").splitlines():
            if not raw.strip():
                continue
            fields = raw.split("\t")
            values = [int(fields[pos]) for pos in positions if pos < len(fields)]
            for left in values:
                adjacency.setdefault(left, set())
                for right in values:
                    if left != right:
                        adjacency[left].add(right)
    return adjacency


def select_ids(source_dir: Path, schemas: Dict[str, List[str]], fraction: float, seed: int) -> Optional[Set[int]]:
    all_ids = collect_s_ids(source_dir, schemas)
    if fraction >= 0.999 or not all_ids:
        return None
    target = max(1, int(round(len(all_ids) * fraction)))
    adjacency = build_adjacency(source_dir, schemas)
    rng = random.Random(seed)
    remaining = set(all_ids)
    selected: Set[int] = set()
    while remaining and len(selected) < target:
        start = rng.choice(sorted(remaining))
        queue: deque[int] = deque([start])
        remaining.discard(start)
        while queue and len(selected) < target:
            current = queue.popleft()
            selected.add(current)
            neighbors = [node for node in adjacency.get(current, set()) if node in remaining]
            rng.shuffle(neighbors)
            for node in neighbors:
                remaining.discard(node)
                queue.append(node)
    return selected


def transform_row(
        fields: Tuple[str, ...],
        schema: Sequence[str],
        offset: int,
        selected_ids: Optional[Set[int]],
) -> Optional[Tuple[str, ...]]:
    out = list(fields)
    for pos in s_positions(schema):
        if pos >= len(fields):
            return None
        value = int(fields[pos])
        if selected_ids is not None and value not in selected_ids:
            return None
        out[pos] = str(value + offset)
    return tuple(out)


def source_sort_key(source: str) -> Tuple[str, int]:
    match = re.fullmatch(r"([A-Za-z]+)(\d+)", source)
    return (match.group(1), int(match.group(2))) if match else (source, 0)


def build_case(
        target: str,
        components: Sequence[Component],
        source_root: Path,
        base_dir: Path,
        seed: int,
        force: bool,
) -> Path:
    target_dir = base_dir / target
    if target_dir.exists():
        if not force:
            raise FileExistsError(f"{target_dir} exists; pass --force to replace it")
        shutil.rmtree(target_dir)
    ensure_dirs(target_dir / "input")

    template_source = max((component.source for component in components), key=source_sort_key)
    template_dir = source_root / template_source
    shutil.copy2(template_dir / "compute.souffle.dl", target_dir / "compute.souffle.dl")
    schemas = parse_decl_schemas(template_dir / "compute.souffle.dl")
    component_inputs: Set[str] = set()
    for component in components:
        component_inputs.update(parse_input_relations(source_root / component.source / "compute.souffle.dl"))
    ensure_input_directives(target_dir / "compute.souffle.dl", component_inputs, schemas)
    relations = sorted(schemas)
    merged: Dict[str, List[Tuple[Tuple[str, ...], str]]] = {relation: [] for relation in relations}
    seen: Dict[str, Set[Tuple[str, ...]]] = {relation: set() for relation in relations}
    provenance = {
        "target": target,
        "template_source": template_source,
        "components": [],
    }

    next_offset = 0
    for idx, component in enumerate(components):
        source_dir = source_root / component.source
        component_schemas = parse_decl_schemas(source_dir / "compute.souffle.dl")
        selected = select_ids(
            source_dir,
            component_schemas,
            component.fraction,
            seed + idx * 1000 + source_sort_key(component.source)[1],
        )
        s_ids = collect_s_ids(source_dir, component_schemas)
        offset = next_offset
        next_offset += (max(s_ids) + 1) if s_ids else 0
        kept = 0
        total = 0
        for relation in relations:
            rows = read_relation(source_dir / "input", relation)
            schema = component_schemas.get(relation, schemas.get(relation, []))
            for fields, prob in rows:
                total += 1
                transformed = transform_row(fields, schema, offset, selected)
                if transformed is None or transformed in seen[relation]:
                    continue
                seen[relation].add(transformed)
                merged[relation].append((transformed, prob))
                kept += 1
        provenance["components"].append({
            "source": component.source,
            "fraction": component.fraction,
            "offset": offset,
            "source_s_ids": len(s_ids),
            "selected_s_ids": len(selected) if selected is not None else len(s_ids),
            "source_facts": total,
            "kept_facts": kept,
        })

    for relation in relations:
        facts_path = target_dir / "input" / f"{relation}.facts"
        prob_path = target_dir / "input" / f"{relation}.prob"
        rows = merged[relation]
        facts_path.write_text("".join("\t".join(fields) + "\n" for fields, _ in rows), encoding="utf-8")
        prob_path.write_text("".join((prob or "1.0") + "\n" for _, prob in rows), encoding="utf-8")
    (target_dir / "synthetic-provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return target_dir


def parse_component_spec(raw: str) -> Component:
    if ":" in raw:
        source, frac_raw = raw.split(":", 1)
        parsed = parse_cases_spec(source)
        if len(parsed) != 1:
            raise ValueError(f"component source must be one case: {source}")
        return Component(case_name(parsed[0]), float(frac_raw))
    parsed = parse_cases_spec(raw)
    if len(parsed) != 1:
        raise ValueError(f"component source must be one case: {raw}")
    return Component(case_name(parsed[0]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR)
    parser.add_argument("--base-dir", type=Path, default=DEFAULT_BASE_DIR)
    parser.add_argument("--cases", default="A1-A11")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--force", action="store_true")
    parser.add_argument(
        "--component",
        action="append",
        default=[],
        metavar="A=SRC[:FRAC][,SRC[:FRAC]...]",
        help="Override or add one target case composition",
    )
    args = parser.parse_args()

    suite = dict(DEFAULT_SUITE)
    for raw in args.component:
        if "=" not in raw:
            raise SystemExit(f"invalid --component: {raw}")
        target, spec = raw.split("=", 1)
        suite[case_name(target)] = [parse_component_spec(part.strip()) for part in spec.split(",") if part.strip()]

    requested = [case_name(case) for case in parse_cases_spec(args.cases)]
    for target in requested:
        if target not in suite:
            raise SystemExit(f"no synthetic recipe for {target}")
        out = build_case(target, suite[target], args.source_dir, args.base_dir, args.seed, args.force)
        print(out)


if __name__ == "__main__":
    main()
