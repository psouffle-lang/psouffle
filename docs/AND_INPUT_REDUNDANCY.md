# AND-input redundancy elimination and audit

`--and-input-redundancy` enables a conservative elimination pass in standalone
full execution. The default remains off. Its order is initial query/evidence
pruning, AND-input elimination, query/evidence cleanup, then ordinary SISO
rewrite and graph/formula fastpaths. `--rewrite` is independent: the input pass
can run alone or before explicit, implicit, or automatic rewrite.

The separate `--dump=and-redundancy` selector measures opportunities and writes
proof certificates without performing any deletion itself.

For an input `c` of an edge `e`, it requires a single nonempty deterministic
definition `A -> c` and no fact source for `c`. Other positive inputs collectively
cover `A` through their own identity or their one-layer necessary inputs:

```text
Must_1(x) = intersection of the positive bodies of every active derivation of x
K = union of {x} and Must_1(x) over the other inputs of e
certificate exists when A is a subset of K
```

This proves that the other inputs imply `c` in every world of the snapshot's
event model. Removing just this occurrence would leave the target edge's event
unchanged, including its rule event. The target and witness derivations may have
random rule events; the definition of `c` must have probability exactly `1.0`
and no probabilistic support tokens.

`Must_1` is empty for facts, missing sources, empty bodies, and unsupported local
definitions. The first version skips negative bodies, shadow aliases, and nodes
in recursive SCCs in the proof's local structure. It uses actual node identity,
never equality of rendered tuple names. Unrelated recursive components are
allowed. Deterministic provenance already omitted by the compiler may produce
fact nodes; these intentionally do not supply necessary-input evidence.

Each read-only call builds fresh source and SCC indexes from the active edges
of the complete working view. The standalone pipeline confirms source completeness;
the detector API defaults to no proofs when completeness is not confirmed.
Inactive raw adjacency left behind by rewrite is not a source in this snapshot.
Post-rewrite certificates describe the residual graph's event model, including
existing SISO summaries; they do not reconstruct original event formulas hidden
by those summaries or establish identities across snapshots.

The general elimination path builds these structural indexes once per invocation and
re-proves each deletion against the current edge bodies and necessary-input
sets. Source identities and eligibility stay fixed; deleting dependencies
cannot create a cycle, and edges involving recursive nodes are never changed.
Indexed body segments mirror every in-place deletion, and cached `Must_1`
entries are updated immediately. Later rounds scan only the preselected target
edges until no opportunities remain, including opportunities exposed by a
shortened definition. The existing detection traversal marks each definition that
could prove a positive candidate, including currently failing proofs. When no
successful deletion changes one of these definitions, this information certifies
the fixpoint without a final target scan:
candidate premises stay fixed while other inputs and provider `Must_1` sets only
shrink, so a failed proof cannot become successful. Every deletion still requires
a current-body proof. It erases
aligned input/negation entries in the existing `Hyperedge` object, retaining
edge ID, rule application, probability and probabilistic support. Edge and
view caches are invalidated, and outgoing adjacency retains a dependency while
any duplicate input occurrence survives. No SAT, BDD or probability estimate
is used to prove a deletion.

In this path, source and SCC construction use flat arrays and CSR adjacency, with work linear
in nodes, edges and body associations. `Must_1` is computed only for providers
needed by candidate proofs, intersecting every source from the shortest current
body. A single complete positive source borrows its current indexed body directly,
without building a copied intersection cache; body deletions update that same
indexed segment. Intersection stamps are allocated only when a requested provider
has multiple complete positive sources. Dense stamp arrays replace per-candidate hash sets. Dense node IDs speed
lookup only after uniqueness checks, and each lookup still checks pointer
identity; sparse or colliding IDs use a pointer index. None of these indexes is
reused across separate calls or online updates.
Generic complete-view preparation also counts inputs while indexing edges,
using an edge-count capacity estimate instead of first scanning the same edges
just to reserve body storage.

