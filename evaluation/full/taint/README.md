# Taint Benchmark

This benchmark contains a fixed five-stage Android taint-analysis pipeline. Each
stage program is checked in as Souffle code:

```
taint/stages.txt
taint/programs/cipt-cg.dl
taint/programs/pre.dl
taint/programs/typefilter.dl
taint/programs/pt-obj.dl
taint/programs/taint-lim.dl
```

Each sampled application case has one base input directory:

```
taint/app-018/input/*.facts
taint/app-018/input/*.prob
```

The checked-in bundle contains only the selected sampled cases below. Each case
has non-empty final taint output under `--rewrite`; `savage-knife` is included
because the baseline times out while rewrite completes.

| Case | Plain | Rewrite | Final output | Node red. | Edge red. |
|---|---:|---:|---|---:|---:|
| `and-roc` | 17.77s | 3.96s | `sink=1`, `facts.prob=5` | 84.23% | 83.49% |
| `andors-trail` | 11.85s | 3.82s | `sink=1`, `facts.prob=5` | 44.32% | 42.49% |
| `angulo` | 4.33s | 3.60s | `sink=70` | 71.35% | 65.27% |
| `app-018` | 12.41s | 4.44s | `sink=1`, `facts.prob=5` | 90.09% | 90.43% |
| `app-ca7` | 92.68s | 12.91s | `sink=1`, `facts.prob=5` | 55.42% | 54.95% |
| `app-kQm` | 11.58s | 3.49s | `sink=1`, `facts.prob=5` | 56.29% | 53.90% |
| `barcode` | 58.10s | 10.54s | `sink=1`, `facts.prob=5` | 56.21% | 55.30% |
| `ginger-master` | 21.18s | 5.32s | `sink=1`, `facts.prob=5` | 55.80% | 54.42% |
| `noisy-sounds` | 14.25s | 3.99s | `sink=1`, `facts.prob=5` | 53.48% | 51.81% |
| `savage-knife` | timeout | 19.49s | `sink=1`, `facts.prob=5` | 74.04% | 73.31% |
| `tilt-mazes` | 23.96s | 5.67s | `sink=1`, `facts.prob=5` | 53.44% | 52.32% |
| `tom-droid` | 25.91s | 6.08s | `sink=1`, `facts.prob=5` | 67.64% | 66.20% |
| `tuio-droid` | 15.62s | 6.35s | `sink=1`, `facts.prob=13` | 43.82% | 39.44% |
| `video-game` | 35.54s | 9.51s | `sink=10`, `facts.prob=10` | 40.53% | 40.98% |
| `yaaic` | 64.93s | 11.29s | `sink=1`, `facts.prob=8` | 67.59% | 66.50% |

The runner compiles each stage once, then executes the stages in the order given
by `stages.txt`. The output facts from stage `i` become additional input facts
for stage `i+1`.

Node and edge reductions are measured from the generated run logs using strict
graph handoff sizes. The before size is `PRUNING.after_prune_*`; the after size
is `FC_WMC_HYBRID.rewrite_final_*`, immediately before the probability backend
runs.

## Stage Intuition

`cipt-cg` computes call-graph and context-insensitive points-to facts used by
later stages.

`pre` normalizes base relations for later analysis.

`typefilter` filters flows using type and class constraints.

`pt-obj` materializes object-level points-to relations consumed by the taint
stage.

`taint-lim` propagates taint through the filtered flow graph and emits the final
probability queries.

## Runner

```bash
cd evaluation/full
python3 FMCAD.py -RQ2 --only taint --rq2-engines souffle --souffle-bin "$SOUFFLE_BIN" --taint-cases app-018
```

The runner compiles each stage once, executes the pipeline in `stages.txt`
order, and writes `facts.prob` plus consistency fields in `run.meta.json`
for each completed non-baseline stage.
