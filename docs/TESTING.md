# Testing

## Source References

- [../CMakeLists.txt:93](../CMakeLists.txt#L93): CMake testing option.
- [../CMakeLists.txt:302](../CMakeLists.txt#L302): regression subdirectory wiring.
- [../tests/regression/CMakeLists.txt:22](../tests/regression/CMakeLists.txt#L22): maintained case registration.
- [../tests/regression/CMakeLists.txt:37](../tests/regression/CMakeLists.txt#L37): `check-regression` target.
- [../tests/regression/run_regression_case.py:153](../tests/regression/run_regression_case.py#L153): maintained compile invocation.
- [topics/testing/README.regression.md:1](topics/testing/README.regression.md#L1): case-level scope.

## Commands

```bash
JOBS=$(nproc || sysctl -n hw.ncpu || echo 2)
cmake -S . -B build
cmake --build build -j${JOBS}
ctest --test-dir build -L regression --output-on-failure --progress -j${JOBS}
cmake --build build --target check-regression
```

## Scope

The regression label includes both imported artifact suites and an execution
contract check. The full suite covers exact probabilities, automatic rewrite
dispatch, strings, numeric tuples, aggregate replay, query equalities, language
examples, graph dumps, and debugger logs. The execution contract compares
standalone rewrite variants and online updates, checks compiler capability
selection, and rejects invalid flag combinations before facts are loaded.
It also checks raw pruning counts, completed per-pattern rewrite counts, and
the simple/general net contribution sums for explicit and implicit rewrite.
A shared-fact fixture exercises nonzero general-SISO mutation counters and
checks the exact result `(a & b) | (a & b & c) = a & b` at probability `0.42`.

The `lifted_fastpath` case checks direct products, disjunctions, repeated
concrete events, probabilistic rules, unique witnesses, shared-event recursive
outputs, extra queries, evidence and multi-witness fallbacks, compiler defaults,
threshold validation, and rejection of online lift. It compares all lifted
variants against ordinary exact probabilities on tiny inputs.

The `evidence_correctness` case checks posteriors against exhaustive possible
worlds on tiny inputs under plain, explicit, implicit, automatic rewrite,
extra pruning, and lifted fallback. It covers unqueried observation relations,
deterministic and nullary observation roots, zero-weight facts and rules,
nullary disjunctions, distinct witnesses and recursive proofs,
shared events and distinct probabilistic rules with identical bodies,
negative and absent observations,
disconnected evidence components, contradictory observations, a positive
`P(E)=1e-400`, typed input-only programs, and source diagnostics for invalid
evidence literals.

Container builds run [docker/smoke.py](../docker/smoke.py) against the installed
compiler with tiny full/rewrite and online inputs. The evaluation image also
checks ProbLog BDD/SDD availability, VProbLog loader/help output, and Scallop
exact inference plus the patched bitwise-and function. These tool checks do
not run paper benchmarks.

The `and_input_redundancy` case checks individually certified deletions and the
actual mutated graph against every random world on small graphs, conservative
rejection cases, graph/event identity, cache invalidation, duplicate adjacency,
stale proof updates and certificate serialization. The execution contract also
checks pass placement after pruning and before SISO/fastpaths, correlated
evidence-conditioned outputs, read-only reports and online rejection.
The detector tests cover shortened definitions enabling later proofs, lazy
intersection across alternative sources, and colliding/sparse node IDs without
merging independent events.
The `and_input_cleanup` case compares local cleanup with ordinary full pruning,
enumerates every retained event's random worlds, and checks roots, cascading
removals, duplicate references, inactive raw adjacency and cache invalidation.
The `and_input_fast` case independently enumerates worlds for the compiler DAG
path, checks the same event identities, current-body proofs and cleanup roots,
and exercises conservative fallback when its certificate is incomplete.
The `compiler_acyclicity` and `compiler_acyclicity_emission` cases check trusted
metadata, mutation invalidation, recursive base clauses, aggregate encoding,
eqrel and reserved aggregate-state namespace exclusions.

The `full_event_order` case checks stable standalone fact/rule variable mappings
across different graph-view insertion orders, shared fact aliases, zero/one
probabilities, seeds and soft reset. An independent random-world oracle checks
marginals, joint events and positive/negative evidence posteriors. It also checks
the existing default registration and incremental insertion/deletion policies.
It compares batched singleton compilation with the ordinary compiler and world
semantics, including shared inputs, signed literals and conservative fallback.
Tuple/support ordering also checks separate rule identities when support keys
are equal or overlap, empty supports, and tuples ordered differently from IDs.

The maintained suite contains 38 tests. The initial unified integration passed
all 30 pre-lift regression tests on 2026-10-05 after a
Release build with `cmake --build build -j2` and
`ctest --test-dir build -L regression --output-on-failure --progress -j2`.

On machines with many cores and limited memory, lower `JOBS` before building
or running CTest; generated C++ compilation can consume substantial memory.

The integration was also checked against the companion artifact inputs:
FMCAD full P1 produced 10 matching output probabilities with plain and rewrite
execution. CAV incremental P13 with `delta/inc0p5_1.txt` produced 627 matching
output probabilities under online `full`, `inc-naive`, and `inc-regional`,
within absolute tolerance `1e-8`; its logs contained no rewrite stage. These
are correctness smoke checks, not a reproduction of paper timings. The input
revisions are described in [PROVENANCE.md](PROVENANCE.md#paper-and-benchmark-records).

The bundled [evaluation workspace](../evaluation/README.md) was checked using
the incremental P13 smoke profile (all six modes and all ten probability
comparisons), its thirteen table/figure collectors, and the full P1 RQ2/RQ3
PSouffle runs. Plain/rewrite P1 probabilities agree within `1e-8`. The full
RQ1 runner also completed all five taint stages for app-018 and the readelf
symbolization case, and both graph-size collectors produced reduction rates.
Python CLI entry points and shell syntax were checked, and the VProbLog
bridge/native conversion was exercised on P1. These checks cover runner
integration; full paper experiments and external-engine timings are separate
evaluation runs.

The full P1/P3 engine smoke run completed both PSouffle cases. The locally
installed ProbLog completed P3; P1 reached the configured 300-second timeout.
VProbLog and Scallop timing runs were not performed in these integration checks.

The incremental suite compares
incremental modes against `full` on small programs covering:

- DRed delete/rederive behavior.
- deterministic-relation analysis under mixed updates.
- `inc-regional` correctness and multi-turn fallback.
- boundary calibration profile details for `inc-regional` single-interface updates.
- shared-delta join closure for `inc-regional` regional analysis.
- independent regional boundary updates that skip unnecessary overlap closure.
- deterministic-chain boundary anchors for `inc-regional`.
- rejection of deterministic-chain anchors whose path crosses the current region.
- recursive derivation guards.
- negated absent tuple grounding.
- `@post_delete_*` timestamp views for non-recursive mixed updates.
- canonical CLI mode switching, dumps, profiles, and output naming.
