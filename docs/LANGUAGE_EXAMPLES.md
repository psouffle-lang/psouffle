# Language Examples

This page introduces the small probabilistic Datalog fragment used by this
artifact. The intended reader is an evaluator who wants to inspect a complete
program before running the larger `problog-benchmark` cases.

The examples below are also regression tests. CTest compiles each program,
runs exact inference with and without `--rewrite`, and checks that both runs
produce the same output tuple keys and probabilities within `1e-8`.

## Files

Each example has the same layout:

```text
tests/regression/cases/<case>/
  compute.dl
  input/<relation>.facts
  input/<relation>.prob
```

Facts are tab-separated tuples. A matching `.prob` file gives one probability
per fact line. If a relation has no `.prob` file, the runtime treats its facts
as deterministic.

The examples use `_` in `query(...)`. `_` is an unnamed wildcard variable. A
query such as `query(explained(_)).` asks for every tuple in the one-column
relation `explained`. A two-column relation uses one wildcard per column, for
example `query(class_total_bytes(_, _)).`.

The output file `facts.prob` should be read as a list of probabilistic answers.
For example, a line such as

```text
class_total_bytes("widget",15) : 0.435666
```

means: the derived fact `class_total_bytes("widget",15)` is true with
probability `0.435666`. The relation name tells the predicate being answered.
The tuple fields give the values in that answer. The number after `:` is the
probability computed by exact inference.

## Mini Side-Channel Case

Source: [../tests/regression/cases/language_side_channel_mini/compute.dl](../tests/regression/cases/language_side_channel_mini/compute.dl)

This program answers the question: which observable events are explained by a
possible hidden value?

The toy setting has two possible hidden-value hypotheses: `admin` and `guest`.
They are represented as probabilistic facts. The deterministic table `mapping`
says what observation each hidden value would produce. `admin` maps to
`cache-hit`; `guest` maps to `cache-miss`. A second probabilistic relation,
`monitor`, says whether each observation is visible to the observer.

The input probabilities are:

```text
secret("admin")      = 0.72
secret("guest")      = 0.18
monitor("cache-hit") = 0.91
monitor("cache-miss")= 0.85
```

The first rule computes which observation each possible secret would produce.
The second rule says that an observation is explained only when the secret
would produce it and the monitor sees it.

```souffle
leak(Observation) :- secret(Value), mapping(Value, Observation).
explained(Observation) :- leak(Observation), monitor(Observation).
query(explained(_)).
```

The query asks for the whole `explained` relation, not one selected tuple. The
runtime therefore reports every observation that can be explained:

```text
explained("cache-hit")  = 0.72 * 0.91 = 0.6552
explained("cache-miss") = 0.18 * 0.85 = 0.153
```

The first output tuple means: "`cache-hit` is explained by some possible hidden
value and is visible to the observer." Its probability is the probability that
`secret("admin")` and `monitor("cache-hit")` are both true. The second tuple has
the same meaning for `cache-miss` and `guest`.

This shape resembles the side-channel benchmark at a small scale:
probabilistic inputs feed deterministic propagation rules, then the solver
computes probabilities for all requested output tuples.

## Mini Taint Case

Source: [../tests/regression/cases/language_taint_mini/compute.dl](../tests/regression/cases/language_taint_mini/compute.dl)

This program answers the question: which sink values may raise a taint alarm?

The toy setting has values such as `user`, `cache`, `log`, and `network`.
`source(X)` means value `X` may start tainted. `flow(X,Y)` means taint may flow
from `X` to `Y`. `sanitizer(Y)` means taint cannot propagate into `Y`.
`sink(Y)` marks a value that should be reported if it becomes tainted.

The relevant input probabilities are:

```text
source("user")        = 0.55
source("config")      = 0.21
flow("user","cache")  = 0.66
flow("cache","network") = 0.74
flow("config","log")  = 0.59
flow("log","network") = 0.64
```

`parser` is listed in `sanitizer.facts`, so the path
`user -> parser -> network` is blocked even though the corresponding flow facts
exist. The first rule marks probabilistic sources as tainted. The recursive
rule computes transitive taint flow through the remaining graph. The
probabilistic rule says that a tainted sink raises an alarm with probability
`0.80`.

```souffle
tainted(Value) :- source(Value).
tainted(Y) :- tainted(X), flow(X,Y), !sanitizer(Y).
0.80::alarm(Y) :- tainted(Y), sink(Y).
query(alarm(_)).
```