Newly compiled programs can also attest that the original rule dependency graph
is acyclic. For that certificate, standalone full execution prepares the existing
flat source and body indexes during its initial graph-summary traversal. Those
indexes provide the summary's degree counts and are then consumed directly by
the indexed proof and deletion logic. This avoids a second graph traversal to
prepare the pass, repeated pointer-keyed cache lookups, and grounded SCC analysis.
Necessary-input sets still intersect every active source and are computed only
when needed. Every deletion is re-proved against the current bodies. Repeated
occurrences, rule events and query/evidence roots keep their original semantics.

This path requires trusted compiler metadata, complete endpoints and the freshly
constructed original graph before other rewrites. Recursive strata, eqrel,
special pruning policies, and a user relation named `__agg_sum_state` disable it.
The original graph and aggregate identities must fit their runtime encoding.
Internally generated aggregate states remain acyclic: witness dependencies
follow the relation order, transitions advance the state step, and the final
state precedes the ordinary head. Older generated executables and manual rule
managers have no compiler certificate and use the indexed analysis above.
Public rule-manager mutations invalidate the certificate. Read-only reports
continue to build their own grounded analysis.

When outgoing occurrence counts are available, the proof checks the current
last premise of the candidate's definition before consulting necessary-input
sets. If its outgoing occurrence count is one,
only that definition uses it. Other inputs cannot contain it or require it in
their own source bodies; only another occurrence of the same candidate can
prove this deletion. This rejects unsuccessful proofs without constructing
provider caches. The check reads current bodies and counts on every attempt,
so a later shortened definition can still enable a proof.
Its definition and necessary-input caches borrow pointers during the analysis;
the view and original graph retain every entity until the analysis ends, before
cleanup is applied. This avoids repeated shared-ownership copies and releases.
Targets, mutation candidates and cleanup plans retain their owning references.

Standalone full BDD compilation uses the existing sorting helpers to order
facts by tuple and rules by their head tuple and normalized support metadata,
with ID tie breakers. Registration and formula initialization share those
sorted lists. Support metadata only orders variables; equal or overlapping
support sets never merge independent rule events. This applies to both SISO and SISO+pass
execution; online initialization and updates retain their existing entrypoint.
The ordering changes internal BDD indexes, with each event and weight still
mapped to its own variable.

Definition preparation skips searching bodies for targets when no eligible
definition exists. On a certified fresh DAG, it also skips filtering the same
targets again: no intervening SCC check has changed their definitions or safety.
Its definition count comes from the initial node traversal, avoiding a second
node scan when SCC analysis is unnecessary.
Generic and read-only cycle checks retain their complete recursive statistics.
If the existing graph summary shows that every rule edge is nondeterministic,
the pass skips building analysis indexes entirely: no candidate can have the
required deterministic definition. This exact structural check also covers
recursive programs and performs no probability estimate.

With splitting disabled, SISO reuses its initial or preceding iteration's exact
random-variable count instead of traversing the unchanged graph again before
each iteration. It still counts after mutations; splitting modes retain their
ordinary recount because a split can change the graph between iterations.

The same full compiler also reuses its existing SCC groups to compile a
nonrecursive singleton head's incoming contributions together and form its OR
once. It reads each body's inputs and signs once, instead of copying them for
every input or repeatedly sorting and combining all incoming edges. Fact and
seed heads, recursive groups, unavailable inputs and prepopulated formula maps
retain the ordinary compilation path.

For definitions with at most two body occurrences, the indexed proof tracks
coverage in local bits, counting repeated occurrences of the same premise once.
Wider definitions mark distinct needed and covered premises in the existing
stamp array. Both paths retain the lazy `Must_1` cache and the first provider for
each certificate. Once all premises are
covered, it stops without expanding later providers. Generic cycle analysis first peels acyclic sources using
the forward adjacency. A fully peeled DAG needs no SCC DFS; any residual graph
still receives exact SCC analysis, with reverse traversal reusing the source
and body indexes. Cleanup reuses the same source and body indexes and
the outgoing occurrence counts from preparation or SCC construction. Starting at inputs
that lose their last active reference, it prepares a complete cleanup plan while
preserving query/evidence roots. The pipeline applies the plan to the existing
working view; it retains graph entities and raw adjacency, just as ordinary
pruning does.
Touching unsupported or recursive structure falls back to full pruning before
any cleanup plan is applied. `--merge-bi-imp` and `--prune-extra` always use the
existing full pruner. This optional plan requires a freshly pruned complete view;
the default core API continues to erase bodies only.

