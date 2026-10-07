# Local private-factor optimizations

`--local-series-contraction` and `--terminal-query-factors` are independent,
opt-in standalone full inference passes. Compiler arguments bake the same
runtime defaults. Both reject online execution and `--derv-only`; neither
enables SISO or aliases. They run after enabled alias, AND-input and SISO passes,
before component solving. When both are enabled, series contraction plans first
and terminal factoring consumes its updated virtual graph in the same workspace.
An explicitly requested lifted fastpath keeps the concrete graph for these passes.

## Private series contraction

For a unique definition `Z = R1 AND A` and sole positive use
`T = R2 AND Z AND B`, substitute the first body into the second:

```
T = R AND A AND B,  P(R) = P(R1) * P(R2).
```

`A` and `B` may be conjunctions with multiple inputs, shared consumers and
correlated events. Signed external literals keep their signs. Identical signed
inputs are coalesced; opposite signs remain distinct. `Z` must have no input
fact, evidence, output requirement, shadow identity or other derivation/use.
The relevant head and body nodes must be outside recursive SCCs. Zero and one
factors are supported, but a positive product that underflows to zero is skipped.

Random-factor independence is proved from the complete active graph's primitive
support-token owners. Each nonconstant factor needs explicit provenance; every
token of either absorbed factor must have exactly one owner. Shared provenance,
unknown random-event provenance, malformed probabilities and incomplete source
information cause rejection. Support tokens identify primitive provenance, not
Boolean events: matching support never justifies equating two bodies.

For each original random world, the substituted formula equals the old formula
with `R = R1 AND R2`. Replacing these private independent factors by one Bernoulli
factor of the product probability preserves the joint distribution of every
retained event and all evidence-conditioned probabilities. Original external
event identities remain intact. The intermediate node and original rule edges
are retired from both the active view and owner; surviving compound edges keep
the union of original primitive provenance.

Analysis borrows indexed body rows and updates a local degree worklist. Virtual
edges reach a fixed point before materialization. A persistent factor tree avoids
repeated support unions, flattening provenance only for surviving compound edges.
Bodies are still copied when a contraction changes their inputs. A linear body
work budget skips further candidates when expanding chains would require excessive
copying; this limits optimization work without changing event semantics.

## Deferred terminal marginal queries

A terminal output `Q = R AND A` with a unique positive unary definition and a
private independent factor can be removed from the inference core. A deferred
output record retains the original name and evaluates
`P(Q | E) = P(R) * P(A | E)` after conditional inference. The parent may contain
OR, shared dependencies and correlated inputs. It becomes a hidden inference
root if no original output already requires it. Chains are resolved to surviving
parents in one reverse scan; hidden chain steps are counted separately.

An observed `Q`, any downstream consumer (including a joint-query formula), an
input fact, an alternative definition, a negative input, a recursive endpoint,
shared factor provenance or an unresolved owner history prevents factoring.
Evidence on `A` or other external events is allowed because the private factor
appears nowhere in their formulas. `Q` remains an output record, never an event
alias of `A` or an independent fact holding the computed marginal. Query aliases
are restored after these conditional output probabilities are available.

Aggregate witness records and grounded aggregate operations remain separate.
This pass applies only to the final graph's marginal output roots; it does not
replace events inside a sum or a joint formula.

## Integration, diagnostics and validation

`PRIVATE_FACTOR_REWRITE` collects the complete active graph once, counting primitive
support ownership during that collection. Both optimizations share the source/body
index, signed consumer index, SCC result and private-factor certificates. SCC
analysis borrows the same consumer CSR rather than rebuilding its forward arcs.
Series substitution updates local sources and degrees; terminal factoring then
uses that virtual graph without another collection or privacy/SCC analysis.
Only surviving compound edges are materialized. After any successful rewrite,
one owner/view commit retires the planned objects and historical SISO definitions,
filters maps and adjacency once, and invalidates caches once. A zero-hit run leaves
the owner unchanged. The standalone helper APIs retain conservative owner checks;
only the full pipeline certifies its complete post-SISO active view.

The single stage records candidates, contractions, removed nodes and
edges, added edges, removed rule variables, input associations, recursive nodes,
privacy/owner rejections and plan time. Series removed edges count retired
originals; added edges count virtual compounds surviving the series stage.
Their difference is the series-stage net edge reduction. Terminal factoring may
consume those virtual compounds before materialization; the shared counters
separately report final materialized compounds and terminal virtual sources.
The terminal counters record factored physical output representatives, hidden
chain steps, removed nodes/edges, promoted roots, rejection reasons and timings.
Physical output representatives can supply several original alias names.
Phase node, edge and input counts come from the workspace, without full graph
summary scans. Shared preparation, plan and final commit timings are recorded
once; collection/SCC/support counters and retirement batches expose repeated work.
With rewrite enabled,
`rewrite_final.dot/json` includes these passes. The existing `rewrite_final_*`
counters describe SISO's residual view; use `local_series_after_*` or
`terminal_query_after_*` counters for the graph after the new passes.

```bash
./compute --rewrite --local-series-contraction --terminal-query-factors \
  -F input -D output
./compute --deterministic-event-aliases --and-input-redundancy --rewrite \
  --local-series-contraction --terminal-query-factors -F input -D output
python3 evaluation/full/and_input_redundancy.py --mode benchmark \
  --binary ./compute --cases readelf --output-root build/private-factor-benchmark \
  --variants baseline siso alias_pass_siso alias_pass_siso_series \
  alias_pass_siso_terminal alias_pass_siso_private_factors --runs 3
```

Regression coverage enumerates primitive worlds and conditions on positive and
negative evidence, checks shared OR parents and correlated external bodies,
and rejects extra sources/consumers, shared or missing provenance, recursion,
malformed endpoints and unsafe owner history. CLI tests compare full output
names and probabilities with plain inference across standalone, alias and
implicit/explicit SISO configurations, including compiler defaults and online
scope rejection. Real benchmark opportunities must be measured on the residual
graph; rule-level candidate counts are not net SISO gains.
