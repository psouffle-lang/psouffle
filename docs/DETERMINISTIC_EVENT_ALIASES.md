# Deterministic event aliases

`--deterministic-event-aliases` is an independent, opt-in standalone full
inference pass. It runs after initial query/evidence pruning, before AND-input
elimination and SISO. It enables neither of those passes. Compiler arguments
bake the same runtime default. Online execution and `--derv-only` reject it
before loading facts. With this flag, an explicitly requested lifted fastpath
retains the concrete graph so that the pass can run.

A node `c` with no fact source and one complete active positive definition
`c <- x, t1, ..., tn` represents exactly `x` when the rule event is deterministic
and each `ti` is structurally proven true. The pass requires probability exactly
one and empty probabilistic support for the rule. True fact seeds must be unshadowed,
have probability one and empty support, and have no active rule sources.
Regular full pruning's backward traversal collects a shared source/body index
while discovering each retained node, edge and body occurrence. Alias analysis
borrows that complete index for the exact owner and view, using guarded dense
node IDs with a pointer-identity fallback. Independent callers retain the
validated standalone path. The indexed bodies record deterministic positive
proof consumers. A queue propagates truth from those seeds and deterministic
empty-body rules: one entirely true derivation proves its head true, including
when other derivations are probabilistic. Repeated body occurrences are counted
separately. Seeded positive cycles can propagate truth; unseeded cycles cannot
prove themselves true. Evidence is never used as a truth seed, and nodes with
both fact and rule sources remain outside this proof. This closure requires
linear work in nodes, edges and body occurrences, without another graph index
construction, SCC traversal or formula compilation for the alias proof.
Other inputs must be occurrences of one identical node. Equal marginal
probabilities or support sets never establish event equality.

For every random world, `Phi_c = Phi_x`. Substituting `x` for each use of `c`
preserves both positive and negative literals, correlations, joint events and
evidence. The pass resolves functional copy chains once. Copy cycles, chains
leading to them, incomplete endpoints, alternative derivations, input facts,
negative definitions and shadow identities are skipped. Classes with conflicting
observations retain their original graph so ordinary inference can reject the
inconsistent evidence. An invocation detects the initial copy chains; it does
not claim a complete event-equality fixed point after input coalescing.

Consumers may be numerous and aliases may themselves be queries. Query aliases
are physically removed from the active graph. An explicit output binding retains
each original name and points to the surviving event representative. Representatives
needed only to supply those outputs are hidden from the final output names.
Probabilities are copied only after conditional inference is complete, from the
current result or an existing precomputed result; no marginal is substituted
for an event during graph processing. This allows formula compilation and WMC
to evaluate one representative while still emitting all original query names.

The owner and active view are updated together. Alias definitions are removed,
all raw consumers are redirected, tuple/evidence lookup resolves to the current
representative, and graph/view caches are invalidated. A subsequent owner prune
cannot resurrect the copy definitions. Other retired sources remain outside
active membership. Remaining rule applications keep their edge object, ID,
probability and probabilistic support. The pass does not deduplicate OR edges.
Endpoint replacement and signed Boolean deduplication use one stable body
compaction; small bodies require no per-edge hash allocation. The shared source
arrays then retire alias rows and remap surviving indexes. Only consumers of
aliases reread their changed physical bodies. AND-input elimination uses this
updated index, with its own required recursive-node checks, rather than
collecting the graph again.

After substitution, repeated occurrences of the same event with the same sign
are removed using `X AND X = X`, including repeated negative literals.
Opposite signs remain aligned and distinct. Aggregate witness tuples, weights,
steps and grounded rule applications remain separate: two score records whose
presence is the same event still contribute both values to `sum`.

The `DETERMINISTIC_EVENT_ALIASES` debugger stage records before/after graph
shape, candidates, proven true facts/derived nodes, deterministic proof edges,
merged and queried aliases, input replacements, duplicate
Boolean input deletions, active/owner removals, rejected copy cycles and evidence
conflicts, analysis, mutation, summary and total costs. Raw consumer counts can
include inactive edges; active counts describe the residual graph. Full native
wall time includes every pass and output restoration. `shared_prune_indexes`
records successful reuse. Index collection is charged to the regular `PRUNING`
stage; alias `analysis_ms` measures the subsequent indexed proof work. After
alias mutation, `summary_ms` includes source-array rebasing and any AND
preparation. The AND `workspace_summary_ms` also includes that preparation and
initial source finalization, so these intervals overlap the pruning and alias
stages and must not be added twice. Use total native wall time when comparing
implementations. Duplicate removal counts cover all same-sign duplicates in
an affected consumer, including ones present before substitution. Named output
counts come from the probability file, while graph output counts describe
physical representatives.

```bash
./compute --deterministic-event-aliases -F input -D output
./compute --deterministic-event-aliases --and-input-redundancy --rewrite \
  -F input -D output
python3 evaluation/full/and_input_redundancy.py --mode benchmark \
  --binary ./compute --variants baseline siso siso_and_pass alias alias_pass_siso \
  --cases readelf --output-root build/event-alias-benchmark
```

The regression suite enumerates original random-event worlds, compares retained
and aliased events, checks evidence and joint-event formulas, retains independent
rule events, tests cache/owner repruning and exercises real aggregate replay with
two correlated score records. Compiler/runtime tests check output names and
positive/negative evidence with explicit and implicit SISO.
