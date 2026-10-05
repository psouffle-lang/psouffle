# Side-Channel Cases

This directory stores the P13-P20 inputs used by the `CAV-INC` artifact
workflow, plus a provisional A1-A11 synthetic snapshot derived from P13-P20.
This is not the final artifact benchmark freeze. Each case contains:

- `compute.souffle.dl`: probabilistic Souffle program.
- `input/`: `.facts` and `.prob` files consumed by Souffle.
- `delta/`: default `inc0p5/inc1p0/inc1p5` update files with five samples per label.

Place run outputs under `benchmarks/side_channel/runs/`. Do not add compiled
binaries, JSON summaries, or other run outputs to this tracked `cases/`
directory.
