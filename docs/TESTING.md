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

The unified integration passed all 30 regression tests on 2026-10-05 after a
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
