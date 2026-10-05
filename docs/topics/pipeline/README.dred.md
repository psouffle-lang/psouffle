# Incremental DRed And Graph Delta Flow

This document describes the incremental semi-naive deletion/rederive/insert
path used before forward compilation. It is part of the generated online
runtime and is not a separate evaluator mode.

## Source References

- [../../../src/ast2ram/online/UnitTranslator.cpp:1095](../../../src/ast2ram/online/UnitTranslator.cpp#L1095): generated rederive clause versions.
- [../../../src/ast2ram/online/UnitTranslator.cpp:1480](../../../src/ast2ram/online/UnitTranslator.cpp#L1480): rederive loop tracing.
- [../../../src/ast2ram/online/UnitTranslator.cpp:1533](../../../src/ast2ram/online/UnitTranslator.cpp#L1533): prefill from overdelete tuples.
- [../../../src/ast2ram/online/UnitTranslator.cpp:1664](../../../src/ast2ram/online/UnitTranslator.cpp#L1664): generated delete/insert phase structure.
- [../../../src/include/souffle/cli/Executor.h:234](../../../src/include/souffle/cli/Executor.h#L234): runtime graph delta application.
- [../../../src/include/souffle/problog/DerivationGraph.h:1804](../../../src/include/souffle/problog/DerivationGraph.h#L1804): graph `applyDelta` entry.
- [../../../src/include/souffle/problog/DerivationGraph.h:2264](../../../src/include/souffle/problog/DerivationGraph.h#L2264): delete-phase profiling output.
- [../../../src/include/souffle/problog/DerivationGraph.h:2326](../../../src/include/souffle/problog/DerivationGraph.h#L2326): prune statistics.

## Role In The Pipeline

DRed maintains the derivation graph under a committed delta. It consumes the
compiled delta relations from the generated RAM program and updates the graph
before forward compilation sees the turn.

The runtime applies deletes first, then inserts. This order is required for
mixed updates because post-delete relation snapshots are used to derive upper
strata against the right intermediate state.

## Delete Phase

The generated RAM code marks tuple and derivation overdelete sets. The runtime
then removes deleted rule applications, removes dangling derivation edges,
records deleted facts, and clears view-level caches before rebuilding the
pruned view.

The delete output is represented in graph delta sets:

- deleted graph nodes;
- deleted graph edges;
- deleted deterministic facts;
- deleted probabilistic facts;
- impacted nodes/edges reachable from deleted facts.

## Rederive Phase

For recursive SCCs, the generated rederive loop seeds
`@inc_delta_tuple_rederive_*` from `@inc_tuple_overdelete_*`, runs the
semi-naive fixpoint, and clears temporary rederive state in the postamble.

The loop covers recursive and non-recursive clauses that belong to recursive
strata, so base rules in a recursive component are rederived consistently with
the SCC.

## Insert Phase

After deletes and rederive finish, the generated insertion phase contributes
new rule applications and facts. The graph applies insertions after deletion
cleanup, so overlapping post-delete/post-insert changes resolve in one turn.

## Output And Profiling

Default runs do not dump graph internals. Use canonical output selectors:

```bash
./compute -F input -D output --setmode inc-naive \
  --dump=stat,json-before-prune --profile-stage=dred,inc
```

`stat` prints graph statistics, `json-before-prune` captures the graph before
pruning, and profile stage `dred` enables generated DRed timers when the binary
was compiled with profiling support.
