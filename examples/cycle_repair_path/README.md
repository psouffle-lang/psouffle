# Cycle Repair Path Example

This example shows a small `source`/`edge`/`path` program under interactive
updates. The important behavior is cycle repair: after the only external entry
into a recursive cycle is deleted, facts that are supported only by the cycle
must be removed.

The committed `example_output/` directory is a reference run produced with
`--setmode inc-regional --dump=dot`. Use a DOT/Graphviz preview extension in VS
Code, or another Graphviz viewer, to inspect the graph files.

## 1. Program

[compute.dl](compute.dl) computes paths reachable from explicit source nodes:

```souffle
path(x,x) :- source(x).
path(x,z) :- path(x,y), edge(y,z).
```

The input graph has one external entry into a cycle:

```text
0 -> 1 -> 2 -> 3
     ^         |
     |---------|
3 -> 4 -> 5
```

The input files are:

- [input/source.facts](input/source.facts) and [input/source.prob](input/source.prob)
- [input/edge.facts](input/edge.facts) and [input/edge.prob](input/edge.prob)

The probability files are included so the `.prob` outputs and derivation graph
labels are visible.

## 2. Run

From this directory:

```bash
mkdir -p output
../../build/src/souffle -F input -D output compute.dl -o compute
./compute -F input -D output --setmode inc-regional --dump=dot < updates.txt
```

If you are using the local review build directory in this workspace, replace
`../../build/src/souffle` with `../../build-review/src/souffle`.

[updates.txt](updates.txt) is the scripted interactive session. The committed
[example_output](example_output) directory is the result of the same run.

## 3. Startup

Before any update, [example_output/facts.prob](example_output/facts.prob)
contains:

```text
path(0,0) : 0.99
path(0,1) : 0.891
path(0,2) : 0.7128
path(0,3) : 0.49896
path(0,4) : 0.24948
path(0,5) : 0.099792
```

Open [example_output/before_prune.dot](example_output/before_prune.dot) or
[example_output/after_prune.dot](example_output/after_prune.dot) to see the
initial derivation graph. The path cycle is visible through the recursive rule
instances for `path(0,1)`, `path(0,2)`, and `path(0,3)`.

## 4. Step 1: Delete The Only Entry

Update:

```text
delete edge(0,1)
commit
```

Expected live output:

[example_output/fact-iter1-inc-regional.prob](example_output/fact-iter1-inc-regional.prob)

```text
path(0,0) : 0.99
```

Interpretation: `edge(0,1)` was the only external support entering the cycle.
After it is deleted, `path(0,1)`, `path(0,2)`, and `path(0,3)` would otherwise
only support each other. Cycle repair removes them, and `path(0,4)` /
`path(0,5)` disappear because the tail is no longer reachable.

DOT files:

- [example_output/derivation-inc-before-prune1.dot](example_output/derivation-inc-before-prune1.dot)
- [example_output/derivation-inc-after-prune1.dot](example_output/derivation-inc-after-prune1.dot)

In the after-prune DOT, the removed entry and stale path facts are pink/dashed
deleted nodes, with red dashed deleted derivation edges.

## 5. Step 2: Repair The Entry

Update:

```text
insert 0.85::edge(0,2)
commit
```

Expected live output:

[example_output/fact-iter2-inc-regional.prob](example_output/fact-iter2-inc-regional.prob)

```text
path(0,0) : 0.99
path(0,1) : 0.35343
path(0,2) : 0.8415
path(0,3) : 0.58905
path(0,4) : 0.294525
path(0,5) : 0.11781
```

Interpretation: the new `edge(0,2)` provides external support into the same
cycle at a different node. The cycle is reachable again, so the tail facts also
return.

DOT files:

- [example_output/derivation-inc-before-prune2.dot](example_output/derivation-inc-before-prune2.dot)
- [example_output/derivation-inc-after-prune2.dot](example_output/derivation-inc-after-prune2.dot)

In the after-prune DOT, the new entry and reintroduced path facts are green
inserted nodes/edges.

## 6. Step 3: Break The Cycle

Update:

```text
delete edge(2,3)
commit
```

Expected live output:

[example_output/fact-iter3-inc-regional.prob](example_output/fact-iter3-inc-regional.prob)

```text
path(0,0) : 0.99
path(0,2) : 0.8415
```

Interpretation: `edge(0,2)` still reaches node `2`, but deleting `edge(2,3)`
breaks the forward path into `3`, `1`, `4`, and `5`.

DOT files:

- [example_output/derivation-inc-before-prune3.dot](example_output/derivation-inc-before-prune3.dot)
- [example_output/derivation-inc-after-prune3.dot](example_output/derivation-inc-after-prune3.dot)

The after-prune DOT marks `edge(2,3)` and the now-unreachable path facts as
deleted.

## 7. Step 4: Restore The Cycle

Update:

```text
insert 0.70::edge(2,3)
commit
```

Expected live output:

[example_output/fact-iter4-inc-regional.prob](example_output/fact-iter4-inc-regional.prob)

```text
path(0,0) : 0.99
path(0,1) : 0.35343
path(0,2) : 0.8415
path(0,3) : 0.58905
path(0,4) : 0.294525
path(0,5) : 0.11781
```

Interpretation: reinserting `edge(2,3)` restores the cycle and the tail.

DOT files:

- [example_output/derivation-inc-before-prune4.dot](example_output/derivation-inc-before-prune4.dot)
- [example_output/derivation-inc-after-prune4.dot](example_output/derivation-inc-after-prune4.dot)

The after-prune DOT shows the restored edge and path facts as inserted again.

## 8. Regional DOT Files

Because the reference run uses `inc-regional`, `example_output/` also contains
final regional diagnostic DOT files:

- [example_output/inc-region-1.dot](example_output/inc-region-1.dot)
- [example_output/inc-region-2.dot](example_output/inc-region-2.dot)

These are useful for seeing the incremental recompilation region. The main
cycle-repair behavior is easiest to inspect in the `derivation-inc-*.dot` files
listed above. Souffle does not emit intermediate `inc-region-step-*.dot` files
by default.