The query asks for the whole `alarm` relation. The deterministic relation
`sanitizer` blocks the `parser` node, so the path `user -> parser -> network`
does not contribute. The expected output has two tuples. `cache` has one
derivation path. `network` has two independent derivation paths: one through
`cache` and one through `log`.

```text
alarm("cache")   = 0.80 * (0.55 * 0.66) = 0.2904
alarm("network") = 0.80 * (1 - (1 - 0.55*0.66*0.74)
                              * (1 - 0.21*0.59*0.64))
                 = 0.26129241
```

The first tuple means: "`cache` is a sink and the alarm for `cache` fires." The
probability multiplies the chance that `user` starts tainted, the chance that
taint flows from `user` to `cache`, and the probabilistic alarm rule. The second
tuple means the same thing for `network`; its probability first combines two
ways for `network` to become tainted, then multiplies by the alarm-rule
probability.

The example covers recursion, deterministic negation, probabilistic facts, and
a probabilistic rule. This shape exercises the rewrite path used for workloads
whose rules have probabilities.

## Mini Symbolization Case

Source: [../tests/regression/cases/language_symbolization_mini/compute.dl](../tests/regression/cases/language_symbolization_mini/compute.dl)

This program answers the question: what total byte size is derived for each
selected object class?

The toy setting has objects such as `obj-a`, `obj-b`, and `obj-c`. Each object
has a class, such as `widget` or `other`. Each object also has a byte size.
This shape mirrors the symbolization benchmark style: input facts describe
program objects, object classes, and numeric attributes.

The input facts say that `obj-a`, `obj-b`, and `obj-c` are widgets with byte
sizes `4`, `6`, and `5`. `obj-noise` belongs to class `other` and has byte size
`10`. The `object_size` facts are probabilistic:

```text
object_size("obj-a",4)      = 0.82
object_size("obj-b",6)      = 0.77
object_size("obj-c",5)      = 0.69
object_size("obj-noise",10) = 0.51
```

`object_kind` facts are deterministic. They assign each object to a class:

```text
object_kind("obj-a","widget")
object_kind("obj-b","widget")
object_kind("obj-c","widget")
object_kind("obj-noise","other")
```

The first rule joins the object-class table with the object-size table. It
turns object-level size facts into class-level size facts, such as
`class_object_size("widget",4)`. The second rule derives the total byte size
for each selected class.

```souffle
selected("widget").
selected("other").

class_object_size(Kind, Bytes) :-
    object_size(Obj, Bytes),
    object_kind(Obj, Kind).

class_total_bytes(Kind, TotalBytes) :-
    selected(Kind),
    TotalBytes = sum Bytes : { class_object_size(Kind, Bytes) }.

query(class_total_bytes(_, _)).
```

The query asks for the whole `class_total_bytes` relation. The output tuple
`class_total_bytes("widget",15)` means: for class `widget`, the derived total
byte size is `15`. The value `15` comes from the deterministic sum `4 + 6 + 5`
over the three widget objects. The output tuple
`class_total_bytes("other",10)` means the same thing for class `other`; its
total is the single object size `10`.

For the single-relation aggregate pattern used here, the derived total depends
on the object-size facts that contributed to the sum. In this example, the
`widget` total depends on all three widget size facts, and the `other` total
depends on the one `other` size fact:

```text
class_total_bytes("widget",15) = 0.82 * 0.77 * 0.69 = 0.435666
class_total_bytes("other",10)  = 0.51
```

Thus `class_total_bytes("widget",15) : 0.435666` should be read as: the
artifact derived the fact "the selected widget-class objects have total byte
size 15", and this derived fact depends on the three probabilistic size facts
for `obj-a`, `obj-b`, and `obj-c`. This example mainly illustrates
symbol-valued facts, numeric attributes, string constants, and object-property
joins. The aggregate is included because this single-relation pattern appears
in the benchmark workload.

## Run the Examples

After building the compiler, run the maintained regression label:

```bash
JOBS=$(nproc || sysctl -n hw.ncpu || echo 2)
ctest --test-dir build -L regression --output-on-failure --progress -j${JOBS}
```

To run one example directly:

```bash
python3 tests/regression/run_full_regression_case.py \
  --case language_taint_mini \
  --souffle-bin build/src/souffle \
  --work-root build/tests/regression
```

The runner compiles `compute.dl`, runs the generated binary once with
`--det-opt`, runs it again with `--det-opt --rewrite`, and compares the two
`facts.prob` files.
