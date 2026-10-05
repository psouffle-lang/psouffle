# Regional Incremental Forward Compilation

`inc-regional` is the regional forward-compilation mode used by this branch. It
reuses formulas outside the affected region and recomputes only the selected
region plus boundary calibration work.

## Source References

- [../../../src/include/souffle/CompiledOptions.h:187](../../../src/include/souffle/CompiledOptions.h#L187): public mode syntax.
- [../../../src/include/souffle/cli/Executor.h:318](../../../src/include/souffle/cli/Executor.h#L318): runtime dispatch to `inc-regional`.
- [../../../src/include/souffle/problog/ForwardCompilation.h:1967](../../../src/include/souffle/problog/ForwardCompilation.h#L1967): regional FC entry.
- [../../../src/include/souffle/problog/ForwardCompilation.h:2476](../../../src/include/souffle/problog/ForwardCompilation.h#L2476): regional orchestrator call.
- [../../../src/include/souffle/problog/IncRegionAnalyzer.h:1197](../../../src/include/souffle/problog/IncRegionAnalyzer.h#L1197): regional anchor search strategy.
- [../../../src/include/souffle/problog/IncRegionAnalyzer.h:1418](../../../src/include/souffle/problog/IncRegionAnalyzer.h#L1418): boundary anchor usability checks.
- [../../../src/include/souffle/problog/RegionalIncremental.h:1201](../../../src/include/souffle/problog/RegionalIncremental.h#L1201): regional FC class.
- [../../../src/include/souffle/problog/RegionalIncremental.h:1537](../../../src/include/souffle/problog/RegionalIncremental.h#L1537): delta-reach dependency graph diagnostics.
- [../../../src/include/souffle/problog/ForwardCompilation.h:163](../../../src/include/souffle/problog/ForwardCompilation.h#L163): adaptive incremental reorder threshold.
- [../../../src/include/souffle/problog/ForwardCompilation.h:228](../../../src/include/souffle/problog/ForwardCompilation.h#L228): weighted incremental reorder work score.
- [../../../src/include/souffle/problog/formula/CuddManager.h:568](../../../src/include/souffle/problog/formula/CuddManager.h#L568): CUDD adaptive reordering initialization.

## Control Flow

After graph pruning, `inc-regional` receives an incremental view containing
delta-insert and delta-delete sets. The forward-compilation path:

1. handles deletion and rederive on the current graph view;
2. releases deleted probabilistic variable indices for reuse;
3. analyzes a delta-reachable regional scope for inserted formulas;
4. builds or reuses a dependency graph for that scope;
5. rebuilds region formulas;
6. calibrates boundary/output weights;
7. runs incremental weighted model counting.

## State Safety

Regional formulas are a persistent state. A later turn may request a consumer
that cannot safely read the currently regionalized state. The CLI tracks the
state class and falls back on the forward-compilation side when needed. The
semantic mode of the turn remains the requested one.

## Boundary Anchors

Boundary anchors certify that a boundary can be reused without pulling the
whole delta-reachable suffix into the region. The default search follows a
bounded deterministic derivation chain to find an upstream probabilistic fact
or non-deterministic edge anchor. For experiment bisects,
`SOUFFLE_INC_REGION_ANCHOR_STRATEGY=local` restores the one-hop local search.

## Reordering

Full BDD construction uses CUDD's normal dynamic reordering path. Incremental
turns use a BDD-pressure gate by default. `inc-naive` and `inc-regional`
compute their own update work score, disable inc-turn auto reordering,
accumulate that score across online turns, and run one explicit CUDD reorder
only when the accumulated score reaches the effective shared threshold. The
score is the weighted maximum of raw delta size, affected graph frontier work,
and BDD update work. The effective threshold is the smaller of the configured
threshold (`2500` by default) and an adaptive threshold derived from the
baseline graph work score. The accumulator resets after an explicit reorder.
Incremental turns create variables only for delta inserts when possible. Use
`--inc-reorder-policy=default` only for legacy adaptive-reorder diagnostics.

## Diagnostics

Default runs only write probabilities. Use canonical selectors when collecting
extra diagnostic material:

```bash
./compute -F input -D output --setmode inc-regional \
  --dump=dot,json,stat --profile-stage=inc,fc,wmc,inc-regional
```

`dot` and `json` expose the pruned graph. `stat` is a broad diagnostic dump for
graph counters, `graph-*.json`, SEM/DRed summaries, and regional scope console
diagnostics. `fc` prints formula construction timing, `wmc` prints
weighted-model-counting timing, and `inc-regional` prints regional analysis and
calibration diagnostics. If `stat` is enabled before startup graph construction,
the runtime also writes deterministic-relation files.
