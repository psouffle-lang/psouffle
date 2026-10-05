#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def write_rows(path: Path, rows: list[tuple[int, ...]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write("\t".join(str(v) for v in row))
            f.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate dred_hub regression case inputs")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir).resolve()
    input_dir = out_dir / "input"
    input_dir.mkdir(parents=True, exist_ok=True)

    write_text(
        out_dir / "compute.dl",
        (
            ".decl seed(x:number)\n"
            ".input seed\n"
            ".decl edge(x:number, y:number)\n"
            ".input edge\n\n"
            ".decl reach(x:number)\n"
            "reach(x) :- seed(x).\n"
            "reach(y) :- reach(x), edge(x,y).\n\n"
            ".decl hot(x:number)\n"
            ".output hot\n"
            "hot(x) :- reach(x).\n"
            "hot(x) :- seed(s), edge(s,x).\n"
        ),
    )

    seed_rows = [(i,) for i in range(1, 9)]
    write_rows(input_dir / "seed.facts", seed_rows)

    edge_rows: list[tuple[int, int]] = []
    for i in range(1, 25):
        edge_rows.append((i, i + 1))
    for i in range(1, 17):
        edge_rows.append((i, 40))
    edge_rows.extend([(40, 41), (41, 42), (42, 43), (10, 30), (12, 32), (32, 43)])
    edge_rows = list(dict.fromkeys(edge_rows))
    write_rows(input_dir / "edge.facts", edge_rows)

    probs = []
    for idx, _ in enumerate(edge_rows):
        p = 0.31 + ((idx * 17) % 59) / 100.0
        probs.append(min(p, 0.97))

    with (input_dir / "edge.prob").open("w", encoding="utf-8") as f:
        for p in probs:
            f.write(f"{p:.12g}\\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
