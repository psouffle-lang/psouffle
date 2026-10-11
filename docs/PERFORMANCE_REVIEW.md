# Pipeline Performance Review

Reviewed on 2026-10-05 against the `main` working tree in `psouffle`, including
the pending evidence fixes for deterministic observation roots, probability-zero
sources, and complete nullary provenance. This is an audit of optimization
opportunities. The runtime optimizations below have not been implemented.

The review follows compilation, typed input loading, semi-naive evaluation and
provenance recording, lift, graph construction/pruning, explicit and implicit
rewrite, formula compilation, WMC, incremental commits, and output/diagnostics.
Measurements use small synthetic inputs; no paper experiment was run. Generated
programs, outputs, traces, and timing logs remain under ignored
`build/performance-review/`.

## Priorities

`Measured` means a local runtime or isolated-kernel measurement. `Source` means
the overhead is visible in the implementation but its end-to-end benefit has
not been measured. Priorities reflect likely usefulness and implementation
risk, rather than a benchmark ranking across the paper workloads.

| Priority | Opportunity | Evidence | Best matching workload |
| --- | --- | --- | --- |
| Prerequisite | Make provenance recording safe before increasing OpenMP threads | Two four-thread runs failed; shared container mutations are unsynchronized | Programs compiled with `-j N`, where `N > 1` |
| High | Coalesce debugger checkpoints instead of serializing all history on every stage transition | Measured; cumulative work grows quadratically with turn count | Long online sessions, especially small updates |
| High | Buffer probability output instead of flushing each row | Measured kernel and syscall counts; low implementation cost | Large output relations or many snapshots |
| High | Fuse typed probability tracking with actual input loading | Measured duplicate reads; source shows temporary relation indexing | Large input files, short inference runs |
| High | Summarize lift witnesses once and share eligibility results | Source: repeated global provenance scans | Many lifted outputs/rules with free body variables |
| Medium | Move the pruned view; avoid redundant component construction in plain WMC | Source: set copies and a second connectivity pass | Large full-inference graphs |
| Medium | Cache empty validity sets with explicit initialization flags | Source: repeated scans when the cached result is empty | Turns deleting all live nodes/edges |
| Medium | Reduce provenance allocations and use structured graph-edge keys | Source: vectors, sets, copies, string formatting and ordered lookup per grounding | Large derivation counts or high fan-in |
| Medium | Deduplicate recursive overdelete scans within a safe phase | Source plus existing DRed scan counters | Heads with many recursive derivations |
| Medium | Group incremental WMC by effective weight configuration | Source: repeated restoration can invalidate the WMC cache | Many outputs sharing regional boundary weights |
| Medium | Reuse log-WMC work within a safely frozen evidence-counting batch | Source: per-call cache in log WMC | Many evidence-conditioned queries sharing BDDs |
| Later | Maintain pruning reachability and batch region analysis updates | Source: graph-wide pruning remains despite local FC | Small deltas on large persistent graphs |
| Later | Tune CUDD sizing/ordering and generated C++ compilation | Source/configuration and illustrative compile measurements | BDD-heavy workloads or large DSL programs |

## Measurements and limits

Local environment: Linux on WSL2, 32 logical CPUs, approximately 7.6 GiB RAM,
GCC 13.3.0, Release build, 32-bit RAM domains, OpenMP enabled, and CUDD from
`/usr/local`. Runtime probes use one thread except the explicit concurrency
check. Filesystem behavior and machine load affect these measurements.

### Debugger history serialization

The program is `q(X) :- a(X)` with one input fact of probability `0.5`.
An `inc-naive` session receives repeated empty `commit` commands and then `q`.
Times include process startup, all commits, probability snapshots, and shutdown;
each entry is the median of three runs.

| Empty commits | Default auto-dump, seconds | `SOUFFLE_DEBUGGER_AUTO_DUMP=0`, seconds |
| --- | ---: | ---: |
| 100 | 1.7206 | 0.0377 |
| 200 | 6.4902 | 0.0708 |
| 400 | 30.5197 | 0.1745 |

