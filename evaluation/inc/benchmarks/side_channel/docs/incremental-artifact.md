# Incremental Artifact Workflow

## Scope

`benchmarks/side_channel/artifact/` is the artifact-facing entry point for the
`CAV-INC` branch. It wraps the low-level Souffle runner and keeps experiment
execution separate from paper data collection.

The runner compares:

- `full`
- `inc-naive`
- `inc-regional`
- `inc-full` for the NoBdd ablation
- `full-inc-naive` and `full-inc-regional` for the NoDer ablation

P13-P20 inputs live under `benchmarks/side_channel/cases/`. The same directory
also contains a provisional A1-A11 synthetic snapshot derived from P13-P20. This
is not the final artifact benchmark freeze. The runner creates writable per-case
workspaces under `benchmarks/side_channel/runs/side_channel_inc/` and, unless
`--skip-delta` is set, regenerates the artifact alpha-grid deltas there.

Synthetic A-cases are extracted and fused from P13-P20 to exercise the same
benchmark scale. The A1-A11 recipes are not direct copies of any P-case; each
component is a connected slice, and fused cases offset `s` identifiers across
sources.

## Requirements

- Python 3.10+
- A Souffle binary with incremental CLI commands: `insert`, `delete`, `commit`,
  `setmode`, and `q`.

## Workflow

Run from `evaluation/inc/`:

```bash
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py smoke
python3 benchmarks/side_channel/artifact/collect_artifact_data.py
```

Profiles:

```bash
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py smoke
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py representative
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py complete
```

Profile coverage:

- `smoke`: P13, `inc0p5`, one sample, one run.
- `representative`: P13/P16/P20, `inc0p5/inc1p0/inc1p5`, two samples, one run.
- `complete`: P13-P20, `inc0p5/inc1p0/inc1p5`, five samples, five runs.

Use `--runs 1` for a single repeat per delta sample:

```bash
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py complete --runs 1
```

Artifact profiles enable materialized full references by default. Pass
`--no-materialized-full` only when the legacy online full path is required.

The collector reads existing JSON summaries only and always writes every mapped
paper table and figure:

```bash
python3 benchmarks/side_channel/artifact/collect_artifact_data.py
```

The low-level CLI can resample update scripts under each case's `delta/`
directory. For artifact tables, use the alpha-grid strategy: each label is a
change-size row (`inc0p5`, `inc1p0`, `inc1p5`) with default ratios
`0.005`, `0.01`, and `0.015`, capped at `150`, `300`, and `450` operations.
Sample indexes 1-5 map to alpha buckets 0.00, 0.25, 0.50, 0.75, and 1.00.

```bash
python3 benchmarks/side_channel/cli/side_channel_inc.py delta --delta-strategy alpha-grid --sets 5 --cleanup
```

To build the synthetic A-case suite:

```bash
python3 benchmarks/side_channel/artifact/build_synthetic_cases.py --cases A1-A11 --force
python3 benchmarks/side_channel/cli/side_channel_inc.py delta --cases A1-A11 --delta-strategy alpha-grid --sets 5 --cleanup
```

The low-level `compile` command builds a `compute` binary for each case. The
low-level `run` command writes one JSON summary per case/delta/sample/run.
The low-level `collect` command writes `results-souffle-inc.tsv` for quick
inspection.

## Paper Data Mapping

- Table 1: `collect_artifact_table1.py` writes
  `table1_case_graph_sizes.tsv` and `table1_benchmark_stats.tsv`.
- Table 2: `collect_artifact_table2.py` writes
  `table2_end_to_end_speedup.tsv`.
- Table 3: `collect_artifact_table3.py` writes `table3_ablation.tsv`.
- Figure 6: `collect_artifact_figure6.py` writes delta-turn speedup grids.
- Figure 7: `collect_artifact_figure7.py` writes runtime breakdown tables.
- Figure 8: `collect_artifact_figure8.py` writes the regional-vs-naive
  `3 x 5` grid and summary.

Speedup collectors compare the first delta turn after the shared initial full
turn. In the JSON logs this is `log.stages.turns[1].time_seconds` for both the
full and incremental modes, not the process-level `elapsed_s`.