The pipeline directly uses the working view returned by initial or fallback
pruning; equivalence merging refreshes that view's evidence roots after moving
observations to representative nodes. Graph statistics read bodies by reference,
and a zero-deletion pass reuses the initial summary. Successful local cleanup
computes exact degree/body maxima from the flat indexes and updates the original
summary by the removed nodes and edges, avoiding another full body traversal and
degree hash table. No additional dependency/component/depth graph is built for
cleanup.

## Run

```bash
mkdir -p build/and-input-audit/results
./build/src/souffle --full-only \
  -F evaluation/full/symbolization/readelf/input \
  evaluation/full/symbolization/symbolization.dl \
  -o build/and-input-audit/symbolization
./build/and-input-audit/symbolization --rewrite --and-input-redundancy \
  -F evaluation/full/symbolization/readelf/input \
  -D build/and-input-audit/results
python3 evaluation/full/and_input_redundancy.py \
  --binary build/and-input-audit/symbolization \
  --output-root build/and-input-audit/results
```

The runner visits all 16 Symbolization cases serially. Use `--cases readelf`
for a smoke run and `--timeout 300` to set the per-case limit. It preserves
partial reports on timeout, records statuses, and writes `summary.json` and
`summary.csv`. Outputs and timing data belong in ignored directories.

Any newly compiled full executable accepts `--dump=and-redundancy`. The compiler
accepts the same selector to bake its default. It writes:

- `and-redundancy-before-rewrite.json`: after query/evidence-aware pruning.
- `and-redundancy-after-rewrite.json`: after an actual rewrite completes.

Plain execution and `--derv-only` produce only the first report. A fully handled
lifted execution does not construct a concrete graph and has no such report.
Online execution rejects the selector before loading facts.

`--and-input-redundancy` can also be baked at compilation. Online execution
rejects it before loading facts. `--derv-only` still applies an explicitly
requested input pass, then skips inference and SISO. When combined with
`--lifted-wmc`, the early pointwise fastpath yields to concrete graph construction
so that pruning and this pass precede graph fastpaths. The debugger records
`lifted_reason=and_input_redundancy_requires_concrete_graph`.

## Read the measurements

Each report contains graph sizes, body input associations, eligible definitions,
individually proven input associations, distinct affected edges and redundant
nodes, recursive-node counts, and analysis time. Certificates identify the
target edge and occurrence, deterministic definition, each covered premise,
its provider, and all provider source edges when using `Must_1`. Node and edge
IDs are snapshot-local strings; rule IDs, probabilities, signs and support tokens
are included for inspection.

**Audit certificates are independent opportunities, not a batch deletion plan.** For
`c1 := a`, `c2 := a`, and `e(c1,c2)`, either occurrence can be removed but both
cannot. The mutation pass re-proves after each removal and keeps one necessary
input. It preserves the existing `Hyperedge` object because the BDD backend
keys rule events by its identity.

The debugger records `and_input_redundancy_{before_rewrite|after_rewrite}_`
`analysis_ms`, `write_ms`, and opportunity counters. Runner wall time includes
detection, report writing, inference and output. Its `bdd_live_nodes` is the
existing sum of CUDD live node counts after compilation of the slow components;
it is not a count of the derivation graph or the last manager alone. Missing
metrics stay blank rather than being reported as zero.

