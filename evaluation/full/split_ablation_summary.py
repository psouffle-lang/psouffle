#!/usr/bin/env python3
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

root = Path(sys.argv[1])
rows = list(csv.DictReader((root / "RQ3result.tsv").open(), delimiter="\t"))
by_case = defaultdict(dict)
for row in rows:
    if row["Variant"] in ("11_split", "11_nosplit") and row["Status"] == "OK":
        by_case[(row["Category"], row["Case"])][row["Variant"]] = float(row["Avg_s"])

for category in ("taint", "symbolization"):
    pairs = [v for (cat, _), v in by_case.items()
             if cat == category and "11_split" in v and "11_nosplit" in v]
    split = [v["11_split"] for v in pairs]
    nosplit = [v["11_nosplit"] for v in pairs]
    ratios = [n / s for s, n in zip(split, nosplit)]  # >1 means splitting faster
    print("\t".join([
        category,
        f"cases={len(pairs)}",
        f"split_mean={statistics.mean(split):.6f}",
        f"nosplit_mean={statistics.mean(nosplit):.6f}",
        f"total_ratio={sum(nosplit)/sum(split):.6f}",
        f"geomean_nosplit_over_split={math.exp(statistics.mean(map(math.log, ratios))):.6f}",
        f"median_ratio={statistics.median(ratios):.6f}",
        f"split_wins={sum(r > 1 for r in ratios)}",
        f"nosplit_wins={sum(r < 1 for r in ratios)}",
    ]))

def probabilities(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if " : " not in line:
            continue
        key, value = line.rsplit(" : ", 1)
        result[key] = float(value)
    return result

def equivalent(left, right, tolerance=1e-8):
    a, b = probabilities(left), probabilities(right)
    if a.keys() != b.keys():
        return False
    return all(abs(a[k] - b[k]) <= tolerance + 1e-12 for k in a)

taint_checked = taint_bad = 0
bad_examples = []
for left in (root / "taint" / "runs").glob("*/11_split/run_*/*/stages/*/output/facts.prob"):
    right = Path(str(left).replace("/11_split/", "/11_nosplit/"))
    if right.is_file():
        taint_checked += 1
        bad = not equivalent(left, right)
        taint_bad += bad
        if bad and len(bad_examples) < 3:
            bad_examples.append(str(left))

symbol_checked = symbol_bad = 0
for case in (root / "symbolization").iterdir():
    if not case.is_dir() or case.name.startswith("_"):
        continue
    split_runs = sorted((case / "11_split").glob("run_*/output/facts.prob"))
    nosplit_runs = sorted((case / "11_nosplit").glob("run_*/output/facts.prob"))
    for left, right in zip(split_runs, nosplit_runs):
        symbol_checked += 1
        symbol_bad += not equivalent(left, right)
print(f"correctness\ttaint={taint_checked-taint_bad}/{taint_checked}"
      f"\tsymbolization={symbol_checked-symbol_bad}/{symbol_checked}")
if bad_examples:
    print("taint_mismatch_examples\t" + "\t".join(bad_examples))
