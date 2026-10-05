# Artifact Source Provenance

PSouffle is a new, independent repository copied from the maintained artifact
snapshots of `Hughshine/souffle`. The original local Lifted worktree is not
modified. Compiler, runtime, tests, licenses, and build support are retained.
New benchmark outputs and timing logs are excluded. Maintained example-output
fixtures from the upstream incremental snapshot are retained.

The integrated compiler/runtime was introduced in a single `initial commit`.
The upstream source revisions and release tags are recorded below; their git
history is not imported into this repository.

| Contribution | Source revision | Release record |
| --- | --- | --- |
| Full inference and rewrite | `d2e28cbdd26b305c1ce97d57f29c49f58530df9f` | `full-artifact-ae`, tag `full-artifact-ae-checkpoint-2026-04-25` |
| Incremental inference | `e0539757c3c4ef789fd375634910d7760d9d1e8e` | `inc-artifact-ae`, tag `inc-artifact-ae-final` |

The incremental snapshot provides the shared baseline. Full graph working
views, aggregate replay, tuple/symbol rendering, query matching, component
solvers, graph rewrite, and implicit split rewrite are copied from the full
snapshot and integrated with the shared incremental graph/BDD interfaces.
`src/problog/FullPipeline.cpp` contains standalone full inference;
`src/problog/Pipeline.cpp` retains online initialization and update orchestration.
Both are compiled in one translation unit because the graph headers define
shared non-inline symbols.

## Branch And Tag Audit

Public HTTPS refs were checked on 2026-10-05 and match the two imported tips.
The source checkout's SSH remote could not authenticate; HTTPS read access
confirmed that its artifact refs were current.

- `inc-artifact-ae-anchor-strategy` (`821d6bf7b`) and
  `inc-artifact-ae-debugger-isolation` (`cd6d067fc`) are already included in
  `inc-artifact-ae-final`. The final snapshot adds adaptive weighted-max reorder
  pressure (`e88cda9fb`) on top of pressure-based reordering, bounded anchor
  search, deletion-only regional fixes, and debugger isolation.
- `full-artifact-ae-general-siso-exp` (`ad1f7e34f`) is already included in
  the full AE snapshot. Later AE changes preserve component fast paths, make
  bi-implication merging opt-in, and record rewrite graph sizes.
- `full-artifact-opt-unified`, `implicit-rewrite-prep`, and
  `artifact-rewrite-followups-20260421` are ancestors of the full AE tip.
  The older `full-artifact-opt`, aggregate worktree, and symbol/string worktree
  branches diverge in git history; their aggregate and symbol ports were
  consolidated into `full-artifact-opt-unified` before the AE cleanup.
- `online`, `approx*`, `query`, `evidence*`, and `Lifted` are not artifact
  import sources. Approximate inference, graph-query experiments, and lifted
  inference are outside this repository's requested scope.

## Paper And Benchmark Records

The companion `Hughshine/problog-benchmark` repository provides the evaluator
workloads. Its incremental and full/rewrite inputs and experiment scripts are
copied into [../evaluation/](../evaluation/README.md), including native inputs
for the external comparison engines. Generated runs and timing logs are excluded.
Incremental evaluation comes from `CAV-INC` revision
`be3fd21e80277bd6c12db8f2ec1f1916b429ecf9`; full/rewrite evaluation comes from
the local `CAV-FULL` revision `1fb689e07a989279bf154d605439bdb33c3ee886`.
Five VProbLog helper scripts are also copied from the source workspace's
untracked `scripts/` directory. Evaluation READMEs describe local path and
execution-selector adaptations.

- CAV full packaging is recorded by branch `CAV-FULL` and tag
  `cav26-full-ae-v1` (`e0bce504a45c0107e38476c839743907bf3a3277`). Internal development notes record the March packaging
  checkpoint `00565d05fb576b66ec624bc601e63c94299e020a`; subsequent full compiler
  AE changes are preserved in the April 25 source snapshot.
- CAV incremental packaging is branch `CAV-INC`, local remote tip `be3fd21`.
  Its run/collect scripts use P13–P20 and the incremental modes and split-mode
  ablations supported here.
- The local benchmark checkout also contains the FMCAD 2026 README and
  `FMCAD.py`. Its Dockerfile explicitly pins the compiler to
  `full-artifact-ae-checkpoint-2026-04-25`. The local paper sources under
  `/home/jiahao/src` describe full derivation-graph construction and exact
  rewrite-driven inference using CUDD.

These records establish source/artifact correspondence. Reproducing published
timings additionally requires the matching benchmark inputs, runner revision,
machine, and experiment settings; local regression success alone does not
establish those performance numbers.
