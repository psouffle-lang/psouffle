# IncBDD Region Example

This example is a runnable version of the IncBDD region-identification example
from the paper. It corresponds to the paper's `G'` figure in the Region
Identification subsection and to the two cases discussed there:

- Case 1: `Delta` adds a derivation only for `O2`.
- Case 2: `Delta` adds correlated derivations for both `O2` and `O3`.

It is designed to show the `inc-regional` recompilation region produced by
`--dump=dot`.

The Souffle program uses lower-case relation names, but keeps the same graph
shape as the paper example:

```text
I1 -> O1 -> O3
I2 -> O2 -> O4
          \   /
            O5
```

There are two update facts:

- `d2` is the paper's Case 1 delta: it adds a new derivation only for `O2`.
- `dshared` is the paper's Case 2 delta: it adds the same new event to both
  `O2` and `O3`.

The single-interface run uses `d2`: downstream `o4` and `o5` are affected, but
the change can be summarized through the `o2` side. The correlated-join run uses
`dshared`: the same new fact reaches both sides of the `o5` join, so the region
must grow to represent the shared dependency explicitly.

For a focused walkthrough of the single-interface calibration path, see
[../inc_bdd_calibration](../inc_bdd_calibration).

## Run

From this directory:

```bash
mkdir -p output_single output_correlated
../../build/src/souffle -F input -D output_single compute.dl -o compute

./compute -F input -D output_single --setmode inc-regional --dump=dot \
  --profile-stage=inc-regional < updates_single_interface.txt
./compute -F input -D output_correlated --setmode inc-regional --dump=dot \
  --profile-stage=inc-regional < updates_correlated_join.txt
```

If your local review build is in `build-review/`, use
`../../build-review/src/souffle` instead.

The `example_output/` directory contains reference outputs from these two runs.

## Single-Interface Run

[updates_single_interface.txt](updates_single_interface.txt) contains:

```text
insert 0.40::d2(0)
commit
q
```

Expected output:

```text
o4(0) : 0.7
o5(0) : 0.42
```

The key regional files are:

- [example_output/single_interface/inc-region-1.dot](example_output/single_interface/inc-region-1.dot)
- [example_output/single_interface/fact-iter1-inc-regional.prob](example_output/single_interface/fact-iter1-inc-regional.prob)

Here the delta-reachable graph includes `o4` and `o5`, but the final region
stays at the `d2/o2` side and calibrates the `o2` boundary.

## Correlated-Join Run

[updates_correlated_join.txt](updates_correlated_join.txt) contains:

```text
insert 0.40::dshared(0)
commit
q
```

Expected output:

```text
o4(0) : 0.7
o5(0) : 0.58
```

The key regional files are:

- [example_output/correlated_join/inc-region-1.dot](example_output/correlated_join/inc-region-1.dot)
- [example_output/correlated_join/fact-iter1-inc-regional.prob](example_output/correlated_join/fact-iter1-inc-regional.prob)

The final region DOT shows `o4/o5` as region members. The profile output also
reports `overlap closure reason=preplan`; that is the signal that `o4/o5` were
pulled into the effective rebuild region because both sides of the join now
share the same new input.

## DOT Files

The regional DOT files are the main point of the example:

- `output/inc-region-*.dot` shows the final effective rebuild region after
  regional planning, including overlap closure.
- `output/derivation-inc-*.dot` shows the before/after derivation graph around
  pruning.

Use a DOT/Graphviz preview extension in VS Code. In the legend, blue marks
region members, green marks delta nodes/edges, orange marks boundary heads,
yellow marks mergeable node anchors, and red marks mergeable anchor paths.
Souffle does
not emit intermediate `inc-region-step-*.dot` files or `region-*.txt` files by
default.
