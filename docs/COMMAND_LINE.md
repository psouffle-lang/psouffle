# Command-Line Reference

Compiler arguments are passed to `build/src/souffle`; runtime arguments are
passed to the executable it generates. Options such as `-F`, `-D`, `--setmode`,
and `--rewrite` at compilation set defaults for that executable. Compiler
`--full-only` and `--inc-only` also determine which execution paths are generated.

## Compiler Parameters

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
| `--lifted-wmc` | Enable the exact pointwise lifted fastpath for standalone full inference. Off by default. |
| `--lifted-threshold=N` | Minimum output relation cardinality to attempt lift; default `1024`. Nonnegative integer. |
| `-d`, `--derv-only` | Default to standalone derivation-graph construction without probability inference. Off by default. |
| `--det-opt` | Compatibility flag for deterministic-relation analysis, which is already enabled. |
| `--dump=LIST` | Bake a default set of graph/statistic dumps; none enabled by default. |
| `--and-input-redundancy` | Enable exact AND-input elimination after pruning and before SISO/graph fastpaths. Off by default; standalone full only. |
| `--profile-stage=LIST` | Bake default pipeline diagnostic stages; none enabled by default. These diagnostics can be used without `-p`. |
| `--log-file=FILE` | Set the generated runtime's debugger log base name; default `log.txt`. |
| `-v`, `--verbose` | Enable compiler diagnostics and default runtime informational diagnostics to on. Off by default. |
| `--inc-reorder-*` | Bake the incremental BDD reordering defaults listed below. |

## Runtime Parameters

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
| `--lifted-wmc` | Attempt exact pointwise lift before constructing the full graph; off by default. Requires standalone full execution. |
| `--lifted-threshold=N` | Override the minimum output relation cardinality for lift; default `1024`. |
| `-d`, `--derv-only[=true\|false]` | Build the standalone full graph without probability inference. A bare flag means `true`; use `--derv-only=false` to enable inference. |
| `-e`, `--merge-bi-imp` | Enable the full artifact's deterministic bi-implication merging; off by default, standalone full only. |
| `--prune-extra` | Enable extra pruning of outputless graph components; off by default, standalone full only. |
| `-Z`, `--det-opt` | Compatibility flag; deterministic-relation analysis is already enabled. |
| `--dump=LIST` | Enable selected graph/statistic outputs; off by default unless baked at compilation. |
| `--and-input-redundancy` | Delete individually proven redundant AND inputs after pruning, re-prune query/evidence roots, then run ordinary SISO/graph fastpaths. Independent of `--rewrite`; off by default. |
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

## Mode, Dump, And Profile Values

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
between commits; see [docs/USAGE.md](USAGE.md#online-cli).

`--dump` and `--profile-stage` accept comma-separated lists, such as
`--dump=dot,json,stat` and `--profile-stage=fc,wmc`.

`--dump=and-redundancy` enables the read-only AND-input opportunity audit before
and after rewrite in standalone full execution. It is rejected for online
execution. See [certificates and audit commands](AND_INPUT_REDUNDANCY.md).

| Dump kind | Output |
| --- | --- |
| `json` | Derivation-graph JSON after pruning. |
| `dot` | Graphviz DOT graphs. |
| `stat` | Graph counters and additional diagnostic/statistic outputs. |
| `and-redundancy` | Read-only standalone full AND-input proof reports before/after actual rewrite. |
| `json-before-graph` | Online startup rule applications before graph materialization. |
| `json-before-prune` | Online graphs before pruning. |

Profile stages are `dred` (delete/rederive), `inc` (incremental processing),
`fc` (formula compilation), `wmc` (weighted model counting), `inc-delete`
(incremental deletion), `inc-regional` (regional scope/calibration), and
`dep-graph` (dependency-graph analysis). Diagnostics are emitted by the stages
that execute in the selected pipeline.

## Incremental BDD Reordering Parameters

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

## Parameter Interactions

| Combination or selection | Behavior |
| --- | --- |
| No compiler execution selector | Generate both paths; default binary execution is standalone full. |
| Compiler `--full-only` | Generate full only; a runtime online request fails. |
| Compiler `--inc-only` | Generate online only, retaining its full baseline/oracle; runtime standalone `--full-only` fails. |
| Compiler `--online` or explicit `--setmode`, without `--inc-only` | Generate both paths; default binary execution is online. |
| Compiler `--inc-only` plus `--online` or `--setmode` | Keep online-only capabilities and set the requested online default mode. |
| `--full-only` plus `--inc-only`, `--online`, or explicit `--setmode` | Error at either parser, independent of argument order. |
| Online execution plus any rewrite flag | Error. Online initialization and online full recomputation never rewrite. This also applies to baked rewrite defaults. |
| Online execution plus `--lifted-wmc` | Error, including a baked lifted default. |
| `--explicit-rewrite` plus `--implicit-rewrite` | Error, independent of argument order. |
| `--rewrite` plus one forced rewrite flag | Use the forced implementation. |
| Online execution plus enabled `--derv-only`, `--merge-bi-imp`, or `--prune-extra` | Error; these controls require standalone full execution. Runtime `--derv-only=false` disables graph-only execution. The pruning flags are runtime-only. |
| Standalone `--derv-only` plus rewrite | Construct the graph and skip rewrite/probability inference. |

BDD, deterministic-relation analysis, and variable-index reuse are fixed
defaults. Approximate backends have no command-line selector in this codebase.