All runs completed. Probability snapshots match byte-for-byte across the two
settings: 101, 201, and 401 files respectively. Both representative 400-turn
final reports contain 401 turns and 1,608 stages with status `completed`.
Turning off automatic checkpoints reduces intermediate crash/progress reporting;
it is a diagnostic control, not a complete replacement for the current behavior.
Explicit full/rewrite progress dumps also bypass this setting.

Empty commits expose fixed per-turn overhead. These timings do not estimate
speedups for substantive updates, larger graphs, or the paper experiments.

### Input reads and probability writes

A second input contains 20,000 distinct facts, each with probability `0.5`.
The generated standalone program uses pointwise lift with threshold `1`.
An `strace` run records:

- `a.facts`: three opens and 217,780 bytes read, exactly twice its 108,890-byte
  contents. One extra open is the prepass's existence-check stream.
- `a.prob`: one open and 80,000 bytes read, matching its file size.
- `facts.prob`: 20,000 `write` calls for 20,000 output rows.

Tracing changes runtime substantially; its timings are excluded. In two
untraced one-thread runs, total wall time was 0.0944 and 0.0974 seconds, while
the reported probability-dump stage took 0.0470 and 0.0468 seconds. This output
cost is visible even when lift bypasses graph construction and concrete BDDs.

A separate C++ `ofstream` kernel writes the same formatted rows using either
`std::endl` per row or `\n` followed by a final flush. Medians of five runs:

| Rows | Per-row flush, seconds | Buffered newline, seconds |
| --- | ---: | ---: |
| 20,000 | 0.04086 | 0.00343 |
| 200,000 | 0.40582 | 0.03318 |

The output bytes are identical. This isolates formatting/writing; it excludes
graph traversal, sorting, inference, and debugger work, so it is not a measured
end-to-end speedup from a runtime patch.

### Parallel recording prerequisite

The same 20,000-fact input was run twice at each thread count using a binary
compiled with `--full-only -j4`. Both `-j1` runs completed with all 20,000
probabilities. Both `-j4` runs failed: one reported a segmentation violation in
the rule and the other aborted with `double free or corruption (!prev)`.

