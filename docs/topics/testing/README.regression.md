# Incremental Regression Suite

## Source References

- [../../../tests/regression/CMakeLists.txt:22](../../../tests/regression/CMakeLists.txt#L22): registered regression cases.
- [../../../tests/regression/CMakeLists.txt:37](../../../tests/regression/CMakeLists.txt#L37): `check-regression` target.
- [../../../tests/regression/run_regression_case.py:153](../../../tests/regression/run_regression_case.py#L153): compile helper.
- [../../../tests/regression/run_regression_case.py:385](../../../tests/regression/run_regression_case.py#L385): deterministic mixed-update case.
- [../../../tests/regression/run_regression_case.py:560](../../../tests/regression/run_regression_case.py#L560): independent regional overlap-skip case.
- [../../../tests/regression/run_regression_case.py:645](../../../tests/regression/run_regression_case.py#L645): deterministic-chain regional anchor case.
- [../../../tests/regression/run_regression_case.py:695](../../../tests/regression/run_regression_case.py#L695): deterministic-chain anchor rejection case.
- [../../../tests/regression/run_regression_case.py:738](../../../tests/regression/run_regression_case.py#L738): regional multi-turn state-machine case.
- [../../../tests/regression/run_regression_case.py:1218](../../../tests/regression/run_regression_case.py#L1218): case dispatch table.

## Case Groups

- `regression.dred_mix` and `regression.dred_hub`: DRed delete/rederive behavior against `full`.
- `regression.deterministic_combo`: deterministic-relation analysis across mixed insert/delete turns.
- `regression.deterministic_regional`: single-turn `inc-regional` against `full`.
- `regression.inc_regional_calibration`: single-interface regional update with boundary calibration.
- `regression.inc_regional_shared_delta_join`: shared-delta join closure for regional analysis.
- `regression.inc_regional_independent_overlap_skip`: independent regional boundaries that skip overlap closure.
- `regression.inc_regional_deterministic_chain_anchor`: regional boundary anchor found through a deterministic derivation chain.
- `regression.inc_regional_deterministic_join_anchor_reject`: deterministic-chain anchor rejected when a join dependency is already in the region.
- `regression.deterministic_regional_multiturn`: regional state-machine fallback and re-entry.
- `regression.deterministic_regional_degenerate`: regional turns that should remain regional.
- `regression.deterministic_derivation_guard`: recursive delete/rederive guard.
- `regression.negated_absent_tuple`: negated absent tuple grounding.
- `regression.nonrecursive_timestamp_views`: `@post_delete_*` timestamp views.
- `regression.canonical_cli`: canonical mode/output CLI surface.

## Run

```bash
ctest --test-dir build -L regression --output-on-failure --progress -j${JOBS}
cmake --build build --target check-regression
```

Each case compiles a generated runtime using the repo-built `souffle` target,
executes scripted CLI turns, and compares incremental outputs against
`full` probability files.
