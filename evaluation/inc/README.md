# Side-Channel Incremental Benchmark Workspace

This workspace bundles the CAV 2026 incremental benchmark, experiment runner,
and paper table/figure collectors. It contains P13-P20 inputs and default
delta files, plus the upstream provisional A1-A11 synthetic suite.
The compiler defaults to this repository's `build/src/souffle`; override it
with `--souffle-bin` or `SOUFFLE_BIN`.

## Entry Points

- `benchmarks/side_channel/artifact/run_artifact_experiment.py`: run
  experiment profiles and write per-delta JSON files.
- `benchmarks/side_channel/artifact/collect_artifact_data.py`: collect paper
  table/figure data from existing JSON files.
- `benchmarks/side_channel/cli/side_channel_inc.py`: low-level incremental
  benchmark driver.

## Layout

- `benchmarks/side_channel/cases/`: P13-P20 benchmark inputs and default
  deltas.
- `benchmarks/side_channel/artifact/`: artifact-facing run and collection
  scripts.
- `benchmarks/side_channel/cli/`: low-level benchmark runner.
- `benchmarks/side_channel/docs/`: workflow notes.
- `benchmarks/side_channel/runs/`: local output root.

## Quick Start

```bash
cd evaluation/inc
python3 benchmarks/side_channel/artifact/run_artifact_experiment.py smoke
python3 benchmarks/side_channel/artifact/collect_artifact_data.py
```

The smoke profile runs P13 with one update sample and one repeat. Use
`representative` for P13/P16/P20 with two samples per delta size, or `complete`
for P13-P20 with five samples and five repeats. `--runs 1` reduces the repeats;
`--skip-delta` reuses the bundled update files. The runner uses `--inc-only`
and writes generated files under the ignored `benchmarks/side_channel/runs/`.
No rewrite is applied to initialization or full recomputation.

The collector writes Tables 1-3 and Figures 6-8 beneath
`benchmarks/side_channel/runs/side_channel_inc/artifact_tables/`.
See [the workflow](benchmarks/side_channel/docs/incremental-artifact.md) for
all profiles, ablations, delta generation, and output formats.

## Documentation

- `benchmarks/side_channel/README.md`
- `benchmarks/side_channel/docs/INDEX.md`

## Source References

- [`benchmarks/side_channel/artifact/run_artifact_experiment.py`](benchmarks/side_channel/artifact/run_artifact_experiment.py)
- [`benchmarks/side_channel/artifact/collect_artifact_data.py`](benchmarks/side_channel/artifact/collect_artifact_data.py)
- [`benchmarks/side_channel/cli/side_channel_inc.py`](benchmarks/side_channel/cli/side_channel_inc.py)
