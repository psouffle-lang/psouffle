# IncBDD Calibration Example

This example is the calibration case from the paper's IncBDD
region-identification example. It corresponds to Case 1 in the paper: `Delta`
adds a derivation only for `O2`.

The point of this example is to show a case where `inc-regional` really reuses
downstream BDDs instead of rebuilding the whole affected subgraph. The important
thing to inspect is the boundary node `o2(0)` and its calibrated anchor
`i2(0)`.

The update changes `O2`, and downstream `O4/O5` are affected. However, the
effect reaches downstream users through the single `O2` interface, so
`inc-regional` can rebuild only `O2` and calibrate the old `I2` anchor weight
instead of rebuilding the downstream BDDs.

```text
I1 -> O1 -> O3
I2 -> O2 -> O4
      ^       \
      Delta    O5
```

After the update:

- `o2(0)` is rebuilt inside the region.
- `o4(0)` and `o5(0)` are delta-reachable but stay outside the rebuild region.
- `o2(0)` is the boundary that summarizes the region for downstream reused BDDs.
- `i2(0)` is the old anchor variable whose weight is calibrated.

## Run

From this directory:

```bash
mkdir -p output
../../build/src/souffle -F input -D output compute.dl -o compute
./compute -F input -D output --setmode inc-regional --dump=dot \
  --profile-stage=inc-regional < updates.txt
```

If your local review build is in `build-review/`, use
`../../build-review/src/souffle` instead.

Expected probabilities:

```text
o4(0) : 0.7
o5(0) : 0.42
```

The result is the same as full recomputation, but the region summary shows less
work:

```text
Region nodes: 2 / DR nodes: 4
Region nodes: d2(0), o2(0)
Delta-reachable but not rebuilt: o4(0), o5(0)
Boundary: o2(0)
Anchor: node:i2(0)
```

The key profile line is:

```text
[inc-regional-calibration] head=o2(0) anchor=node:i2(0) var=0 old_node_prob=0.5 target_prob=0.7 p_when_false=0 p_when_true=1 old_weight=0.5 calibrated_weight=0.7 calibrated_neg_weight=0.3
```

This says that the old `i2(0)` anchor weight was `0.5`; after inserting
`0.40::d2(0)`, the rebuilt boundary node `o2(0)` has target probability `0.7`,
so the reused downstream BDDs see an effective calibrated weight of `0.7`.

Read that line as the concrete calibration equation:

```text
old P(o2) = 0.5
new target P(o2) = 0.7
old anchor weight P(i2) = 0.5
new effective anchor weight = 0.7
```

Useful reference files:

- [example_output/inc-region-1.dot](example_output/inc-region-1.dot)
- [example_output/fact-iter1-inc-regional.prob](example_output/fact-iter1-inc-regional.prob)
- [example_output/calibration-profile.txt](example_output/calibration-profile.txt)

The committed reference output keeps the final `inc-region-1.dot` graph and
the calibration profile line. Souffle does not emit `inc-region-step-*.dot` or
`region-*.txt` files by default.
