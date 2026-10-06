# Usage

## Execution And Rewrite Contract

There are two independent execution paths. Standalone full inference can use
rewrite. An online session starts with plain full inference, retains its graph
and BDD state, and processes commits using `--setmode`. Online `full` is the
recomputation oracle and never uses rewrite.

| Compiler selection | Generated paths | Default binary execution |
| --- | --- | --- |
| No selection | Standalone full and online | Standalone full |
| `--full-only` | Standalone full only | Standalone full |
| `--inc-only` | Online, including its full baseline/oracle | Online |
| `--online` or explicit `--setmode` | Both | Online |

| Binary arguments | Behavior |
| --- | --- |
| No selection or `--full-only` | Full inference once, then exit |
| `--full-only --rewrite` | Full inference using automatic rewrite dispatch |
| `--inc-only`, `--online`, or explicit `--setmode` | Online session, plain baseline |
| `--setmode full` | Online session, plain full recomputation for commits |
| Online selection plus any rewrite flag | Error before reading facts |
| `--full-only` plus online selection or `--setmode` | Error, independent of argument order |
| `--explicit-rewrite --implicit-rewrite` | Error, independent of argument order |
| A path omitted at compilation | Runtime requests for that path fail |

`--rewrite` dispatches to implicit split rewrite for probabilistic rules and
explicit graph rewrite without splitting for deterministic rules.
`--explicit-rewrite` and `--implicit-rewrite` force their respective paths.
Compiler rewrite arguments bake defaults into the generated binary. A binary
with a baked rewrite default rejects a later online selection.

`--derv-only` is available at compilation and runtime for standalone graph
construction. Runtime `--derv-only=false` restores inference; combining graph-only
execution with rewrite skips rewrite, matching the full artifact.
`--merge-bi-imp` and `--prune-extra` retain the full artifact's opt-in pruning
passes. All three controls reject online execution to preserve its update graph.

Deterministic-relation analysis, BDD, and variable-index reuse are enabled by
default. `--det-opt` remains an accepted compatibility flag. Old full-artifact
dump/profile arguments (`--dumpjson`, `--dumpdot`, `--dumpstat`, `--fc-profile`,
`--profile-wmc`, `--profile-dep-graph`) remain runtime aliases for the canonical
selectors below. Approximate inference is unavailable.

## Naive Lift Fastpath

`--lifted-wmc` enables exact pointwise lift before standalone full graph
construction. `--lifted-threshold=N` sets the minimum output relation size
(default `1024`, nonnegative integer). Both are compiler defaults and runtime
options; online execution rejects enabled lift. Graph-only execution skips it.

