# Side-Channel Benchmark

This benchmark contains fixed Souffle programs for side-channel information-flow
queries. Each case directory is self-contained:

```
side_channel/full/P19/
  compute.souffle.dl
  input/*.facts
  input/*.prob
```

The program computes leakage-related relations such as `KEY_SENSITIVE` and
`KEY_IND` from SSA-style assignment, sharing, and bit-vector helper relations.
The `.facts` files store relation tuples. The matching `.prob` files store one
probability per input tuple, in the same line order.

The checked-in cases are `P1` and `P3` through `P20` (19 cases).

## Runner

Use `FMCAD.py -sc`:

Run from `evaluation/full/`:

```bash
python3 FMCAD.py -sc --souffle-bin "$SOUFFLE_BIN" P19
```

The runner generates each case, runs the Souffle rewrite engine, and runs
ProbLog over `compute.problog.dl`. Output lands under
`artifact/FMCADSC/P<n>/` with a summary TSV at `artifact/FMCADSC/FMCADresult.tsv`.
