# Symbolization Benchmark

This benchmark contains a fixed DDisasm symbolization/data-object selection
program:

```
symbolization/symbolization.dl
symbolization/readelf/input/*.facts
symbolization/readelf/input/*.prob
```

The program selects data objects and symbolic pointer-sized data from extracted
binary facts. The benchmark-facing output relations are:

- `data_object(ea, size, type)`: selected data object at effective address
  `ea`, with byte size `size` and object kind `type`.
- `symbolic_data(ea, size, val)`: selected pointer-sized symbolic data at `ea`,
  with byte size `size` and refined value `val`.
- `labeled_ea(ea)`: address labeled by selected symbolic data or selected
  string data.

The input relations encode candidate addresses, data limits, possible symbolic
immediates, pointer size, preferred data accesses, and string candidates. The
`.facts` files store tuples, and the `.prob` files store tuple probabilities in
the same line order.

## Runner

```bash
cd evaluation/full
python3 FMCAD.py -dis --souffle-bin "$SOUFFLE_BIN" --dis-cases readelf
```

The runner compiles `symbolization.dl` once and runs each case under both
engines:

- `souffle_no_rewrite`: `--det-opt`
- `souffle_implicit_rewrite`: `--det-opt --rewrite`

It writes `DISresult.tsv` with runtime, status, and consistency columns.
