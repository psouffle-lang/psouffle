# Documentation Index

- [PROVENANCE.md](PROVENANCE.md): imported artifact revisions and branch audit.
- [USAGE.md](USAGE.md): full/online selectors and rewrite interaction contract.
- [COMMAND_LINE.md](COMMAND_LINE.md): complete compiler/runtime parameters,
  defaults, aliases, and option interactions.
- [USAGE.md#evidence](USAGE.md#evidence): observation syntax, conditional
  probabilities, impossible evidence, and literal restrictions.
- [../README.md#naive-lift-fastpath](../README.md#naive-lift-fastpath): opt-in
  exact pointwise lift, thresholds, fallbacks, and diagnostics.
- [../README.md#containers](../README.md#containers): compiler and evaluation
  containers, pinned dependencies, and output mounts.
- [USAGE.md#diagnostic-counts](USAGE.md#diagnostic-counts): pruning and rewrite
  counters in debugger JSON.
- [AND_INPUT_REDUNDANCY.md](AND_INPUT_REDUNDANCY.md): exact input elimination
  after pruning and before SISO, proof certificates and Symbolization benchmarks.
- [../evaluation/README.md](../evaluation/README.md): bundled paper benchmarks
  and experiment automation.
- [../evaluation/inc/README.md](../evaluation/inc/README.md): incremental
  experiment profiles and table/figure collection.
- [../evaluation/inc/benchmarks/side_channel/docs/INDEX.md](../evaluation/inc/benchmarks/side_channel/docs/INDEX.md): incremental benchmark documentation index.
- [../evaluation/full/README.md](../evaluation/full/README.md): full/rewrite
  experiments, competitor inputs, and conversion/analysis scripts.
- Full benchmark descriptions: [side-channel](../evaluation/full/side_channel/README.md),
  [taint](../evaluation/full/taint/README.md), and
  [symbolization](../evaluation/full/symbolization/README.md).

Read these documents in order when evaluating or modifying the unified
probabilistic Souffle compiler/runtime.

1. [../README.md](../README.md): branch scope, build, and local run.
2. [USAGE.md](USAGE.md): compiler/runtime flags and interactive commands.
3. [ARCHITECTURE.md](ARCHITECTURE.md): end-to-end control flow.
4. [topics/pipeline/README.dred.md](topics/pipeline/README.dred.md): DRed and graph delta flow.
5. [topics/pipeline/README.inc.region.md](topics/pipeline/README.inc.region.md): regional incremental forward compilation.
6. [TESTING.md](TESTING.md): regression command set.
7. [topics/testing/README.regression.md](topics/testing/README.regression.md): maintained case list.
8. [SECURITY.md](SECURITY.md): dependency and local-output hygiene.
9. [RUNBOOK.md](RUNBOOK.md): compact local run guide.
10. [../AGENTS.md](../AGENTS.md): agent constraints.

## Examples

- [LANGUAGE_EXAMPLES.md](LANGUAGE_EXAMPLES.md): full/rewrite language examples
  from the full artifact, including symbols, negation, and aggregate replay.
- [../examples/cycle_repair_path/README.md](../examples/cycle_repair_path/README.md): small
  interactive path program for observing recursive delete/rederive cycle repair
  with `--dump=dot`.

## Source Entry Points

- [../src/problog/FullPipeline.cpp](../src/problog/FullPipeline.cpp): standalone
  full inference and rewrite dispatcher.
- [../src/MainDriver.cpp:636](../src/MainDriver.cpp#L636): compiler-facing incremental options.
- [../src/synthesiser/Synthesiser.cpp:673](../src/synthesiser/Synthesiser.cpp#L673): generated pipeline call.
- [../src/problog/Pipeline.cpp:923](../src/problog/Pipeline.cpp#L923): baseline graph and runtime pipeline.
- [../src/include/souffle/cli/Executor.h:234](../src/include/souffle/cli/Executor.h#L234): incremental commit path.
- [../src/include/souffle/problog/ForwardCompilation.h:517](../src/include/souffle/problog/ForwardCompilation.h#L517): naive incremental forward compilation.
- [../src/include/souffle/problog/ForwardCompilation.h:1967](../src/include/souffle/problog/ForwardCompilation.h#L1967): regional forward compilation.
- [../tests/regression/CMakeLists.txt:22](../tests/regression/CMakeLists.txt#L22): regression suite.
