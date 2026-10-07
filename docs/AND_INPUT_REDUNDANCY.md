# Read-only AND-input redundancy audit

The opt-in detector measures opportunities for conservative AND-input redundancy
elimination. It does not rewrite the graph, estimate probabilities, remove random
events, or invoke extra SISO passes. It runs only in standalone full execution.

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

Each call builds fresh adjacency and SCC indexes from the active edges of the
complete working view. The standalone pipeline confirms source completeness;
the detector API defaults to no proofs when completeness is not confirmed.
Inactive raw adjacency left behind by rewrite is not a source in this snapshot.
Post-rewrite certificates describe the residual graph's event model, including
existing SISO summaries; they do not reconstruct original event formulas hidden
by those summaries or establish identities across snapshots.

## Run

```bash
mkdir -p build/and-input-audit/results
./build/src/souffle --full-only \
  -F evaluation/full/symbolization/readelf/input \
  evaluation/full/symbolization/symbolization.dl \
  -o build/and-input-audit/symbolization
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

## Read the measurements

Each report contains graph sizes, body input associations, eligible definitions,
individually proven input associations, distinct affected edges and redundant
nodes, recursive-node counts, and analysis time. Certificates identify the
target edge and occurrence, deterministic definition, each covered premise,
its provider, and all provider source edges when using `Must_1`. Node and edge
IDs are snapshot-local strings; rule IDs, probabilities, signs and support tokens
are included for inspection.

**Certificates are independent opportunities, not a batch deletion plan.** For
`c1 := a`, `c2 := a`, and `e(c1,c2)`, either occurrence can be removed but both
cannot. A future mutation pass must rebuild or update evidence after each
removal. A future random-edge mutation must preserve the existing `Hyperedge`
object: the BDD backend keys rule events by its identity.

The debugger records `and_input_redundancy_{before_rewrite|after_rewrite}_`
`analysis_ms`, `write_ms`, and opportunity counters. Runner wall time includes
detection, report writing, inference and output. Its `bdd_live_nodes` is the
existing sum of CUDD live node counts after compilation of the slow components;
it is not a count of the derivation graph or the last manager alone. Missing
metrics stay blank rather than being reported as zero.

Deleted input associations, cleaned nodes/edges, and additional SISO rewrites
caused by this detector are all zero. Measuring those effects and any net
compilation/WMC benefit requires the later mutation pass and repeated baseline,
SISO, and SISO-plus-pass runs. Pre/post opportunity counts alone do not establish
a performance improvement.

## Verification

`regression.and_input_redundancy` enumerates random worlds for tiny acyclic
graphs and compares every node's truth value after each individually certified
removal. It covers collective witnesses, random rule events, alternative
sources, facts, exact determinism, empty bodies, negative literals, recursion,
aliases, incomplete snapshots, independent events, report serialization and
the unsafe batch-deletion example. The execution-contract regression checks
runtime reports, unchanged probabilities/graph counts, opt-in behavior and
online rejection.
