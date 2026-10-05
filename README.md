# PSouffle

This is the shared PSouffle codebase for the **CAV 2026** and **FMCAD 2026**
works on exact probabilistic Datalog inference for program analysis.

PSouffle is built on [Soufflé](https://souffle-lang.github.io/), the Datalog
compiler and runtime. It extends Soufflé's frontend, AST-to-RAM translation,
generated C++ evaluation, and relation infrastructure with derivation-graph
recording, exact BDD-based probabilistic inference, graph rewrite, and
incremental probability updates. Upstream and third-party license notices are
retained under [licenses/](licenses/).

| Work | Inference approach | Supported execution |
| --- | --- | --- |
| CAV 2026 | Incremental inference after fact insertions and deletions | `inc-naive`, `inc-regional`, and an online `full` recomputation oracle |
| FMCAD 2026 | Full inference with derivation-graph rewrite | Plain full inference, automatic rewrite dispatch, explicit graph rewrite, and implicit split rewrite |

Both approaches share one compiler/runtime. The FMCAD 2026 paper is
*PSOUFFLE: Towards Scalable Probabilistic Logic Inference for Program Analysis*.
The imported branches, tags, and paper/artifact correspondence are recorded in
[docs/PROVENANCE.md](docs/PROVENANCE.md). Benchmark inputs and paper experiment
drivers live in the companion
[problog-benchmark](https://github.com/Hughshine/problog-benchmark) repository.
Lifted inference is excluded.

The compiler generates both execution paths by default. A generated binary
runs standalone full inference and exits by default. `--setmode` or `--inc-only`
starts an incremental session. Rewrite is supported only in standalone full
execution: online initialization and online `full` recomputation always use
the original graph.

Compiler `--full-only` omits incremental maintenance code. Compiler
`--inc-only` generates an online program with a plain full baseline and full
recomputation oracle. These selectors are mutually exclusive. The
[command-line reference](#command-line-reference) below covers compiler and
runtime parameters; [docs/USAGE.md](docs/USAGE.md) describes file formats and
the online CLI.

## Start With Examples

Before reading the implementation notes, start with the small programs under
`examples/`:

- [examples/cycle_repair_path/README.md](examples/cycle_repair_path/README.md):
  interactive recursive delete/rederive cycle repair with DOT output.
- [examples/inc_bdd_region/README.md](examples/inc_bdd_region/README.md):
  regional forward-compilation scope and mergeable-anchor diagnostics.
- [examples/inc_bdd_calibration/README.md](examples/inc_bdd_calibration/README.md):
  boundary calibration for reused downstream BDDs.

## Source References

- [src/MainDriver.cpp:636](src/MainDriver.cpp#L636): compiler mode/output options.
- [src/MainDriver.cpp:719](src/MainDriver.cpp#L719): compiler default canonicalization.
- [src/synthesiser/Synthesiser.cpp](src/synthesiser/Synthesiser.cpp): generated runtime selects standalone full or online execution.
- [src/include/souffle/CompiledOptions.h:187](src/include/souffle/CompiledOptions.h#L187): supported mode syntax.
- [src/include/souffle/CompiledOptions.h:709](src/include/souffle/CompiledOptions.h#L709): generated runtime option parser.
- [src/include/souffle/cli/Cli.h:679](src/include/souffle/cli/Cli.h#L679): interactive update commands.
- [tests/regression/CMakeLists.txt:22](tests/regression/CMakeLists.txt#L22): maintained regression cases.

## Build

The build requires the system C++ toolchain, CMake, Flex/Bison, Python 3,
Readline headers/libraries, and CUDD. CMake now fails during configure if
Readline or CUDD is missing; set `CUDD_ROOT`, `CUDD_INCLUDE_DIR`, or
`CUDD_LIBRARY` for nonstandard CUDD installs.

```bash
JOBS=$(nproc || sysctl -n hw.ncpu || echo 2)
cmake -S . -B build
cmake --build build -j${JOBS}
```

Lower `JOBS` on machines with limited memory; `JOBS=2` was used for the local
integration checks.

## Command-Line Reference

Compiler arguments are passed to `build/src/souffle`; runtime arguments are
passed to the executable it generates. Options such as `-F`, `-D`, `--setmode`,
and `--rewrite` at compilation set defaults for that executable. Compiler
`--full-only` and `--inc-only` also determine which execution paths are generated.

### Compiler Parameters

```bash
./build/src/souffle [COMPILER_OPTIONS] program.dl -o compute
./build/src/souffle --help
```

Build, input/output, and inherited Souffle compiler options:

| Parameter | Meaning and default |
| --- | --- |
| `-o FILE`, `--dl-program=FILE` | Generate C++, compile an executable, and leave execution to the user. Without a generation/compilation option, this branch defaults to `-o` with the input program's base name. |
| `-g FILE`, `--generate=FILE` | Write generated C++ without compiling it; `FILE=-` writes to stdout. |
| `-G DIR`, `--generate-many=DIR` | Write generated C++ as multiple files. |
| `-c`, `--compile` | Generate, compile, and run the executable. |
| `-C`, `--compile-many` | Generate multiple C++ files, compile, and run the executable. |
| `-N NS`, `--generate-namespace=NS` | Set the generated C++ namespace. |
| `-F DIR`, `--fact-dir=DIR`, `--input-dir=DIR` | Default runtime input directory; default `.`. Inputs use `R.facts` and optional line-aligned `R.prob` files. |
| `-D DIR`, `--output-dir=DIR` | Default runtime output directory; default `.`. Use an existing directory for probability outputs. |
| `-I DIR`, `--include-dir=DIR` | Preprocessor include directory; repeat for multiple directories; default `.`. |
| `-j N`, `--jobs=N` | Compiler thread count and generated runtime default; default `1`, or `auto` for automatic selection. |
| `-p FILE`, `--profile=FILE` | Compile in the Souffle profiler and set its default output filename. Disabled by default. |
| `--profile-frequency` | Include frequency counters in the Souffle profiler. |
| `-l FILE`, `--libraries=FILE` | Add a library for generated programs; may be repeated. |
| `-L DIR`, `--library-dir=DIR` | Add a library search directory; may be repeated. |
| `-M MACROS`, `--macro=MACROS` | Set preprocessor macro definitions. |
| `-P OPTIONS`, `--pragma=OPTIONS` | Set pragma options, using `key:value`; may be repeated. |
| `--preprocessor=CMD`, `--no-preprocessor` | Select the C preprocessor or disable preprocessing. |
| `-m RELATIONS`, `--magic-transform=RELATIONS` | Enable magic-set transformation for selected relations; use `'*'` for all. |
| `--magic-transform-exclude=RELATIONS` | Exclude relations from magic-set transformation and inlining. |
| `--inline-exclude=RELATIONS` | Prevent selected relations from being inlined. |
| `-z TRANSFORMERS`, `--disable-transformers=TRANSFORMERS` | Disable selected AST transformers. |
| `--no-souffle-opt` | Disable RAM optimizations such as join ordering and guard hoisting. |
| `--emit-statistics` | Collect RAM join-size statistics. |
| `-r FILE`, `--debug-report=FILE` | Write the compiler's HTML debug report. |
| `--show=MODE` | Inspect compiler representations: `initial-ast`, `transformed-ast`, `initial-ram`, `transformed-ram`, `type-analysis`, `precedence-graph`, `precedence-graph-text`, `scc-graph`, `scc-graph-text`, or `parse-errors`. May be repeated. |
| `--parse-errors` | Report parser errors and exit. |
| `-w`, `--no-warn`; `-W WARN`, `--warn=WARN`; `--wno=WARN` | Suppress all warnings, enable selected warnings, or suppress selected warnings. |
| `--legacy` | Enable inherited Souffle legacy support. |
| `--version` | Print compiler version/build information. |
| `-h`, `--help` | Print the compiler parameter list. |

Execution selection and generated runtime defaults:

| Parameter | Meaning and default |
| --- | --- |
| `-x`, `--full-only` | Generate standalone full inference only; omit incremental maintenance. |
| `--inc-only` | Generate online execution, including plain full initialization and the full recomputation oracle; default to online execution. |
| `-O`, `--online` | Generate both paths and default to online execution. |
| `--setmode=MODE` | Set the online mode and default the binary to online execution. The online mode defaults to `inc-naive`; accepted modes are described below. |
| `--rewrite` | Enable automatic rewrite dispatch for standalone full execution by default. Off by default. |
| `--explicit-rewrite`, `--implicit-rewrite` | Enable rewrite and force the corresponding implementation by default. |
| `-d`, `--derv-only` | Default to standalone derivation-graph construction without probability inference. Off by default. |
| `--det-opt` | Compatibility flag for deterministic-relation analysis, which is already enabled. |
| `--dump=LIST` | Bake a default set of graph/statistic dumps; none enabled by default. |
| `--profile-stage=LIST` | Bake default pipeline diagnostic stages; none enabled by default. These diagnostics can be used without `-p`. |
| `--log-file=FILE` | Set the generated runtime's debugger log base name; default `log.txt`. |
| `-v`, `--verbose` | Enable compiler diagnostics and default runtime informational diagnostics to on. Off by default. |
| `--inc-reorder-*` | Bake the incremental BDD reordering defaults listed below. |

### Runtime Parameters

```bash
./compute [RUNTIME_OPTIONS]
./compute --help
```

Runtime value options override their compiled defaults. Boolean enable flags
add to the compiled settings: for example, runtime `--dump` enables additional
dump kinds. There is no `--no-rewrite` flag to undo a baked rewrite default;
`--derv-only=false` explicitly disables a baked graph-only default.

| Parameter | Meaning and default |
| --- | --- |
| `-F DIR`, `--facts=DIR`, `--input-dir=DIR` | Override the input directory baked by compiler `-F`. |
| `-D DIR`, `--output=DIR`, `--output-dir=DIR` | Override the output directory baked by compiler `-D`; create the directory before running. |
| `--full-only` | Select standalone full inference once, then exit, if that path was generated. This is the default for a compiler invocation without execution selectors. |
| `--inc-only`, `--online` | Start an online session with plain full initialization, if online execution was generated. |
| `-m MODE`, `--setmode=MODE` | Start an online session and select its commit mode; default online mode `inc-naive`, unless changed at compilation. Even `--setmode=full` selects an online session. |
| `-r`, `--rewrite` | Enable automatic rewrite dispatch for standalone full inference. |
| `--explicit-rewrite`, `--implicit-rewrite` | Enable rewrite and force explicit graph rewrite or implicit split rewrite. |
| `-d`, `--derv-only[=true\|false]` | Build the standalone full graph without probability inference. A bare flag means `true`; use `--derv-only=false` to enable inference. |
| `-e`, `--merge-bi-imp` | Enable the full artifact's deterministic bi-implication merging; off by default, standalone full only. |
| `--prune-extra` | Enable extra pruning of outputless graph components; off by default, standalone full only. |
| `-Z`, `--det-opt` | Compatibility flag; deterministic-relation analysis is already enabled. |
| `--dump=LIST` | Enable selected graph/statistic outputs; off by default unless baked at compilation. |
| `--profile-stage=LIST` | Enable selected pipeline diagnostics; off by default unless baked at compilation. |
| `-l FILE`, `--logfile=FILE`, `--log-file=FILE` | Override the debugger JSON log base name. Logs are written in the runtime output directory with a timestamped filename. |
| `-v`, `--verbose` | Enable informational runtime diagnostics. |
| `-j N`, `--jobs=N` | Override the baked runtime thread count; supports positive integers or `auto` when built with OpenMP. |
| `-p FILE`, `--profile=FILE` | Override the Souffle profiler output file; requires compilation with compiler `-p`. |
| `-h`, `--help` | Print generated-program help. |
| `--inc-reorder-*` | Override incremental BDD reordering settings; see below. |

The two parsers have different short-option meanings: compiler `-r` writes an
HTML debug report, while runtime `-r` enables rewrite; compiler `-m` selects
magic-set transformation, while runtime `-m` selects an online mode; compiler
`-l` adds a library, while runtime `-l` selects a log filename.

Old full-artifact aliases remain supported by the runtime:

| Runtime alias | Canonical equivalent |
| --- | --- |
| `-J`, `--dumpjson` | `--dump=json` |
| `-T`, `--dumpdot` | `--dump=dot` |
| `-S`, `--dumpstat` | `--dump=stat` |
| `--fc-profile` | `--profile-stage=fc` |
| `--profile-wmc` | `--profile-stage=wmc` |
| `--profile-dep-graph` | `--profile-stage=dep-graph` |

### Mode, Dump, And Profile Values

These values apply to both compiler defaults and runtime overrides:

| `--setmode` value | Datalog tuple evaluation (`sem`) | Formula compilation and weighted model counting (`fc`) |
| --- | --- | --- |
| `inc-naive` | Incremental | Naive incremental |
| `inc-regional` | Incremental | Regional incremental |
| `full` | Full recomputation | Full recomputation |

For ablations, use a split specification such as
`--setmode=sem=inc,fc=full` or `--setmode=sem=full,fc=inc-regional`.
`sem` accepts `inc` or `full`; `fc` accepts `full`, `inc-naive`, or
`inc-regional`. The parser also accepts `semantic` for `sem` and `bdd` for `fc`.
A split specification that sets only one component preserves the other
component's current default. Online `setmode` commands can change the mode
between commits; see [docs/USAGE.md](docs/USAGE.md#online-cli).

`--dump` and `--profile-stage` accept comma-separated lists, such as
`--dump=dot,json,stat` and `--profile-stage=fc,wmc`.

| Dump kind | Output |
| --- | --- |
| `json` | Derivation-graph JSON after pruning. |
| `dot` | Graphviz DOT graphs. |
| `stat` | Graph counters and additional diagnostic/statistic outputs. |
| `json-before-graph` | Online startup rule applications before graph materialization. |
| `json-before-prune` | Online graphs before pruning. |

Profile stages are `dred` (delete/rederive), `inc` (incremental processing),
`fc` (formula compilation), `wmc` (weighted model counting), `inc-delete`
(incremental deletion), `inc-regional` (regional scope/calibration), and
`dep-graph` (dependency-graph analysis). Diagnostics are emitted by the stages
that execute in the selected pipeline.

### Incremental BDD Reordering Parameters

All of these options are accepted at compilation and runtime. They control
incremental formula updates; they do not enable graph rewrite.

| Parameter | Meaning and default |
| --- | --- |
| `--inc-reorder-policy=POLICY` | Reordering policy; default `pressure`. Accepted values: `default`, `off`, `pressure`, `auto`, `explicit`, `both`. |
| `--inc-reorder-work-threshold=N` | Work/pressure gate for explicit reordering; default `2500`, also bounded by an adaptive graph-work threshold. `0` disables explicit reordering in `pressure` mode. |
| `--inc-reorder-auto-gap=N` | Set the next automatic reorder threshold to the current trigger count plus `N`; default `0` leaves this configuration unchanged. Used by `auto`/`both`. |
| `--inc-reorder-count-dead` | Count dead CUDD nodes in automatic reorder triggers; off by default. Used with a positive auto gap. |
| `--inc-reorder-allow-large` | Allow configured automatic reordering on managers with at least 3 million live nodes; off by default. Used with a positive auto gap. |

`default` retains the legacy CUDD adaptive policy. `off` disables automatic
and explicit incremental reordering. `pressure` disables automatic reordering
and accumulates weighted update work across turns until the explicit gate is
reached. `auto` configures automatic reordering; `explicit` uses the explicit
work gate on each turn; `both` enables both mechanisms. These controls preserve
inference semantics and are useful for profiling and artifact ablations.

### Parameter Interactions

| Combination or selection | Behavior |
| --- | --- |
| No compiler execution selector | Generate both paths; default binary execution is standalone full. |
| Compiler `--full-only` | Generate full only; a runtime online request fails. |
| Compiler `--inc-only` | Generate online only, retaining its full baseline/oracle; runtime standalone `--full-only` fails. |
| Compiler `--online` or explicit `--setmode`, without `--inc-only` | Generate both paths; default binary execution is online. |
| Compiler `--inc-only` plus `--online` or `--setmode` | Keep online-only capabilities and set the requested online default mode. |
| `--full-only` plus `--inc-only`, `--online`, or explicit `--setmode` | Error at either parser, independent of argument order. |
| Online execution plus any rewrite flag | Error. Online initialization and online full recomputation never rewrite. This also applies to baked rewrite defaults. |
| `--explicit-rewrite` plus `--implicit-rewrite` | Error, independent of argument order. |
| `--rewrite` plus one forced rewrite flag | Use the forced implementation. |
| Online execution plus enabled `--derv-only`, `--merge-bi-imp`, or `--prune-extra` | Error; these controls require standalone full execution. Runtime `--derv-only=false` disables graph-only execution. The pruning flags are runtime-only. |
| Standalone `--derv-only` plus rewrite | Construct the graph and skip rewrite/probability inference. |

BDD, deterministic-relation analysis, and variable-index reuse are fixed
defaults. Lifted and approximate backends have no command-line selector in
this codebase.

## Rewrite Example: Full Inference On A Fixed Graph

Run the following commands from the repository root. This small path program
derives reachability from probabilistic edges:

```souffle
path(x,y) :- edge(x,y).
path(x,z) :- path(x,y), edge(y,z).
```

Compile the supplied fixture with standalone full inference only:

```bash
mkdir -p build/examples/rewrite/compile build/examples/rewrite/plain build/examples/rewrite/rewritten
./build/src/souffle --full-only \
  -F tests/regression/cases/smoke_exact_inference/input \
  -D build/examples/rewrite/compile \
  tests/regression/cases/smoke_exact_inference/compute.dl \
  -o build/examples/rewrite/compute
```

Compare plain full inference with the FMCAD rewrite path on the same inputs:

```bash
./build/examples/rewrite/compute \
  -D build/examples/rewrite/plain --logfile plain
./build/examples/rewrite/compute \
  -D build/examples/rewrite/rewritten --rewrite --logfile rewritten
```

Both `facts.prob` files should contain the same tuple probabilities, including
`path(1,4) : 0.6032` (the number of printed decimal places may vary).
`--rewrite` selects explicit graph rewrite for deterministic rules and implicit
split rewrite for probabilistic rules. Use `--explicit-rewrite` or
`--implicit-rewrite` to force either implementation. These flags apply to
standalone full inference.

## Incremental Example: Delete And Repair A Recursive Cycle

The supplied cycle-repair example starts with an edge entering a recursive
cycle. Its update script deletes that entry, inserts a new entry, then breaks
and restores the cycle. This exercises the CAV incremental update path.

Compile an online program, then process the script with regional inference:

```bash
mkdir -p build/examples/inc/regional build/examples/inc/naive build/examples/inc/full
./build/src/souffle --inc-only \
  -F examples/cycle_repair_path/input \
  -D build/examples/inc/regional \
  examples/cycle_repair_path/compute.dl \
  -o build/examples/inc/compute
./build/examples/inc/compute --setmode inc-regional \
  < examples/cycle_repair_path/updates.txt
```

Each `commit` applies the queued updates and writes a probability snapshot.
After the first deletion, only `path(0,0)` remains. After the next insertion,
the cycle and its downstream paths become reachable again. The commands can
also be entered interactively:

```text
delete edge(0,1)
commit
insert 0.85::edge(0,2)
commit
q
```

Compare the same script with naive incremental inference and full recomputation:

```bash
./build/examples/inc/compute -D build/examples/inc/naive --setmode inc-naive \
  < examples/cycle_repair_path/updates.txt
./build/examples/inc/compute -D build/examples/inc/full --setmode full \
  < examples/cycle_repair_path/updates.txt
```

The three modes should agree on the probabilities after each commit.
Online initialization and online `full` recomputation always use the original
graph. Combining online execution with a rewrite flag is an error.
Evidence and exact sum aggregate replay are supported in standalone full
execution; online execution rejects them.

For the program, expected per-turn outputs, and graph illustrations, see
[examples/cycle_repair_path/README.md](examples/cycle_repair_path/README.md).
Runtime results stay under the ignored `build/` directory.

## Optional Outputs

Extra graph/profiling outputs are off by default. Enable them explicitly when
collecting debugging material:

```bash
./build/examples/inc/compute --setmode inc-regional \
  --dump=dot,json,stat --profile-stage=inc,wmc,fc \
  < examples/cycle_repair_path/updates.txt
```

Full inference also accepts the canonical dump/profile selectors.

## Verification

```bash
ctest --test-dir build -L regression --output-on-failure --progress -j${JOBS}
cmake --build build --target check-regression
```

## Documentation

- [docs/INDEX.md](docs/INDEX.md): reading order.
- [docs/USAGE.md](docs/USAGE.md): compiler/runtime interface.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): control-flow map.
- [docs/TESTING.md](docs/TESTING.md): regression checks.
- [docs/LANGUAGE_EXAMPLES.md](docs/LANGUAGE_EXAMPLES.md): full/rewrite examples.
- [docs/SECURITY.md](docs/SECURITY.md): dependency and local-output hygiene.
