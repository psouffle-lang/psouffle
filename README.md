# PSouffle

PSouffle is an exact probabilistic Datalog compiler and runtime built on
[Soufflé](https://souffle-lang.github.io/). This is the shared codebase for
our **CAV 2026** and **FMCAD 2026** work:

- **CAV 2026:** incremental inference after fact insertions and deletions.
- **FMCAD 2026:** full inference with derivation-graph rewrite and exact BDD counting.

An optional naive lifted fastpath handles eligible pointwise outputs.
Paper benchmarks and experiment scripts are bundled under
[evaluation/](evaluation/README.md).

## Build

Requires a C++ toolchain, CMake, Flex/Bison, Python 3, Readline, and CUDD.
For a nonstandard CUDD installation, set `CUDD_ROOT` when configuring.

```bash
JOBS=$(nproc || sysctl -n hw.ncpu || echo 2)
cmake -S . -B build
cmake --build build -j${JOBS}
```

Use fewer jobs on machines with limited memory. Build and regression details
are in [docs/TESTING.md](docs/TESTING.md).

### Containers

```bash
docker build --target compiler -t psouffle:compiler .
docker build -t psouffle:evaluation .
```

The evaluation image also includes the comparison engines. See
[container usage](docs/USAGE.md#containers) for setup and output mounts.

## Full Inference and Rewrite

Compile the supplied probabilistic reachability example, then compare plain
inference with rewrite:

```bash
mkdir -p build/examples/full/plain build/examples/full/rewrite
./build/src/souffle --full-only \
  -F tests/regression/cases/smoke_exact_inference/input \
  tests/regression/cases/smoke_exact_inference/compute.dl \
  -o build/examples/full/compute
./build/examples/full/compute -D build/examples/full/plain
./build/examples/full/compute --rewrite -D build/examples/full/rewrite
```

Inputs use `R.facts` and optional line-aligned `R.prob` files; missing probability
files mean `1.0`. Each run writes `facts.prob` in its output directory.

## Incremental Inference

Run the cycle-repair example with an insertion/deletion script:

```bash
mkdir -p build/examples/inc/output
./build/src/souffle --inc-only \
  -F examples/cycle_repair_path/input \
  examples/cycle_repair_path/compute.dl -o build/examples/inc/compute
./build/examples/inc/compute --setmode inc-regional \
  -D build/examples/inc/output < examples/cycle_repair_path/updates.txt
```

Use `inc-naive` for naive incremental inference or `full` for the recomputation
oracle. Each `commit` writes a probability snapshot. Online initialization and
recomputation use the original graph; rewrite is available in standalone full
execution. See the [example walkthrough](examples/cycle_repair_path/README.md)
and [online commands](docs/USAGE.md#online-cli).

## Naive Lift Fastpath

For a program with eligible nonrecursive pointwise outputs:

```bash
./build/src/souffle --full-only program.dl -o compute
mkdir -p output
./compute --lifted-wmc --lifted-threshold=1 -F input -D output
```

Lift is opt-in and exact; unsupported outputs and evidence use ordinary
inference. See [eligibility and diagnostics](docs/USAGE.md#naive-lift-fastpath).

## Documentation

- [Command-line reference](docs/COMMAND_LINE.md): compiler and runtime parameters.
- [Usage](docs/USAGE.md): file formats, evidence, online commands, and diagnostics.
- [Architecture](docs/ARCHITECTURE.md): inference pipeline.
- [Evaluation](evaluation/README.md): CAV incremental and FMCAD full/rewrite experiments.
- [Documentation index](docs/INDEX.md): examples, testing, and source provenance.

Upstream and third-party license notices are retained under [licenses/](licenses/).

## Citation

If you use PSouffle or build on these methods in your research, please cite
the corresponding papers:

- **CAV 2026:** Xuyang Li, Weiyi Chen, Isil Dillig, and Jingbo Wang.
  [Incremental Inference for Probabilistic Datalog](https://doi.org/10.1007/978-3-032-32537-2_8).
- **FMCAD 2026:** Xuyang Li, Jiahao Xia, Ahmed Adnan, and Jingbo Wang.
  [PSoufflé: Scaling Exact Probabilistic Logic Inference for Program Analysis](https://doi.org/10.34727/2026/isbn.978-3-85448-093-8_42).
