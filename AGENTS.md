# AGENTS

Repo-local constraints for the unified probabilistic artifact branch.
Read [README.md](README.md) first.

## Source References

- [README.md:1](README.md#L1): branch scope.
- [CMakeLists.txt:93](CMakeLists.txt#L93): CMake testing option.
- [CMakeLists.txt:302](CMakeLists.txt#L302): regression subdirectory wiring.
- [tests/regression/CMakeLists.txt:33](tests/regression/CMakeLists.txt#L33): `check-regression` target.
- [docs/INDEX.md:1](docs/INDEX.md#L1): documentation reading order.

## Scope

- Keep changes focused on the full/rewrite and incremental compiler/runtime surface.
- Online initialization and full recomputation must never rewrite the graph.
- Keep Lifted and approximate backends out of this artifact.
- Keep generated outputs, logs, timing data, and local run files out of git.
- Update [docs/INDEX.md](docs/INDEX.md) when adding, deleting, or renaming documentation.
- Do not modify unrelated local research workspaces unless explicitly requested.

## Build And Test

```bash
JOBS=$(nproc || sysctl -n hw.ncpu || echo 2)
cmake -S . -B build
cmake --build build -j${JOBS}
ctest --test-dir build -L regression --output-on-failure --progress -j${JOBS}
cmake --build build --target check-regression
```

## Change Discipline

- Use minimal patches.
- Use `rg` for search.
- Use `apply_patch` for manual edits.
- Rebuild after C++ changes.
- Run the regression label after compiler, runtime, or probabilistic semantics changes.

## Reference Docs

- [docs/USAGE.md](docs/USAGE.md)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/TESTING.md](docs/TESTING.md)
- [docs/RUNBOOK.md](docs/RUNBOOK.md)
- [docs/INDEX.md](docs/INDEX.md)