[ParallelTransformer](../src/ram/transform/Parallel.cpp#L40) excludes guarded
insertion and erasure, but permits operations containing `RecordDerivation`.
[Generated recording](../src/synthesiser/Synthesiser.cpp#L2603) mutates the shared
`untypedTuple2RuleApplications` unordered map and its per-head sets without
synchronization. The generated probe places these writes inside an OpenMP loop.
Concurrent insertions into different map keys are still unsafe. These failures
support treating the shared recording path as a correctness prerequisite for
parallel speedups; the recordings are not a sanitizer diagnosis of every race.

A possible design is per-thread application buffers with a merge at the
appropriate stratum/round barrier. Complete provenance, delta maps, derivation
checks, and diagnostic counters must all observe the required visibility.
A temporary exclusion of recording queries from parallelization is simpler;
a single mutex would need throughput measurement. Validation should compare
one-thread and multi-thread results, including recursive and mixed DRed turns,
and use a concurrency sanitizer where supported.

### Compilation

For the same one-rule DSL program, `-g` generation was measured three times;
generation plus external C++ compilation with `-o` was measured once per mode.

| Generated capabilities | C++ bytes | `-g` median, seconds | `-o` single run, seconds |
| --- | ---: | ---: | ---: |
| Default full + online | 85,077 | 0.0664 | 10.8508 |
| `--full-only` | 37,989 | 0.0631 | 10.1111 |

This illustrates the existing capability selector and the cost of the C++
toolchain on a tiny program. A single compile measurement cannot establish a
general improvement rate. Large-program transformer and generated-code costs
remain unmeasured.

## Reproducing the small probes

From the repository root, create a disposable program and input directories:

```bash
mkdir -p build/performance-review/reproduce
python3 - <<'PY'
from pathlib import Path
r = Path('build/performance-review/reproduce')
(r / 'probe.dl').write_text(
    '.decl a(x:number)\n.input a\n.decl q(x:number)\n.output q\n'
    'q(X) :- a(X).\nquery(q(_)).\n')
for name, count in [('small', 1), ('large', 20000)]:
    d = r / name
    d.mkdir(exist_ok=True)
    (d / 'a.facts').write_text(''.join(f'{i}\n' for i in range(count)))
    (d / 'a.prob').write_text('0.5\n' * count)
PY
./build/src/souffle build/performance-review/reproduce/probe.dl \
  -o build/performance-review/reproduce/probe
```

Run an empty-session comparison, including final output and report checks:

```bash
python3 - <<'PY'
import json, os, statistics, subprocess, time
from pathlib import Path
r = Path('build/performance-review/reproduce').resolve()
for turns in (100, 200, 400):
    snapshots = []
    for flag in ('1', '0'):
        elapsed = []
        for repeat in range(3):
            d = r / f'online_{turns}_{flag}_{repeat}'
            d.mkdir(exist_ok=True)
            start = time.perf_counter()
            subprocess.run([str(r / 'probe'), '--setmode=inc-naive', '-j1',
                            '-F', str(r / 'small'), '-D', str(d)],
                           input='commit\n' * turns + 'q\n', text=True,
                           env={**os.environ, 'SOUFFLE_DEBUGGER_AUTO_DUMP': flag},
                           stdout=subprocess.DEVNULL, check=True)
            elapsed.append(time.perf_counter() - start)
            current = {p.name: p.read_bytes() for p in d.glob('*.prob')}
            assert len(current) == turns + 1
            snapshots.append(current)
            reports = [json.loads(p.read_text()) for p in d.glob('*.json')]
            assert any(x['status'] == 'completed' and len(x['turns']) == turns + 1
                       for x in reports)
        print(turns, flag, statistics.median(elapsed))
    assert all(x == snapshots[0] for x in snapshots)
PY
```

For the input/output trace, use this program's `large` input with standalone
`--lifted-wmc --lifted-threshold=1 -j1`, an existing output directory, and
`SOUFFLE_DEBUGGER_AUTO_DUMP=0`. Trace `openat,read,write,writev` using
`strace -f -yy`; compare syscall/byte counts rather than traced execution time.
To reproduce the parallel prerequisite, compile a separate binary with
`--full-only -j4` and compare runtime `-j1` with `-j4` on the same large input.
Disable core dumps with `ulimit -c 0` for that failing probe.

## End-to-end coverage

| Stage | Entry points reviewed | Finding and existing optimization |
| --- | --- | --- |
| Parse/AST transforms | [MainDriver](../src/MainDriver.cpp#L454) | Repeated minimization/alias/empty-relation passes need per-pass timing before selective invalidation; preserve probabilistic clause identity and evidence roots. |
| AST-to-RAM and indexes | [online ClauseTranslator](../src/ast2ram/online/ClauseTranslator.cpp), [UnitTranslator](../src/ast2ram/online/UnitTranslator.cpp), [RAM pipeline](../src/MainDriver.cpp#L507) | Index creation, guard hoisting, join transformations, and delta evaluation already exist. DRed snapshots and complete-derivation checks carry semantic information. |
| C++ generation/toolchain | [Synthesiser](../src/synthesiser/Synthesiser.cpp), [emission](../src/MainDriver.cpp#L1138) | `--full-only` already omits maintenance; `-G`/`-C` already support multiple files. Measure synthesis and C++ compilation separately. |
| Input/determinism | [typed prepass](../src/synthesiser/Synthesiser.cpp#L4898) | Typed facts are loaded twice into different relation objects. Determinism SCC propagation is already generated from compiler metadata. |
| SEM/provenance | [recording](../src/synthesiser/Synthesiser.cpp#L2603), [tuple representation](../src/include/souffle/Derivation.h#L91) | Recording safety, per-grounding allocations, repeated copies, and recursive overdelete scans are candidates. SEM map summaries are already diagnostic-gated. |
| Pointwise lift | [LiftedWmc](../src/problog/LiftedWmc.cpp#L548) | Witness checks scan global provenance repeatedly; lift runs after SEM. Distinct events already use products; OR/repeated events use exact BDDs. |
| Graph/aggregate replay | [DerivationGraph](../src/include/souffle/problog/DerivationGraph.h#L1434) | Structured edge keys and cached rule metadata can reduce construction costs. Sum replay already indexes witness tuples; reachable sum states can inherently grow combinatorially. |
| Pruning/dependency analysis | [full pruning](../src/problog/FullPipeline.cpp#L2100), [incremental pruning](../src/include/souffle/problog/DerivationGraph.h#L3270) | Extra full-view copies and repeated connectivity work; online pruning still traverses global state. Per-view dependency graphs and sorted adjacency are cached. |
| Explicit rewrite | [GraphRewriter](../src/include/souffle/problog/GraphRewriter.h#L147) | Dirty-frontier detection already exists. Investigate analysis reuse and invalidation frequency using existing rewrite counters. Evidence-affected components must retain correlation. |
| Implicit rewrite | [ImplicitSplitRewrite](../src/problog/ImplicitSplitRewrite.cpp#L847) | Dirty fact/edge worklists and outgoing-edge caches already exist. Consider batched index invalidation, after measuring rebuild counts. |
| Full/naive FC | [ForwardCompilation](../src/include/souffle/problog/ForwardCompilation.h#L366) | Cycle/depth scheduling and borrowed component views already exist. Empty validity caching and eager error-context rendering are avoidable costs. |
| Regional FC/calibration | [RegionalIncremental](../src/include/souffle/problog/RegionalIncremental.h#L1370), [IncRegionAnalyzer](../src/include/souffle/problog/IncRegionAnalyzer.h) | Local dependency graphs, region-limited rollback snapshots, and target/snapshot/anchor WMC caches already exist; repeated closure/anchor rebuilding needs profiling. |
| BDD/WMC/evidence | [CuddManager](../src/include/souffle/problog/formula/CuddManager.h#L1388), [full WMC](../src/problog/FullPipeline.cpp#L1240), [online WMC](../src/include/souffle/cli/Cli.h#L988) | Ordinary WMC caches across queries. Log WMC currently caches within each call; boundary weight restoration can invalidate ordinary WMC reuse. |
| Commit and snapshots | [Executor](../src/include/souffle/cli/Executor.h#L65), [operation staging](../src/include/souffle/cli/PendingOperationStager.h) | Empty commits still execute the pipeline; staging scans relation names for delta relations. Online full-SEM ablations intentionally rebuild more state. |
| Output/debugger | [dumpProbabilities](../src/include/souffle/problog/DerivationGraph.h#L3803), [Debugger](../src/problog/debug/Debugger.cpp#L171) | Mandatory snapshots have an output-size floor; buffering and checkpoint coalescing have direct supporting measurements. Extra graph dumps remain opt-in. |

## Changes worth developing

### 1. Bound debugger checkpoint work

[Stage/turn transitions](../src/problog/debug/Debugger.cpp#L24) call
`maybeAutoDumpLocked`. Serialization walks all stored turns, stages, information,
and messages, constructs JSON, and writes a complete replacement file while
holding the debugger mutex. With bounded per-turn data and a fixed number of
checkpoints per turn, cumulative work is quadratic in session length. Adding
an information field alone does not trigger a dump.

Coalesce checkpoints by turn or elapsed time, or append completed turn records
and keep a small checkpoint for the current turn. Preserve final report schema,
stage timings, termination/exception status, and useful partial reports. Measure
serialized bytes, checkpoint count, serializer CPU, mutex hold time, and total
wall time. The explicit progress dumps in
[full rewrite](../src/problog/FullPipeline.cpp#L494) require the same policy.

### 2. Buffer snapshots without changing their contract

Replace the per-row `std::endl` in probability output with buffered newlines
and a checked final flush/close. Preserve the current tuple-key order, precision,
probability precedence, complete per-turn files, and typed symbol rendering.
Next, measure redundant tuple rendering and the temporary `seen` set/vector
plus ordered string map before changing their structure.

Writing every output on every commit imposes at least linear work in the number
of output tuples. A delta-output protocol would be a separate interface change;
it should not be folded into a buffering patch.

### 3. Parse inputs once

The generated `ProbTrackingSink` inserts typed facts into temporary generated
relations, associates probabilities per input row, and then discards the
relations. The ordinary `runAll` load path parses and indexes the facts again.
Load into the real relations with the tracking sink, propagate determinism,
then start SEM without reloading. This requires a clear load/evaluate split.

Preserve typed reader semantics for symbols, signed/unsigned numbers, floats,
records and input directives; duplicate-row probability precedence; `.prob`
line alignment; missing probability defaults; and initial input snapshots used
by online recomputation. Probability zero must continue to mark a relation as
having a non-unit source. A numeric-only parser is insufficient.

### 4. Summarize lift witnesses once

[hasUniqueRuntimeWitnesses](../src/problog/LiftedWmc.cpp#L749) scans the complete
head-to-applications map and filters by relation for each rule that requires a
free-body-variable witness check. Let `U` be the number of recorded head keys
and `K` the number of such checks: the map traversal alone costs `O(KU)`.
There can be additional work scanning matching application sets.

Build a per-relation/rule summary in one provenance pass: grounding arity validity
and witness counts per head, capped at two where only uniqueness is needed.
Share eligibility memoization for common relation ancestry between output checks,
while keeping cycle detection, rejection explanations, and relation closures
correct. Reject an all-below-threshold run before scanning all active rules.
Measure eligibility time separately from abstract-graph, symbolic-DD, and tuple
instantiation time using the existing lift diagnostics.

Lift currently bypasses concrete graph/BDD work, not SEM or provenance recording.
Skipping provenance earlier is a larger change: it needs complete output and
explicit-query coverage, sound fallback, shared-dependency ownership, and runtime
witness validation. Keep the exact pointwise model; no experimental template or
approximate inference machinery is needed for the first improvements.

### 5. Remove repeated graph work

- Full pruning returns a `WorkingSubgraphView`; the caller reconstructs a second
  view from its sets. Transfer ownership of the returned view instead of copying
  `O(V+E)` entries. Move constructors exist; cache lifetimes must still be valid.
- Plain full FC builds a cached SCC/dependency graph that includes weak-component
  membership. Plain WMC then runs `buildComponentSubgraphs` with another
  connectivity pass and component set construction, even without evidence.
  A no-evidence path can count output formulas directly; evidence paths can reuse
  compatible component metadata. Rewrite still needs component classification.
- [Validity getters](../src/include/souffle/problog/DerivationGraph.h#L867) use a
  nonempty set as their cache-initialized test. An empty valid result is rescanned
  for every getter call. Formula cleanup calls these getters repeatedly, making
  the all-deleted case potentially quadratic in graph/formula count. Use explicit
  computed flags and reset them on structural invalidation.

The dependency graph's depth maps are used by FC work queues. Removing depth
computation globally would change scheduling; make expensive analyses lazy only
for callers that do not need them. Borrowed component views already avoid some
set copies and should be retained.

### 6. Lower per-grounding representation costs

Generated recording constructs a tuple vector and binding vector, copies bindings
into `RuleApplication`, allocates per-head unordered sets, and may copy applications
into several delta maps. Reserve known arities, use safe moves, and measure
allocations/hash time before introducing pools or shared immutable applications.
Ownership across complete and delta maps must remain explicit.

[createEdgeKey](../src/include/souffle/problog/DerivationGraph.h#L2079) copies and
sorts variable names and builds a string for lookup/insertion. A structured key
or precomputed per-rule binding layout can avoid this formatting. Preserve rule
identity and variable-to-value correspondence; distinct weighted clauses and
ground witnesses cannot be merged merely because their output tuple is equal.

Recursive deletion repeatedly scans a head's complete application set in
[recording](../src/synthesiser/Synthesiser.cpp#L2697). Consider a per-phase/head
processed marker, with invalidation when the complete set changes. Use existing
`dred_del_complete_scan_calls` and `dred_del_complete_scan_elems` to establish
benefit, and compare recursive cycle repair and mixed delete/reinsert results.
Do not merge tuple deltas and derivation deltas or remove `@post_delete_*` state.

### 7. Make WMC reuse respect weights and CUDD lifetime

Ordinary WMC already shares a cache across queries under a weight epoch.
Regional output counting can restore selected upstream weights for a query and
restore calibrated weights afterward. Each change can invalidate the cache.
Group queries by their complete effective weight configuration when that can be
proved equivalent, and measure restoration counts/cache hit rates first. Anchor
calibration already caches target, snapshot, and alpha/beta results.

Evidence counting uses log WMC to preserve nonzero conditioning denominators
below double's range. Each call has a fresh memo table. Shared memoization could
help when queries overlap, but only within a phase with stable weights and safe
BDD node lifetimes. Raw `DdNode*` keys cannot survive arbitrary garbage collection,
reordering, or address reuse. One option is to construct and pin all counted
roots, freeze BDD mutation, count the batch, and discard the cache before mutation.
Preserve complemented-cofactor handling, log-sum-exp, impossible-evidence
behavior, and exact conditional probabilities.

### 8. Reduce the fixed floor of incremental turns

Every commit clears/stages delta state and enters SEM/graph/FC/WMC processing.
Incremental pruning scans nodes to find roots, traverses backward dependencies,
and marks nodes/edges over the persistent graph. Regional FC therefore does not
make the entire turn local. Consider cached roots, touched-state updates, and
maintained reachability, but deletion in recursive cycles requires more than
simple incoming-edge reference counts. Previously pruned state may become live
after a later insertion and must remain recoverable.

An empty-update fastpath could reuse probabilities after validating that no
other queued state needs processing. It must still advance turn numbering,
write the required snapshot, resolve requested/effective modes, update FC state,
and preserve diagnostics. Relation/delta-relation handles can also be cached
instead of repeatedly classifying all relation names during staging.

The regional path already uses delta-reachable local dependency graphs and
region-limited rollback snapshots. Profile repeated support closure, boundary,
and anchor recomputation before adding finer invalidation. Deletion-only regional
turns intentionally use the classic path. Full-SEM plus incremental-FC ablation
modes intentionally rebuild/diff more state and should not be presented as
the normal regional cost.

### 9. Tune the measured BDD and compile bottleneck

[CUDD initialization](../src/problog/FullPipeline.cpp#L496) already scales down
for small graphs, reserves twice the estimated variable count, and changes cache
size at cardinality thresholds. Rewrite reuses one manager between slow
components; fast-only components avoid manager construction. Incremental code
already reuses variable indexes and gates reordering by accumulated pressure.

Measure actual live BDD nodes, cache occupancy/hits, resident memory, reorder
time, and variable estimates before tuning. A `maxMemory` setting is a limit,
not measured allocation. Abrupt cache-size thresholds and variable headroom are
candidates, while exact BDD size can still be exponential. Evidence and sum
replay may retain correlation that prevents marginal-product shortcuts.

For large DSL programs, use compiler verbose/debug timings to identify expensive
transformers and separate synthesis from external compilation. Selective pass
invalidation, generated-code factoring, and parallel multi-file compilation
need measurements on larger programs. Do not delete repeated AST passes merely
because they have the same name: intervening transforms can enable new work.

## Suggested patch sequence

1. Repair parallel recording safety; then implement buffered probability output
   and bounded debugger checkpointing as small, separate changes.
2. Fuse typed loading, remove pruned-view/component duplication, fix empty-result
   caches, and summarize lift witnesses. Validate each candidate on matching
   synthetic workloads before claiming end-to-end improvements.
3. Use DRed, regional FC/WMC, rewrite, and dependency-graph diagnostics to select
   representation, pruning, and cache changes. Tune BDD ordering/sizing only when
   measurements show it dominates.

Compiler/runtime/semantic patches require rebuilding and the regression label.
Evidence optimizations additionally need exhaustive possible-world comparisons,
including zero/one probabilities, tiny denominators, repeated events, nullary
heads, multiple witnesses, and rewrite equivalence. None of these opportunities
requires running the paper experiments to start implementing and validating a
focused patch.
