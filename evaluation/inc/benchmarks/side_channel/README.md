# Side-Channel Benchmark (Souffle Incremental)

This directory is the canonical home for the `CAV-INC` side-channel benchmark.
It evaluates Souffle incremental execution (`inc-naive` and `inc-regional`)
against full recomputation on the same delta workloads.

The branch contains P13-P20 case templates under `cases/` plus a provisional
A1-A11 synthetic snapshot derived from P13-P20. This is not the final artifact
benchmark freeze. The artifact runner initializes writable workspaces under
`runs/` and prepares the alpha-grid `inc0p5/inc1p0/inc1p5` delta set there before
compiling/running experiments. These labels use the paper ratios
`0.005/0.01/0.015` with caps `150/300/450`. The A1-A11 recipes avoid direct copies of
P13-P20: every generated case is built from connected slices, slice fusions, or
both.

## Layout

- `cases/`: tracked P13-P20 inputs and default delta files.
- `artifact/`: artifact-facing experiment runner and paper data collectors.
- `cli/`: benchmark runner.
- `core/`: shared logging and utility logic.
- `docs/`: workflow notes.
- `runs/`: local output root for compiled binaries, deltas, and run results.

## Primary Commands

Run from `evaluation/inc/`:

```bash
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py smoke
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py complete --runs 1
python3 benchmarks/side_channel/artifact/collect_artifact_data.py
```

Experiment profiles:

```bash
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py representative
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py complete
```

The collector only reads JSON files produced by the runner and always writes
all paper table/figure TSVs:

```bash
python3 benchmarks/side_channel/artifact/collect_artifact_data.py
```

To resample delta files before running an experiment:

```bash
python3 benchmarks/side_channel/cli/side_channel_inc.py delta --delta-strategy alpha-grid --sets 5 --cleanup
```

To build the synthetic A-case suite and prepare its alpha-grid deltas:

```bash
python3 benchmarks/side_channel/artifact/build_synthetic_cases.py --cases A1-A11 --force
python3 benchmarks/side_channel/cli/side_channel_inc.py delta --cases A1-A11 --delta-strategy alpha-grid --sets 5 --cleanup
```

`run_artifact_experiment.py` compiles selected cases unless `--skip-compile` is
set, then runs full recomputation, `inc-naive`, `inc-regional`, and the staged
ablation modes needed by the paper collectors. It enables materialized full
references by default; pass `--no-materialized-full` to force the legacy online
full path.

For focused runs, the low-level CLI accepts `--modes` to select a subset such
as `full,inc-regional`. `--materialized-full` keeps the low-level default off,
but can run single-commit full references by first applying the delta to a
temporary final input and then executing one full turn.

## Paper Data Mapping

- Table 1: benchmark graph statistics.
- Table 2: delta-turn speedup summary; the shared initial full turn is excluded.
- Table 3: ablation summary for PINQ, NoDer, and NoBdd.
- Figure 6: delta-turn speedup grid by delta label and deletion fraction.
- Figure 7: runtime breakdown by derivation, compilation, and WMC.
- Figure 8: regional BDD update speedup over naive incremental BDD update,
  measured on turn-2 forward-compilation time; alpha-grid output is a `3 x 5`
  grid for `inc0p5/inc1p0/inc1p5` by `alpha = 0, 0.25, 0.5, 0.75, 1`.
- Figure 8 companion: detected region and affected delta-reachable node
  percentages relative to the pruned FC view, averaged over all runs in each
  alpha regime.

## Open TODOs

- Investigate Figure 8 `alpha=1.00` timing noise on deletion-only deltas. After
  Souffle commit `59fb2b25c`, deletion-only `inc-regional` uses the shared
  `inc-naive` FC path, so the measured `inc-naive/inc-regional` FC ratio should
  center near `1.0`. The current single-run P19 probe still shows visible
  wall-clock spread: `inc0p5_5=1.099124`, `inc1p0_5=1.016003`,
  `inc1p5_5=1.031644` (`arith_mean=1.048924`, `geom_mean=1.048312`).
  This probe predates the deletion-only preparation-path alignment in source
  commit `cd6d067fc`, which is included in PSouffle. Repeated timing checks on
  the integrated compiler are still pending; the old ratios do not establish
  that the same spread remains in the current version.
  Do not normalize this in the collector; follow up with repeated runs and
  lightweight profiling to explain scheduler/cache/BDD-manager noise and reduce
  or report the variance.

## Documentation

- Index: [`docs/INDEX.md`](docs/INDEX.md)
- Incremental artifact workflow: [`docs/incremental-artifact.md`](docs/incremental-artifact.md)
- Cases: [`cases/README.md`](cases/README.md)

## Source References

- [`artifact/run_artifact_experiment.py`](artifact/run_artifact_experiment.py)
- [`artifact/collect_artifact_data.py`](artifact/collect_artifact_data.py)
- [`artifact/build_synthetic_cases.py`](artifact/build_synthetic_cases.py)
- [`artifact/paper_data.py`](artifact/paper_data.py)
- [`cli/side_channel_inc.py`](cli/side_channel_inc.py)
