# Architecture

Generated runtimes support standalone full inference and online updates.
Standalone full inference optionally rewrites a working graph before exact
component solving. Online inference builds a plain baseline graph, accepts
insert/delete turns, and writes probabilities after each commit. Online full
recomputation always uses the original graph.

The opt-in `--and-input-redundancy` pass runs on the complete standalone working
graph after initial pruning by default. With
`--and-input-redundancy-placement=after-siso` and `--rewrite`, it runs after
SISO reaches its fixpoint. It deletes locally proven redundant
AND inputs in place, re-proves after each deletion, and reaches a fixpoint.
Query/evidence-aware cleanup then removes unreachable definitions before
graph fastpaths run. The post-SISO placement uses only active derivations
and never re-prunes the original owner graph. See
[the pass and audit guide](AND_INPUT_REDUNDANCY.md).

`--deterministic-event-aliases` independently contracts unique deterministic
copy definitions after initial pruning, before the AND pass and SISO. Regular
backward pruning collects a source/body index shared by alias and AND analysis;
alias mutation rebases that index before AND checks rather than rescanning the graph. It
retains query names through explicit output bindings, substitutes equal events
in consumers and removes repeated Boolean inputs without combining rule events
or aggregate records. Conditional results are copied to aliases only after
inference. Online and derivation-only execution reject this option. See
[event-alias semantics](DETERMINISTIC_EVENT_ALIASES.md).

`--local-series-contraction` and `--terminal-query-factors` run independently
after enabled SISO and AND passes. The first substitutes a private intermediate
definition into its sole consumer, retaining all correlated external inputs.
The second defers terminal unary marginal queries until conditional parent
probabilities are available. A shared preparation step retires inactive owner
history; global primitive support ownership proves absorbed factors private.
Original output names survive through explicit records. Online and graph-only
execution reject both flags. See [private-factor semantics](LOCAL_PRIVATE_FACTORS.md).

The local correctness comparison is `full` versus `inc-naive` or `inc-regional`.
`full` is the exact recomputation oracle; incremental modes must produce
matching tuple keys and probabilities on the same delta stream.

## Source References

- [../src/MainDriver.cpp:636](../src/MainDriver.cpp#L636): compiler-facing incremental options.
- [../src/synthesiser/Synthesiser.cpp:673](../src/synthesiser/Synthesiser.cpp#L673): generated runtime pipeline entry.
- [../src/synthesiser/Synthesiser.cpp:4533](../src/synthesiser/Synthesiser.cpp#L4533): baked runtime defaults.
- [../src/problog/Pipeline.cpp:923](../src/problog/Pipeline.cpp#L923): baseline pipeline and online CLI handoff.
- [../src/include/souffle/cli/Cli.h:679](../src/include/souffle/cli/Cli.h#L679): interactive command surface.
- [../src/include/souffle/cli/Executor.h:234](../src/include/souffle/cli/Executor.h#L234): incremental commit graph update.
- [../src/include/souffle/problog/ForwardCompilation.h:517](../src/include/souffle/problog/ForwardCompilation.h#L517): naive incremental forward compilation.
- [../src/include/souffle/problog/ForwardCompilation.h:1967](../src/include/souffle/problog/ForwardCompilation.h#L1967): regional incremental forward compilation.
- [../src/include/souffle/problog/formula/CuddManager.h:568](../src/include/souffle/problog/formula/CuddManager.h#L568): CUDD full-turn dynamic reordering and inc-turn pressure reordering.

## 1. Compile

The normal Souffle frontend parses and transforms the program. This branch
uses the shared AST-to-RAM translator and generates entry points according to
the selected execution capabilities. The default binary dispatches to
`runFullPipeline`; online selection dispatches to `runPipeline`.

Compiler `--full-only` omits incremental maintenance; `--inc-only` keeps the
online baseline and recomputation oracle. Other parameters set runtime defaults.
Backend, determinism, and
variable-index reuse are fixed. Incremental BDD reordering uses the default
pressure gate, with options available only for controlled local diagnostics.

## Standalone Full

`src/problog/FullPipeline.cpp` retains the full artifact's working graph,
aggregate replay, evidence handling, graph rewrite, implicit split rewrite,
and component solvers. After pruning, plain execution builds exact BDD
formulas directly. Rewrite execution dispatches according to rule
probabilities, then uses component analysis and the full artifact's fast paths
before weighted model counting.

The shared graph and BDD interfaces retain incremental metadata and reorder
support. The full implementation is included by `Pipeline.cpp` so that
non-inline graph-header definitions belong to one translation unit. Online
execution never calls the full rewrite dispatcher.

## 2. Online Baseline

The generated binary reads `<relation>.facts` and optional `<relation>.prob`
files, runs the compiled semi-naive evaluator, records rule applications, and
builds the baseline derivation graph. The baseline initializes BDD formulas and
writes the initial probability output.

## 3. Commit

Each `commit` applies queued insertions and deletions. The runtime:

1. stages tuple operations from the CLI;
2. reruns the generated incremental RAM relations for the delta;
3. applies graph deletes before inserts;
4. prunes the graph to output-relevant state;
5. runs the selected forward-compilation mode;
6. writes per-turn probabilities.

`@post_delete_*` snapshots are part of the generated RAM protocol for exact
mixed insert/delete semantics in non-recursive upper strata.

## 4. Forward Compilation

`inc-naive` updates formulas on the delta-reachable scope. It handles deletion,
rederive, insertion, CUDD variable reuse, and incremental weighted model
counting without exposing those internal choices as flags.

`inc-regional` analyzes the delta-reachable region, chooses boundaries, reuses
unaffected formulas outside the region, and calibrates boundary weights. If a
regional turn would consume unsafe persistent state, the runtime falls back on
the forward-compilation side while preserving semantic mode.

## 5. Outputs

Default probability outputs are always written. Additional material is opt-in:
graph DOT/JSON and statistics use `--dump`, and timing or diagnostic streams
use `--profile-stage`. The `stat` dump is a broad diagnostic selector: it emits
graph counters, `graph-*.json`, SEM/DRed summaries, regional scope diagnostics
for `inc-regional`, and startup deterministic-relation files when enabled
before baseline graph construction.
