# FMCAD 2026 Full/Rewrite Evaluation

`FMCAD.py` runs the paper's graph-size, engine-comparison, and rewrite-runtime
experiments. It includes resume support and collection from existing logs.
Run the examples below from the repository root, after building PSouffle.
The default compiler is `build/src/souffle`; override it with `--souffle-bin`
or `SOUFFLE_BIN`. Default outputs are under `evaluation/full/artifact/`.

## Benchmarks

| Family | Layout | Cases |
| --- | --- | --- |
| Side-channel information flow | `side_channel/full/P*/compute.souffle.dl` and `input/` | P1 and P3–P20 (19 cases) |
| Android taint analysis | `taint/programs/*.dl`, `taint/stages.txt`, and `<case>/input/` | 15 applications; five chained stages |
| DDisasm symbolization | `symbolization/symbolization.dl` and `<case>/input/` | 16 binaries |

Inputs include line-aligned `.facts`/`.prob` files, ProbLog programs, native
VProbLog rules/EDB/mappings/data, and Scallop `.scl` programs. See the family
READMEs for program descriptions. Taint execution materializes outputs from
each stage as inputs to the next stage.

ProbLog runs stage a copy with `:- unknown(fail).` so empty input relations
have the same closed-world behavior as Datalog. Bundled program files are
preserved.

## Experiments

| Command | Measurement | Main outputs |
| --- | --- | --- |
| `-RQ1` | Derivation-graph sizes and rewrite reduction | `RQ1result.tsv`, `RQ1summary.tsv` (Tables I and II) |
| `-RQ2` | Runtime comparison with selected engines | `RQ2result.tsv` (Table III) |
| `-RQ3` | Runtime with rewrite off (`01`) and on (`11`) | `RQ3result.tsv` (Figure 2) |

Quick PSouffle-only checks:

```bash
python3 evaluation/full/FMCAD.py -RQ1 --only sc --cases 1 --runs 1
python3 evaluation/full/FMCAD.py -RQ3 --only sc --cases 1 --runs 1
python3 evaluation/full/FMCAD.py -RQ2 --only sc --cases 1 --runs 1 --rq2-engines souffle
```

Run all paper workloads:

```bash
python3 evaluation/full/FMCAD.py -RQ1
python3 evaluation/full/FMCAD.py -RQ3
python3 evaluation/full/FMCAD.py -RQ2 --rq2-engines souffle,problog,vproblog
```

`--runs` defaults to five. `--resume` skips completed case/variant cells.
`--only sc`, `--only taint`, or `--only dis` selects families. `--cases`
selects side-channel case numbers; `--taint-cases` and `--dis-cases` select
the other families. Runtime and compilation limits are configurable via
`--rq1-run-timeout`, `--rq2-run-timeout`, `--rq3-run-timeout`, and
`--compile-timeout`. `--mem-limit-mb` sets an optional child-process memory cap.
Full paper workloads can take more than a day, especially with external engines.

Collect existing outputs without rerunning experiments:

```bash
python3 evaluation/full/FMCAD.py -RQ1 --rq1-collect
python3 evaluation/full/FMCAD.py -RQ2 --rq2-collect
python3 evaluation/full/FMCAD.py -RQ3 --rq3-collect
```

Use `--rqN-base-dir` to read or write a different output root. Each live run
writes engine logs and `run.meta.json` for the collectors.

RQ1 reads the graph sizes after pruning and at the rewrite/backend handoff.
It accepts both `rewrite_final_*` and newer `after_rewrite_*` log fields.
The extra `Nodes_Before_Prune`/`Edges_Before_Prune` columns stay blank when
the compiler does not emit raw graph counts before pruning.

## External Engines

PSouffle-only RQ1/RQ3 runs need no external Python packages. For comparisons:

- ProbLog: use the source artifact's `Hughshine/problog` profiling branch,
  with the required SDD/BDD libraries, and put `problog` on `PATH`.
- VProbLog: use `jjjxia/Vproblog`; select the executable with `--vlog-bin` or
  `VLOG_BIN`. The fallback path is `/opt/vproblog/src/vlog-beta-sdd/build/vlog`.
- Scallop: the latest runner supports a fourth engine, selected with
  `--rq2-engines scallop`. The source artifact pins Scallop revision
  `668bfb6d45ce302fd4ffa7f29916baf3c7ce36ef` plus the included
  [scallop/scallop-fmcad.patch](scallop/scallop-fmcad.patch). Select the patched
  `scli` executable with `SCLI_BIN`. Its default provenance is `topkproofs`
  with `SCALLOP_TOP_K=1000000000`; `SCALLOP_MEM_LIMIT_MB` defaults to 12000.

RQ2 retains the latest runner's default of all four engines. Pass
`--rq2-engines souffle,problog,vproblog` for the three-engine paper comparison,
or `--rq2-engines souffle` for a check using only this repository.
`--smoke` runs P1/P3 with PSouffle and ProbLog; `--representative` runs five
cases per family with one repeat and the selected RQ2 engines.

## Conversion And Analysis Scripts

The `scripts/` directory contains the VProbLog bridge generator, native-format
transformer, per-family artifact builder, and standalone/chained taint drivers.
The benchmark inputs are already materialized. To regenerate VProbLog inputs,
run from `evaluation/full/`:

```bash
python3 scripts/build_vproblog_artifacts.py sc
python3 scripts/build_vproblog_artifacts.py taint --vlog-bin "$VLOG_BIN"
python3 scripts/build_vproblog_artifacts.py symbol
```

These commands update case input forms. The chained taint driver instead
accepts `--out` for a separate run workspace:

```bash
python3 scripts/run_t2_taint_vproblog_chained.py --out artifact/taint-chained --vlog-bin "$VLOG_BIN"
```

`rewrite_contribution.py` summarizes simple-pattern and general-SISO graph
reductions from profiles containing the detailed per-pattern counters. It
reports unavailable counters instead of treating missing fields as zero;
use the RQ1 collector for aggregate reductions from this compiler's logs.
`rewrite_timing_summary.py` reports rewrite/BDD
time contributions from its named profile directories (run it from
`evaluation/full/`). `split_ablation_summary.py` checks probabilities and
summarizes existing `11_split`/`11_nosplit` RQ3 results.

Default RQ3 uses the supported rewrite off/on interface. Add
`--rq3-split-ablation --rq3-collect` to collect upstream split-ablation logs;
live split-ablation runs require another compiler whose runtime supports
`--split-mode no-split`. PSouffle in this repository does not expose that flag.