The `AND_INPUT_REDUNDANCY` debugger stage sits after `PRUNING` and before
`FC_WMC_HYBRID` or plain formula compilation. Its `and_input_redundancy_*`
counters include deleted input associations, affected edges, detection rounds
(excluding a final scan when unchanged definitions already certify the fixpoint),
cleaned nodes/hyperedges, graph/body sizes, and detection/mutation/pruning/total
times. `pruning_ms` includes `cleanup_planning_ms` as well as applying the plan
or full pruning. `cleanup_strategy` records `none`, `local`, or `full`.
`analysis_strategy` records `indexed_fresh_dag` for the prepared summary indexes,
`lazy_fresh_dag` for a degree-only workspace supplied directly to the helper, or
`indexed` for generic analysis. `summary_no_definitions` records the summary-only
rejection when there are no deterministic rule edges; no detection round runs.
`workspace_summary_ms` measures the whole initial
summary traversal when it prepares a workspace, including the common summary
work. It is a subset of the existing pruning stage, separate from the pass's
`total_ms`; benchmark wall time includes both stages.
`initialization_ms` measures analysis setup and target preparation inside
`detection_ms`; it is a submeasurement and must not be added to total time again.
`remaining_proven_input_associations` counts opportunities at the pass's
fixpoint **before SISO**; SISO may subsequently expose new opportunities.
`initial_input_associations` and `final_input_associations` count all body links
before the pass and after cleanup. Their difference includes links on pruned
edges as well as explicitly deleted inputs. The core pass alone removes no
nodes or edges; the pipeline applies the query/evidence-aware cleanup plan or
uses ordinary pruning.

`rewrite_initial_*` describes the graph after this pass and cleanup. Simple and
general SISO contribution counters use that snapshot, while `after_prune_*`
retains the initial pruning counts. Additional SISO rewrites require comparing
completed SISO region counts against a paired SISO-only run; they are not an
invented per-pass counter. Read-only audit runs have zero mutation effects.

Benchmark all cases without proof dumps or verbose formula profiling:

```bash
python3 evaluation/full/and_input_redundancy.py --mode benchmark --runs 3 \
  --binary build/and-input-audit/symbolization \
  --output-root build/and-input-audit/benchmark
```

This compares plain baseline, SISO, and SISO plus the input pass. It checks
query keys/probabilities, includes detection costs in wall time, records BDD
node counts and signed changes in completed SISO counts, and retains failures
and timeouts. Use `--variants siso,siso_and_pass` for just the paired rewrite
runs and `--resume --variants baseline` to append baseline trials later with
the same binary. `--mem-limit-mb 4096` caps each benchmark child's address space
at 4096 MiB; the default is uncapped. Resource limits and failed trials remain
in the collected records. Probability comparisons use the printed decimal
values with an absolute tolerance of `1e-8`, avoiding binary subtraction errors
at that boundary. Pre/post opportunity counts alone do not establish a speedup.

Timed children use blocking process waits with a separate timeout watchdog,
avoiding Python's timeout-wait polling delay of up to roughly 50 ms. Wall time
still includes process startup, all pipeline stages and ordinary output. The
summary records the timing method, and resume requires the same method so older
polling-based records cannot enter these comparisons.

## Verification

`regression.and_input_redundancy` enumerates random worlds for tiny acyclic
graphs and compares every node's truth value after each individually certified
removal and after the actual pass reaches its fixpoint. It covers collective witnesses, random rule events, alternative
sources, facts, exact determinism, empty bodies, negative literals, recursion,
aliases, incomplete snapshots, independent events, report serialization and
the unsafe batch-deletion example, stale necessary-input evidence, duplicate
occurrences, and preserved random-event/cache/adjacency identities. The
optimized detector also covers shortened definitions enabling later deletions,
shared providers with alternative derivations, unrelated recursive components,
colliding or maximum sparse IDs, and foreign endpoints with matching IDs.
The cleanup regression compares planned cleanup with ordinary full pruning,
including shared consumers, query/evidence roots, duplicate occurrences,
inactive raw adjacency, and cascading removals.
`regression.and_input_fast` exercises the lazy DAG path with independent world
enumeration, rule-event identity checks, shortened definitions, stale proofs,
duplicate occurrences, query/evidence cleanup and certificate fallback. The
compiler-certificate regressions check emitted recursion metadata, invalidation
after public mutations, recursive base clauses, eqrel, aggregate identities and
the reserved state namespace.
The execution-contract regression checks runtime reports, stage ordering, actual
deletion/cleanup, correlated evidence-conditioned outputs, opt-in behavior,
both rewrite dispatchers and online rejection.
