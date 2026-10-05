# Paper Evaluation

This directory bundles the benchmark inputs and experiment automation for the
CAV 2026 incremental work and the FMCAD 2026 full/rewrite work. Build PSouffle
using the [repository instructions](../README.md#build), then run the commands
below from the repository root. The runners use `build/src/souffle` by default;
`--souffle-bin` or `SOUFFLE_BIN` selects another compiler.

| Work | Directory | Inputs | Experiment entry point |
| --- | --- | --- | --- |
| CAV 2026 incremental | [inc/](inc/README.md) | P13–P20, default update scripts, and the upstream provisional A1–A11 synthetic suite | `inc/benchmarks/side_channel/artifact/run_artifact_experiment.py` |
| FMCAD 2026 full/rewrite | [full/](full/README.md) | 19 side-channel cases, 15 Android taint cases, and 16 DDisasm symbolization cases | `full/FMCAD.py` |

The Python runners require Python 3.10+ and use the standard library. PSouffle
experiments require the built compiler. Full engine comparisons additionally
require the selected external engines: ProbLog, VProbLog (`vlog`), or Scallop
(`scli`). See the full evaluation README for engine selection and setup.

## Incremental Smoke Run

```bash
python3 evaluation/inc/benchmarks/side_channel/artifact/run_artifact_experiment.py smoke
python3 evaluation/inc/benchmarks/side_channel/artifact/collect_artifact_data.py
```

The runner prepares P13 and a small update, compiles an incremental executable,
and compares full recomputation, `inc-naive`, `inc-regional`, and the paper's
split-mode ablations. Initialization and full recomputation use the original
graph. Generated executables, logs, and tables are written beneath
`evaluation/inc/benchmarks/side_channel/runs/`, which is ignored by git.

## Full/Rewrite Smoke Run

This PSouffle-only command compares plain inference and rewrite on P1:

```bash
python3 evaluation/full/FMCAD.py -RQ3 --only sc --cases 1 --runs 1
python3 evaluation/full/FMCAD.py -RQ3 --rq3-collect --only sc
```

To exercise both PSouffle and ProbLog on P1 and P3, run:

```bash
python3 evaluation/full/FMCAD.py --smoke
```

Full outputs default to the ignored `evaluation/full/artifact/` directory.
The default RQ3 experiment measures rewrite off (`01`) versus on (`11`).

## Sources

The source repository is [Hughshine/problog-benchmark](https://github.com/Hughshine/problog-benchmark).
Incremental inputs and scripts come from its `CAV-INC` snapshot
`be3fd21e80277bd6c12db8f2ec1f1916b429ecf9`. Full inputs and scripts come from
the local `CAV-FULL` snapshot `1fb689e07a989279bf154d605439bdb33c3ee886`, whose
artifact README identifies the FMCAD 2026 paper. The five VProbLog helpers in
`full/scripts/` were also copied from that local workspace; they were not
tracked in the source snapshot.

Benchmark programs, facts, probabilities, delta files, insertion-pool manifests,
synthetic provenance, and competitor input forms are retained. Runner paths and
execution selectors are adapted to this repository. ProbLog runs add the
closed-world `unknown(fail)` directive to a separate staged program so empty
relations behave as they do in Datalog. The original split-rewrite
ablation is available with `--rq3-split-ablation` for collecting its existing
logs or running with a compiler that supports `--split-mode`; that runtime flag
is outside this codebase's current command-line interface.

The imported benchmark package's MIT notice is preserved in [LICENSE](LICENSE).