The fastpath supports nonrecursive positive rules with distinct head variables
and connected, unique runtime witnesses. It shares a symbolic formula across
tuples, uses direct products for distinct-event conjunctions, and instantiates
exact BDDs for disjunctions or repeated events. Unsupported outputs retain
their complete concrete dependencies, including events shared with lifted
outputs; evidence falls back to ordinary exact inference.
See [the example and diagnostics](../README.md#naive-lift-fastpath).

## Source References

- [../src/MainDriver.cpp:636](../src/MainDriver.cpp#L636): compiler options.
- [../src/MainDriver.cpp:719](../src/MainDriver.cpp#L719): runtime-default canonicalization.
- [../src/synthesiser/Synthesiser.cpp:673](../src/synthesiser/Synthesiser.cpp#L673): generated online pipeline call.
- [../src/synthesiser/Synthesiser.cpp:4533](../src/synthesiser/Synthesiser.cpp#L4533): generated runtime defaults.
- [../src/include/souffle/CompiledOptions.h:187](../src/include/souffle/CompiledOptions.h#L187): mode and output syntax.
- [../src/include/souffle/CompiledOptions.h:709](../src/include/souffle/CompiledOptions.h#L709): generated runtime parser.
- [../src/include/souffle/cli/Cli.h:679](../src/include/souffle/cli/Cli.h#L679): online CLI commands.
- [../src/include/souffle/problog/ForwardCompilation.h:163](../src/include/souffle/problog/ForwardCompilation.h#L163): adaptive incremental reorder threshold.
- [../src/include/souffle/problog/ForwardCompilation.h:228](../src/include/souffle/problog/ForwardCompilation.h#L228): weighted incremental reorder work score.

## Input Format

For each `.input` relation `R`, provide `R.facts` in the fact directory. A
tuple is one tab-separated line matching the `.decl` order. An optional
`R.prob` file supplies one probability per fact line; absent `.prob` files make
the relation deterministic.

Rules may also carry ProbLog-style probabilities:

```souffle
0.7::path(x,y) :- edge(x,y).
```

Rule probability annotations retain double precision through parsing and code
generation, independently of the tuple RAM domain size.

Full-artifact evidence and exact sum aggregate replay are available in standalone
full execution. The incremental artifact does not maintain these features
across updates; online execution rejects evidence and aggregate replay with
an explicit error.

### Evidence

Observations condition all query probabilities on their joint event:

```souffle
evidence(alarm(1), true).
evidence(fault("sensor"), false).
```

Evidence targets and their dependencies are retained without `.output` or
`query` declarations, including tuples derived by deterministic rules and
nullary facts. Zero-weight sources contribute no probability, even when the
RAM evaluation derives their heads and descendants.
Arguments must be ground primitive literals matching
the declared attribute types: numbers, unsigned integers, floats, or quoted
symbols. Variables, expressions, records, and algebraic data types are rejected
with a source diagnostic; numeric literals must fit the configured RAM domain.

A tuple absent from the deterministic evaluation is false: observing it as
false adds no constraint, while observing it as true fails. Conflicting or
jointly impossible observations also fail with `Inconsistent evidence`, even
when their component is independent of every query. Successful inference uses
`P(query AND evidence) / P(evidence)`. BDD counting uses logarithms for conditioned
components so a positive evidence probability below double's range remains
usable. Rewrite preserves shared events in components containing observations,
and `--prune-extra` retains evidence components.

## Compile

```bash
./build/src/souffle -F input -D output compute.souffle.dl -o compute
```

Compile-time `-F` and `-D` bake default runtime directories into the generated
binary. Runtime flags can override them.

Public compiler options for this branch:

- `-F, --fact-dir <DIR>`: default fact directory.
- `-D, --output-dir <DIR>`: default output directory.
- `-o, --dl-program <FILE>`: generated executable.
- `--setmode=<MODE>`: default runtime mode, one of `inc-naive`, `inc-regional`, `full`.
- `--dump=<json|json-before-graph|json-before-prune|dot|stat>`: bake default graph/stat dumps.
- `--profile-stage=<dred|inc|fc|wmc|inc-delete|inc-regional|dep-graph>`: bake default profiling stages.
- `--log-file=<FILE>`: default debugger log filename.
- `-v, --verbose`: bake informational runtime diagnostics on by default.

Inherited Souffle options such as `--jobs`, `--include-dir`, `--profile`,
`--show`, and warning controls remain available but are not incremental knobs.

## Runtime

Public generated runtime options for this branch:

- `-F, --facts, --input-dir <DIR>`: fact directory.
- `-D, --output, --output-dir <DIR>`: output directory.
- `-m, --setmode=<MODE>`: turn mode, one of `inc-naive`, `inc-regional`, `full`.
- `--dump=<json|json-before-graph|json-before-prune|dot|stat>`: default-off graph/stat dumps.
- `--profile-stage=<dred|inc|fc|wmc|inc-delete|inc-regional|dep-graph>`: default-off profiling output.
- `--inc-reorder-policy=<default|off|pressure|auto|explicit|both>`: incremental
  CUDD reordering policy. The branch default is `pressure`.
- `--inc-reorder-work-threshold=<N>`: BDD-pressure threshold for `pressure`
  reordering. The branch default is `2500`.
- `--logfile=<FILE>` or `--log-file=<FILE>`: debugger log filename.
- `-v, --verbose`: print informational graph, CUDD, and pipeline diagnostics.
- `-p, --profile=<FILE>`: profile output, only for binaries compiled with profiling enabled.
- `-j, --jobs=<N>`: runtime thread count when OpenMP is available.

There are no backend or variable-index reuse switches in this repository.
BDD, deterministic-relation analysis, and variable-index reuse
are fixed implementation defaults. Incremental BDD reordering uses the same
pressure gate for `inc-naive` and `inc-regional`. The trigger score is the
weighted maximum of raw update size, affected graph frontier work, and BDD
update work. In `pressure` mode the score accumulates across online turns and
resets after an explicit CUDD reorder. The configured threshold is also bounded
by an adaptive threshold derived from the baseline graph work score.
`--inc-reorder-policy=default` restores legacy CUDD adaptive reordering for
controlled local diagnostics.

## Online CLI

Commands:

- `insert [prob::]Rel(v1, v2, ...) [prob]`
- `delete Rel(v1, v2, ...)`
- `commit`
- `setmode inc-naive|inc-regional|full`
- `set dump json|json-before-prune|dot|stat` and matching `unset dump ...`
- `set profile-stage <stage>` and `unset profile-stage <stage>`
- `show config`
- `list`
- `help`
- `q`

Example:

```bash
./compute -F input -D output --setmode inc-regional
insert 0.3::edge(1,2)
delete edge(3,4)
commit
q
```

## Optional Outputs

The default run writes `facts.prob` and per-turn snapshots such as
`fact-iter2-inc-regional.prob`. To collect graph material:

```bash
./compute -F input -D output --setmode inc-regional \
  --dump=dot,json,stat --profile-stage=inc,wmc,fc
```

`dot` writes derivation graph DOT files, `json` writes post-prune graph JSON,
`json-before-graph` writes the startup rule-application JSON before baseline
graph materialization, `json-before-prune` writes per-turn graphs before pruning,
and `stat` is a broad diagnostic dump: graph counters, `graph-*.json`, SEM/DRed
summaries, and regional scope console diagnostics for `inc-regional`. When
enabled before startup graph construction, `stat` also writes
deterministic-relation analysis files.

## Diagnostic Counts

Standalone full runs that construct a concrete graph record
`before_prune_nodes`/`before_prune_edges` and the
existing `after_prune_*` graph summaries in the `PRUNING` debugger stage.
These counters do not require dump or profiling flags.

With rewrite enabled, `FC_WMC_HYBRID` includes:

- `rewrite_all_facts_regions`, `rewrite_single_regions`,
  `rewrite_linear_regions`, `rewrite_parallel_regions`,
  `rewrite_fan_out_regions`, and `rewrite_simple_fact_regions`: completed
  simple-pattern rewrites, including implicit overlay rewrites.
- `rewrite_simple_regions` and `rewrite_general_regions`: completed regions,
  counted once, rather than detected candidates.
- `graph_rewrite_rewritten_regions` and `graph_rewrite_general_*`: completed
  graph rewrites and the general pass's gross node/edge removals and edge additions.
- `implicit_overlay_*_regions`, `implicit_graph_*`, and
  `implicit_materialized_*`: overlay counts and physical graph sizes before
  and after the residual graph pass, including the direct-commit path.
- `implicit_graph_detect_ms`, `implicit_graph_rewrite_ms`, and
  `implicit_total_ms`: detection, residual rewriting, and combined pipeline
  times; direct commits include the physical graph rewrite.
- `rewrite_simple_nodes_net_removed`, `rewrite_general_nodes_net_removed`,
  `rewrite_simple_edges_net_removed`, and `rewrite_general_edges_net_removed`:
  signed contributions to the reduction from `after_prune_*` to `rewrite_final_*`.

General contributions come from the general rewriter's actual mutations.
The simple group includes all remaining graph changes: overlay commits,
splitting, compaction, cleanup, and output recovery. Its net contribution may
be negative when these operations add graph structure. The two groups sum
exactly to the observed node/edge reduction; they are not independent ablation
measurements.