## Output Layout

```text
benchmarks/side_channel/runs/side_channel_inc/
  P13/
    compute.souffle.dl
    input/
    delta/
      inc0p5_1.txt
      ...
      inc1p5_5.txt
    compute
    output/
      baseline/
      delta-inc0p5/
      delta-inc1p0/
      delta-inc1p5/
  A1/
    compute.souffle.dl
    input/
    delta/
    synthetic-provenance.json
```

Per-turn probability outputs are named by the physical semantic lane that
produces the probabilities:

- `fact-iter*-full.prob`
- `fact-iter*-inc-naive.prob`
- `fact-iter*-inc-regional.prob`

The ablation modes split semantic graph construction from DD construction:

- `inc-full` runs `sem=inc fc=full`; it emits `fact-iter*-inc-naive.prob`.
- `full-inc-naive` runs `sem=full fc=inc-naive`; it emits
  `fact-iter*-full.prob`.
- `full-inc-regional` runs `sem=full fc=inc-regional`; it emits
  `fact-iter*-full.prob`.

The runner tags those files with the logical ablation mode when it moves them
into each delta run directory, so JSON summaries can still report
`inc_full`, `full_inc_naive`, and `full_inc_regional` distinctly.

For each delta run, the JSON summary compares incremental outputs against
`full`.

Paper collector TSVs default to:

```text
benchmarks/side_channel/runs/side_channel_inc/artifact_tables/
```

## Flags

Artifact runner:

- `run_artifact_experiment.py {smoke,representative,complete}`
- `--base-dir --souffle-bin --compile-timeout --timeout --jobs`
- `--cases --delta-labels --delta-samples --runs`
- `--skip-delta --skip-compile --quiet`

Synthetic case builder:

- `build_synthetic_cases.py`
- `--source-dir --base-dir --cases --seed --force`
- `--component A=SRC[:FRAC][,SRC[:FRAC]...]`

Artifact collector:

- `collect_artifact_data.py`
- `--base-dir --output-dir --cases --labels --label-delta-map --pinq-mode --out-dir`

Low-level CLI:

Global:

- `--base-dir`: writable workspace root.
- `--log-file`: operation log path.
- `--quiet`: silence console progress.
- `--verbose`: include debug logging.

Delta construction:

- `delta --cases --change-spec --change-cap --change-cap-tol --sets --seed --cleanup`
- `delta --delta-strategy {standard,mix,alpha-grid}`
- `delta --delta-cluster {assign,assign-transitive,id,random}`
- `delta --delta-transitive-depth`
- `delta --mix-ratio --mix-cap --mix-pool-ratio --mix-pool-cap-mult --mix-distributions`

Execution:

- `compile --cases --timeout --souffle-bin --souffle-arg --jobs`
- `run --cases --timeout --delta-timeout-multiplier`
- `run --delta-labels --delta-samples --delta-runs` (`--delta-samples` defaults to 5)
- `run --delta-root --delta-shuffle --delta-seed --output-dir`
- `run --run-arg --inc-regional-only`
- `run --compare-all` / `--no-compare-all`
- `run --modes full,inc-regional` to override the default mode set without changing legacy defaults
- `run --materialized-full` to run single-commit full references on a materialized final input
- `collect --cases --output-dir`
- `clean --cases` / `--all`

## Source References

- [`../artifact/run_artifact_experiment.py`](../artifact/run_artifact_experiment.py)
- [`../artifact/collect_artifact_data.py`](../artifact/collect_artifact_data.py)
- [`../artifact/build_synthetic_cases.py`](../artifact/build_synthetic_cases.py)
- [`../artifact/paper_data.py`](../artifact/paper_data.py)
- [`../cli/side_channel_inc.py`](../cli/side_channel_inc.py)

## Related Commits

- `UNCOMMITTED` - simplify artifact collection entry point
- `UNCOMMITTED` - artifact run/collect split for incremental paper data
- `UNCOMMITTED` - add synthetic A-case generation from P13-P20
