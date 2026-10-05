# Regression Cases

Each maintained case is incremental-runtime focused and runs through
`tests/regression/run_regression_case.py`.

Layout:

- static case: `compute.dl` plus `input/*.facts` and optional `input/*.prob`
- dynamic case: `generate.py` writes `compute.dl` and `input/*` into the
  workspace passed via `--out-dir`
- variant-group case: one registered case directory containing named subcases,
  each with its own static layout

Do not add unrelated fixtures to this branch's regression suite.
