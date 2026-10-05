#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""FMCAD evaluation runner.

Mirrors side_channel_full_evaluation.py but exposes two entry points:

* ``-sc``  generates the Souffle rewrite program for each side-channel
  case and runs Souffle + ProbLog under ``artifact/FMCADSC/P<n>/...``.
* ``-dis`` runs the shared symbolization program through two Souffle
  engines — without rewrite and with implicit rewrite — for every case
  under ``symbolization/<case>/input/``, landing outputs in
  ``artifact/runs/disasm/<case>/<engine>/``.

After a run (or when invoked with ``--collect``), a runtime summary table
is aggregated from ``run.meta.json`` logs and written to
``<base-dir>/FMCADresult.tsv`` (sc) or ``<base-dir>/DISresult.tsv``
(dis).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# --- Per-subprocess memory cap ----------------------------------------------
# No per-subprocess memory cap by default; pass --mem-limit-mb <MB> to enable
# RLIMIT_AS/RLIMIT_DATA on spawned children.

DEFAULT_MEM_LIMIT_MB = 0
_MEM_LIMIT_BYTES: Optional[int] = None
_subprocess_run_orig = subprocess.run


def _apply_mem_limit_in_child() -> None:
    if _MEM_LIMIT_BYTES is None:
        return
    for rlim in (resource.RLIMIT_AS, resource.RLIMIT_DATA):
        try:
            resource.setrlimit(rlim, (_MEM_LIMIT_BYTES, _MEM_LIMIT_BYTES))
        except (ValueError, OSError):
            pass


def _subprocess_run_with_limit(*args, **kwargs):
    if _MEM_LIMIT_BYTES is not None and "preexec_fn" not in kwargs:
        kwargs["preexec_fn"] = _apply_mem_limit_in_child
    return _subprocess_run_orig(*args, **kwargs)


def _install_mem_limit(mb: int) -> None:
    """Cap every subsequently-spawned child at `mb` MiB. mb<=0 disables."""
    global _MEM_LIMIT_BYTES
    if mb is None or mb <= 0:
        _MEM_LIMIT_BYTES = None
        subprocess.run = _subprocess_run_orig
        return
    _MEM_LIMIT_BYTES = mb * 1024 * 1024
    subprocess.run = _subprocess_run_with_limit


# --- OOM detection ----------------------------------------------------------
# A run is OOM when (a) the kernel killed it for exceeding RLIMIT_AS / OOM-killer
# (rc == 137), (b) it aborted on bad_alloc / Python MemoryError, or (c) the log
# carries an unambiguous allocator-failure marker. Used to override the generic
# "error" / "exit=N" classification with "OOM" in RQ2's log + result TSV so a
# memory-cap hit is distinguishable from a real benchmark failure.
_OOM_LOG_MARKERS: Tuple[str, ...] = (
    "memoryerror",
    "std::bad_alloc",
    "bad_alloc",
    "cannot allocate memory",
    "out of memory",
    "outofmemoryerror",
    "killed",
)


def _tail_text(log_path: Optional[Path], n_bytes: int = 32768) -> str:
    if log_path is None:
        return ""
    try:
        if not log_path.is_file():
            return ""
        sz = log_path.stat().st_size
        with log_path.open("rb") as fh:
            if sz > n_bytes:
                fh.seek(-n_bytes, 2)
            return fh.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _looks_like_oom(
    rc: int,
    log_path: Optional[Path] = None,
    extra_text: str = "",
) -> bool:
    # SIGKILL: rc==137 from a shell-style 128+sig encoding, rc==-9 from
    # subprocess.run (which reports terminating signals as negative). On
    # Linux/WSL2 the kernel OOM-killer is by far the most likely sender of
    # SIGKILL to a benchmark child, so treat both encodings as OOM.
    if rc in (137, -9):
        return True
    text = (extra_text or "") + "\n" + _tail_text(log_path)
    low = text.lower()
    if any(m in low for m in _OOM_LOG_MARKERS):
        return True
    # SIGABRT (134 / -6) only counts as OOM when the log explicitly mentions
    # the allocator — bare aborts can come from assertions, signal handlers,
    # etc., and those are covered by the marker scan above.
    return False


def _classify_status(
    rc: int,
    log_path: Optional[Path] = None,
    extra_text: str = "",
) -> Tuple[str, str]:
    """Return (status, note). status ∈ {ok, timeout, oom, error}."""
    if rc == 0:
        return "ok", ""
    if rc == 124:
        return "timeout", "timeout"
    if _looks_like_oom(rc, log_path=log_path, extra_text=extra_text):
        return "oom", f"oom (exit={rc})"
    return "error", f"exit={rc}"


CASE_PREFIX = "P"
SCRIPT_ROOT = Path(__file__).resolve().parent
PSOUFFLE_ROOT = SCRIPT_ROOT.parents[1]
DEFAULT_SOURCE_DIR = (SCRIPT_ROOT / "side_channel").resolve()
SIDE_CHANNEL_DATASET_DIR = "full"
EXCLUDED_CASES: set = set()
CREATE_GRAPH_STAGE = "CREATE_GRAPH"
FC_STAGE = "FORWARD_COMPILATION"
WMC_STAGE = "WEIGHTED_MODEL_COUNTING"
FC_WMC_HYBRID_STAGE = "FC_WMC_HYBRID"


_LEGACY_STAGE_SUFFIX = "_FULL"


def _stage_name_aliases(stage_name: str) -> Tuple[str, ...]:
    if stage_name == FC_WMC_HYBRID_STAGE:
        return (stage_name,)
    return (stage_name, stage_name + _LEGACY_STAGE_SUFFIX)


def _sudo_user_local_bin(tool: str) -> Optional[str]:
    sudo_user = os.environ.get("SUDO_USER")
    if not sudo_user or sudo_user == "root":
        return None
    cand = Path("/home") / sudo_user / ".local" / "bin" / tool
    return str(cand) if cand.is_file() and os.access(cand, os.X_OK) else None


def _default_souffle_bin() -> str:
    explicit = os.environ.get("SOUFFLE_BIN")
    if explicit:
        return explicit
    repo_bin = PSOUFFLE_ROOT / "build" / "src" / "souffle"
    if repo_bin.is_file():
        return str(repo_bin)
    sudo_user_bin = _sudo_user_local_bin("souffle")
    if sudo_user_bin:
        return sudo_user_bin
    return "souffle"


PROBLOG_BIN_CANDIDATES = [
    *([p] if (p := _sudo_user_local_bin("problog")) else []),
    "problog",
    "problog-cli",
]
_SOUFFLE_HELP_CACHE: Dict[str, str] = {}
DEFAULT_SOUFFLE_BIN = _default_souffle_bin()


class Logger:
    def __init__(self, base_dir: Path, log_file: Optional[Path], quiet: bool = False):
        self.base_dir = base_dir
        self.path = log_file if log_file else (base_dir / "rq1.log")
        self.quiet = quiet
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def _write(self, level: str, msg: str):
        line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {level:<5} {msg}"
        with open(self.path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if not self.quiet:
            print(line, flush=True)

    def info(self, msg: str):
        self._write("INFO", msg)

    def warn(self, msg: str):
        self._write("WARN", msg)

    def error(self, msg: str):
        self._write("ERROR", msg)

    def banner(self, title: str):
        sep = "=" * max(10, min(78, len(title) + 10))
        self.info(sep)
        self.info(title)
        self.info(sep)


def _to_text(x):
    if x is None:
        return ""
    if isinstance(x, (bytes, bytearray)):
        try:
            return x.decode("utf-8", errors="replace")
        except Exception:
            return x.decode(errors="replace")
    return x


def time_cmd(cmd: Sequence[str], cwd: Optional[Path], timeout: int) -> Tuple[int, float, str, str]:
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        elapsed = time.perf_counter() - t0
        return proc.returncode, elapsed, _to_text(proc.stdout), _to_text(proc.stderr)
    except subprocess.TimeoutExpired as e:
        return 124, timeout, _to_text(e.stdout), _to_text(e.stderr) + f"TIMEOUT after {timeout}s"
    except FileNotFoundError as e:
        return 127, 0.0, "", f"NOTFOUND: {e}"
    except Exception as e:
        return 1, 0.0, "", f"ERROR: {e}"


def souffle_help_text(bin_path: str) -> str:
    cached = _SOUFFLE_HELP_CACHE.get(bin_path)
    if cached is not None:
        return cached
    code, _, out, err = time_cmd([bin_path, "--help"], cwd=None, timeout=30)
    text = (out or "") + "\n" + (err or "")
    if code not in (0, 1):
        text = text or ""
    _SOUFFLE_HELP_CACHE[bin_path] = text
    return text


def souffle_supports_flag(bin_path: str, flag: str) -> bool:
    return flag in souffle_help_text(bin_path)


def resolve_source_dir(source_dir: Optional[str]) -> Path:
    if source_dir:
        return Path(source_dir).resolve()
    return DEFAULT_SOURCE_DIR


def ensure_dirs(*paths: Path) -> None:
    for p in paths:
        p.mkdir(parents=True, exist_ok=True)


def find_cases_in_source(source_dir: Path) -> List[int]:
    out: List[int] = []
    if not source_dir.exists():
        return out
    for child in source_dir.iterdir():
        if not child.is_dir():
            continue
        m = re.match(rf"^{CASE_PREFIX}(\d+)$", child.name)
        if m:
            n = int(m.group(1))
            if n in EXCLUDED_CASES:
                continue
            out.append(n)
    return sorted(out)


@dataclass
class GenCfg:
    base_dir: Path
    source_dir: Path
    cases: List[int]
    logger: "Logger"


def _source_variant_dir(source_dir: Path) -> Path:
    """Pre-built fixed inputs live under <source_dir>/<SIDE_CHANNEL_DATASET_DIR>/.
    If the user already pointed at the variant dir, fall through unchanged."""
    candidate = source_dir / SIDE_CHANNEL_DATASET_DIR
    return candidate if candidate.is_dir() else source_dir


def generate_case(n: int, cfg: GenCfg) -> None:
    """Stage a case from the self-contained dataset at <source>/full/P{n}/.
    Each case dir ships `compute.souffle.dl`, `compute.problog.dl`, and
    `input/*.facts/.prob`; we just copy them into <base_dir>/P{n}/."""
    name = case_name(n)
    src = _source_variant_dir(cfg.source_dir) / name
    if not src.is_dir():
        cfg.logger.error(f"[{name}] Source directory missing: {src}")
        return

    cdir = case_dir(cfg.base_dir, n)
    ensure_dirs(cdir)
    ensure_dirs(cdir / "output")

    required = ("compute.souffle.dl", "compute.problog.dl")
    for fname in required:
        if not (src / fname).is_file():
            cfg.logger.error(f"[{name}] Missing {fname} under {src}")
            return
    if not (src / "input").is_dir():
        cfg.logger.error(f"[{name}] Missing input/ under {src}")
        return

    same_dir = cdir.resolve() == src.resolve()
    if not same_dir:
        for fname in required:
            dstf = cdir / fname
            if dstf.exists() or dstf.is_symlink():
                dstf.unlink()
            shutil.copy2(src / fname, dstf)
        input_dir = cdir / "input"
        if input_dir.exists():
            shutil.rmtree(input_dir)
        shutil.copytree(src / "input", input_dir)

    cfg.logger.info(
        f"[{name}] Prepared from {src.relative_to(cfg.source_dir)}"
        + (" (in-place)" if same_dir else "")
    )


def run_generate(
    base_dir: Path,
    source_dir: Optional[str],
    cases: List[int],
    cleanup: bool,
    timeout: int,
    logger: Logger,
) -> bool:
    source_root = resolve_source_dir(source_dir)
    start = time.perf_counter()
    if cleanup and base_dir.exists():
        shutil.rmtree(base_dir)
    base_dir.mkdir(parents=True, exist_ok=True)

    selected_cases = sorted(set(cases)) if cases else find_cases_in_source(source_root)
    if not selected_cases:
        logger.error("[GEN] No cases available. Check --source-dir or provide cases explicitly.")
        return False

    logger.info(f"[GEN] source={source_root}")
    logger.info(f"[GEN] cases={selected_cases} count={len(selected_cases)}")
    cfg = GenCfg(
        base_dir=base_dir,
        source_dir=source_root,
        cases=selected_cases,
        logger=logger,
    )
    for idx, n in enumerate(selected_cases, 1):
        if timeout > 0 and (time.perf_counter() - start) >= timeout:
            logger.error(f"[GEN] generate timeout after {timeout}s")
            return False
        logger.info(f"[GEN] [{idx}/{len(selected_cases)}] Start {case_name(n)}")
        try:
            generate_case(n, cfg)
        except Exception as exc:
            logger.error(f"[GEN] [{case_name(n)}] failed: {exc}")
            return False
        logger.info(f"[GEN] [{idx}/{len(selected_cases)}] Done {case_name(n)}")
    logger.info(f"[GEN] generate OK in {time.perf_counter() - start:.3f}s")
    return True


def parse_cases_spec(spec: Optional[str], default_cases: Optional[Iterable[int]] = None) -> List[int]:
    if not spec:
        return sorted(set(default_cases)) if default_cases else []
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_str, hi_str = part.split("-", 1)
            try:
                lo = int(lo_str)
                hi = int(hi_str)
            except ValueError:
                raise argparse.ArgumentTypeError(f"Invalid range in --cases: '{part}'")
            if lo < 1 or hi < lo:
                raise argparse.ArgumentTypeError(f"Invalid range bounds in --cases: '{part}'")
            for v in range(lo, hi + 1):
                out.add(v)
        else:
            try:
                v = int(part)
            except ValueError:
                raise argparse.ArgumentTypeError(f"Invalid case value in --cases: '{part}'")
            if v < 1:
                raise argparse.ArgumentTypeError(f"Case must be >= 1: '{part}'")
            out.add(v)
    return sorted(out)


def parse_case_tokens(tokens: Sequence[str]) -> List[int]:
    out: set[int] = set()
    for raw in tokens:
        text = raw.strip()
        if not text:
            continue
        if text[0] in ("P", "p"):
            text = text[1:]
        if "-" in text:
            lo_str, hi_str = text.split("-", 1)
            if lo_str[:1] in ("P", "p"):
                lo_str = lo_str[1:]
            if hi_str[:1] in ("P", "p"):
                hi_str = hi_str[1:]
            try:
                lo = int(lo_str)
                hi = int(hi_str)
            except ValueError:
                raise argparse.ArgumentTypeError(f"Invalid range in cases: '{raw}'")
            if lo < 1 or hi < lo:
                raise argparse.ArgumentTypeError(f"Invalid range bounds in cases: '{raw}'")
            for v in range(lo, hi + 1):
                out.add(v)
        else:
            try:
                v = int(text)
            except ValueError:
                raise argparse.ArgumentTypeError(f"Invalid case value: '{raw}'")
            if v < 1:
                raise argparse.ArgumentTypeError(f"Case must be >= 1: '{raw}'")
            out.add(v)
    return sorted(out)


def case_name(n: int) -> str:
    return f"{CASE_PREFIX}{n}"


def case_dir(base_dir: Path, n: int) -> Path:
    return base_dir / case_name(n)


def find_cases_on_disk(base_dir: Path) -> List[int]:
    out: List[int] = []
    if not base_dir.exists():
        return out
    for child in base_dir.iterdir():
        if not child.is_dir():
            continue
        if child.name.startswith(CASE_PREFIX):
            try:
                n = int(child.name[len(CASE_PREFIX):])
            except ValueError:
                continue
            if n in EXCLUDED_CASES:
                continue
            out.append(n)
    return sorted(out)


def select_cases(
    cases_spec: Optional[str],
    size: Optional[int],
    base_dir: Path,
    source_dir: Optional[Path] = None,
) -> List[int]:
    """When source_dir is given, prefer the source's case list (so adding a
    new case under <source>/full/P{n}/ picks it up automatically). Fall back
    to whatever's already on disk under base_dir if source has no cases —
    keeps re-runs working when the user only has a prepared base dir."""
    if source_dir is not None:
        from_source = find_cases_in_source(source_variant_dir(source_dir))
        defaults = from_source if from_source else find_cases_on_disk(base_dir)
    else:
        defaults = find_cases_on_disk(base_dir)
    cases = parse_cases_spec(cases_spec, default_cases=defaults)
    if size is not None and size > 0:
        return cases[:size]
    return cases


def source_variant_dir(source_dir: Path) -> Path:
    """Public alias for the private helper, kept module-level so external
    callers (e.g. select_cases above) can resolve <source>/full/."""
    return _source_variant_dir(source_dir)


def latest_log_json(case_path: Path, prefix: str) -> Optional[Path]:
    logs = sorted(case_path.glob(f"{prefix}*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    return logs[0] if logs else None


_prob_file_rx = re.compile(r"^\s*(.+?)\s*:\s*([0-9eE\.\+\-]+)\s*$")


def read_souffle_prob_file(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    if not path.exists():
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = _prob_file_rx.match(line)
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            try:
                out[key.strip().upper()] = float(val)
            except ValueError:
                continue
    return out


def compare_prob_maps(a: Dict[str, float], b: Dict[str, float], tol: float = 1e-6) -> Tuple[bool, int, float]:
    keys = set(a.keys()) | set(b.keys())
    mismatches = 0
    max_delta = 0.0
    for key in keys:
        va = a.get(key, 0.0)
        vb = b.get(key, 0.0)
        delta = abs(va - vb)
        if delta > tol:
            mismatches += 1
            if delta > max_delta:
                max_delta = delta
    return mismatches == 0, mismatches, max_delta


def reference_variant_name(variant_name: str) -> Optional[str]:
    if variant_name.endswith("_r"):
        return variant_name[:-2]
    if variant_name == "r":
        return "plain"
    return None


def parse_souffle_stage_data(log_path: Path) -> Tuple[Dict[str, float], Dict[str, Dict[str, str]]]:
    try:
        data = json.loads(log_path.read_text(encoding="utf-8"))
    except Exception:
        return {}, {}
    turns = data.get("turns", [])
    if not turns:
        return {}, {}
    stages = turns[0].get("stages", [])
    if not isinstance(stages, list):
        return {}, {}

    stage_times: Dict[str, float] = {}
    stage_infos: Dict[str, Dict[str, str]] = {}
    for stage in stages:
        if not isinstance(stage, dict):
            continue
        name = stage.get("name")
        if not isinstance(name, str):
            continue
        val = stage.get("time_seconds")
        if isinstance(val, (int, float)):
            stage_times[name] = float(val)
        info = stage.get("info")
        if isinstance(info, dict):
            stage_infos[name] = {str(k): str(v) for k, v in info.items()}
    return stage_times, stage_infos


def find_run_log_json(run_dir: Path) -> Optional[Path]:
    root_log = run_dir / "log.json"
    if root_log.exists():
        return root_log
    root_logs = sorted(run_dir.glob("log*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    if root_logs:
        return root_logs[0]
    output_dir = run_dir / "output"
    if output_dir.exists():
        out_logs = list(output_dir.glob("log*.json"))
        if out_logs:
            return sorted(out_logs, key=lambda p: p.stat().st_mtime, reverse=True)[0]
    return None


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def move_if_exists(src: Path, dst: Path) -> None:
    if not src.exists():
        return
    ensure_dir(dst.parent)
    try:
        shutil.move(str(src), str(dst))
    except Exception:
        shutil.copy2(str(src), str(dst))
        if src.is_dir():
            shutil.rmtree(src)
        else:
            src.unlink(missing_ok=True)


def clean_rq_artifacts(
    base_dir: Path,
    cases: Sequence[int],
    runs_dir_name: str,
    result_name: str,
    logger: Logger,
) -> bool:
    removed_any = False
    for n in cases:
        runs_dir = case_dir(base_dir, n) / runs_dir_name
        if runs_dir.exists():
            shutil.rmtree(runs_dir)
            logger.info(f"[{case_name(n)}] Removed {runs_dir}")
            removed_any = True
    result_file = base_dir / result_name
    if result_file.exists():
        result_file.unlink()
        logger.info(f"Removed {result_file}")
        removed_any = True
    return removed_any


@dataclass
class Variant:
    name: str
    input_dir: str
    output_dir: str
    exe_name: str
    compile_extra: List[str]
    run_extra: List[str]


@dataclass
class CompileCfg:
    base_dir: Path
    cases: List[int]
    timeout: int
    souffle_bin: str
    souffle_args: List[str]
    include_dir: str
    derv_only: bool
    compile_det_opt: bool
    force_compile: bool
    logger: Logger
    use_full_only: bool = True


@dataclass
class RunCfg:
    base_dir: Path
    cases: List[int]
    runs: int
    timeout: int
    log_prefix: str
    run_args: List[str]
    logger: Logger


def compile_case(n: int, cfg: CompileCfg, variants: Sequence[Variant]) -> None:
    cdir = case_dir(cfg.base_dir, n)
    src = cdir / "compute.souffle.dl"
    if not src.exists():
        cfg.logger.warn(f"[{case_name(n)}] Missing {src.name}; skipping compile")
        return

    compiled_exes: set[str] = set()
    for variant in variants:
        if variant.exe_name in compiled_exes:
            cfg.logger.info(f"[{case_name(n)}] {variant.exe_name} already compiled; skipping")
            continue
        exe = cdir / variant.exe_name
        if exe.exists() and not cfg.force_compile:
            cfg.logger.info(f"[{case_name(n)}] {variant.name} already compiled; skipping")
            compiled_exes.add(variant.exe_name)
            continue
        input_path = cdir / variant.input_dir
        if not input_path.exists():
            cfg.logger.warn(f"[{case_name(n)}] Missing input dir {variant.input_dir}; using 'input'")
            input_path = cdir / "input"

        cmd = [cfg.souffle_bin]
        if cfg.use_full_only and souffle_supports_flag(cfg.souffle_bin, "--full-only"):
            cmd.append("--full-only")
        if cfg.derv_only:
            cmd.append("--derv-only")
        if cfg.compile_det_opt:
            cmd.append("--det-opt")
        cmd.extend([
            "--profile=/dev/null",
            "-F",
            str(input_path),
            "-D",
            str(cdir / variant.output_dir),
            str(src),
            "-I",
            cfg.include_dir,
            "-o",
            variant.exe_name,
        ])
        cmd.extend(variant.compile_extra)
        if cfg.souffle_args:
            cmd.extend(cfg.souffle_args)

        cfg.logger.info(f"[{case_name(n)}] Compile {variant.name} -> {variant.exe_name}")
        code, elapsed, _, err = time_cmd(cmd, cwd=cdir, timeout=cfg.timeout)
        if code == 0:
            cfg.logger.info(f"[{case_name(n)}] {variant.name} compile OK in {elapsed:.3f}s")
        else:
            cfg.logger.error(f"[{case_name(n)}] {variant.name} compile failed (exit={code}) in {elapsed:.3f}s")
            if err.strip():
                cfg.logger.error(f"[{case_name(n)}] stderr: {err.strip()[:300]}")
        compiled_exes.add(variant.exe_name)


def run_case(n: int, cfg: RunCfg, variants: Sequence[Variant], run_root: str) -> None:
    cdir = case_dir(cfg.base_dir, n)
    completed_runs: Dict[Tuple[str, int], Path] = {}

    for variant in variants:
        exe = cdir / variant.exe_name
        if not exe.exists():
            cfg.logger.warn(f"[{case_name(n)}] Missing executable {variant.exe_name}; skip")
            continue

        base_output = cdir / variant.output_dir
        runs_root = cdir / run_root / variant.name
        ensure_dir(runs_root)
        log_prefix = f"{cfg.log_prefix}_{variant.name}"

        for run_idx in range(1, cfg.runs + 1):
            if base_output.exists():
                stamp = time.strftime("%Y%m%d_%H%M%S")
                move_if_exists(base_output, runs_root / f"preexisting_{stamp}")

            ensure_dir(base_output)
            cmd = [
                f"./{exe.name}",
                "-F",
                str(cdir / variant.input_dir),
                "-D",
                str(base_output),
                "--logfile",
                log_prefix,
                "--dumpdot",
            ]
            if variant.run_extra:
                cmd.extend(variant.run_extra)
            if cfg.run_args:
                cmd.extend(cfg.run_args)

            cfg.logger.info(f"[{case_name(n)}] Run {variant.name} ({run_idx}/{cfg.runs})")
            code, elapsed, _, err = time_cmd(cmd, cwd=cdir, timeout=cfg.timeout)

            stamp = time.strftime("%Y%m%d_%H%M%S")
            run_dir = runs_root / f"run_{run_idx:02d}_{stamp}"
            ensure_dir(run_dir)

            if base_output.exists():
                move_if_exists(base_output, run_dir / "output")

            new_log = latest_log_json(cdir, log_prefix)
            if new_log:
                move_if_exists(new_log, run_dir / "log.json")

            run_status, status_note = _classify_status(
                code, extra_text=err or "",
            )
            meta = {
                "exit": code,
                "elapsed": elapsed,
                "variant": variant.name,
                "case": case_name(n),
                "run": run_idx,
                "timestamp": stamp,
                "logfile": str(new_log.name) if new_log else "",
                "status": run_status,
                "note": status_note,
            }
            final_log_path = find_run_log_json(run_dir)
            if final_log_path is not None:
                stage_times, stage_infos = parse_souffle_stage_data(final_log_path)
                if stage_times:
                    meta["stage_times_seconds"] = stage_times
                if stage_infos:
                    meta["stage_info"] = stage_infos

            ref_variant = reference_variant_name(variant.name)
            if code == 0 and ref_variant is not None:
                ref_run_dir = completed_runs.get((ref_variant, run_idx))
                if ref_run_dir is None:
                    meta["consistency_status"] = "missing_reference"
                    meta["consistency_against"] = ref_variant
                else:
                    ref_map = read_souffle_prob_file(ref_run_dir / "output" / "facts.prob")
                    cur_map = read_souffle_prob_file(run_dir / "output" / "facts.prob")
                    ok, mismatches, max_delta = compare_prob_maps(ref_map, cur_map, tol=1e-6)
                    meta["consistency_status"] = "ok" if ok else "mismatch"
                    meta["consistency_against"] = ref_variant
                    meta["consistent"] = ok
                    meta["mismatches"] = mismatches
                    meta["max_abs_delta"] = max_delta
                    if ok:
                        cfg.logger.info(
                            f"[{case_name(n)}] {variant.name} matches {ref_variant} "
                            f"(max|Δ|={max_delta:.3e})"
                        )
                    else:
                        cfg.logger.error(
                            f"[{case_name(n)}] {variant.name} mismatches {ref_variant} "
                            f"(mismatches={mismatches}, max|Δ|={max_delta:.3e})"
                        )

            with open(run_dir / "run.meta.json", "w", encoding="utf-8") as f:
                json.dump(meta, f, indent=2)

            completed_runs[(variant.name, run_idx)] = run_dir

            if code == 0:
                cfg.logger.info(f"[{case_name(n)}] {variant.name} run OK in {elapsed:.3f}s")
            elif run_status == "timeout":
                cfg.logger.warn(f"[{case_name(n)}] {variant.name} run TIMEOUT at {elapsed:.3f}s")
                break
            elif run_status == "oom":
                cfg.logger.error(f"[{case_name(n)}] {variant.name} run OOM in {elapsed:.3f}s (exit={code})")
                if err.strip():
                    cfg.logger.error(f"[{case_name(n)}] stderr: {err.strip()[:300]}")
                break
            else:
                cfg.logger.error(f"[{case_name(n)}] {variant.name} run failed (exit={code}) in {elapsed:.3f}s")
                if err.strip():
                    cfg.logger.error(f"[{case_name(n)}] stderr: {err.strip()[:300]}")


def _stage_problog_program(source: Path, work_dir: Path) -> Path:
    """Use Datalog's empty-relation semantics without changing bundled inputs."""
    work_dir.mkdir(parents=True, exist_ok=True)
    staged = work_dir / "compute.closed-world.problog.dl"
    staged.write_text(":- unknown(fail).\n" + source.read_text(encoding="utf-8"),
                      encoding="utf-8")
    return staged.resolve()


def run_problog_case(n: int, cfg: RunCfg, run_root: str) -> None:
    cdir = case_dir(cfg.base_dir, n)
    prob_src = cdir / "compute.problog.dl"
    if not prob_src.exists():
        cfg.logger.warn(f"[{case_name(n)}] Missing compute.problog.dl; skip ProbLog")
        return

    used_bin: Optional[str] = None
    for b in PROBLOG_BIN_CANDIDATES:
        if shutil.which(b):
            used_bin = b
            break
    if not used_bin:
        cfg.logger.warn(f"[{case_name(n)}] ProbLog not found on PATH; skip")
        return

    runs_root = cdir / run_root / "problog"
    ensure_dir(runs_root)
    prob_src = _stage_problog_program(prob_src, runs_root)

    for run_idx in range(1, cfg.runs + 1):
        cmd = [used_bin, str(prob_src)]
        cfg.logger.info(f"[{case_name(n)}] Run problog ({run_idx}/{cfg.runs})")
        code, elapsed, out, err = time_cmd(cmd, cwd=None, timeout=cfg.timeout)

        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = runs_root / f"run_{run_idx:02d}_{stamp}"
        ensure_dir(run_dir)

        with open(run_dir / "problog.out", "w", encoding="utf-8") as f:
            f.write(out)
        if err:
            with open(run_dir / "problog.err", "w", encoding="utf-8") as f:
                f.write(err)

        ground_ok = code == 1 and "(True, " in err
        effective_code = 0 if ground_ok else code

        if ground_ok:
            run_status, status_note = "ok", ""
        else:
            run_status, status_note = _classify_status(
                code, extra_text=(out or "") + "\n" + (err or ""),
            )
        meta = {
            "exit": effective_code,
            "elapsed": elapsed,
            "case": case_name(n),
            "run": run_idx,
            "timestamp": stamp,
            "bin": used_bin,
            "status": run_status,
            "note": status_note,
        }
        if ground_ok:
            meta["raw_exit"] = code
        with open(run_dir / "run.meta.json", "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2)

        if effective_code == 0:
            cfg.logger.info(f"[{case_name(n)}] problog run OK in {elapsed:.3f}s")
        elif run_status == "timeout":
            cfg.logger.warn(f"[{case_name(n)}] problog run TIMEOUT at {elapsed:.3f}s")
            break
        elif run_status == "oom":
            cfg.logger.error(f"[{case_name(n)}] problog run OOM in {elapsed:.3f}s (exit={code})")
            if err.strip():
                cfg.logger.error(f"[{case_name(n)}] problog stderr: {err.strip()[:300]}")
            break
        else:
            cfg.logger.error(f"[{case_name(n)}] problog run failed (exit={code}) in {elapsed:.3f}s")
            if err.strip():
                cfg.logger.error(f"[{case_name(n)}] problog stderr: {err.strip()[:300]}")


# --- end inlined helper layer ----------------------------------------------
ROOT_DIR = Path(__file__).resolve().parent
DEFAULT_TAINT_BUNDLE = ROOT_DIR / "taint"

DEFAULT_BASE_DIR = SCRIPT_ROOT / "artifact" / "FMCADSC"
RUN_ROOT = "fmcad_sc_runs"
RESULT_FILENAME = "FMCADresult.tsv"
SOUFFLE_VARIANT_NAME = "souffle_rewrite"
PROBLOG_VARIANT_NAME = "problog"

# --- -dis (symbolization) defaults ------------------------------------------
DEFAULT_DIS_BASE_DIR = SCRIPT_ROOT / "artifact" / "runs" / "disasm"
DEFAULT_DIS_SOURCE_DL = ROOT_DIR / "symbolization" / "symbolization.dl"
DEFAULT_DIS_INPUTS_DIR = ROOT_DIR / "symbolization"
DIS_RESULT_FILENAME = "DISresult.tsv"
DIS_SHARED_DIRNAME = "_shared"
# (variant_name, run_extra). Compile is plain souffle for both — flags only
# kick in at runtime, matching how sc/taint gate det-opt + rewrite.
DIS_SOUFFLE_VARIANTS: List[Tuple[str, List[str]]] = [
    ("souffle_no_rewrite", ["--det-opt"]),
    ("souffle_implicit_rewrite", ["--det-opt", "--rewrite"]),
]

# --- -RQ1 (souffle --det-opt --rewrite, metric collection only) ---------------
DEFAULT_RQ1_BASE_DIR = SCRIPT_ROOT / "artifact" / "runs" / "rq1"
RQ1_RESULT_FILENAME = "RQ1result.tsv"
RQ1_RUN_ROOT = "rq1_runs"
RQ1_SOUFFLE_RUN_EXTRA = ["--det-opt", "--rewrite"]
RQ1_TAINT_RUN_EXTRA = ["--det-opt", "-r"]
RQ1_SC_VARIANTS: List[Variant] = [
    Variant(
        name="rq1_souffle",
        input_dir="input",
        output_dir="output_rq1",
        exe_name="compute_rq1",
        compile_extra=[],
        run_extra=RQ1_SOUFFLE_RUN_EXTRA,
    ),
]
RQ1_DIS_VARIANT_NAME = "rq1_souffle"
# before_prune_* / after_prune_* are emitted by the PRUNING stage; the latter
# is also echoed in FC_WMC_HYBRID. Final rewrite counts are emitted in
# FC_WMC_HYBRID as rewrite_final_* (or after_rewrite_* in newer logs).
# The pair after_prune_* / rewrite_final_* are the authoritative
# node/edge counts before/after the rewrite pass; before_prune_* are the raw
# graph counts prior to pruning.
RQ1_NODES_BEFORE_PRUNE_KEYS = ["before_prune_nodes"]
RQ1_EDGES_BEFORE_PRUNE_KEYS = ["before_prune_edges"]
RQ1_NODES_BEFORE_KEYS = ["after_prune_nodes"]
RQ1_EDGES_BEFORE_KEYS = ["after_prune_edges"]
RQ1_NODES_AFTER_KEYS = ["after_rewrite_nodes", "rewrite_final_nodes"]
RQ1_EDGES_AFTER_KEYS = ["after_rewrite_edges", "rewrite_final_edges"]
RQ1_SUMMARY_HEADER = [
    "Category",
    "Case",
    "Nodes_Before_Prune",
    "Nodes_Before",
    "Nodes_After",
    "Node_Rate_Pct",
    "Edges_Before_Prune",
    "Edges_Before",
    "Edges_After",
    "Edge_Rate_Pct",
]
RQ1_CATEGORY_SUMMARY_HEADER = [
    "Category",
    "Cases",
    "Nodes_Before_Prune_Avg",
    "Nodes_Before_Prune_Max",
    "Nodes_Before_Prune_Med",
    "Nodes_Before_Avg",
    "Nodes_Before_Max",
    "Nodes_Before_Med",
    "Nodes_After_Avg",
    "Nodes_After_Max",
    "Nodes_After_Med",
    "Edges_Before_Prune_Avg",
    "Edges_Before_Prune_Max",
    "Edges_Before_Prune_Med",
    "Edges_Before_Avg",
    "Edges_Before_Max",
    "Edges_Before_Med",
    "Edges_After_Avg",
    "Edges_After_Max",
    "Edges_After_Med",
    "Node_Rate_Pct_Avg",
    "Node_Rate_Pct_Max",
    "Node_Rate_Pct_Med",
    "Edge_Rate_Pct_Avg",
    "Edge_Rate_Pct_Max",
    "Edge_Rate_Pct_Med",
]

# --- -RQ3 (runtime comparison across souffle configs) ------------------------
# --det-opt is always on. The paper comparison runs rewrite off and on.
#   01 : --det-opt                 (no rewrite)
#   11 : --det-opt -r              (with rewrite)
DEFAULT_RQ3_BASE_DIR = SCRIPT_ROOT / "artifact" / "runs" / "rq3"
RQ3_RESULT_FILENAME = "RQ3result.tsv"
RQ3_RUN_ROOT = "rq3_runs"
RQ3_DEFAULT_TIMEOUT = 1800  # 30 minutes — matches RQ2; no mem cap unless --mem-limit-mb set.
RQ3_VARIANT_SPECS: List[Tuple[str, List[str], List[str]]] = [
    ("01", [], ["--det-opt"]),
    ("11", [], ["--det-opt", "-r"]),
]
RQ3_SPLIT_VARIANT_SPECS: List[Tuple[str, List[str], List[str]]] = [
    ("11_split", [], ["--det-opt", "-r"]),
    ("11_nosplit", [], ["--det-opt", "-r", "--split-mode", "no-split"]),
]
# Single full-only executable shared by both variants; runtime flags differ.
RQ3_SC_VARIANTS: List[Variant] = [
    Variant(
        name=name,
        input_dir="input",
        output_dir=f"output_rq3_{name}",
        exe_name="compute_rq3",
        compile_extra=list(compile_flags),
        run_extra=list(run_flags),
    )
    for name, compile_flags, run_flags in RQ3_VARIANT_SPECS
]
RQ3_SUMMARY_HEADER = [
    "Category",
    "Case",
    "Variant",
    "Runs",
    "Avg_s",
    "Min_s",
    "Max_s",
    "RandVars",
    "Status",
    "Note",
]

# --- -RQ2 (souffle --det-opt -r + problog runtime comparison) -----------------
# One souffle variant (--det-opt -r) plus problog, compared per case across
# sidechannel / taint / symbolization (disasm). Default per-run timeout is
# 1800s (30 min); no per-process memory cap unless --mem-limit-mb is set.
DEFAULT_RQ2_BASE_DIR = SCRIPT_ROOT / "artifact" / "runs" / "rq2"
RQ2_RESULT_FILENAME = "RQ2result.tsv"
RQ2_RUN_ROOT = "rq2_runs"
RQ2_DEFAULT_TIMEOUT = 1800  # 30 minutes
RQ2_SOUFFLE_VARIANT = "souffle_det_opt_r"
RQ2_PROBLOG_VARIANT = "problog"
RQ2_VPROBLOG_VARIANT = "vproblog"
RQ2_SCALLOP_VARIANT = "scallop"
RQ2_ALL_ENGINES = ("souffle", "problog", "vproblog", "scallop")
_RQ2_ENGINE_ALIASES = {
    "souffle": "souffle",
    "souffle_det_opt_r": "souffle",
    RQ2_SOUFFLE_VARIANT: "souffle",
    "problog": "problog",
    "vproblog": "vproblog",
    "vlog": "vproblog",
    "scallop": "scallop",
    "scli": "scallop",
}

# --- Scallop (exact WMC) engine config ---------------------------------------
# Scallop runs the *same* program each baseline uses, pre-materialised in its
# .scl dialect as `compute.scl` next to each case's compute.problog.dl, and
# evaluated with an exact WMC provenance.  `topkproofs` with k >> #proofs
# performs zero proof-truncation, so the marginal it reports is the exact
# SDD-WMC value (verified bit-identical to ProbLog on side_channel).
# Overridable via env for experiments.
SCALLOP_PROVENANCE = os.environ.get("SCALLOP_PROVENANCE", "topkproofs")
SCALLOP_TOP_K = os.environ.get("SCALLOP_TOP_K", "1000000000")
# Memory ceiling (MB) applied to scli via `ulimit -v`, so an exploding exact-WMC
# case fails as OOM instead of thrashing the host.  0 disables the cap.
SCALLOP_MEM_LIMIT_MB = int(os.environ.get("SCALLOP_MEM_LIMIT_MB", "12000"))
# Candidate order matters: the pinned patched build (adds the `band` foreign
# fn side_channel/P1 needs) must beat any unpatched `scli` that happens to be
# on PATH (e.g. a plain `cargo install` at ~/.cargo/bin/scli).
SCALLOP_BIN_CANDIDATES = [
    *([os.environ["SCLI_BIN"]] if os.environ.get("SCLI_BIN") else []),
    str(Path.home() / "scallop" / "target" / "release" / "scli"),
    "/usr/local/bin/scli",
    "scli",
]
# Default vlog binary path mirrors what the Dockerfile installs at
# /opt/vproblog. Override via $VLOG_BIN or --vlog-bin if running outside the
# container. Pre-built artefacts live at <case>/vproblog/ (sc/symbolization)
# or <case>/vproblog/<stage>/ (taint) and are committed to the repo.
DEFAULT_VLOG_BIN = Path(
    os.environ.get("VLOG_BIN", "/opt/vproblog/src/vlog-beta-sdd/build/vlog")
)
DEFAULT_RQ2_DIS_SOURCE_DL = ROOT_DIR / "symbolization" / "symbolization.dl"
DEFAULT_RQ2_DIS_INPUTS_DIR = ROOT_DIR / "symbolization"
# Stage suffix the legacy taint bundle used; the shim renames each new short
# stage (e.g. cipt-cg) to its old name (cipt-cg-dlog) so the legacy
# pipeline / shared_build paths find their expected files.
RQ2_TAINT_STAGE_SUFFIX = "-dlog"
RQ2_SC_VARIANTS: List[Variant] = [
    Variant(
        name=RQ2_SOUFFLE_VARIANT,
        input_dir="input",
        output_dir="output_rq2",
        exe_name="compute_rq2",
        compile_extra=[],
        run_extra=["--det-opt", "-r"],
    ),
]
RQ2_SUMMARY_HEADER = [
    "Category",
    "Case",
    "Engine",
    "Runs",
    "Avg_s",
    "Min_s",
    "Max_s",
    "Status",
    "Note",
]

# Standalone exact BDD inference with rewrite enabled at runtime.
SOUFFLE_REWRITE_VARIANTS: List[Variant] = [
    Variant(
        name=SOUFFLE_VARIANT_NAME,
        input_dir="input",
        output_dir="output_souffle_rewrite",
        exe_name="compute_fmcad",
        compile_extra=[],
        run_extra=["--det-opt", "--rewrite"],
    ),
]


# --- Collection --------------------------------------------------------------

SUMMARY_HEADER = [
    "Case",
    "Souffle_Runs",
    "Souffle_Avg_s",
    "Souffle_CreateGraph_s",
    "Souffle_Status",
    "ProbLog_Runs",
    "ProbLog_Avg_s",
    "ProbLog_Status",
]

_LOG_RUN_RX = re.compile(
    r"\[(?P<case>P\d+)\]\s+(?P<variant>souffle_rewrite|problog)\s+run\s+"
    r"(?:(?P<run_idx>\d+)/\d+\s+)?"
    r"(?P<status>OK|timeout|TIMEOUT|failed)"
    r"(?:\s+\(exit=\d+\))?"
    r"\s+(?:in|at)\s+(?P<elapsed>[0-9.]+)s"
)


def _avg(xs: Sequence[float]) -> Optional[float]:
    return (sum(xs) / len(xs)) if xs else None


def _fmt_secs(v: Optional[float]) -> str:
    return f"{v:.6f}" if v is not None else ""


def _iter_run_metas(runs_root: Path) -> List[Tuple[int, Path, dict]]:
    """Return (run_idx, run_dir, meta) sorted by run index, newest per idx."""
    latest: Dict[int, Tuple[str, Path, dict]] = {}
    if not runs_root.exists():
        return []
    for run_dir in sorted(runs_root.iterdir()):
        if not run_dir.is_dir():
            continue
        meta_path = run_dir / "run.meta.json"
        if not meta_path.exists():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        run_idx = meta.get("run")
        if not isinstance(run_idx, int):
            continue
        stamp = str(meta.get("timestamp", ""))
        prev = latest.get(run_idx)
        if prev is None or stamp >= prev[0]:
            latest[run_idx] = (stamp, run_dir, meta)
    return [(idx, p, m) for idx, (_, p, m) in sorted(latest.items())]


def _summarize_runs(
    entries: Sequence[Tuple[int, Path, dict]],
    stage_key: Optional[str] = None,
) -> Optional[dict]:
    if not entries:
        return None
    elapsed: List[float] = []
    stage_vals: List[float] = []
    timeouts = 0
    fails = 0
    for _, _, meta in entries:
        exit_code = meta.get("exit")
        if exit_code == 124:
            timeouts += 1
        elif exit_code not in (0, None):
            fails += 1
        el = meta.get("elapsed")
        if isinstance(el, (int, float)):
            elapsed.append(float(el))
        if stage_key:
            stages = meta.get("stage_times_seconds") or {}
            for alias in _stage_name_aliases(stage_key):
                sv = stages.get(alias)
                if isinstance(sv, (int, float)):
                    stage_vals.append(float(sv))
                    break
    status = "OK"
    if timeouts and timeouts == len(entries):
        status = "TIMEOUT"
    elif fails and fails == len(entries):
        status = "FAIL"
    elif timeouts or fails:
        status = "PARTIAL"
    return {
        "runs": len(entries),
        "avg_elapsed": _avg(elapsed),
        "avg_stage": _avg(stage_vals) if stage_key else None,
        "status": status,
    }


def _collect_case_from_metas(
    base_dir: Path, n: int
) -> Tuple[Optional[dict], Optional[dict]]:
    cdir = case_dir(base_dir, n)
    souffle = _summarize_runs(
        _iter_run_metas(cdir / RUN_ROOT / SOUFFLE_VARIANT_NAME),
        stage_key=CREATE_GRAPH_STAGE,
    )
    problog = _summarize_runs(
        _iter_run_metas(cdir / RUN_ROOT / PROBLOG_VARIANT_NAME),
    )
    return souffle, problog


def _collect_from_log(log_path: Path) -> Dict[str, Dict[str, dict]]:
    """Fallback: parse fmcad_sc.log for per-case elapsed times and skip notes."""
    out: Dict[str, Dict[str, dict]] = {}
    if not log_path.exists():
        return out
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _LOG_RUN_RX.search(line)
        if m:
            case = m.group("case")
            variant = m.group("variant")
            status_raw = m.group("status").lower()
            elapsed = float(m.group("elapsed"))
            bucket = out.setdefault(case, {}).setdefault(
                variant, {"elapsed": [], "status": "OK"}
            )
            bucket["elapsed"].append(elapsed)
            if status_raw == "timeout":
                bucket["status"] = "TIMEOUT"
            elif status_raw == "failed" and bucket["status"] != "TIMEOUT":
                bucket["status"] = "FAIL"
    return out


def _fallback_from_log(
    log_fallback: Dict[str, Dict[str, dict]], case: str, variant: str
) -> Optional[dict]:
    lf = log_fallback.get(case, {}).get(variant)
    if not lf:
        return None
    if lf.get("elapsed"):
        return {
            "runs": len(lf["elapsed"]),
            "avg_elapsed": _avg(lf["elapsed"]),
            "avg_stage": None,
            "status": lf.get("status", "OK"),
        }
    if lf.get("status", "").startswith("SKIP"):
        return {
            "runs": 0,
            "avg_elapsed": None,
            "avg_stage": None,
            "status": lf["status"],
        }
    return None


def collect_summary(
    base_dir: Path,
    cases: Sequence[int],
    logger: Optional[Logger] = None,
    log_file: Optional[Path] = None,
) -> List[List[str]]:
    log_fallback = _collect_from_log(log_file) if log_file else {}
    rows: List[List[str]] = []
    for n in cases:
        case = case_name(n)
        souffle, problog = _collect_case_from_metas(base_dir, n)

        if souffle is None:
            souffle = _fallback_from_log(log_fallback, case, "souffle_rewrite")
        if problog is None:
            problog = _fallback_from_log(log_fallback, case, "problog")

        s = souffle or {}
        p = problog or {}
        rows.append([
            case,
            str(s.get("runs", 0)),
            _fmt_secs(s.get("avg_elapsed")),
            _fmt_secs(s.get("avg_stage")),
            str(s.get("status", "MISSING")) if souffle else "MISSING",
            str(p.get("runs", 0)),
            _fmt_secs(p.get("avg_elapsed")),
            str(p.get("status", "MISSING")) if problog else "MISSING",
        ])

    if logger and not rows:
        logger.warn("No cases had collectable runtime data.")
    return rows


def _render_table(header: Sequence[str], rows: Sequence[Sequence[str]]) -> str:
    cols = [list(header)] + [list(r) for r in rows]
    widths = [max(len(str(c[i])) for c in cols) for i in range(len(header))]

    def fmt(row: Sequence[str]) -> str:
        return "  ".join(str(row[i]).ljust(widths[i]) for i in range(len(header)))

    lines = [fmt(header), "  ".join("-" * w for w in widths)]
    lines.extend(fmt(r) for r in rows)
    return "\n".join(lines)


def write_and_print_summary(
    base_dir: Path,
    rows: Sequence[Sequence[str]],
    logger: Logger,
) -> Path:
    out_tsv = base_dir / RESULT_FILENAME
    base_dir.mkdir(parents=True, exist_ok=True)
    with open(out_tsv, "w", encoding="utf-8") as f:
        f.write("\t".join(SUMMARY_HEADER) + "\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    logger.banner("FMCAD runtime summary")
    logger.info("\n" + _render_table(SUMMARY_HEADER, rows))
    logger.info(f"Wrote {out_tsv}")
    return out_tsv


# --- Commands ----------------------------------------------------------------


def cmd_sc(args: argparse.Namespace) -> None:
    base = Path(args.base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    source_root = resolve_source_dir(args.source_dir)

    log_file = Path(args.log_file) if args.log_file else (base / "fmcad_sc.log")

    if args.collect:
        # Don't truncate the log when collecting.
        logger = Logger(base, log_file, quiet=args.quiet)
        logger.banner("FMCAD --collect: aggregate runtime from existing logs")
        case_tokens = list(args.case_tokens) if args.case_tokens else []
        if case_tokens:
            cases = parse_case_tokens(case_tokens)
        elif args.cases or args.size:
            cases = select_cases(args.cases, args.size, base, source_root)
        else:
            cases = find_cases_on_disk(base) or select_cases(None, None, base, source_root)
        if not cases:
            logger.error(f"No cases found under {base}.")
            sys.exit(2)
        rows = collect_summary(base, cases, logger=logger, log_file=log_file)
        write_and_print_summary(base, rows, logger)
        return

    log_file.unlink(missing_ok=True)
    logger = Logger(base, log_file, quiet=args.quiet)
    logger.banner(
        "FMCAD -sc: GENERATE + COMPILE + RUN (Souffle rewrite + ProbLog)"
    )

    case_tokens = list(args.case_tokens) if args.case_tokens else []
    cases = (
        parse_case_tokens(case_tokens)
        if case_tokens
        else select_cases(args.cases, args.size, base, source_root)
    )
    if not cases:
        logger.error("No cases found. Provide --cases, case tokens, or populate --source-dir.")
        sys.exit(2)

    clean_rq_artifacts(base, cases, RUN_ROOT, RESULT_FILENAME, logger)

    if not args.skip_generate:
        ok = run_generate(
            base_dir=base,
            source_dir=str(source_root),
            cases=cases,
            cleanup=args.cleanup,
            timeout=args.generate_timeout,
            logger=logger,
        )
        if not ok:
            sys.exit(2)

    compile_cfg = CompileCfg(
        base_dir=base,
        cases=cases,
        timeout=args.compile_timeout,
        souffle_bin=args.souffle_bin,
        souffle_args=args.souffle_arg or [],
        include_dir=args.include_dir,
        derv_only=False,
        compile_det_opt=False,
        force_compile=args.force_compile,
        logger=logger,
    )
    run_cfg = RunCfg(
        base_dir=base,
        cases=cases,
        runs=args.runs,
        timeout=args.run_timeout,
        log_prefix=args.log_prefix,
        run_args=args.run_arg or [],
        logger=logger,
    )

    logger.info(f"Cases: {cases} (count={len(cases)})")
    logger.info(f"Runs per engine: {args.runs}")
    logger.info(f"Output root: {base}")

    for idx, n in enumerate(cases, 1):
        label = case_name(n)
        logger.info(f"[{idx}/{len(cases)}] Compile souffle_rewrite for {label}")
        compile_case(n, compile_cfg, SOUFFLE_REWRITE_VARIANTS)
        logger.info(f"[{idx}/{len(cases)}] Run souffle_rewrite for {label}")
        run_case(n, run_cfg, SOUFFLE_REWRITE_VARIANTS, RUN_ROOT)

        logger.info(f"[{idx}/{len(cases)}] Run ProbLog for {label}")
        run_problog_case(n, run_cfg, RUN_ROOT)

    logger.info(f"[FMCAD] -sc done. Results under {base}")
    rows = collect_summary(base, cases, logger=logger, log_file=log_file)
    write_and_print_summary(base, rows, logger)


# --- -dis (symbolization) ---------------------------------------------------


def discover_dis_cases(
    inputs_root: Path, case_filter: Optional[Sequence[str]]
) -> List[Path]:
    if not inputs_root.is_dir():
        return []
    all_cases = sorted(
        [p for p in inputs_root.iterdir() if p.is_dir()],
        key=lambda p: p.name,
    )
    if case_filter:
        wanted = {c.strip() for c in case_filter if c.strip()}
        return [p for p in all_cases if p.name in wanted]
    return all_cases


def discover_dis_case_inputs(
    inputs_root: Path, case_filter: Optional[Sequence[str]]
) -> Tuple[List[Tuple[str, Path]], bool]:
    """Return (case_name, facts_dir) pairs for symbolization.

    The current layout stores facts under symbolization/<case>/input/.  The
    legacy layout stores facts directly under the case directory.
    """
    if not inputs_root.is_dir():
        return [], False

    new_cases = sorted(
        [p for p in inputs_root.iterdir()
         if p.is_dir() and (p / "input").is_dir()],
        key=lambda p: p.name,
    )
    if new_cases:
        if case_filter:
            wanted = {c.strip() for c in case_filter if c.strip()}
            new_cases = [p for p in new_cases if p.name in wanted]
        return [(p.name, p / "input") for p in new_cases], True

    legacy_cases = discover_dis_cases(inputs_root, case_filter)
    return [(p.name, p) for p in legacy_cases], False


def _run_subproc_capture(
    cmd: Sequence[str],
    cwd: Optional[Path],
    timeout: int,
    log_path: Optional[Path],
    env: Optional[Dict[str, str]] = None,
) -> Tuple[int, float, str]:
    """Run cmd, capturing stdout+stderr to log_path. Returns (rc, elapsed, note)."""
    t0 = time.perf_counter()
    note = ""
    eff_timeout = timeout if timeout and timeout > 0 else None
    try:
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with log_path.open("w", encoding="utf-8") as fh:
                proc = subprocess.run(
                    list(cmd),
                    cwd=str(cwd) if cwd else None,
                    stdout=fh,
                    stderr=subprocess.STDOUT,
                    timeout=eff_timeout,
                    env=env,
                )
        else:
            proc = subprocess.run(
                list(cmd),
                cwd=str(cwd) if cwd else None,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=eff_timeout,
                env=env,
            )
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        return 124, time.perf_counter() - t0, "timeout"
    except FileNotFoundError as exc:
        return 127, time.perf_counter() - t0, f"notfound: {exc}"
    return rc, time.perf_counter() - t0, note


def _souffle_supports(bin_path: str, flag: str) -> bool:
    try:
        r = subprocess.run(
            [bin_path, "--help"], capture_output=True, text=True, timeout=30
        )
    except Exception:
        return False
    return flag in (r.stdout + r.stderr)


def _souffle_compile_dis(
    souffle_bin: str,
    src_dl: Path,
    probe_input_dir: Path,
    include_dir: str,
    build_dir: Path,
    exe_name: str,
    timeout: int,
    logger: Logger,
    label: str,
) -> Optional[Path]:
    """Compile src_dl into build_dir/exe_name. Returns the exe path or None."""
    build_dir.mkdir(parents=True, exist_ok=True)
    exe = build_dir / exe_name
    if exe.is_file():
        logger.info(f"[{label}] souffle binary cached at {exe}")
        return exe
    cmd: List[str] = [souffle_bin]
    if _souffle_supports(souffle_bin, "--full-only"):
        cmd.append("--full-only")
    cmd.extend([
        "--profile=/dev/null",
        "-F", str(probe_input_dir),
        "-D", str(build_dir / "compile_probe_output"),
        str(src_dl),
        "-I", include_dir,
        "-o", exe_name,
    ])
    log_path = build_dir / "compile.log"
    rc, elapsed, note = _run_subproc_capture(
        cmd, cwd=build_dir, timeout=timeout, log_path=log_path
    )
    if rc != 0 or not exe.is_file():
        logger.error(
            f"[{label}] souffle compile failed (rc={rc}, {note}, "
            f"{elapsed:.2f}s); see {log_path}"
        )
        return None
    logger.info(f"[{label}] souffle compile OK in {elapsed:.2f}s -> {exe}")
    return exe


def _run_souffle_dis_case(
    exe: Path,
    case: str,
    case_input: Path,
    variant_name: str,
    variant_dir: Path,
    run_flags: Sequence[str],
    runs: int,
    timeout: int,
    logger: Logger,
) -> Dict[str, object]:
    """Run a compiled souffle binary `runs` times against case_input.
    Per-run output goes to variant_dir/run_NN/output/. Stops the case after
    the first non-zero run (matches sc/taint per-variant loop semantics)."""
    elapseds: List[float] = []
    n_ok = 0
    status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = variant_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        out_dir = run_dir / "output"
        out_dir.mkdir(parents=True, exist_ok=True)
        log_path = run_dir / "souffle.log"
        cmd = [
            str(exe.resolve()),
            "-F", str(case_input.resolve()),
            "-D", str(out_dir.resolve()),
            *run_flags,
        ]
        rc, elapsed, note = _run_subproc_capture(
            cmd, cwd=run_dir, timeout=timeout, log_path=log_path
        )
        run_status = "ok" if rc == 0 else ("timeout" if rc == 124 else "error")
        meta = {
            "variant": variant_name,
            "case": case,
            "run": run_idx,
            "timestamp": stamp,
            "exit": rc,
            "elapsed": elapsed,
            "status": run_status,
            "note": note,
            "cmd": cmd,
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(meta, indent=2), encoding="utf-8"
        )
        elapseds.append(elapsed)
        if rc == 0:
            n_ok += 1
            logger.info(
                f"[{case}] {variant_name} run {run_idx}/{runs} OK in {elapsed:.2f}s"
            )
            continue
        status = "timeout" if rc == 124 else "error"
        last_note = note or f"exit={rc}"
        logger.error(
            f"[{case}] {variant_name} run {run_idx}/{runs} {status} in {elapsed:.2f}s"
        )
        break
    return {
        "runs_completed": n_ok,
        "runs_attempted": len(elapseds),
        "avg_elapsed": (sum(elapseds) / len(elapseds)) if elapseds else None,
        "min_elapsed": min(elapseds) if elapseds else None,
        "max_elapsed": max(elapseds) if elapseds else None,
        "total_elapsed": sum(elapseds) if elapseds else None,
        "status": status,
        "note": last_note,
    }


DIS_SUMMARY_HEADER = [
    "Case",
    "Engine",
    "Runs",
    "Avg_s",
    "Min_s",
    "Max_s",
    "Status",
    "Note",
]


def _write_dis_summary(
    base_dir: Path, rows: Sequence[Dict[str, object]], logger: Logger
) -> Path:
    out_tsv = base_dir / DIS_RESULT_FILENAME
    base_dir.mkdir(parents=True, exist_ok=True)
    flat_rows: List[List[str]] = []
    for record in rows:
        case = str(record.get("case", ""))
        for engine in (
            "souffle_no_rewrite",
            "souffle_implicit_rewrite",
        ):
            stats = record.get(engine)
            if not isinstance(stats, dict):
                flat_rows.append([case, engine, "0", "", "", "", "MISSING", ""])
                continue
            flat_rows.append([
                case,
                engine,
                str(stats.get("runs_completed", 0)),
                _fmt_secs(stats.get("avg_elapsed")),
                _fmt_secs(stats.get("min_elapsed")),
                _fmt_secs(stats.get("max_elapsed")),
                str(stats.get("status", "")),
                str(stats.get("note", "")),
            ])
    with out_tsv.open("w", encoding="utf-8") as fh:
        fh.write("\t".join(DIS_SUMMARY_HEADER) + "\n")
        for r in flat_rows:
            fh.write("\t".join(r) + "\n")
    logger.banner("FMCAD -dis runtime summary")
    logger.info("\n" + _render_table(DIS_SUMMARY_HEADER, flat_rows))
    logger.info(f"Wrote {out_tsv}")
    return out_tsv


def cmd_dis(args: argparse.Namespace) -> None:
    base = Path(args.dis_base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    log_file = (
        Path(args.dis_log_file)
        if args.dis_log_file
        else (base / "fmcad_dis.log")
    )
    log_file.unlink(missing_ok=True)
    logger = Logger(base, log_file, quiet=args.quiet)
    logger.banner(
        "FMCAD -dis: SYMBOLIZATION (souffle no-rewrite + implicit-rewrite)"
    )

    source_dl = Path(args.dis_source_dl)
    if not source_dl.is_absolute():
        source_dl = (ROOT_DIR / source_dl).resolve()
    if not source_dl.is_file():
        logger.error(f"Symbolization .dl not found: {source_dl}")
        sys.exit(2)

    inputs_root = Path(args.dis_inputs_dir)
    if not inputs_root.is_absolute():
        inputs_root = (ROOT_DIR / inputs_root).resolve()
    if not inputs_root.is_dir():
        logger.error(f"Symbolization inputs root not found: {inputs_root}")
        sys.exit(2)

    case_filter: Optional[List[str]] = None
    if args.dis_cases:
        case_filter = [c for c in re.split(r"[,\s]+", args.dis_cases) if c]
    case_inputs, new_layout = discover_dis_case_inputs(inputs_root, case_filter)
    if not case_inputs:
        logger.error(f"No symbolization cases under {inputs_root}")
        sys.exit(2)

    logger.info(f"Cases: {[name for name, _ in case_inputs]} (count={len(case_inputs)})")
    logger.info(f"Source DL: {source_dl}")
    logger.info(f"Inputs root: {inputs_root}")
    logger.info(f"Inputs layout: {'<case>/input' if new_layout else '<case>'}")
    logger.info(f"Output root: {base}")
    logger.info(f"Runs per engine: {args.runs}, run timeout: {args.dis_run_timeout}s")

    shared_dir = base / DIS_SHARED_DIRNAME
    souffle_builds: Dict[str, Optional[Path]] = {}
    probe_input = case_inputs[0][1]
    for variant_name, _ in DIS_SOUFFLE_VARIANTS:
        build_dir = shared_dir / "souffle_build" / variant_name
        exe = _souffle_compile_dis(
            souffle_bin=args.souffle_bin,
            src_dl=source_dl,
            probe_input_dir=probe_input,
            include_dir=args.include_dir,
            build_dir=build_dir,
            exe_name="compute_symbol",
            timeout=args.compile_timeout,
            logger=logger,
            label=variant_name,
        )
        souffle_builds[variant_name] = exe

    rows: List[Dict[str, object]] = []
    for idx, (case, case_input) in enumerate(case_inputs, 1):
        case_out = base / case
        case_out.mkdir(parents=True, exist_ok=True)
        logger.info(f"[{idx}/{len(case_inputs)}] Case {case}")
        record: Dict[str, object] = {"case": case}

        for variant_name, run_flags in DIS_SOUFFLE_VARIANTS:
            exe = souffle_builds.get(variant_name)
            variant_dir = case_out / variant_name
            variant_dir.mkdir(parents=True, exist_ok=True)
            if exe is None:
                logger.warn(f"[{case}] {variant_name}: souffle binary missing")
                record[variant_name] = {
                    "runs_completed": 0, "avg_elapsed": None,
                    "min_elapsed": None, "max_elapsed": None,
                    "status": "compile_failed", "note": "no binary",
                }
                continue
            record[variant_name] = _run_souffle_dis_case(
                exe=exe, case=case, case_input=case_input,
                variant_name=variant_name, variant_dir=variant_dir,
                run_flags=run_flags, runs=args.runs,
                timeout=args.dis_run_timeout, logger=logger,
            )

        rows.append(record)

    logger.info(f"[FMCAD] -dis done. Results under {base}")
    _write_dis_summary(base, rows, logger)


# --- -RQ1 (rewrite metric collection) -----------------------------------------


def _rq1_first_float(
    stage_info: Dict[str, Dict[str, str]], keys: Sequence[str]
) -> Optional[float]:
    preferred = (FC_WMC_HYBRID_STAGE, FC_STAGE, WMC_STAGE)
    for stage_name in preferred:
        info = stage_info.get(stage_name) or {}
        for key in keys:
            val = info.get(key)
            if val not in (None, ""):
                try:
                    return float(val)
                except (TypeError, ValueError):
                    pass
    for info in stage_info.values():
        for key in keys:
            val = info.get(key)
            if val not in (None, ""):
                try:
                    return float(val)
                except (TypeError, ValueError):
                    pass
    return None


def _rq1_turn_info(log_path: Path) -> Dict[str, str]:
    """Read the turn-level fallback for legacy before_prune_* counters."""
    try:
        data = json.loads(log_path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    turns = data.get("turns") or []
    if not turns or not isinstance(turns[0], dict):
        return {}
    info = turns[0].get("info")
    if not isinstance(info, dict):
        return {}
    return {str(k): str(v) for k, v in info.items()}


def _first_float_from_info(info: Dict[str, str], keys: Sequence[str]) -> Optional[float]:
    for key in keys:
        val = info.get(key)
        if val not in (None, ""):
            try:
                return float(val)
            except (TypeError, ValueError):
                pass
    return None


def _rq1_extract_fields(log_path: Path) -> Dict[str, Optional[float]]:
    _, stage_info = parse_souffle_stage_data(log_path)
    turn_info = _rq1_turn_info(log_path)

    def _lookup(keys: Sequence[str]) -> Optional[float]:
        v = _rq1_first_float(stage_info, keys)
        if v is not None:
            return v
        return _first_float_from_info(turn_info, keys)

    return {
        "nodes_before_prune": _lookup(RQ1_NODES_BEFORE_PRUNE_KEYS),
        "edges_before_prune": _lookup(RQ1_EDGES_BEFORE_PRUNE_KEYS),
        "nodes_before": _lookup(RQ1_NODES_BEFORE_KEYS),
        "nodes_after": _lookup(RQ1_NODES_AFTER_KEYS),
        "edges_before": _lookup(RQ1_EDGES_BEFORE_KEYS),
        "edges_after": _lookup(RQ1_EDGES_AFTER_KEYS),
    }


def _rq1_aggregate_logs(
    log_paths: Sequence[Path],
) -> Dict[str, Optional[float]]:
    """Average node/edge counts across runs, then compute rates."""
    nbp: List[float] = []
    ebp: List[float] = []
    nb: List[float] = []
    na: List[float] = []
    eb: List[float] = []
    ea: List[float] = []
    for p in log_paths:
        if p is None or not p.is_file():
            continue
        f = _rq1_extract_fields(p)
        if f["nodes_before_prune"] is not None:
            nbp.append(f["nodes_before_prune"])
        if f["edges_before_prune"] is not None:
            ebp.append(f["edges_before_prune"])
        if f["nodes_before"] is not None:
            nb.append(f["nodes_before"])
        if f["nodes_after"] is not None:
            na.append(f["nodes_after"])
        if f["edges_before"] is not None:
            eb.append(f["edges_before"])
        if f["edges_after"] is not None:
            ea.append(f["edges_after"])
    avg_nbp = _avg(nbp)
    avg_ebp = _avg(ebp)
    avg_nb = _avg(nb)
    avg_na = _avg(na)
    avg_eb = _avg(eb)
    avg_ea = _avg(ea)
    n_rate = (
        (avg_nb - avg_na) / avg_nb
        if (avg_nb is not None and avg_na is not None and avg_nb > 0)
        else None
    )
    e_rate = (
        (avg_eb - avg_ea) / avg_eb
        if (avg_eb is not None and avg_ea is not None and avg_eb > 0)
        else None
    )
    return {
        "nodes_before_prune": avg_nbp,
        "edges_before_prune": avg_ebp,
        "nodes_before": avg_nb,
        "nodes_after": avg_na,
        "edges_before": avg_eb,
        "edges_after": avg_ea,
        "node_rate": n_rate,
        "edge_rate": e_rate,
    }


def _rq1_sum_aggs(
    aggs: Sequence[Dict[str, Optional[float]]],
) -> Dict[str, Optional[float]]:
    """Sum nodes/edges across sub-aggregates (used for the taint pipeline
    where metrics are emitted per stage). Reduction rate is recomputed from
    the summed totals."""
    def _add(cur: Optional[float], delta: Optional[float]) -> Optional[float]:
        if delta is None:
            return cur
        return (cur or 0.0) + delta

    nbp = ebp = nb = na = eb = ea = None
    for a in aggs:
        nbp = _add(nbp, a.get("nodes_before_prune"))
        ebp = _add(ebp, a.get("edges_before_prune"))
        nb = _add(nb, a.get("nodes_before"))
        na = _add(na, a.get("nodes_after"))
        eb = _add(eb, a.get("edges_before"))
        ea = _add(ea, a.get("edges_after"))
    n_rate = (
        (nb - na) / nb
        if (nb is not None and na is not None and nb > 0)
        else None
    )
    e_rate = (
        (eb - ea) / eb
        if (eb is not None and ea is not None and eb > 0)
        else None
    )
    return {
        "nodes_before_prune": nbp,
        "edges_before_prune": ebp,
        "nodes_before": nb,
        "nodes_after": na,
        "edges_before": eb,
        "edges_after": ea,
        "node_rate": n_rate,
        "edge_rate": e_rate,
    }


def _rq1_compile_souffle(
    souffle_bin: str,
    src_dl: Path,
    cwd: Path,
    exe_name: str,
    include_dir: str,
    timeout: int,
    logger: Logger,
    label: str,
    force: bool = False,
) -> Optional[Path]:
    """RQ1's own souffle compile — drops --online (per user request) but keeps
    --full-only when supported. No --derv-only so the probabilistic outputs
    (facts.prob) and FC_WMC_HYBRID stage info land in the output dir."""
    cwd.mkdir(parents=True, exist_ok=True)
    exe = cwd / exe_name
    if exe.is_file() and not force:
        logger.info(f"[{label}] souffle binary cached at {exe}")
        return exe
    if exe.is_file() and force:
        try:
            exe.unlink()
        except OSError:
            pass
    dl_copy = cwd / src_dl.name
    if src_dl.resolve() != dl_copy.resolve():
        shutil.copyfile(src_dl, dl_copy)
    cmd: List[str] = [souffle_bin]
    if _souffle_supports(souffle_bin, "--full-only"):
        cmd.append("--full-only")
    cmd.extend([
        "--profile=/dev/null",
        dl_copy.name,
        "-I", include_dir,
        "-o", exe_name,
    ])
    log_path = cwd / "compile.log"
    rc, elapsed, note = _run_subproc_capture(
        cmd, cwd=cwd, timeout=timeout, log_path=log_path
    )
    if rc != 0 or not exe.is_file():
        logger.error(
            f"[{label}] souffle compile failed (rc={rc}, {note}, "
            f"{elapsed:.2f}s); see {log_path}"
        )
        return None
    logger.info(f"[{label}] souffle compile OK in {elapsed:.2f}s -> {exe}")
    return exe


def _rq1_row(
    category: str,
    case_label: str,
    agg: Dict[str, Optional[float]],
) -> List[str]:
    def _num(v: Optional[float], digits: int) -> str:
        return f"{v:.{digits}f}" if isinstance(v, (int, float)) else ""

    def _pct(v: Optional[float], digits: int = 4) -> str:
        return f"{v * 100:.{digits}f}%" if isinstance(v, (int, float)) else ""

    return [
        category,
        case_label,
        _num(agg.get("nodes_before_prune"), 3),
        _num(agg.get("nodes_before"), 3),
        _num(agg.get("nodes_after"), 3),
        _pct(agg.get("node_rate")),
        _num(agg.get("edges_before_prune"), 3),
        _num(agg.get("edges_before"), 3),
        _num(agg.get("edges_after"), 3),
        _pct(agg.get("edge_rate")),
    ]


def _rq1_category_summary(
    rows: Sequence[Sequence[str]],
) -> List[List[str]]:
    """Collapse per-case rows into one row per category with avg/max/median
    over node/edge counts (before and after rewrite) and reduction rates."""
    def _num(v: Optional[float], digits: int) -> str:
        return f"{v:.{digits}f}" if isinstance(v, (int, float)) else ""

    def _pct(v: Optional[float], digits: int = 4) -> str:
        return f"{v * 100:.{digits}f}%" if isinstance(v, (int, float)) else ""

    def _parse(s: str) -> Optional[float]:
        if s == "":
            return None
        s2 = s.rstrip("%")
        try:
            v = float(s2)
        except (TypeError, ValueError):
            return None
        return v / 100.0 if s.endswith("%") else v

    # Per-category vectors in row order: nbp, nb, na, ebp, eb, ea, n_rate, e_rate
    by_cat: Dict[str, List[List[float]]] = {}
    cat_order: List[str] = []
    for r in rows:
        if len(r) < len(RQ1_SUMMARY_HEADER):
            continue
        cat = r[0]
        if cat not in by_cat:
            by_cat[cat] = [[], [], [], [], [], [], [], []]
            cat_order.append(cat)
        vals = [_parse(r[2]), _parse(r[3]), _parse(r[4]),
                _parse(r[6]), _parse(r[7]), _parse(r[8]),
                _parse(r[5]), _parse(r[9])]
        for i, v in enumerate(vals):
            if v is not None:
                by_cat[cat][i].append(v)

    def _stats3(xs: Sequence[float]) -> Tuple[Optional[float], Optional[float], Optional[float]]:
        if not xs:
            return None, None, None
        return (sum(xs) / len(xs), max(xs), statistics.median(xs))

    out: List[List[str]] = []
    for cat in cat_order:
        nbp, nb, na, ebp, eb, ea, nr, er = by_cat[cat]
        # Case count = number of rows for this category that contributed at
        # least one numeric field. Pick the most-populated of the raw counts.
        cases = max(len(nbp), len(nb), len(na), len(ebp), len(eb), len(ea),
                    len(nr), len(er))
        (nbp_a, nbp_m, nbp_md) = _stats3(nbp)
        (nb_a, nb_m, nb_md) = _stats3(nb)
        (na_a, na_m, na_md) = _stats3(na)
        (ebp_a, ebp_m, ebp_md) = _stats3(ebp)
        (eb_a, eb_m, eb_md) = _stats3(eb)
        (ea_a, ea_m, ea_md) = _stats3(ea)
        (nr_a, nr_m, nr_md) = _stats3(nr)
        (er_a, er_m, er_md) = _stats3(er)
        out.append([
            cat,
            str(cases),
            _num(nbp_a, 3), _num(nbp_m, 3), _num(nbp_md, 3),
            _num(nb_a, 3), _num(nb_m, 3), _num(nb_md, 3),
            _num(na_a, 3), _num(na_m, 3), _num(na_md, 3),
            _num(ebp_a, 3), _num(ebp_m, 3), _num(ebp_md, 3),
            _num(eb_a, 3), _num(eb_m, 3), _num(eb_md, 3),
            _num(ea_a, 3), _num(ea_m, 3), _num(ea_md, 3),
            _pct(nr_a), _pct(nr_m), _pct(nr_md),
            _pct(er_a), _pct(er_m), _pct(er_md),
        ])
    return out


def _run_rq1_sidechannel(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    sc_base = base / "sidechannel"
    sc_base.mkdir(parents=True, exist_ok=True)
    source_root = resolve_source_dir(args.source_dir)

    case_tokens = list(args.case_tokens) if args.case_tokens else []
    cases = (
        parse_case_tokens(case_tokens)
        if case_tokens
        else select_cases(args.cases, args.size, sc_base, source_root)
    )
    if not cases:
        logger.warn("[RQ1/sc] no side-channel cases discovered; skipping.")
        return []

    if not args.skip_generate:
        ok = run_generate(
            base_dir=sc_base,
            source_dir=str(source_root),
            cases=cases,
            cleanup=args.cleanup,
            timeout=args.generate_timeout,
            logger=logger,
        )
        if not ok:
            logger.error("[RQ1/sc] generate step failed")
            return []

    run_cfg = RunCfg(
        base_dir=sc_base,
        cases=cases,
        runs=args.runs,
        timeout=args.rq1_run_timeout,
        log_prefix=args.log_prefix,
        run_args=args.run_arg or [],
        logger=logger,
    )

    rows: List[List[str]] = []
    variant = RQ1_SC_VARIANTS[0]
    for idx, n in enumerate(cases, 1):
        label = case_name(n)
        logger.info(f"[RQ1/sc {idx}/{len(cases)}] {label}")
        cdir = case_dir(sc_base, n)
        src_dl = cdir / "compute.souffle.dl"
        if not src_dl.is_file():
            logger.warn(f"[RQ1/sc] {label} missing {src_dl.name}; skipping")
            continue
        exe = _rq1_compile_souffle(
            souffle_bin=args.souffle_bin,
            src_dl=src_dl,
            cwd=cdir,
            exe_name=variant.exe_name,
            include_dir=args.include_dir,
            timeout=args.compile_timeout,
            logger=logger,
            label=f"RQ1/sc {label}",
            force=args.force_compile,
        )
        if exe is None:
            continue
        run_case(n, run_cfg, RQ1_SC_VARIANTS, RQ1_RUN_ROOT)
        variant_root = case_dir(sc_base, n) / RQ1_RUN_ROOT / RQ1_SC_VARIANTS[0].name
        log_paths: List[Path] = []
        if variant_root.is_dir():
            for run_dir in sorted(variant_root.iterdir()):
                if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                    continue
                lp = find_run_log_json(run_dir)
                if lp is not None:
                    log_paths.append(lp)
        rows.append(_rq1_row("sidechannel", label, _rq1_aggregate_logs(log_paths)))
    return rows


def _run_rq1_symbolization(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    source_dl = Path(args.dis_source_dl)
    if not source_dl.is_absolute():
        source_dl = (ROOT_DIR / source_dl).resolve()
    inputs_root = Path(args.dis_inputs_dir)
    if not inputs_root.is_absolute():
        inputs_root = (ROOT_DIR / inputs_root).resolve()
    if not source_dl.is_file() or not inputs_root.is_dir():
        logger.warn(
            f"[RQ1/dis] missing inputs (dl={source_dl}, inputs={inputs_root}); "
            f"skipping symbolization."
        )
        return []

    dis_base = base / "symbolization"
    dis_base.mkdir(parents=True, exist_ok=True)
    case_filter: Optional[List[str]] = None
    if args.dis_cases:
        case_filter = [c for c in re.split(r"[,\s]+", args.dis_cases) if c]
    case_inputs, new_layout = discover_dis_case_inputs(inputs_root, case_filter)
    if not case_inputs:
        logger.warn(f"[RQ1/dis] no cases under {inputs_root}; skipping.")
        return []
    logger.info(f"[RQ1/dis] inputs layout: {'<case>/input' if new_layout else '<case>'}")

    shared_dir = dis_base / DIS_SHARED_DIRNAME
    build_dir = shared_dir / "souffle_build" / RQ1_DIS_VARIANT_NAME
    exe = _rq1_compile_souffle(
        souffle_bin=args.souffle_bin,
        src_dl=source_dl,
        cwd=build_dir,
        exe_name="compute_symbol_rq1",
        include_dir=args.include_dir,
        timeout=args.compile_timeout,
        logger=logger,
        label=f"RQ1/dis {RQ1_DIS_VARIANT_NAME}",
        force=args.force_compile,
    )
    if exe is None:
        logger.error("[RQ1/dis] souffle compile failed; skipping symbolization")
        return []

    rows: List[List[str]] = []
    for idx, (case, case_input) in enumerate(case_inputs, 1):
        logger.info(f"[RQ1/dis {idx}/{len(case_inputs)}] {case}")
        variant_dir = dis_base / case / RQ1_DIS_VARIANT_NAME
        variant_dir.mkdir(parents=True, exist_ok=True)
        _run_souffle_dis_case(
            exe=exe,
            case=case,
            case_input=case_input,
            variant_name=RQ1_DIS_VARIANT_NAME,
            variant_dir=variant_dir,
            run_flags=RQ1_SOUFFLE_RUN_EXTRA,
            runs=args.runs,
            timeout=args.rq1_run_timeout,
            logger=logger,
        )
        log_paths: List[Path] = []
        for run_dir in sorted(variant_dir.iterdir()):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            lp = find_run_log_json(run_dir)
            if lp is not None:
                log_paths.append(lp)
        rows.append(_rq1_row("symbolization", case, _rq1_aggregate_logs(log_paths)))
    return rows


def _taint_detect_souffle_bin(explicit: Optional[str]) -> str:
    if explicit:
        return explicit
    env = os.environ.get("SOUFFLE_BIN")
    if env:
        return env
    return "souffle"


def _taint_read_stages(stages_file: Path) -> List[str]:
    result: List[str] = []
    for raw in stages_file.read_text(encoding="utf-8").splitlines():
        text = raw.strip()
        if not text or text.startswith("#"):
            continue
        result.append(text)
    return result


def _taint_copy_base_input(bundle_input: Path, dst_input: Path) -> None:
    dst_input.mkdir(parents=True, exist_ok=True)
    for f in bundle_input.glob("*.facts"):
        shutil.copyfile(f, dst_input / f.name)
    for f in bundle_input.glob("*.prob"):
        shutil.copyfile(f, dst_input / f.name)


def _taint_merge_outputs_as_facts(prev_out: Path, next_in: Path) -> None:
    if not prev_out.is_dir():
        return
    skip = {
        "facts.prob",
        "det-relations.txt",
        "det-scc.txt",
        "initial-input-relations-iter0.txt",
    }
    for p in prev_out.iterdir():
        if not p.is_file() or p.name in skip:
            continue
        if p.name.startswith("log.txt_") and p.suffix == ".json":
            continue
        if p.suffix not in {".csv", ".facts", ".tsv"}:
            continue
        dest = next_in / f"{p.stem}.facts"
        if dest.exists():
            dest.unlink()
        shutil.copyfile(p, dest)
        prob_dest = dest.with_suffix(".prob")
        with dest.open("r", encoding="utf-8") as fh, prob_dest.open(
            "w", encoding="utf-8"
        ) as pf:
            for _ in fh:
                pf.write("1.0\n")


def _coerce_stdout_str(data: object) -> str:
    """`subprocess.TimeoutExpired.stdout` can be bytes even when text=True was
    set on subprocess.run (the partial buffer collected before the kill is
    surfaced raw). Always return str so downstream `Path.write_text` is safe."""
    if data is None:
        return ""
    if isinstance(data, (bytes, bytearray)):
        return bytes(data).decode("utf-8", errors="replace")
    return str(data)


def _taint_run_cmd(
    cmd: Sequence[str],
    cwd: Optional[Path],
    timeout: Optional[float],
) -> Tuple[int, float, str, str]:
    t0 = time.perf_counter()
    eff_timeout = timeout if timeout and timeout > 0 else None
    try:
        proc = subprocess.run(
            list(cmd),
            cwd=str(cwd) if cwd else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=eff_timeout,
            check=False,
        )
        return proc.returncode, time.perf_counter() - t0, _coerce_stdout_str(proc.stdout), ""
    except subprocess.TimeoutExpired as exc:
        return 124, time.perf_counter() - t0, _coerce_stdout_str(exc.stdout), "TIMEOUT"
    except FileNotFoundError as exc:
        return 127, time.perf_counter() - t0, "", f"NOTFOUND: {exc}"
    except Exception as exc:  # noqa: BLE001
        return 1, time.perf_counter() - t0, "", f"ERROR: {exc}"


def _taint_compile_stage(
    souffle_bin: str,
    src_dl: Path,
    out_dir: Path,
    compile_extra: Sequence[str],
    timeout: Optional[int],
) -> Tuple[int, float, str]:
    """Direct `souffle ... compute.souffle.dl -o compute`. No --online,
    keeps --full-only if the binary supports it. Mirrors the bench script
    behaviour but runs the command inline here."""
    out_dir.mkdir(parents=True, exist_ok=True)
    dl_copy = out_dir / "compute.souffle.dl"
    if src_dl.resolve() != dl_copy.resolve():
        shutil.copyfile(src_dl, dl_copy)
    cmd: List[str] = [souffle_bin]
    if _souffle_supports(souffle_bin, "--full-only"):
        cmd.append("--full-only")
    cmd.extend(list(compile_extra))
    cmd.extend(["compute.souffle.dl", "-o", "compute"])
    code, elapsed, stdout, err = _taint_run_cmd(cmd, out_dir, timeout)
    (out_dir / "compile.stdout").write_text(stdout, encoding="utf-8")
    (out_dir / "compile.meta.json").write_text(
        json.dumps(
            {"cmd": cmd, "exit": code, "elapsed": elapsed, "note": err},
            indent=2,
        ),
        encoding="utf-8",
    )
    return code, elapsed, err


class _TaintRunResult:
    __slots__ = ("status", "elapsed_s", "stages_completed", "note")

    def __init__(
        self,
        status: str,
        elapsed_s: float,
        stages_completed: int,
        note: str = "",
    ) -> None:
        self.status = status
        self.elapsed_s = elapsed_s
        self.stages_completed = stages_completed
        self.note = note


def _taint_run_case(
    souffle_bin: str,
    bundle_case: Path,
    shared_build: Path,
    stages: Sequence[str],
    run_extra: Sequence[str],
    work_root: Path,
    timeout: Optional[float],
) -> _TaintRunResult:
    """Run every compiled stage in order:
      - seed each stage's -F with bundle_case/input
      - fold each previous stage's output csv back in as *.facts (+ .prob=1.0)
      - invoke ./compute -F <stage_in> -D <stage_out> <run_extra...>

    `timeout` is a per-case wall-clock budget across all stages — each stage's
    subprocess gets the remaining budget, and the case is aborted with status
    "timeout" once the budget is exhausted (so taint cases can't blow past the
    --rq3-run-timeout setting just because they have many stages).
    """
    del souffle_bin  # runtime uses the compiled ./compute directly
    case_name_ = bundle_case.name
    work_case = work_root / case_name_
    if work_case.exists():
        shutil.rmtree(work_case)
    work_case.mkdir(parents=True, exist_ok=True)

    base_in = work_case / "base_input"
    _taint_copy_base_input(bundle_case / "input", base_in)

    total_elapsed = 0.0
    completed = 0
    note = ""
    status = "ok"
    has_budget = bool(timeout and timeout > 0)
    budget = float(timeout) if has_budget else 0.0

    for stage in stages:
        exe = shared_build / stage / "compute"
        if not exe.is_file():
            status = "missing_exe"
            note = f"no compute for {stage}"
            break

        stage_dir = work_case / "stages" / stage
        stage_in = stage_dir / "input"
        stage_out = stage_dir / "output"
        stage_in.mkdir(parents=True, exist_ok=True)
        stage_out.mkdir(parents=True, exist_ok=True)

        for f in base_in.glob("*.facts"):
            shutil.copyfile(f, stage_in / f.name)
        for f in base_in.glob("*.prob"):
            shutil.copyfile(f, stage_in / f.name)
        for prev in stages[: stages.index(stage)]:
            _taint_merge_outputs_as_facts(
                work_case / "stages" / prev / "output", stage_in
            )

        cmd = [
            str(exe.resolve()),
            "-F",
            str(stage_in.resolve()),
            "-D",
            str(stage_out.resolve()),
            "--dumpdot",
            *list(run_extra),
        ]
        if has_budget:
            remaining = budget - total_elapsed
            if remaining <= 0:
                status = "timeout"
                note = f"wall-clock budget exhausted before {stage}"
                break
            stage_timeout: Optional[float] = remaining
        else:
            stage_timeout = None
        code, elapsed, stdout, err = _taint_run_cmd(cmd, stage_dir, stage_timeout)
        (stage_dir / "run.stdout").write_text(stdout, encoding="utf-8")
        (stage_dir / "run.meta.json").write_text(
            json.dumps(
                {"cmd": cmd, "exit": code, "elapsed": elapsed, "note": err},
                indent=2,
            ),
            encoding="utf-8",
        )

        total_elapsed += elapsed
        if code == 0:
            completed += 1
            if has_budget and total_elapsed >= budget:
                status = "timeout"
                note = f"wall-clock budget exhausted after {stage}"
                break
            continue
        status = "timeout" if code == 124 else "error"
        note = err or f"exit={code} at {stage}"
        break

    return _TaintRunResult(
        status=status,
        elapsed_s=total_elapsed,
        stages_completed=completed,
        note=note,
    )


def _resolve_taint_bundle(
    raw_bundle: Path,
    base: Path,
    label: str,
    logger: Logger,
) -> Optional[Path]:
    """Return a Path that has the legacy bundle layout
    (`pipeline/stages.txt`, `cases/<case>/...`).

    If `raw_bundle` already has that layout it's returned as-is. Otherwise we
    expect the new reorganised layout (`programs/<stage>.dl`, top-level
    `stages.txt`, `<case>/input/`) and synthesise the legacy shim under
    `<base>/taint/_legacy_shim/`. Returns None if neither layout matches."""
    if (raw_bundle / "pipeline" / "stages.txt").is_file() \
            and (raw_bundle / "cases").is_dir():
        return raw_bundle

    new_stages_file = raw_bundle / "stages.txt"
    programs_dir = raw_bundle / "programs"
    if not (new_stages_file.is_file() and programs_dir.is_dir()):
        logger.warn(
            f"[{label}] bundle missing — neither legacy layout "
            f"(pipeline/stages.txt + cases/) nor new layout "
            f"(stages.txt + programs/) found at {raw_bundle}"
        )
        return None

    stages = _taint_read_stages(new_stages_file)
    case_names = sorted(
        p.name for p in raw_bundle.iterdir()
        if p.is_dir() and p.name not in ("programs", "pipeline", "cases")
        and (p / "input").is_dir()
    )
    if not case_names:
        logger.warn(f"[{label}] no cases found under {raw_bundle}")
        return None

    shim_root = (base / "taint" / "_legacy_shim").resolve()
    shim = _rq2_build_taint_shim(raw_bundle, shim_root, case_names, stages, logger)
    if shim is None:
        return None
    logger.info(f"[{label}] using shim {shim} (from new layout {raw_bundle})")
    return shim


def _run_rq1_taint(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    raw_bundle = Path(args.taint_bundle).expanduser().resolve()
    bundle = _resolve_taint_bundle(raw_bundle, base, "RQ1/taint", logger)
    if bundle is None:
        return []
    stages_file = bundle / "pipeline" / "stages.txt"
    cases_root = bundle / "cases"

    stages = _taint_read_stages(stages_file)
    if args.taint_cases:
        case_names = [c for c in re.split(r"[,\s]+", args.taint_cases) if c]
    else:
        case_names = sorted(p.name for p in cases_root.iterdir() if p.is_dir())
    if not case_names:
        logger.warn("[RQ1/taint] no cases; skipping.")
        return []

    taint_base = base / "taint"
    taint_base.mkdir(parents=True, exist_ok=True)
    shared_build = taint_base / "_shared_build"
    shared_build.mkdir(parents=True, exist_ok=True)

    souffle_bin = args.souffle_bin or _taint_detect_souffle_bin(None)
    for stage in stages:
        stage_build = shared_build / stage
        src_dl = bundle / "pipeline" / "shared_build" / stage / "compute.souffle.dl"
        if not src_dl.is_file():
            logger.warn(f"[RQ1/taint] missing stage dl: {src_dl}")
            continue
        _rq1_compile_souffle(
            souffle_bin=souffle_bin,
            src_dl=src_dl,
            cwd=stage_build,
            exe_name="compute",
            include_dir=args.include_dir,
            timeout=args.compile_timeout,
            logger=logger,
            label=f"RQ1/taint {stage}",
            force=args.force_compile,
        )

    rows: List[List[str]] = []
    for idx, case_name_ in enumerate(case_names, 1):
        bundle_case = cases_root / case_name_
        if not bundle_case.is_dir():
            logger.warn(f"[RQ1/taint] missing case dir: {bundle_case}")
            continue
        logger.info(f"[RQ1/taint {idx}/{len(case_names)}] {case_name_}")
        case_work_root = taint_base / "runs" / case_name_
        case_work_root.mkdir(parents=True, exist_ok=True)
        stage_log_paths: Dict[str, List[Path]] = {s: [] for s in stages}
        for run_idx in range(1, args.runs + 1):
            run_work = case_work_root / f"run_{run_idx:02d}"
            result = _taint_run_case(
                souffle_bin=souffle_bin,
                bundle_case=bundle_case,
                shared_build=shared_build,
                stages=stages,
                run_extra=RQ1_TAINT_RUN_EXTRA,
                work_root=run_work,
                timeout=args.rq1_run_timeout,
            )
            logger.info(
                f"[RQ1/taint {case_name_}] run {run_idx}/{args.runs} "
                f"status={result.status} stages={result.stages_completed}/{len(stages)}"
            )
            case_out = run_work / case_name_ / "stages"
            if not case_out.is_dir():
                continue
            for stage in stages:
                stage_out = case_out / stage / "output"
                if not stage_out.is_dir():
                    continue
                logs = sorted(
                    stage_out.glob("log*.json"),
                    key=lambda p: p.stat().st_mtime,
                    reverse=True,
                )
                if logs:
                    stage_log_paths[stage].append(logs[0])

        stage_aggs = [
            _rq1_aggregate_logs(stage_log_paths[s])
            for s in stages
            if stage_log_paths.get(s)
        ]
        rows.append(_rq1_row("taint", case_name_, _rq1_sum_aggs(stage_aggs)))
    return rows


def _write_rq1_summary(
    base: Path, rows: Sequence[Sequence[str]], logger: Logger
) -> Path:
    out_tsv = base / RQ1_RESULT_FILENAME
    base.mkdir(parents=True, exist_ok=True)
    with out_tsv.open("w", encoding="utf-8") as fh:
        fh.write("\t".join(RQ1_SUMMARY_HEADER) + "\n")
        for r in rows:
            fh.write("\t".join(str(x) for x in r) + "\n")
    logger.banner("FMCAD -RQ1 rewrite-metric summary (per case)")
    logger.info("\n" + _render_table(RQ1_SUMMARY_HEADER, rows))
    logger.info(f"Wrote {out_tsv}")

    cat_rows = _rq1_category_summary(rows)
    cat_tsv = base / "RQ1summary.tsv"
    with cat_tsv.open("w", encoding="utf-8") as fh:
        fh.write("\t".join(RQ1_CATEGORY_SUMMARY_HEADER) + "\n")
        for r in cat_rows:
            fh.write("\t".join(str(x) for x in r) + "\n")
    logger.banner("FMCAD -RQ1 per-category avg/max/median")
    logger.info("\n" + _render_table(RQ1_CATEGORY_SUMMARY_HEADER, cat_rows))
    logger.info(f"Wrote {cat_tsv}")
    return out_tsv


def _collect_rq1_sidechannel(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    sc_base = base / "sidechannel"
    if not sc_base.is_dir():
        return rows
    def _case_key(p: Path) -> Tuple[int, str]:
        name = p.name
        if name.startswith("P") and name[1:].isdigit():
            return (int(name[1:]), name)
        return (10**9, name)
    for case_dir_ in sorted(sc_base.iterdir(), key=_case_key):
        if not case_dir_.is_dir() or not case_dir_.name.startswith("P"):
            continue
        variant_root = case_dir_ / RQ1_RUN_ROOT / RQ1_SC_VARIANTS[0].name
        if not variant_root.is_dir():
            continue
        log_paths: List[Path] = []
        for run_dir in sorted(variant_root.iterdir()):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            lp = find_run_log_json(run_dir)
            if lp is not None:
                log_paths.append(lp)
        if not log_paths:
            continue
        rows.append(_rq1_row("sidechannel", case_dir_.name,
                            _rq1_aggregate_logs(log_paths)))
    return rows


def _collect_rq1_symbolization(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    dis_base = base / "symbolization"
    if not dis_base.is_dir():
        return rows
    for case_dir_ in sorted(dis_base.iterdir()):
        if not case_dir_.is_dir() or case_dir_.name.startswith("_"):
            continue
        variant_dir = case_dir_ / RQ1_DIS_VARIANT_NAME
        if not variant_dir.is_dir():
            continue
        log_paths: List[Path] = []
        for run_dir in sorted(variant_dir.iterdir()):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            lp = find_run_log_json(run_dir)
            if lp is not None:
                log_paths.append(lp)
        if not log_paths:
            continue
        rows.append(_rq1_row("symbolization", case_dir_.name,
                            _rq1_aggregate_logs(log_paths)))
    return rows


def _collect_rq1_taint(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    taint_root = base / "taint" / "runs"
    if not taint_root.is_dir():
        return rows
    for case_dir_ in sorted(taint_root.iterdir()):
        if not case_dir_.is_dir():
            continue
        stage_log_paths: Dict[str, List[Path]] = {}
        for run_dir in sorted(case_dir_.iterdir()):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            stages_root = run_dir / case_dir_.name / "stages"
            if not stages_root.is_dir():
                continue
            for stage_dir in sorted(stages_root.iterdir()):
                stage_out = stage_dir / "output"
                if not stage_out.is_dir():
                    continue
                logs = sorted(
                    stage_out.glob("log*.json"),
                    key=lambda p: p.stat().st_mtime,
                    reverse=True,
                )
                if logs:
                    stage_log_paths.setdefault(stage_dir.name, []).append(logs[0])
        if not stage_log_paths:
            continue
        stage_aggs = [
            _rq1_aggregate_logs(paths)
            for paths in stage_log_paths.values()
            if paths
        ]
        rows.append(_rq1_row("taint", case_dir_.name, _rq1_sum_aggs(stage_aggs)))
    return rows


def cmd_rq1(args: argparse.Namespace) -> None:
    base = Path(args.rq1_base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    log_file = (
        Path(args.rq1_log_file)
        if args.rq1_log_file
        else (base / "fmcad_rq1.log")
    )
    log_file.unlink(missing_ok=True)
    logger = Logger(base, log_file, quiet=args.quiet)

    if args.rq1_collect:
        logger.banner("FMCAD -RQ1 --collect: aggregate existing logs only")
        logger.info(f"Output root: {base}")
        rows: List[List[str]] = []
        if not args.skip_sc:
            rows.extend(_collect_rq1_sidechannel(base))
        if not args.skip_taint:
            rows.extend(_collect_rq1_taint(base))
        if not args.skip_dis:
            rows.extend(_collect_rq1_symbolization(base))
        _write_rq1_summary(base, rows, logger)
        return

    logger.banner(
        "FMCAD -RQ1: souffle --det-opt --rewrite across sidechannel / taint / symbolization"
    )
    logger.info(f"Output root: {base}")
    logger.info(f"Runs per case: {args.runs}, run timeout: {args.rq1_run_timeout}s")

    rows: List[List[str]] = []
    if not args.skip_sc:
        rows.extend(_run_rq1_sidechannel(args, base, logger))
    if not args.skip_taint:
        rows.extend(_run_rq1_taint(args, base, logger))
    if not args.skip_dis:
        rows.extend(_run_rq1_symbolization(args, base, logger))

    _write_rq1_summary(base, rows, logger)


# --- Resume helpers (shared by -RQ3 / -RQ2) -----------------------------------


def _variant_run_done(variant_root: Path, runs_target: int) -> bool:
    """Resume predicate for one (case, variant) cell. A variant is "done" — the
    live runner won't make further progress — if either:
      - it already has `runs_target` successful runs on disk, OR
      - it has at least one timeout/error run (the runner is break-on-first-
        failure within a variant, so one bad meta means it already gave up;
        re-running just burns another timeout budget).
    Delete the offending run_* dirs to force a retry."""
    if not variant_root.is_dir():
        return False
    n_ok = 0
    saw_failure = False
    for run_dir in variant_root.iterdir():
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        meta_path = run_dir / "run.meta.json"
        if not meta_path.is_file():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        rc = meta.get("exit")
        if rc == 0:
            n_ok += 1
        elif rc is not None:
            saw_failure = True
    return n_ok >= runs_target or saw_failure


# --- -RQ3 (runtime comparison across 3 souffle configs) ----------------------


def _rq3_run_times_from_metas(
    variant_root: Path,
) -> Tuple[List[float], int, str, str]:
    """Read all run.meta.json under variant_root; return
    (elapseds, n_ok, status, note).

    `elapseds` includes timed-out / failed runs (wall-clock at the moment of
    timeout/failure) so the RQ3 summary always reflects how long each variant
    actually ran, even when nothing finished within the budget. `n_ok` is the
    count of runs that exited 0 — used for the "Runs" column.
    """
    if not variant_root.is_dir():
        return [], 0, "missing", "no runs dir"
    elapseds: List[float] = []
    n_ok = 0
    any_timeout = False
    any_fail = False
    note = ""
    for run_dir in sorted(variant_root.iterdir()):
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        meta_path = run_dir / "run.meta.json"
        if not meta_path.is_file():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        rc = meta.get("exit")
        el = meta.get("elapsed")
        if rc == 0 and isinstance(el, (int, float)):
            elapseds.append(float(el))
            n_ok += 1
        elif rc == 124:
            any_timeout = True
            note = meta.get("note") or "timeout"
            if isinstance(el, (int, float)):
                elapseds.append(float(el))
        else:
            any_fail = True
            note = meta.get("note") or f"exit={rc}"
            if isinstance(el, (int, float)):
                elapseds.append(float(el))
    if any_timeout and n_ok == 0:
        status = "TIMEOUT"
    elif any_fail and n_ok == 0:
        status = "FAIL"
    elif any_timeout or any_fail:
        status = "PARTIAL"
    elif n_ok:
        status = "OK"
    else:
        status = "MISSING"
    return elapseds, n_ok, status, note


def _rq3_stats(elapseds: Sequence[float]) -> Tuple[int, Optional[float], Optional[float], Optional[float]]:
    if not elapseds:
        return 0, None, None, None
    return (
        len(elapseds),
        sum(elapseds) / len(elapseds),
        min(elapseds),
        max(elapseds),
    )


def _fmt_int(v: Optional[int]) -> str:
    return str(v) if v is not None else ""


def _rq3_rand_vars_from_log(log_path: Path) -> Optional[int]:
    # Pre-rewrite rand_vars, uniform across variants:
    #   -r runs   -> FC_WMC_HYBRID.info.rand_vars_before_rewrite (added in
    #                souffle Pipeline.cpp; see GraphRewriteStats.randomVarsBefore)
    #   no-r runs -> FORWARD_COMPILATION.info.rand_vars (no rewrite ran, so
    #                the value is the pre-rewrite count by definition)
    # We never read FC_WMC_HYBRID.info.rand_vars — that one is post-rewrite
    # and not comparable across variants.
    try:
        data = json.loads(log_path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if not isinstance(data, dict):
        return None
    turns = data.get("turns")
    if not isinstance(turns, list) or not turns:
        return None
    first = turns[0] if isinstance(turns[0], dict) else None
    if first is None:
        return None
    stages = first.get("stages")
    if not isinstance(stages, list):
        return None
    targets = (
        (FC_WMC_HYBRID_STAGE, "rand_vars_before_rewrite"),
        (FC_WMC_HYBRID_STAGE, "rewrite_random_vars_before"),
        (FC_STAGE, "rand_vars"),
    )
    for stage_name, key in targets:
        aliases = _stage_name_aliases(stage_name)
        for stage in stages:
            if not isinstance(stage, dict) or stage.get("name") not in aliases:
                continue
            info = stage.get("info")
            if not isinstance(info, dict):
                continue
            rv = info.get(key)
            if rv is None:
                continue
            try:
                return int(float(rv))
            except (TypeError, ValueError):
                continue
    return None


def _rq3_iter_output_logs(output_dir: Path) -> List[Path]:
    if not output_dir.is_dir():
        return []
    return sorted(output_dir.glob("log.txt_*.json"))


def _rq3_rand_vars_from_metas(variant_root: Path) -> Optional[int]:
    # rand_vars is a property of the program + input — same across runs;
    # we read the first run's output log that carries it.
    if not variant_root.is_dir():
        return None
    for run_dir in sorted(variant_root.iterdir()):
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        for log_path in _rq3_iter_output_logs(run_dir / "output"):
            rv = _rq3_rand_vars_from_log(log_path)
            if rv is not None:
                return rv
    return None


def _rq3_taint_rand_vars(variant_root: Path, case: str) -> Optional[int]:
    # Taint runs each stage as its own souffle invocation with its own
    # output/log.txt_*.json — sum rand_vars across stages of the first run
    # that has any.
    if not variant_root.is_dir():
        return None
    for run_dir in sorted(variant_root.iterdir()):
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        stages_root = run_dir / case / "stages"
        if not stages_root.is_dir():
            continue
        total = 0
        any_found = False
        for stage_dir in sorted(stages_root.iterdir()):
            if not stage_dir.is_dir():
                continue
            for log_path in _rq3_iter_output_logs(stage_dir / "output"):
                rv = _rq3_rand_vars_from_log(log_path)
                if rv is not None:
                    total += rv
                    any_found = True
                    break
        if any_found:
            return total
    return None


def _run_rq3_sidechannel(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    sc_base = base / "sidechannel"
    sc_base.mkdir(parents=True, exist_ok=True)
    source_root = resolve_source_dir(args.source_dir)

    case_tokens = list(args.case_tokens) if args.case_tokens else []
    cases = (
        parse_case_tokens(case_tokens)
        if case_tokens
        else select_cases(args.cases, args.size, sc_base, source_root)
    )
    if not cases:
        logger.warn("[RQ3/sc] no side-channel cases discovered; skipping.")
        return []

    if not args.skip_generate:
        ok = run_generate(
            base_dir=sc_base,
            source_dir=str(source_root),
            cases=cases,
            cleanup=args.cleanup,
            timeout=args.generate_timeout,
            logger=logger,
        )
        if not ok:
            logger.error("[RQ3/sc] generate step failed")
            return []

    compile_cfg = CompileCfg(
        base_dir=sc_base,
        cases=cases,
        timeout=args.compile_timeout,
        souffle_bin=args.souffle_bin,
        souffle_args=args.souffle_arg or [],
        include_dir=args.include_dir,
        derv_only=False,
        compile_det_opt=False,
        force_compile=args.force_compile,
        logger=logger,
    )
    run_cfg = RunCfg(
        base_dir=sc_base,
        cases=cases,
        runs=args.runs,
        timeout=args.rq3_run_timeout,
        log_prefix=args.log_prefix,
        run_args=args.run_arg or [],
        logger=logger,
    )

    resume = bool(getattr(args, "resume", False))
    rows: List[List[str]] = []
    for idx, n in enumerate(cases, 1):
        label = case_name(n)
        cdir = case_dir(sc_base, n)

        active_variants = list(RQ3_SC_VARIANTS)
        if resume and all(
            _variant_run_done(cdir / RQ3_RUN_ROOT / v.name, args.runs)
            for v in active_variants
        ):
            logger.info(
                f"[RQ3/sc {idx}/{len(cases)}] {label}: SKIP (resume — all variants complete)"
            )
        else:
            logger.info(f"[RQ3/sc {idx}/{len(cases)}] {label}")
            compile_case(n, compile_cfg, active_variants)
            run_case(n, run_cfg, active_variants, RQ3_RUN_ROOT)
        for variant in RQ3_SC_VARIANTS:
            variant_root = cdir / RQ3_RUN_ROOT / variant.name
            elapseds, n_ok, status, note = _rq3_run_times_from_metas(variant_root)
            _, avg, mn, mx = _rq3_stats(elapseds)
            rand_vars = _rq3_rand_vars_from_metas(variant_root)
            rows.append([
                "sidechannel",
                label,
                variant.name,
                str(n_ok),
                _fmt_secs(avg),
                _fmt_secs(mn),
                _fmt_secs(mx),
                _fmt_int(rand_vars),
                status,
                note,
            ])
    return rows


def _run_rq3_symbolization(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    source_dl = Path(args.dis_source_dl)
    if not source_dl.is_absolute():
        source_dl = (ROOT_DIR / source_dl).resolve()
    inputs_root = Path(args.dis_inputs_dir)
    if not inputs_root.is_absolute():
        inputs_root = (ROOT_DIR / inputs_root).resolve()
    if not source_dl.is_file() or not inputs_root.is_dir():
        logger.warn(
            f"[RQ3/dis] missing inputs (dl={source_dl}, inputs={inputs_root}); "
            f"skipping symbolization."
        )
        return []

    dis_base = base / "symbolization"
    dis_base.mkdir(parents=True, exist_ok=True)
    case_filter: Optional[List[str]] = None
    if args.dis_cases:
        case_filter = [c for c in re.split(r"[,\s]+", args.dis_cases) if c]
    case_inputs, new_layout = discover_dis_case_inputs(inputs_root, case_filter)
    if not case_inputs:
        logger.warn(f"[RQ3/dis] no cases under {inputs_root}; skipping.")
        return []
    logger.info(f"[RQ3/dis] inputs layout: {'<case>/input' if new_layout else '<case>'}")

    # Single default-compiled exe shared by all four variants — only the
    # runtime flags differ.
    shared_dir = dis_base / DIS_SHARED_DIRNAME
    probe_input = case_inputs[0][1]
    exe_name = "compute_symbol_rq3"
    build_dir = shared_dir / "souffle_build_rq3"
    build_dir.mkdir(parents=True, exist_ok=True)
    exe: Optional[Path] = build_dir / exe_name
    if exe.is_file() and not args.force_compile:
        logger.info(f"[RQ3/dis] souffle binary cached at {exe}")
    else:
        if exe.is_file():
            try:
                exe.unlink()
            except OSError:
                pass
        cmd: List[str] = [args.souffle_bin]
        if _souffle_supports(args.souffle_bin, "--full-only"):
            cmd.append("--full-only")
        cmd.extend([
            "--profile=/dev/null",
            "-F", str(probe_input),
            "-D", str(build_dir / "compile_probe_output"),
            str(source_dl),
            "-I", args.include_dir,
            "-o", exe_name,
        ])
        log_path = build_dir / "compile.log"
        rc, elapsed, note = _run_subproc_capture(
            cmd, cwd=build_dir, timeout=args.compile_timeout, log_path=log_path
        )
        if rc != 0 or not exe.is_file():
            logger.error(
                f"[RQ3/dis] souffle compile failed (rc={rc}, {note}, "
                f"{elapsed:.2f}s); see {log_path}"
            )
            exe = None
        else:
            logger.info(f"[RQ3/dis] souffle compile OK in {elapsed:.2f}s")

    # Variant -> run_flags (single shared exe)
    variant_plan: List[Tuple[str, List[str]]] = [
        (name, list(run_flags)) for name, _, run_flags in RQ3_VARIANT_SPECS
    ]

    resume = bool(getattr(args, "resume", False))
    rows: List[List[str]] = []
    for idx, (case, case_input) in enumerate(case_inputs, 1):
        logger.info(f"[RQ3/dis {idx}/{len(case_inputs)}] {case}")
        case_out = dis_base / case
        case_out.mkdir(parents=True, exist_ok=True)
        for variant_name, run_flags in variant_plan:
            variant_dir = case_out / variant_name
            variant_dir.mkdir(parents=True, exist_ok=True)

            if resume and _variant_run_done(variant_dir, args.runs):
                logger.info(
                    f"[RQ3/dis {case}/{variant_name}] SKIP (resume — already complete)"
                )
                elapseds, n_ok, status_norm, note = _rq3_run_times_from_metas(variant_dir)
                _, avg, mn, mx = _rq3_stats(elapseds)
                rand_vars = _rq3_rand_vars_from_metas(variant_dir)
                rows.append([
                    "symbolization", case, variant_name,
                    str(n_ok),
                    _fmt_secs(avg), _fmt_secs(mn), _fmt_secs(mx),
                    _fmt_int(rand_vars),
                    status_norm, note,
                ])
                continue
            if exe is None:
                rows.append([
                    "symbolization", case, variant_name, "0",
                    "", "", "", "", "FAIL", "compile failed",
                ])
                continue
            stats = _run_souffle_dis_case(
                exe=exe, case=case, case_input=case_input,
                variant_name=variant_name, variant_dir=variant_dir,
                run_flags=run_flags, runs=args.runs,
                timeout=args.rq3_run_timeout, logger=logger,
            )
            status_raw = str(stats.get("status", ""))
            status = {
                "ok": "OK", "timeout": "TIMEOUT", "error": "FAIL",
            }.get(status_raw, status_raw.upper() or "MISSING")
            runs_n = int(stats.get("runs_completed") or 0)
            if status == "OK" and runs_n < args.runs:
                status = "PARTIAL"
            rand_vars = _rq3_rand_vars_from_metas(variant_dir)
            rows.append([
                "symbolization", case, variant_name,
                str(runs_n),
                _fmt_secs(stats.get("avg_elapsed")),
                _fmt_secs(stats.get("min_elapsed")),
                _fmt_secs(stats.get("max_elapsed")),
                _fmt_int(rand_vars),
                status,
                str(stats.get("note", "")),
            ])
    return rows


def _run_rq3_taint(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    raw_bundle = Path(args.taint_bundle).expanduser().resolve()
    bundle = _resolve_taint_bundle(raw_bundle, base, "RQ3/taint", logger)
    if bundle is None:
        return []
    stages_file = bundle / "pipeline" / "stages.txt"
    cases_root = bundle / "cases"

    stages = _taint_read_stages(stages_file)
    if args.taint_cases:
        case_names = [c for c in re.split(r"[,\s]+", args.taint_cases) if c]
    else:
        case_names = sorted(p.name for p in cases_root.iterdir() if p.is_dir())
    if not case_names:
        logger.warn("[RQ3/taint] no cases; skipping.")
        return []

    taint_base = base / "taint"
    taint_base.mkdir(parents=True, exist_ok=True)

    souffle_bin = args.souffle_bin or _taint_detect_souffle_bin(None)

    # Compile each stage once with the default flags — shared by all variants.
    shared_build = taint_base / "_shared_build"
    shared_build.mkdir(parents=True, exist_ok=True)
    for stage in stages:
        stage_build = shared_build / stage
        exe = stage_build / "compute"
        if exe.is_file() and not args.force_compile:
            continue
        if exe.is_file():
            try:
                exe.unlink()
            except OSError:
                pass
        src_dl = bundle / "pipeline" / "shared_build" / stage / "compute.souffle.dl"
        if not src_dl.is_file():
            logger.warn(f"[RQ3/taint] missing stage dl: {src_dl}")
            continue
        code, elapsed, note = _taint_compile_stage(
            souffle_bin, src_dl, stage_build, [], args.compile_timeout,
        )
        if code != 0:
            logger.error(
                f"[RQ3/taint compile {stage}] failed (rc={code}, {note}, "
                f"{elapsed:.2f}s)"
            )
        else:
            logger.info(
                f"[RQ3/taint compile {stage}] OK in {elapsed:.2f}s"
            )

    variant_plan: List[Tuple[str, List[str]]] = [
        (name, list(run_flags)) for name, _, run_flags in RQ3_VARIANT_SPECS
    ]

    resume = bool(getattr(args, "resume", False))
    rows: List[List[str]] = []
    for idx, case in enumerate(case_names, 1):
        bundle_case = cases_root / case
        if not bundle_case.is_dir():
            logger.warn(f"[RQ3/taint] missing case dir: {bundle_case}")
            continue
        logger.info(f"[RQ3/taint {idx}/{len(case_names)}] {case}")
        case_work_root = taint_base / "runs" / case
        case_work_root.mkdir(parents=True, exist_ok=True)
        for variant_name, run_extra in variant_plan:
            variant_work = case_work_root / variant_name
            variant_work.mkdir(parents=True, exist_ok=True)
            if resume:
                ok_runs = 0
                saw_failure = False
                for rd in variant_work.iterdir() if variant_work.is_dir() else ():
                    if rd.is_dir() and rd.name.startswith("run_"):
                        _, st, _ = _rq3_taint_run_summary(rd, case)
                        if st == "ok":
                            ok_runs += 1
                        elif st in ("timeout", "error"):
                            saw_failure = True
                if ok_runs >= args.runs or saw_failure:
                    logger.info(
                        f"[RQ3/taint {case}/{variant_name}] SKIP (resume — "
                        + ("done" if ok_runs >= args.runs else "prior failure on disk")
                        + ")"
                    )
                    coll_elapseds: List[float] = []
                    coll_n_ok = 0
                    coll_status = "OK"
                    coll_note = ""
                    for rd in sorted(variant_work.iterdir()):
                        if not rd.is_dir() or not rd.name.startswith("run_"):
                            continue
                        total, st, nt = _rq3_taint_run_summary(rd, case)
                        if st == "missing":
                            continue
                        coll_elapseds.append(total)
                        if st == "ok":
                            coll_n_ok += 1
                            continue
                        coll_note = nt or st
                        coll_status = "TIMEOUT" if st == "timeout" else "FAIL"
                        break
                    if coll_n_ok and coll_status == "OK" and coll_n_ok < len(coll_elapseds):
                        coll_status = "PARTIAL"
                    _, c_avg, c_mn, c_mx = _rq3_stats(coll_elapseds)
                    rand_vars = _rq3_taint_rand_vars(variant_work, case)
                    rows.append([
                        "taint", case, variant_name,
                        str(coll_n_ok),
                        _fmt_secs(c_avg), _fmt_secs(c_mn), _fmt_secs(c_mx),
                        _fmt_int(rand_vars),
                        coll_status, coll_note,
                    ])
                    continue
            elapseds: List[float] = []
            n_ok = 0
            status = "OK"
            note = ""
            for run_idx in range(1, args.runs + 1):
                run_work = variant_work / f"run_{run_idx:02d}"
                if run_work.exists():
                    shutil.rmtree(run_work)
                result = _taint_run_case(
                    souffle_bin=souffle_bin,
                    bundle_case=bundle_case,
                    shared_build=shared_build,
                    stages=stages,
                    run_extra=run_extra,
                    work_root=run_work,
                    timeout=args.rq3_run_timeout,
                )
                logger.info(
                    f"[RQ3/taint {case}/{variant_name}] run {run_idx}/{args.runs} "
                    f"status={result.status} stages={result.stages_completed}/{len(stages)} "
                    f"elapsed={result.elapsed_s:.2f}s"
                )
                elapseds.append(result.elapsed_s)
                if result.status == "ok":
                    n_ok += 1
                    continue
                note = result.note or result.status
                status = "TIMEOUT" if result.status == "timeout" else "FAIL"
                break
            if n_ok and status == "OK" and n_ok < args.runs:
                status = "PARTIAL"
            _, avg, mn, mx = _rq3_stats(elapseds)
            rand_vars = _rq3_taint_rand_vars(variant_work, case)
            rows.append([
                "taint", case, variant_name,
                str(n_ok),
                _fmt_secs(avg),
                _fmt_secs(mn),
                _fmt_secs(mx),
                _fmt_int(rand_vars),
                status,
                note,
            ])
    return rows


RQ3_VARIANT_NAMES: List[str] = [name for name, _, _ in RQ3_VARIANT_SPECS]


def _rq3_case_key_p(p: Path) -> Tuple[int, str]:
    name = p.name
    if name.startswith("P") and name[1:].isdigit():
        return (int(name[1:]), name)
    return (10**9, name)


def _rq3_taint_run_summary(run_dir: Path, case: str) -> Tuple[float, str, str]:
    """Sum per-stage elapseds under run_dir/<case>/stages/* and derive the
    run-level (total, status, note). Mirrors _taint_run_case but reads the
    stage run.meta.json files instead of running anything.

    Stage iteration order doesn't matter — failed stages with no meta are
    silently skipped, and the first non-zero exit we encounter dictates the
    status (the live runner breaks on first failure too, so only the failed
    stage and its predecessors leave metas behind).
    """
    stages_root = run_dir / case / "stages"
    if not stages_root.is_dir():
        return 0.0, "missing", "no stages dir"
    total = 0.0
    status = "ok"
    note = ""
    for stage_dir in sorted(stages_root.iterdir()):
        if not stage_dir.is_dir():
            continue
        meta_path = stage_dir / "run.meta.json"
        if not meta_path.is_file():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        rc = meta.get("exit")
        el = meta.get("elapsed")
        if isinstance(el, (int, float)):
            total += float(el)
        if rc != 0 and status == "ok":
            status = "timeout" if rc == 124 else "error"
            note = str(meta.get("note") or f"exit={rc} at {stage_dir.name}")
    return total, status, note


def _collect_rq3_sidechannel(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    sc_base = base / "sidechannel"
    if not sc_base.is_dir():
        return rows
    for case_dir_ in sorted(sc_base.iterdir(), key=_rq3_case_key_p):
        if not case_dir_.is_dir() or not case_dir_.name.startswith("P"):
            continue
        for variant in RQ3_SC_VARIANTS:
            variant_root = case_dir_ / RQ3_RUN_ROOT / variant.name
            if not variant_root.is_dir():
                continue
            elapseds, n_ok, status, note = _rq3_run_times_from_metas(variant_root)
            _, avg, mn, mx = _rq3_stats(elapseds)
            rand_vars = _rq3_rand_vars_from_metas(variant_root)
            rows.append([
                "sidechannel", case_dir_.name, variant.name,
                str(n_ok),
                _fmt_secs(avg), _fmt_secs(mn), _fmt_secs(mx),
                _fmt_int(rand_vars),
                status, note,
            ])
    return rows


def _collect_rq3_symbolization(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    dis_base = base / "symbolization"
    if not dis_base.is_dir():
        return rows
    for case_dir_ in sorted(dis_base.iterdir()):
        if not case_dir_.is_dir() or case_dir_.name.startswith("_"):
            continue
        for variant_name in RQ3_VARIANT_NAMES:
            variant_dir = case_dir_ / variant_name
            if not variant_dir.is_dir():
                continue
            elapseds, n_ok, status, note = _rq3_run_times_from_metas(variant_dir)
            _, avg, mn, mx = _rq3_stats(elapseds)
            rand_vars = _rq3_rand_vars_from_metas(variant_dir)
            rows.append([
                "symbolization", case_dir_.name, variant_name,
                str(n_ok),
                _fmt_secs(avg), _fmt_secs(mn), _fmt_secs(mx),
                _fmt_int(rand_vars),
                status, note,
            ])
    return rows


def _collect_rq3_taint(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    taint_root = base / "taint" / "runs"
    if not taint_root.is_dir():
        return rows
    for case_dir_ in sorted(taint_root.iterdir()):
        if not case_dir_.is_dir():
            continue
        case = case_dir_.name
        for variant_name in RQ3_VARIANT_NAMES:
            variant_dir = case_dir_ / variant_name
            if not variant_dir.is_dir():
                continue
            elapseds: List[float] = []
            n_ok = 0
            status = "OK"
            note = ""
            for run_dir in sorted(variant_dir.iterdir()):
                if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                    continue
                total, run_status, run_note = _rq3_taint_run_summary(run_dir, case)
                if run_status == "missing":
                    continue
                elapseds.append(total)
                if run_status == "ok":
                    n_ok += 1
                    continue
                note = run_note or run_status
                status = "TIMEOUT" if run_status == "timeout" else "FAIL"
                break
            if not elapseds:
                continue
            if n_ok and status == "OK" and n_ok < len(elapseds):
                status = "PARTIAL"
            _, avg, mn, mx = _rq3_stats(elapseds)
            rand_vars = _rq3_taint_rand_vars(variant_dir, case)
            rows.append([
                "taint", case, variant_name,
                str(n_ok),
                _fmt_secs(avg), _fmt_secs(mn), _fmt_secs(mx),
                _fmt_int(rand_vars),
                status, note,
            ])
    return rows


def _write_rq3_summary(
    base: Path, rows: Sequence[Sequence[str]], logger: Logger
) -> Path:
    out_tsv = base / RQ3_RESULT_FILENAME
    base.mkdir(parents=True, exist_ok=True)
    with out_tsv.open("w", encoding="utf-8") as fh:
        fh.write("\t".join(RQ3_SUMMARY_HEADER) + "\n")
        for r in rows:
            fh.write("\t".join(str(x) for x in r) + "\n")
    logger.banner("FMCAD -RQ3 runtime summary")
    logger.info("\n" + _render_table(RQ3_SUMMARY_HEADER, rows))
    logger.info(f"Wrote {out_tsv}")
    return out_tsv


def cmd_rq3(args: argparse.Namespace) -> None:
    base = Path(args.rq3_base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    log_file = (
        Path(args.rq3_log_file)
        if args.rq3_log_file
        else (base / "fmcad_rq3.log")
    )
    log_file.unlink(missing_ok=True)
    logger = Logger(base, log_file, quiet=args.quiet)

    if args.rq3_collect:
        logger.banner("FMCAD -RQ3 --collect: aggregate existing run.meta.json only")
        logger.info(f"Output root: {base}")
        rows: List[List[str]] = []
        if not args.skip_sc:
            rows.extend(_collect_rq3_sidechannel(base))
        if not args.skip_taint:
            rows.extend(_collect_rq3_taint(base))
        if not args.skip_dis:
            dis_rows = _collect_rq3_symbolization(base)
            if not dis_rows:
                logger.warn(f"[RQ3/collect] no symbolization rows under {base / 'symbolization'}")
            rows.extend(dis_rows)
        _write_rq3_summary(base, rows, logger)
        return

    logger.banner(
        "FMCAD -RQ3: souffle runtime comparison "
        "(01 / 11) "
        "across sidechannel / taint / symbolization"
    )
    logger.info(f"Output root: {base}")
    logger.info(f"Runs per case/variant: {args.runs}, run timeout: {args.rq3_run_timeout}s")

    rows: List[List[str]] = []
    if not args.skip_sc:
        rows.extend(_run_rq3_sidechannel(args, base, logger))
    if not args.skip_taint:
        rows.extend(_run_rq3_taint(args, base, logger))
    if not args.skip_dis:
        rows.extend(_run_rq3_symbolization(args, base, logger))

    _write_rq3_summary(base, rows, logger)


# --- -RQ2 (souffle --det-opt -r + problog) -----------------------------------


def _rq2_status_for(stats_status: str, runs_completed: int, runs_target: int) -> str:
    """Normalise inline-runner status to OK/PARTIAL/TIMEOUT/FAIL/MISSING."""
    s = (stats_status or "").lower()
    if s in ("ok", ""):
        if runs_completed == 0:
            return "MISSING"
        return "OK" if runs_completed >= runs_target else "PARTIAL"
    if s in ("timeout",):
        return "TIMEOUT"
    if s in ("oom",):
        return "OOM"
    if s in ("missing_config", "skipped", "compile_failed"):
        return "SKIP"
    if s in ("error", "fail"):
        return "FAIL"
    return s.upper() or "MISSING"


def _rq2_row(
    category: str,
    case: str,
    engine: str,
    runs: int,
    avg: Optional[float],
    mn: Optional[float],
    mx: Optional[float],
    status: str,
    note: str = "",
) -> List[str]:
    return [
        category, case, engine,
        str(runs),
        _fmt_secs(avg), _fmt_secs(mn), _fmt_secs(mx),
        status, note,
    ]


def _rq2_skip_row(category: str, case: str, engine: str, note: str) -> List[str]:
    return _rq2_row(category, case, engine, 0, None, None, None, "SKIP", note)


_PROBLOG_INFRA_ERROR_MARKERS = (
    b"ModuleNotFoundError: No module named 'problog'",
    b"ImportError: ",
)


def _rq2_problog_run_diagnose(run_dir: Path) -> Tuple[bool, bool]:
    """Inspect captured ProbLog stdio under `run_dir`. Returns (ground_quirk_ok,
    infra_error).

    `ground_quirk_ok=True` means ProbLog 2.2.10's `problog ground` exited 1 but
    actually succeeded — it prints `(True, '...')` to stderr next to the
    grounded program on stdout. The dis/taint runner detects this at write
    time; legacy sc metas (written by run_problog_case before its 2026-04-26
    fix) record raw exit=1 with no quirk handling, so the collector has to
    look at captured streams retroactively.

    `infra_error=True` means the failure is environmental (missing/broken
    interpreter, e.g. ~/.local/bin/problog dispatching to a python whose
    site-packages don't have `problog`), not a result of the benchmark itself.
    Such metas should be ignored by the aggregator instead of counted as
    failures, otherwise transient venv breakage poisons the historical OK
    runs.

    Sc writes `problog.err`/`problog.out` separately; dis/taint write a merged
    `problog.log`. We scan every plausible name."""
    quirk_ok = False
    infra = False
    for fname in ("problog.err", "problog.log", "problog.out"):
        fpath = run_dir / fname
        if not fpath.is_file():
            continue
        try:
            with fpath.open("rb") as fh:
                data = fh.read()
        except OSError:
            continue
        if b"(True, " in data:
            quirk_ok = True
        if any(m in data for m in _PROBLOG_INFRA_ERROR_MARKERS):
            infra = True
    return quirk_ok, infra


def _rq2_collect_meta_runs(
    runs_root: Path,
) -> Tuple[List[float], str, str]:
    """Read every run_XX/run.meta.json under `runs_root`; return (elapseds, status, note)."""
    if not runs_root.is_dir():
        return [], "missing", ""
    elapseds: List[float] = []
    any_timeout = False
    any_oom = False
    any_fail = False
    note = ""
    for run_dir in sorted(runs_root.iterdir()):
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        meta_path = run_dir / "run.meta.json"
        if not meta_path.is_file():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        rc = meta.get("exit")
        el = meta.get("elapsed")
        meta_status = str(meta.get("status") or "").lower()
        quirk_ok, infra_err = (False, False)
        if rc == 1:
            quirk_ok, infra_err = _rq2_problog_run_diagnose(run_dir)
        if infra_err and not quirk_ok:
            # Skip — broken interpreter, not a benchmark result. Don't count
            # as success or failure; preserves older valid runs in the
            # aggregate.
            continue
        if rc == 0 and isinstance(el, (int, float)):
            elapseds.append(float(el))
        elif rc == 1 and isinstance(el, (int, float)) and quirk_ok:
            elapseds.append(float(el))
        elif rc == 124 or meta_status == "timeout":
            any_timeout = True
            note = meta.get("note") or "timeout"
        elif meta_status == "oom":
            any_oom = True
            note = meta.get("note") or f"oom (exit={rc})"
        else:
            any_fail = True
            note = meta.get("note") or f"exit={rc}"
    # Precedence when no successful runs: OOM > TIMEOUT > FAIL. PARTIAL is
    # raised when at least one run succeeded but others didn't.
    if any_oom and not elapseds:
        status = "OOM"
    elif any_timeout and not elapseds:
        status = "TIMEOUT"
    elif any_fail and not elapseds:
        status = "FAIL"
    elif any_oom or any_timeout or any_fail:
        status = "PARTIAL"
    elif elapseds:
        status = "OK"
    else:
        status = "MISSING"
    return elapseds, status, note


def _rq2_engine_row_from_runs_root(
    category: str,
    case: str,
    engine: str,
    runs_root: Path,
    runs_target: int,
) -> List[str]:
    elapseds, status, note = _rq2_collect_meta_runs(runs_root)
    runs_n, avg, mn, mx = _rq3_stats(elapseds)
    if status == "OK" and runs_n < runs_target:
        status = "PARTIAL"
    return _rq2_row(category, case, engine, runs_n, avg, mn, mx, status, note)


def _rq2_discover_dis_cases(
    inputs_root: Path, case_filter: Optional[Sequence[str]]
) -> List[Path]:
    """Like discover_dis_cases but filters to dirs that have an `input/`
    subdir — matches the new symbolization/ layout where each <case>/ holds
    facts under <case>/input/, while peer dirs like programs/ exist but are
    not cases."""
    if not inputs_root.is_dir():
        return []
    cands = sorted(
        [p for p in inputs_root.iterdir()
         if p.is_dir() and (p / "input").is_dir()],
        key=lambda p: p.name,
    )
    if case_filter:
        wanted = {c.strip() for c in case_filter if c.strip()}
        return [p for p in cands if p.name in wanted]
    return cands


def _rq2_build_taint_shim(
    new_root: Path,
    shim_root: Path,
    case_names: Sequence[str],
    new_stages: Sequence[str],
    logger: Logger,
) -> Optional[Path]:
    """Synthesise the legacy taint-bundle layout from the reorganised dataset
    so the existing RQ2 taint runners keep working without rewrites.

    New layout (input)::
      taint/programs/<stage>.dl
      taint/stages.txt          (short stage names, one per line)
      taint/<case>/input/*.facts,*.prob

    Legacy layout (output, all symlinks)::
      shim/pipeline/stages.txt                                    (renamed
                                                                   <stage>-dlog)
      shim/pipeline/shared_build/<stage>-dlog/compute.souffle.dl  -> programs/<stage>.dl
      shim/cases/<case>/input/                                    -> taint/<case>/input/
      shim/cases/<case>/stages/<stage>-dlog/compute.souffle.dl    -> programs/<stage>.dl

    Returns shim_root on success, or None if any required source file is
    missing.
    """
    programs_dir = new_root / "programs"
    if not programs_dir.is_dir():
        logger.error(f"[RQ2/taint shim] missing {programs_dir}")
        return None

    try:
        if shim_root.exists():
            shutil.rmtree(shim_root)
        pipeline = shim_root / "pipeline"
        pipeline.mkdir(parents=True, exist_ok=True)
        cases_root = shim_root / "cases"
        cases_root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        logger.error(f"[RQ2/taint shim] cannot reset {shim_root}: {exc}")
        return None

    legacy_stages: List[str] = []
    for stage in new_stages:
        src_dl = programs_dir / f"{stage}.dl"
        if not src_dl.is_file():
            logger.error(f"[RQ2/taint shim] missing program: {src_dl}")
            return None
        legacy = stage + RQ2_TAINT_STAGE_SUFFIX
        legacy_stages.append(legacy)
        sb = pipeline / "shared_build" / legacy
        sb.mkdir(parents=True, exist_ok=True)
        link = sb / "compute.souffle.dl"
        if link.exists() or link.is_symlink():
            link.unlink()
        link.symlink_to(src_dl.resolve())

    (pipeline / "stages.txt").write_text(
        "\n".join(legacy_stages) + "\n", encoding="utf-8"
    )

    for case in case_names:
        case_src = new_root / case
        if not case_src.is_dir():
            logger.warn(f"[RQ2/taint shim] case dir missing: {case_src}")
            continue
        case_dst = cases_root / case
        case_dst.mkdir(parents=True, exist_ok=True)
        # Symlink input dir verbatim.
        in_src = case_src / "input"
        in_dst = case_dst / "input"
        if in_dst.exists() or in_dst.is_symlink():
            in_dst.unlink() if in_dst.is_symlink() else shutil.rmtree(in_dst)
        if in_src.is_dir():
            in_dst.symlink_to(in_src.resolve())
        # Per-stage compute.souffle.dl symlink under cases/<case>/stages/<stage>/
        for stage in new_stages:
            legacy = stage + RQ2_TAINT_STAGE_SUFFIX
            sd = case_dst / "stages" / legacy
            sd.mkdir(parents=True, exist_ok=True)
            link = sd / "compute.souffle.dl"
            if link.exists() or link.is_symlink():
                link.unlink()
            link.symlink_to((programs_dir / f"{stage}.dl").resolve())
    return shim_root


def _run_rq2_sidechannel(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    # RQ2 sc base: when --rq2-sc-base is set, RQ2 reads case dirs (compute.dl,
    # compute.problog.dl, input/) directly from there and writes its rq2_runs/
    # subdir alongside them. Useful when the SMT source is gone but the
    # already-generated case artefacts (e.g. side_channel_full/) survive — we
    # then treat that dir as the sc workspace rather than rebuilding from SMT.
    rq2_sc_base = getattr(args, "rq2_sc_base", None)
    if rq2_sc_base:
        sc_base = Path(rq2_sc_base).expanduser().resolve()
        if not sc_base.is_dir():
            logger.error(f"[RQ2/sc] --rq2-sc-base does not exist: {sc_base}")
            return []
        # Force-skip generate when reusing an external sc workspace.
        args.skip_generate = True
    else:
        sc_base = base / "sidechannel"
        sc_base.mkdir(parents=True, exist_ok=True)
    source_root = resolve_source_dir(args.source_dir)

    case_tokens = list(args.case_tokens) if args.case_tokens else []
    cases = (
        parse_case_tokens(case_tokens)
        if case_tokens
        else select_cases(args.cases, args.size, sc_base, source_root)
    )
    if not cases:
        logger.warn("[RQ2/sc] no side-channel cases discovered; skipping.")
        return []

    if not args.skip_generate:
        ok = run_generate(
            base_dir=sc_base,
            source_dir=str(source_root),
            cases=cases,
            cleanup=args.cleanup,
            timeout=args.generate_timeout,
            logger=logger,
        )
        if not ok:
            logger.error("[RQ2/sc] generate step failed")
            return []

    compile_cfg = CompileCfg(
        base_dir=sc_base,
        cases=cases,
        timeout=args.compile_timeout,
        souffle_bin=args.souffle_bin,
        souffle_args=args.souffle_arg or [],
        include_dir=args.include_dir,
        # Runtime flags select rewrite; full inference includes BDD/WMC.
        derv_only=False,
        compile_det_opt=False,
        force_compile=args.force_compile,
        logger=logger,
        use_full_only=True,
    )
    run_cfg = RunCfg(
        base_dir=sc_base,
        cases=cases,
        runs=args.runs,
        timeout=args.rq2_run_timeout,
        log_prefix=args.log_prefix,
        run_args=args.run_arg or [],
        logger=logger,
    )

    resume = bool(getattr(args, "resume", False))
    engines = getattr(args, "_rq2_engines_set", set(RQ2_ALL_ENGINES))
    rows: List[List[str]] = []
    for idx, n in enumerate(cases, 1):
        label = case_name(n)
        logger.info(f"[RQ2/sc {idx}/{len(cases)}] {label}")
        cdir = case_dir(sc_base, n)

        if "souffle" in engines:
            souffle_root = cdir / RQ2_RUN_ROOT / RQ2_SOUFFLE_VARIANT
            if resume and _variant_run_done(souffle_root, args.runs):
                logger.info(f"[RQ2/sc {label}/{RQ2_SOUFFLE_VARIANT}] SKIP (resume)")
            else:
                compile_case(n, compile_cfg, RQ2_SC_VARIANTS)
                run_case(n, run_cfg, RQ2_SC_VARIANTS, RQ2_RUN_ROOT)
            rows.append(_rq2_engine_row_from_runs_root(
                "sidechannel", label, RQ2_SOUFFLE_VARIANT,
                souffle_root, args.runs,
            ))

        if "problog" in engines:
            pl_root = cdir / RQ2_RUN_ROOT / "problog"
            if resume and _variant_run_done(pl_root, args.runs):
                logger.info(f"[RQ2/sc {label}/{RQ2_PROBLOG_VARIANT}] SKIP (resume)")
            else:
                run_problog_case(n, run_cfg, RQ2_RUN_ROOT)
            rows.append(_rq2_engine_row_from_runs_root(
                "sidechannel", label, RQ2_PROBLOG_VARIANT, pl_root, args.runs,
            ))

        if "scallop" in engines:
            rows.append(_rq2_run_scallop_dispatch(
                "sidechannel", label,
                ROOT_DIR / "side_channel" / "full" / label / "compute.problog.dl",
                cdir / RQ2_RUN_ROOT / RQ2_SCALLOP_VARIANT,
                args, logger, "RQ2/sc", resume,
            ))

        if "vproblog" not in engines:
            continue

        # vproblog: pre-materialised at side_channel/full/<case>/vproblog/.
        # Source dir for sc cases is the full/ tree (same as compute.problog.dl).
        vp_src_dir = ROOT_DIR / "side_channel" / "full" / label / "vproblog"
        vp_root = cdir / RQ2_RUN_ROOT / RQ2_VPROBLOG_VARIANT
        if resume and _variant_run_done(vp_root, args.runs):
            logger.info(f"[RQ2/sc {label}/{RQ2_VPROBLOG_VARIANT}] SKIP (resume)")
            row = _rq2_row_from_meta_runs(
                "sidechannel", label, RQ2_VPROBLOG_VARIANT, vp_root,
            )
            rows.append(row or _rq2_skip_row(
                "sidechannel", label, RQ2_VPROBLOG_VARIANT, "no runs on disk",
            ))
        else:
            vp_elapseds, vp_status, vp_note = _rq2_run_vproblog_on_dir(
                vp_dir=vp_src_dir,
                work_dir=vp_root,
                runs=args.runs,
                timeout=args.rq2_run_timeout,
                logger=logger,
                case_label=label,
                category_label="RQ2/sc",
                vlog_bin=Path(getattr(args, "vlog_bin", DEFAULT_VLOG_BIN)),
            )
            vp_runs_n, vp_avg, vp_mn, vp_mx = _rq3_stats(vp_elapseds)
            vp_status_norm = _rq2_status_for(vp_status, vp_runs_n, args.runs)
            rows.append(_rq2_row(
                "sidechannel", label, RQ2_VPROBLOG_VARIANT,
                vp_runs_n, vp_avg, vp_mn, vp_mx, vp_status_norm, vp_note,
            ))
    return rows


def _run_rq2_symbolization(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    # RQ2 prefers the reorganised symbolization/ layout; fall back to the
    # legacy --dis-source-dl / --dis-inputs-dir args if rq2-specific overrides
    # are unset.
    src_raw = getattr(args, "rq2_dis_source_dl", None) or args.dis_source_dl
    inputs_raw = getattr(args, "rq2_dis_inputs_dir", None) or args.dis_inputs_dir
    cases_filter_raw = getattr(args, "rq2_dis_cases", None) or args.dis_cases
    source_dl = Path(src_raw)
    if not source_dl.is_absolute():
        source_dl = (ROOT_DIR / source_dl).resolve()
    inputs_root = Path(inputs_raw)
    if not inputs_root.is_absolute():
        inputs_root = (ROOT_DIR / inputs_root).resolve()
    if not source_dl.is_file() or not inputs_root.is_dir():
        logger.warn(
            f"[RQ2/dis] missing inputs (dl={source_dl}, inputs={inputs_root}); "
            f"skipping symbolization."
        )
        return []

    dis_base = base / "symbolization"
    dis_base.mkdir(parents=True, exist_ok=True)
    case_filter: Optional[List[str]] = None
    if cases_filter_raw:
        case_filter = [c for c in re.split(r"[,\s]+", cases_filter_raw) if c]
    # New layout has facts at <case>/input/; only treat dirs containing
    # input/ as cases (filters out symbolization/programs/, README.md, etc.).
    cases = _rq2_discover_dis_cases(inputs_root, case_filter)
    new_layout = bool(cases)
    if not cases:
        cases = discover_dis_cases(inputs_root, case_filter)
        new_layout = False
    if not cases:
        logger.warn(f"[RQ2/dis] no cases under {inputs_root}; skipping.")
        return []

    engines = getattr(args, "_rq2_engines_set", set(RQ2_ALL_ENGINES))
    if "souffle" in engines:
        shared_dir = dis_base / DIS_SHARED_DIRNAME
        build_dir = shared_dir / "souffle_build_rq2"
        probe_input = (cases[0] / "input") if new_layout else cases[0]
        exe = _souffle_compile_dis(
            souffle_bin=args.souffle_bin,
            src_dl=source_dl,
            probe_input_dir=probe_input,
            include_dir=args.include_dir,
            build_dir=build_dir,
            exe_name="compute_symbol_rq2",
            timeout=args.compile_timeout,
            logger=logger,
            label="RQ2/dis",
        )
    else:
        exe = None

    resume = bool(getattr(args, "resume", False))
    rows: List[List[str]] = []
    for idx, case_path in enumerate(cases, 1):
        case = case_path.name
        # New layout points facts at <case>/input/; legacy points directly at
        # <case>/. _run_souffle_dis_case uses the path verbatim with `-F`, so
        # resolve once here.
        case_input = (case_path / "input") if new_layout else case_path
        case_out = dis_base / case
        case_out.mkdir(parents=True, exist_ok=True)
        logger.info(f"[RQ2/dis {idx}/{len(cases)}] {case}")

        if "souffle" in engines:
            souffle_variant_dir = case_out / RQ2_SOUFFLE_VARIANT
            if resume and _variant_run_done(souffle_variant_dir, args.runs):
                logger.info(f"[RQ2/dis {case}/{RQ2_SOUFFLE_VARIANT}] SKIP (resume)")
                row = _rq2_row_from_meta_runs(
                    "symbolization", case, RQ2_SOUFFLE_VARIANT, souffle_variant_dir,
                )
                if row is not None:
                    rows.append(row)
                else:
                    rows.append(_rq2_skip_row(
                        "symbolization", case, RQ2_SOUFFLE_VARIANT, "no runs on disk",
                    ))
            elif exe is None:
                rows.append(_rq2_row(
                    "symbolization", case, RQ2_SOUFFLE_VARIANT,
                    0, None, None, None, "FAIL", "compile failed",
                ))
            else:
                souffle_variant_dir.mkdir(parents=True, exist_ok=True)
                stats = _run_souffle_dis_case(
                    exe=exe, case=case, case_input=case_input,
                    variant_name=RQ2_SOUFFLE_VARIANT, variant_dir=souffle_variant_dir,
                    run_flags=["--det-opt", "-r"], runs=args.runs,
                    timeout=args.rq2_run_timeout, logger=logger,
                )
                runs_n = int(stats.get("runs_completed") or 0)
                status = _rq2_status_for(str(stats.get("status", "")), runs_n, args.runs)
                rows.append(_rq2_row(
                    "symbolization", case, RQ2_SOUFFLE_VARIANT,
                    runs_n,
                    stats.get("avg_elapsed"),
                    stats.get("min_elapsed"),
                    stats.get("max_elapsed"),
                    status, str(stats.get("note", "")),
                ))

        if "problog" in engines:
            # ProbLog: per-case compute.problog.dl in symbolization/<case>/.
            problog_src = inputs_root / case / "compute.problog.dl"
            problog_variant_dir = case_out / RQ2_PROBLOG_VARIANT
            if not problog_src.is_file():
                rows.append(_rq2_skip_row(
                    "symbolization", case, RQ2_PROBLOG_VARIANT,
                    "no compute.problog.dl in dis benchmark",
                ))
            elif resume and _variant_run_done(problog_variant_dir, args.runs):
                logger.info(f"[RQ2/dis {case}/{RQ2_PROBLOG_VARIANT}] SKIP (resume)")
                row = _rq2_row_from_meta_runs(
                    "symbolization", case, RQ2_PROBLOG_VARIANT, problog_variant_dir,
                )
                rows.append(row or _rq2_skip_row(
                    "symbolization", case, RQ2_PROBLOG_VARIANT, "no runs on disk",
                ))
            else:
                pl_elapseds, pl_status, pl_note = _rq2_run_problog_on_file(
                    problog_src=problog_src,
                    work_dir=problog_variant_dir,
                    runs=args.runs,
                    timeout=args.rq2_run_timeout,
                    logger=logger,
                    case_label=case,
                    category_label="RQ2/dis",
                )
                pl_runs_n, pl_avg, pl_mn, pl_mx = _rq3_stats(pl_elapseds)
                pl_status_norm = _rq2_status_for(pl_status, pl_runs_n, args.runs)
                rows.append(_rq2_row(
                    "symbolization", case, RQ2_PROBLOG_VARIANT,
                    pl_runs_n, pl_avg, pl_mn, pl_mx, pl_status_norm, pl_note,
                ))

        if "scallop" in engines:
            rows.append(_rq2_run_scallop_dispatch(
                "symbolization", case,
                inputs_root / case / "compute.problog.dl",
                case_out / RQ2_SCALLOP_VARIANT,
                args, logger, "RQ2/dis", resume,
            ))

        if "vproblog" not in engines:
            continue

        # vproblog: pre-materialised at symbolization/<case>/vproblog/.
        vp_src_dir = inputs_root / case / "vproblog"
        vp_variant_dir = case_out / RQ2_VPROBLOG_VARIANT
        if not vp_src_dir.is_dir():
            rows.append(_rq2_skip_row(
                "symbolization", case, RQ2_VPROBLOG_VARIANT,
                "no vproblog/ in dis benchmark",
            ))
        elif resume and _variant_run_done(vp_variant_dir, args.runs):
            logger.info(f"[RQ2/dis {case}/{RQ2_VPROBLOG_VARIANT}] SKIP (resume)")
            row = _rq2_row_from_meta_runs(
                "symbolization", case, RQ2_VPROBLOG_VARIANT, vp_variant_dir,
            )
            rows.append(row or _rq2_skip_row(
                "symbolization", case, RQ2_VPROBLOG_VARIANT, "no runs on disk",
            ))
        else:
            vp_elapseds, vp_status, vp_note = _rq2_run_vproblog_on_dir(
                vp_dir=vp_src_dir,
                work_dir=vp_variant_dir,
                runs=args.runs,
                timeout=args.rq2_run_timeout,
                logger=logger,
                case_label=case,
                category_label="RQ2/dis",
                vlog_bin=Path(getattr(args, "vlog_bin", DEFAULT_VLOG_BIN)),
            )
            vp_runs_n, vp_avg, vp_mn, vp_mx = _rq3_stats(vp_elapseds)
            vp_status_norm = _rq2_status_for(vp_status, vp_runs_n, args.runs)
            rows.append(_rq2_row(
                "symbolization", case, RQ2_VPROBLOG_VARIANT,
                vp_runs_n, vp_avg, vp_mn, vp_mx, vp_status_norm, vp_note,
            ))
    return rows


def _rq2_run_problog_on_file(
    problog_src: Path,
    work_dir: Path,
    runs: int,
    timeout: int,
    logger: Logger,
    case_label: str,
    category_label: str,
) -> Tuple[List[float], str, str]:
    """Run full `problog <src>` (probability inference) `runs` times.
    Returns (elapseds, status, note).

    Switched 2026-04-26 from `problog ground` (which only emits the grounded
    propositional program — useful for timing the grounder but tells us
    nothing about whether problog and souffle agree on query probabilities)
    to full inference, where stdout contains lines of the form
        pred(args):    prob
    that can be cross-checked against souffle's facts.prob output.

    Used by both RQ2/dis (symbolization) and RQ2/taint. ProbLog 2.2.x exits 0
    on inference success (no ground-task quirk to work around). The legacy
    quirk handler is kept as a defensive fallback in case some grounding-
    only version of the wrapper sneaks back in.
    """
    if not problog_src.is_file():
        return [], "skipped", f"no compute.problog.dl at {problog_src}"

    used_bin: Optional[str] = None
    for b in PROBLOG_BIN_CANDIDATES:
        if shutil.which(b):
            used_bin = b
            break
    if used_bin is None:
        return [], "skipped", "problog not on PATH"

    work_dir.mkdir(parents=True, exist_ok=True)
    staged_src = _stage_problog_program(problog_src, work_dir)

    elapseds: List[float] = []
    last_status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = work_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        log_path = run_dir / "problog.log"
        cmd = [used_bin, str(staged_src)]
        rc, elapsed, note = _run_subproc_capture(
            cmd, cwd=run_dir, timeout=timeout, log_path=log_path,
        )
        # Defensive: legacy `problog ground` exited 1 on success with a
        # `(True, '...')` marker in stderr. Full inference exits 0 cleanly.
        # We still scan for the marker so a downgrade to ground mode wouldn't
        # silently re-poison the metas.
        legacy_ground_ok = False
        if rc == 1 and log_path.is_file():
            try:
                with log_path.open("rb") as fh:
                    if b"(True, " in fh.read():
                        legacy_ground_ok = True
            except OSError:
                pass
        if legacy_ground_ok:
            run_status, status_note = "ok", ""
        else:
            run_status, status_note = _classify_status(rc, log_path=log_path)
        if status_note and not note:
            note = status_note
        meta = {
            "engine": RQ2_PROBLOG_VARIANT,
            "case": case_label,
            "run": run_idx,
            "timestamp": stamp,
            "exit": 0 if run_status == "ok" else rc,
            "raw_exit": rc,
            "elapsed": elapsed,
            "status": run_status,
            "note": note,
            "cmd": cmd,
            "bin": used_bin,
            "src": str(problog_src),
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(meta, indent=2), encoding="utf-8",
        )
        if run_status == "ok":
            elapseds.append(elapsed)
            logger.info(
                f"[{category_label} {case_label}/problog] run {run_idx}/{runs} "
                f"OK in {elapsed:.2f}s"
            )
            continue
        last_status = run_status
        last_note = note or f"exit={rc}"
        logger.error(
            f"[{category_label} {case_label}/problog] run {run_idx}/{runs} "
            f"{last_status.upper()} in {elapsed:.2f}s (exit={rc})"
        )
        break
    return elapseds, last_status, last_note


def _scallop_bin() -> Optional[str]:
    for b in SCALLOP_BIN_CANDIDATES:
        if os.path.isabs(b):
            if os.path.isfile(b) and os.access(b, os.X_OK):
                return b
        elif shutil.which(b):
            return shutil.which(b)
    return None


def _rq2_run_scallop_on_file(
    problog_src: Path,
    work_dir: Path,
    runs: int,
    timeout: int,
    logger: Logger,
    case_label: str,
    category_label: str,
) -> Tuple[List[float], str, str]:
    """Run Scallop exact WMC on a case's pre-materialised `compute.scl`,
    `runs` times.  Returns (elapseds, status, note).

    Pipeline per case: pick up `compute.scl` (the Scallop rendering of the
    same program compute.problog.dl encodes, shipped alongside it), then run
        scli <case>.scl -p <prov> --top-k <k>
    under a `ulimit -v` memory cap.  scli's stdout (query marginals, one
    `pred: {p::(args), ...}` line per queried relation) is captured to
    scallop.out for cross-engine correctness checking.  Mirrors
    _rq2_run_problog_on_file's meta/return contract so the RQ2 collector treats
    Scallop identically to the other engines.
    """
    scl_src = problog_src.parent / "compute.scl"
    if not scl_src.is_file():
        return [], "skipped", f"no compute.scl at {scl_src}"
    scli = _scallop_bin()
    if scli is None:
        return [], "skipped", "scli not found (set $SCLI_BIN or add to PATH)"

    work_dir.mkdir(parents=True, exist_ok=True)

    # Snapshot the .scl into the work dir; reuse it across timing runs.
    scl_path = work_dir / "compute.scl"
    scl_path.write_text(scl_src.read_text(encoding="utf-8"), encoding="utf-8")

    scli_cmd = (
        f'"{scli}" "{scl_path.resolve()}" -p {SCALLOP_PROVENANCE} '
        f'--top-k {SCALLOP_TOP_K}'
    )
    if SCALLOP_MEM_LIMIT_MB and SCALLOP_MEM_LIMIT_MB > 0:
        cmd = ["bash", "-c", f"ulimit -v {SCALLOP_MEM_LIMIT_MB * 1024}; exec {scli_cmd}"]
    else:
        cmd = ["bash", "-c", f"exec {scli_cmd}"]

    elapseds: List[float] = []
    last_status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = work_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        log_path = run_dir / "scallop.out"
        rc, elapsed, note = _run_subproc_capture(
            cmd, cwd=run_dir, timeout=timeout, log_path=log_path,
        )
        # A ulimit-v hit makes Rust's allocator abort (SIGABRT / "memory
        # allocation ... failed"); fold that into OOM alongside the generic
        # OOM-killer detection.
        run_status, status_note = _classify_status(rc, log_path=log_path)
        if run_status == "error" and rc in (134, -6) and log_path.is_file():
            try:
                if "memory allocation" in log_path.read_text(
                    encoding="utf-8", errors="ignore"
                ).lower():
                    run_status, status_note = "oom", f"oom (exit={rc})"
            except OSError:
                pass
        if status_note and not note:
            note = status_note
        meta = {
            "engine": RQ2_SCALLOP_VARIANT,
            "case": case_label,
            "run": run_idx,
            "timestamp": stamp,
            "exit": 0 if run_status == "ok" else rc,
            "raw_exit": rc,
            "elapsed": elapsed,
            "status": run_status,
            "note": note,
            "cmd": cmd,
            "bin": scli,
            "provenance": SCALLOP_PROVENANCE,
            "top_k": SCALLOP_TOP_K,
            "mem_limit_mb": SCALLOP_MEM_LIMIT_MB,
            "src": str(scl_src),
            "scl": str(scl_path),
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(meta, indent=2), encoding="utf-8",
        )
        if run_status == "ok":
            elapseds.append(elapsed)
            logger.info(
                f"[{category_label} {case_label}/scallop] run {run_idx}/{runs} "
                f"OK in {elapsed:.2f}s"
            )
            continue
        last_status = run_status
        last_note = note or f"exit={rc}"
        logger.error(
            f"[{category_label} {case_label}/scallop] run {run_idx}/{runs} "
            f"{last_status.upper()} in {elapsed:.2f}s (exit={rc})"
        )
        break
    return elapseds, last_status, last_note


def _rq2_run_scallop_dispatch(
    category: str,
    case_label: str,
    problog_src: Path,
    work_dir: Path,
    args: argparse.Namespace,
    logger: Logger,
    category_label: str,
    resume: bool,
) -> List[str]:
    """Run (or resume) Scallop for one case and return its RQ2 result row."""
    if resume and _variant_run_done(work_dir, args.runs):
        logger.info(f"[{category_label} {case_label}/scallop] SKIP (resume)")
        row = _rq2_row_from_meta_runs(category, case_label, RQ2_SCALLOP_VARIANT, work_dir)
        return row or _rq2_skip_row(category, case_label, RQ2_SCALLOP_VARIANT, "no runs on disk")
    elapseds, status, note = _rq2_run_scallop_on_file(
        problog_src=problog_src,
        work_dir=work_dir,
        runs=args.runs,
        timeout=args.rq2_run_timeout,
        logger=logger,
        case_label=case_label,
        category_label=category_label,
    )
    n, avg, mn, mx = _rq3_stats(elapseds)
    return _rq2_row(category, case_label, RQ2_SCALLOP_VARIANT, n, avg, mn, mx,
                    _rq2_status_for(status, n, args.runs), note)


def _rq2_run_vproblog_on_dir(
    vp_dir: Path,
    work_dir: Path,
    runs: int,
    timeout: int,
    logger: Logger,
    case_label: str,
    category_label: str,
    vlog_bin: Path,
) -> Tuple[List[float], str, str]:
    """Run `vlog mat` on a pre-materialized vproblog/ directory `runs` times.

    The directory must contain ``rules``, ``edb.conf``, ``mappings.csv``, and
    a ``data/`` subtree with one CSV per esrc_<rel>. Vlog is invoked with
    ``cwd=vp_dir`` so the edb.conf's ``param0=data`` resolves correctly.

    Returns (elapseds, status, note). Vlog's existential-skolem chase does
    not converge on most of these benchmarks (recursive rules + conj()
    heads), so TIMEOUT is the expected outcome under RQ2's default 30-min
    cap — the call still records the run honestly.
    """
    def _skip(note: str) -> Tuple[List[float], str, str]:
        logger.warn(
            f"[{category_label} {case_label}/vproblog] SKIP — {note}"
        )
        return [], "skipped", note
    if not vp_dir.is_dir():
        return _skip(f"no vproblog/ at {vp_dir}")
    rules = vp_dir / "rules"
    edb = vp_dir / "edb.conf"
    probs = vp_dir / "mappings.csv"
    if not all(p.is_file() for p in (rules, edb, probs)):
        return _skip("rules/edb.conf/mappings.csv missing")
    if not vlog_bin.is_file():
        return _skip(f"vlog binary missing at {vlog_bin} (set $VLOG_BIN)")

    env = os.environ.copy()
    extra_libdir = str(vlog_bin.parent)
    cur = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{extra_libdir}:{cur}" if cur else extra_libdir

    work_dir.mkdir(parents=True, exist_ok=True)

    elapseds: List[float] = []
    last_status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = work_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        storemat = run_dir / "storemat"
        storemat.mkdir(parents=True, exist_ok=True)
        log_path = run_dir / "vlog.log"
        cmd = [
            str(vlog_bin),
            "mat",
            "-e", str(edb.resolve()),
            "--rules", str(rules.resolve()),
            "--prob_file", str(probs.resolve()),
            "--storemat_path", str(storemat.resolve()),
            "--storemat_format", "csv",
            "--rewriteMultihead", "true",
            "--restrictedChase", "false",
            "--ignoreMagic", "false",
            "-l", "info",
        ]
        rc, elapsed, note = _run_subproc_capture(
            cmd, cwd=vp_dir, timeout=timeout, log_path=log_path, env=env,
        )
        run_status, status_note = _classify_status(rc, log_path=log_path)
        if status_note and not note:
            note = status_note
        meta = {
            "engine": RQ2_VPROBLOG_VARIANT,
            "case": case_label,
            "run": run_idx,
            "timestamp": stamp,
            "exit": rc,
            "elapsed": elapsed,
            "status": run_status,
            "note": note,
            "cmd": cmd,
            "bin": str(vlog_bin),
            "vp_dir": str(vp_dir),
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(meta, indent=2), encoding="utf-8",
        )
        if run_status == "ok":
            elapseds.append(elapsed)
            logger.info(
                f"[{category_label} {case_label}/vproblog] run {run_idx}/{runs} "
                f"OK in {elapsed:.2f}s"
            )
            continue
        last_status = run_status
        last_note = note or f"exit={rc}"
        logger.warn(
            f"[{category_label} {case_label}/vproblog] run {run_idx}/{runs} "
            f"{last_status.upper()} in {elapsed:.2f}s (exit={rc})"
        )
        # On TIMEOUT/OOM subsequent runs would just repeat the same outcome on
        # the same divergent chase / oversized state. Stop early — same
        # convention as ProbLog runner.
        break
    return elapseds, last_status, last_note


def _rq2_taint_run_problog(
    bundle_case: Path,
    work_dir: Path,
    runs: int,
    timeout: int,
    logger: Logger,
) -> Tuple[List[float], str, str]:
    """Run ProbLog on a taint case's per-case `taint/<name>/compute.problog.dl`
    (the merged-stage program). `bundle_case.name` is the case name; the
    program lives at ROOT_DIR/taint/<case>/compute.problog.dl regardless of
    the bundle layout."""
    case_name = bundle_case.name
    problog_src = ROOT_DIR / "taint" / case_name / "compute.problog.dl"
    return _rq2_run_problog_on_file(
        problog_src=problog_src,
        work_dir=work_dir,
        runs=runs,
        timeout=timeout,
        logger=logger,
        case_label=case_name,
        category_label="RQ2/taint",
    )


def _run_rq2_taint(
    args: argparse.Namespace,
    base: Path,
    logger: Logger,
) -> List[List[str]]:
    raw_bundle = getattr(args, "rq2_taint_bundle", None) or args.taint_bundle
    raw_taint_cases = getattr(args, "rq2_taint_cases", None) or args.taint_cases
    bundle = Path(raw_bundle).expanduser().resolve()

    taint_base = base / "taint"
    taint_base.mkdir(parents=True, exist_ok=True)

    # Detect reorganised layout (taint/programs/<stage>.dl + taint/<case>/input)
    # and synthesise a legacy-shaped shim so the rest of RQ2 keeps working.
    new_layout_root = bundle if (bundle / "programs").is_dir() else None
    if new_layout_root is not None:
        new_stages_file = new_layout_root / "stages.txt"
        if not new_stages_file.is_file():
            logger.warn(
                f"[RQ2/taint] new-layout bundle at {new_layout_root} is missing "
                f"stages.txt; skipping taint."
            )
            return []
        new_stages = _taint_read_stages(new_stages_file)
        if raw_taint_cases:
            new_case_names = [c for c in re.split(r"[,\s]+", raw_taint_cases) if c]
        else:
            new_case_names = sorted(
                p.name for p in new_layout_root.iterdir()
                if p.is_dir() and (p / "input").is_dir()
            )
        if not new_case_names:
            logger.warn("[RQ2/taint] no cases; skipping.")
            return []
        shim = _rq2_build_taint_shim(
            new_root=new_layout_root,
            shim_root=taint_base / "_legacy_shim",
            case_names=new_case_names,
            new_stages=new_stages,
            logger=logger,
        )
        if shim is None:
            logger.error("[RQ2/taint] failed to build legacy shim; skipping taint.")
            return []
        bundle = shim
        case_names = new_case_names
        stages = [s + RQ2_TAINT_STAGE_SUFFIX for s in new_stages]
        cases_root = bundle / "cases"
    else:
        stages_file = bundle / "pipeline" / "stages.txt"
        cases_root = bundle / "cases"
        if not stages_file.is_file() or not cases_root.is_dir():
            logger.warn(
                f"[RQ2/taint] bundle missing (stages={stages_file}, cases={cases_root}); "
                f"skipping taint."
            )
            return []
        stages = _taint_read_stages(stages_file)
        if raw_taint_cases:
            case_names = [c for c in re.split(r"[,\s]+", raw_taint_cases) if c]
        else:
            case_names = sorted(p.name for p in cases_root.iterdir() if p.is_dir())
        if not case_names:
            logger.warn("[RQ2/taint] no cases; skipping.")
            return []

    shared_build = taint_base / "_shared_build_rq2"
    shared_build.mkdir(parents=True, exist_ok=True)

    souffle_bin = args.souffle_bin or _taint_detect_souffle_bin(None)

    engines = getattr(args, "_rq2_engines_set", set(RQ2_ALL_ENGINES))
    if "souffle" in engines:
        # Compile each stage once, no extra compile flags.
        for stage in stages:
            stage_build = shared_build / stage
            exe = stage_build / "compute"
            if exe.is_file() and not args.force_compile:
                continue
            if exe.is_file():
                try:
                    exe.unlink()
                except OSError:
                    pass
            src_dl = bundle / "pipeline" / "shared_build" / stage / "compute.souffle.dl"
            if not src_dl.is_file():
                logger.warn(f"[RQ2/taint] missing stage dl: {src_dl}")
                continue
            code, elapsed, note = _taint_compile_stage(
                souffle_bin, src_dl, stage_build, [], args.compile_timeout,
            )
            if code != 0:
                logger.error(
                    f"[RQ2/taint compile {stage}] failed (rc={code}, {note}, "
                    f"{elapsed:.2f}s)"
                )
            else:
                logger.info(f"[RQ2/taint compile {stage}] OK in {elapsed:.2f}s")

    resume = bool(getattr(args, "resume", False))
    rows: List[List[str]] = []
    for idx, case in enumerate(case_names, 1):
        bundle_case = cases_root / case
        if not bundle_case.is_dir():
            logger.warn(f"[RQ2/taint] missing case dir: {bundle_case}")
            continue
        logger.info(f"[RQ2/taint {idx}/{len(case_names)}] {case}")

        if "souffle" in engines:
            # --- Souffle (--det-opt -r): full pipeline per run, sum stage times.
            case_work_root = taint_base / "runs" / case / RQ2_SOUFFLE_VARIANT
            case_work_root.mkdir(parents=True, exist_ok=True)
            souffle_resumed = False
            if resume and case_work_root.is_dir():
                ok_runs = 0
                saw_failure = False
                for rd in case_work_root.iterdir():
                    if rd.is_dir() and rd.name.startswith("run_"):
                        _, st, _ = _rq3_taint_run_summary(rd, case)
                        if st == "ok":
                            ok_runs += 1
                        elif st in ("timeout", "error"):
                            saw_failure = True
                if ok_runs >= args.runs or saw_failure:
                    souffle_resumed = True
                    logger.info(
                        f"[RQ2/taint {case}/{RQ2_SOUFFLE_VARIANT}] SKIP (resume — "
                        + ("done" if ok_runs >= args.runs else "prior failure on disk")
                        + ")"
                    )
                    row = _rq2_taint_souffle_row_from_disk(case, case_work_root)
                    if row is not None:
                        rows.append(row)
                    else:
                        rows.append(_rq2_skip_row(
                            "taint", case, RQ2_SOUFFLE_VARIANT, "no runs on disk",
                        ))
            if not souffle_resumed:
                elapseds: List[float] = []
                souffle_status = "OK"
                souffle_note = ""
                for run_idx in range(1, args.runs + 1):
                    run_work = case_work_root / f"run_{run_idx:02d}"
                    if run_work.exists():
                        shutil.rmtree(run_work)
                    result = _taint_run_case(
                        souffle_bin=souffle_bin,
                        bundle_case=bundle_case,
                        shared_build=shared_build,
                        stages=stages,
                        run_extra=["--det-opt", "-r"],
                        work_root=run_work,
                        timeout=args.rq2_run_timeout,
                    )
                    logger.info(
                        f"[RQ2/taint {case}/souffle] run {run_idx}/{args.runs} "
                        f"status={result.status} stages={result.stages_completed}/{len(stages)} "
                        f"elapsed={result.elapsed_s:.2f}s"
                    )
                    if result.status == "ok":
                        elapseds.append(result.elapsed_s)
                        continue
                    souffle_note = result.note or result.status
                    souffle_status = "TIMEOUT" if result.status == "timeout" else "FAIL"
                    break
                if elapseds and souffle_status == "OK" and len(elapseds) < args.runs:
                    souffle_status = "PARTIAL"
                runs_n, avg, mn, mx = _rq3_stats(elapseds)
                rows.append(_rq2_row(
                    "taint", case, RQ2_SOUFFLE_VARIANT,
                    runs_n, avg, mn, mx, souffle_status, souffle_note,
                ))

        if "problog" in engines:
            # --- ProbLog: pt-obj-dlog stage only (matches taint_full convention).
            pl_work_dir = taint_base / "runs" / case / RQ2_PROBLOG_VARIANT
            if resume and _variant_run_done(pl_work_dir, args.runs):
                logger.info(f"[RQ2/taint {case}/{RQ2_PROBLOG_VARIANT}] SKIP (resume)")
                row = _rq2_row_from_meta_runs(
                    "taint", case, RQ2_PROBLOG_VARIANT, pl_work_dir,
                )
                if row is not None:
                    rows.append(row)
                else:
                    rows.append(_rq2_skip_row(
                        "taint", case, RQ2_PROBLOG_VARIANT, "no runs on disk",
                    ))
            else:
                pl_elapseds, pl_status, pl_note = _rq2_taint_run_problog(
                    bundle_case=bundle_case,
                    work_dir=pl_work_dir,
                    runs=args.runs,
                    timeout=args.rq2_run_timeout,
                    logger=logger,
                )
                pl_runs_n, pl_avg, pl_mn, pl_mx = _rq3_stats(pl_elapseds)
                pl_status_norm = _rq2_status_for(pl_status, pl_runs_n, args.runs)
                rows.append(_rq2_row(
                    "taint", case, RQ2_PROBLOG_VARIANT,
                    pl_runs_n, pl_avg, pl_mn, pl_mx, pl_status_norm, pl_note,
                ))

        if "scallop" in engines:
            scallop_work_dir = taint_base / "runs" / case / RQ2_SCALLOP_VARIANT
            if resume and _variant_run_done(scallop_work_dir, args.runs):
                logger.info(f"[RQ2/taint {case}/{RQ2_SCALLOP_VARIANT}] SKIP (resume)")
                row = _rq2_row_from_meta_runs(
                    "taint", case, RQ2_SCALLOP_VARIANT, scallop_work_dir,
                )
                rows.append(row or _rq2_skip_row(
                    "taint", case, RQ2_SCALLOP_VARIANT, "no runs on disk",
                ))
            else:
                scallop_elapseds, scallop_status, scallop_note = (
                    _rq2_run_scallop_taint_stages(
                        case=case,
                        case_input=ROOT_DIR / "taint" / case / "input",
                        work_dir=scallop_work_dir,
                        stages=_rq2_taint_vproblog_stages(),
                        runs=args.runs,
                        timeout=args.rq2_run_timeout,
                        logger=logger,
                    )
                )
                sc_runs_n, sc_avg, sc_mn, sc_mx = _rq3_stats(scallop_elapseds)
                rows.append(_rq2_row(
                    "taint", case, RQ2_SCALLOP_VARIANT,
                    sc_runs_n, sc_avg, sc_mn, sc_mx,
                    _rq2_status_for(scallop_status, sc_runs_n, args.runs),
                    scallop_note,
                ))

        if "vproblog" not in engines:
            continue

        # --- vproblog: chain ALL stages, sum per-stage wall-clock to match
        # souffle's per-case multi-stage total. Per-stage artefacts live at
        # taint/<case>/vproblog/<stage>/. Stops on first stage failure.
        vp_case_root = ROOT_DIR / "taint" / case / "vproblog"
        vp_work_dir = taint_base / "runs" / case / RQ2_VPROBLOG_VARIANT
        vp_stages = _rq2_taint_vproblog_stages()
        missing_stages = [s for s in vp_stages if not (vp_case_root / s).is_dir()]
        if missing_stages:
            rows.append(_rq2_skip_row(
                "taint", case, RQ2_VPROBLOG_VARIANT,
                f"missing vproblog stages: {','.join(missing_stages)}",
            ))
        elif resume and _variant_run_done(vp_work_dir, args.runs):
            logger.info(f"[RQ2/taint {case}/{RQ2_VPROBLOG_VARIANT}] SKIP (resume)")
            row = _rq2_row_from_meta_runs(
                "taint", case, RQ2_VPROBLOG_VARIANT, vp_work_dir,
            )
            rows.append(row or _rq2_skip_row(
                "taint", case, RQ2_VPROBLOG_VARIANT, "no runs on disk",
            ))
        else:
            vp_elapseds, vp_status, vp_note = _rq2_run_vproblog_taint_stages(
                case=case,
                case_vp_root=vp_case_root,
                work_dir=vp_work_dir,
                stages=vp_stages,
                runs=args.runs,
                timeout=args.rq2_run_timeout,
                logger=logger,
                vlog_bin=Path(getattr(args, "vlog_bin", DEFAULT_VLOG_BIN)),
            )
            vp_runs_n, vp_avg, vp_mn, vp_mx = _rq3_stats(vp_elapseds)
            vp_status_norm = _rq2_status_for(vp_status, vp_runs_n, args.runs)
            rows.append(_rq2_row(
                "taint", case, RQ2_VPROBLOG_VARIANT,
                vp_runs_n, vp_avg, vp_mn, vp_mx, vp_status_norm, vp_note,
            ))
    return rows


def _rq2_taint_vproblog_stages() -> List[str]:
    """Read taint/stages.txt for short stage names (cipt-cg, pre, ...).
    These are the per-stage subdirs under taint/<case>/vproblog/."""
    p = ROOT_DIR / "taint" / "stages.txt"
    if not p.is_file():
        return []
    return [
        ln.strip()
        for ln in p.read_text(encoding="utf-8").splitlines()
        if ln.strip() and not ln.strip().startswith("#")
    ]


def _rq2_run_scallop_taint_stages(
    case: str,
    case_input: Path,
    work_dir: Path,
    stages: Sequence[str],
    runs: int,
    timeout: int,
    logger: Logger,
) -> Tuple[List[float], str, str]:
    """Run taint as five chained Scallop stages, matching VProbLog's grain.

    Each stage is generated from ``taint/programs/<stage>.dl`` and the current
    chained EDB. Successful output tuples become deterministic EDB facts for
    the next stage, deliberately matching the existing Souffle/VProbLog stage
    boundary semantics. Only scli wall time is included in the per-case total.
    """
    scli = _scallop_bin()
    if scli is None:
        return [], "skipped", "scli not found (set $SCLI_BIN or add to PATH)"
    if not case_input.is_dir():
        return [], "skipped", f"missing taint input at {case_input}"
    if not stages:
        return [], "skipped", "no taint stages"

    work_dir.mkdir(parents=True, exist_ok=True)
    elapseds: List[float] = []
    last_status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = work_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        total = 0.0
        run_status = "ok"
        run_note = ""
        run_exit = 0
        stage_metas: List[Dict[str, object]] = []
        stage_logs: List[Tuple[str, Path]] = []

        with tempfile.TemporaryDirectory(prefix="chained_input_", dir=run_dir) as scratch:
            chained_input = Path(scratch)
            _taint_copy_base_input(case_input, chained_input)
            for stage in stages:
                program_path = ROOT_DIR / "taint" / "programs" / f"{stage}.dl"
                stage_dir = run_dir / stage
                stage_dir.mkdir(parents=True, exist_ok=True)
                scl_path = stage_dir / "compute.scl"
                log_path = stage_dir / "scallop.out"
                logger.info(
                    f"[RQ2/taint {case}/scallop/{stage}] Run "
                    f"({run_idx}/{runs})"
                )
                stage_started = time.monotonic()
                try:
                    scl_text, output_names = build_stage_scl(program_path, chained_input)
                    scl_path.write_text(scl_text, encoding="utf-8")
                except Exception as exc:
                    elapsed = time.monotonic() - stage_started
                    stage_status = "error"
                    note = f"stage build failed: {exc}"
                    stage_meta = {
                        "stage": stage,
                        "exit": 1,
                        "elapsed": elapsed,
                        "status": stage_status,
                        "note": note,
                        "program": str(program_path),
                    }
                    (stage_dir / "run.meta.json").write_text(
                        json.dumps(stage_meta, indent=2), encoding="utf-8",
                    )
                    stage_metas.append(stage_meta)
                    run_status, run_note, run_exit = stage_status, note, 1
                    break

                scli_cmd = (
                    f'"{scli}" "{scl_path.resolve()}" -p {SCALLOP_PROVENANCE} '
                    f'--top-k {SCALLOP_TOP_K}'
                )
                if SCALLOP_MEM_LIMIT_MB and SCALLOP_MEM_LIMIT_MB > 0:
                    cmd = [
                        "bash", "-c",
                        f"ulimit -v {SCALLOP_MEM_LIMIT_MB * 1024}; exec {scli_cmd}",
                    ]
                else:
                    cmd = ["bash", "-c", f"exec {scli_cmd}"]
                rc, elapsed, note = _run_subproc_capture(
                    cmd, cwd=stage_dir, timeout=timeout, log_path=log_path,
                )
                total += elapsed
                stage_status, status_note = _classify_status(rc, log_path=log_path)
                if stage_status == "error" and rc in (134, -6) and log_path.is_file():
                    if "memory allocation" in log_path.read_text(
                        encoding="utf-8", errors="ignore",
                    ).lower():
                        stage_status, status_note = "oom", f"oom (exit={rc})"
                if status_note and not note:
                    note = status_note
                output_counts: Dict[str, int] = {}
                if stage_status == "ok":
                    output_tuples = parse_scallop_outputs(
                        log_path.read_text(encoding="utf-8", errors="replace"),
                        output_names,
                    )
                    materialize_scallop_stage_outputs(
                        chained_input, output_names, output_tuples,
                    )
                    output_counts = {
                        relation: len(rows) for relation, rows in output_tuples.items()
                    }
                stage_meta = {
                    "stage": stage,
                    "exit": rc,
                    "elapsed": elapsed,
                    "status": stage_status,
                    "note": note,
                    "cmd": cmd,
                    "program": str(program_path),
                    "outputs": output_counts,
                }
                (stage_dir / "run.meta.json").write_text(
                    json.dumps(stage_meta, indent=2), encoding="utf-8",
                )
                stage_metas.append(stage_meta)
                stage_logs.append((stage, log_path))
                if stage_status == "ok":
                    logger.info(
                        f"[RQ2/taint {case}/scallop/{stage}] OK in {elapsed:.2f}s"
                    )
                    continue
                run_status = stage_status
                run_note = f"{note or f'exit={rc}'} at {stage}"
                run_exit = rc
                logger.warn(
                    f"[RQ2/taint {case}/scallop/{stage}] {stage_status.upper()} "
                    f"in {elapsed:.2f}s (exit={rc})"
                )
                break

        # Keep a conventional top-level scallop.out for collectors and manual
        # correctness checks; it contains the query output from every stage.
        with (run_dir / "scallop.out").open("w", encoding="utf-8") as combined:
            for stage, log_path in stage_logs:
                combined.write(f"// stage: {stage}\n")
                combined.write(log_path.read_text(encoding="utf-8", errors="replace"))
                combined.write("\n")

        summary = {
            "engine": RQ2_SCALLOP_VARIANT,
            "case": case,
            "run": run_idx,
            "timestamp": stamp,
            "exit": run_exit,
            "raw_exit": run_exit,
            "elapsed": total,
            "status": run_status,
            "note": run_note,
            "bin": scli,
            "provenance": SCALLOP_PROVENANCE,
            "top_k": SCALLOP_TOP_K,
            "mem_limit_mb": SCALLOP_MEM_LIMIT_MB,
            "staged": True,
            "stages": stage_metas,
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8",
        )
        if run_status == "ok":
            elapseds.append(total)
            logger.info(
                f"[RQ2/taint {case}/scallop] run {run_idx}/{runs} OK total "
                f"{total:.2f}s across {len(stages)} stages"
            )
            continue
        last_status, last_note = run_status, run_note
        break
    return elapseds, last_status, last_note


def _rq2_run_vproblog_taint_stages(
    case: str,
    case_vp_root: Path,
    work_dir: Path,
    stages: Sequence[str],
    runs: int,
    timeout: int,
    logger: Logger,
    vlog_bin: Path,
) -> Tuple[List[float], str, str]:
    """Run vlog mat for every taint stage in order; sum per-stage wall-clock
    so the per-case total compares to souffle's chained-stage total. Each
    stage runs on its own pre-materialised artefact at
    taint/<case>/vproblog/<stage>/. The pre-materialized stage artifacts
    already carry upstream-derived facts as EDB in their esrc_<rel>.csv and
    mappings.csv files. At
    runtime we just run each stage's vlog mat. Stops on first stage failure.

    Per-run layout::
        work_dir/run_NN_<stamp>/
            run.meta.json   (summary: engine/exit/elapsed=sum/status/stages=[...])
            <stage>/run.meta.json + vlog.log + storemat/   (per stage)
    """
    if not vlog_bin.is_file():
        note = f"vlog binary missing at {vlog_bin} (set $VLOG_BIN)"
        logger.warn(f"[RQ2/taint {case}/vproblog] SKIP — {note}")
        return [], "skipped", note
    if not stages:
        logger.warn(f"[RQ2/taint {case}/vproblog] SKIP — no stages")
        return [], "skipped", "no stages"

    work_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    extra_libdir = str(vlog_bin.parent)
    cur = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{extra_libdir}:{cur}" if cur else extra_libdir

    elapseds: List[float] = []
    last_status = "ok"
    last_note = ""
    for run_idx in range(1, runs + 1):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        run_dir = work_dir / f"run_{run_idx:02d}_{stamp}"
        run_dir.mkdir(parents=True, exist_ok=True)
        total = 0.0
        stage_metas: List[Dict[str, object]] = []
        run_status = "ok"
        run_note = ""
        run_exit = 0
        for stage in stages:
            stage_src = case_vp_root / stage
            stage_dir = run_dir / stage
            stage_dir.mkdir(parents=True, exist_ok=True)
            storemat = stage_dir / "storemat"
            storemat.mkdir(parents=True, exist_ok=True)
            log_path = stage_dir / "vlog.log"
            cmd = [
                str(vlog_bin), "mat",
                "-e", str((stage_src / "edb.conf").resolve()),
                "--rules", str((stage_src / "rules").resolve()),
                "--prob_file", str((stage_src / "mappings.csv").resolve()),
                "--storemat_path", str(storemat.resolve()),
                "--storemat_format", "csv",
                "--rewriteMultihead", "true",
                "--restrictedChase", "false",
                "--ignoreMagic", "false",
                "-l", "info",
            ]
            # Per-stage gets the full per-run timeout — souffle does the same
            # (each stage has the remaining wall-clock budget). Use full timeout
            # here for consistency with the per-stage measurement intent.
            rc, elapsed, note = _run_subproc_capture(
                cmd, cwd=stage_src, timeout=timeout, log_path=log_path, env=env,
            )
            total += elapsed
            stage_status, stage_status_note = _classify_status(rc, log_path=log_path)
            if stage_status_note and not note:
                note = stage_status_note
            stage_meta = {
                "stage": stage,
                "exit": rc,
                "elapsed": elapsed,
                "status": stage_status,
                "note": note,
                "cmd": cmd,
            }
            (stage_dir / "run.meta.json").write_text(
                json.dumps(stage_meta, indent=2), encoding="utf-8",
            )
            stage_metas.append(stage_meta)
            if rc == 0:
                logger.info(
                    f"[RQ2/taint {case}/vproblog/{stage}] OK in {elapsed:.2f}s"
                )
                continue
            run_status = stage_status
            run_note = note or f"exit={rc} at {stage}"
            run_exit = rc
            logger.warn(
                f"[RQ2/taint {case}/vproblog/{stage}] {stage_status.upper()} in "
                f"{elapsed:.2f}s (exit={rc}; {run_note})"
            )
            break

        summary = {
            "engine": RQ2_VPROBLOG_VARIANT,
            "case": case,
            "run": run_idx,
            "timestamp": stamp,
            "exit": run_exit,
            "elapsed": total,
            "status": run_status,
            "note": run_note,
            "bin": str(vlog_bin),
            "stages": stage_metas,
        }
        (run_dir / "run.meta.json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8",
        )

        if run_status == "ok":
            elapseds.append(total)
            logger.info(
                f"[RQ2/taint {case}/vproblog] run {run_idx}/{runs} OK total "
                f"{total:.2f}s across {len(stages)} stages"
            )
            continue
        last_status = run_status
        last_note = run_note
        logger.warn(
            f"[RQ2/taint {case}/vproblog] run {run_idx}/{runs} {run_status.upper()} "
            f"after {total:.2f}s"
        )
        # Same break-on-failure convention as the single-stage runner — the
        # failing stage will reproduce on retry.
        break
    return elapseds, last_status, last_note


def _rq2_row_from_meta_runs(
    category: str,
    case: str,
    engine: str,
    runs_root: Path,
) -> Optional[List[str]]:
    """Aggregate one (case, engine) cell from existing run.meta.json files
    under `runs_root`. Returns None when there are no runs to fold in."""
    if not runs_root.is_dir():
        return None
    elapseds, status, note = _rq2_collect_meta_runs(runs_root)
    if not elapseds and status == "missing":
        return None
    runs_n, avg, mn, mx = _rq3_stats(elapseds)
    status_norm = _rq2_status_for(status, runs_n, runs_n)
    return _rq2_row(category, case, engine, runs_n, avg, mn, mx, status_norm, note)


def _rq2_taint_souffle_row_from_disk(
    case: str, variant_dir: Path,
) -> Optional[List[str]]:
    """RQ2 taint souffle stores per-stage metas under
    <variant_dir>/run_NN/<case>/stages/<stage>/run.meta.json. Sum per-run wall
    times and synthesise the row."""
    if not variant_dir.is_dir():
        return None
    elapseds: List[float] = []
    n_ok = 0
    last_status = "ok"
    last_note = ""
    for run_dir in sorted(variant_dir.iterdir()):
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        total, run_status, run_note = _rq3_taint_run_summary(run_dir, case)
        if run_status == "missing":
            continue
        elapseds.append(total)
        if run_status == "ok":
            n_ok += 1
            continue
        last_status = run_status
        last_note = run_note or run_status
        break
    if not elapseds:
        return None
    status_norm = _rq2_status_for(last_status, n_ok, len(elapseds))
    runs_n, avg, mn, mx = _rq3_stats(elapseds)
    return _rq2_row(
        "taint", case, RQ2_SOUFFLE_VARIANT,
        runs_n, avg, mn, mx, status_norm, last_note,
    )


def _collect_rq2_sidechannel(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    sc_base = base / "sidechannel"
    if not sc_base.is_dir():
        return rows
    for case_dir_ in sorted(sc_base.iterdir(), key=_rq3_case_key_p):
        if not case_dir_.is_dir() or not case_dir_.name.startswith("P"):
            continue
        label = case_dir_.name
        runs_dir = case_dir_ / RQ2_RUN_ROOT
        for engine, sub in (
            (RQ2_SOUFFLE_VARIANT, RQ2_SOUFFLE_VARIANT),
            (RQ2_PROBLOG_VARIANT, "problog"),
            (RQ2_SCALLOP_VARIANT, RQ2_SCALLOP_VARIANT),
            (RQ2_VPROBLOG_VARIANT, RQ2_VPROBLOG_VARIANT),
        ):
            row = _rq2_row_from_meta_runs(
                "sidechannel", label, engine, runs_dir / sub
            )
            if row is not None:
                rows.append(row)
    return rows


def _collect_rq2_symbolization(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    dis_base = base / "symbolization"
    if not dis_base.is_dir():
        return rows
    # Match the live runner: per-case `compute.problog.dl` lives under
    # symbolization/<case>/, and the actual run metas land in
    # artifact/runs/rq2/symbolization/<case>/problog/run_*/.
    inputs_root = ROOT_DIR / "symbolization"
    for case_dir_ in sorted(dis_base.iterdir()):
        if not case_dir_.is_dir() or case_dir_.name.startswith("_"):
            continue
        case = case_dir_.name
        row = _rq2_row_from_meta_runs(
            "symbolization", case, RQ2_SOUFFLE_VARIANT,
            case_dir_ / RQ2_SOUFFLE_VARIANT,
        )
        if row is not None:
            rows.append(row)
        problog_meta_dir = case_dir_ / RQ2_PROBLOG_VARIANT
        problog_src = inputs_root / case / "compute.problog.dl"
        pl_row = None
        if problog_meta_dir.is_dir():
            pl_row = _rq2_row_from_meta_runs(
                "symbolization", case, RQ2_PROBLOG_VARIANT, problog_meta_dir,
            )
        if pl_row is not None:
            rows.append(pl_row)
        elif not problog_src.is_file():
            rows.append(_rq2_skip_row(
                "symbolization", case, RQ2_PROBLOG_VARIANT,
                "no compute.problog.dl in dis benchmark",
            ))
        else:
            rows.append(_rq2_skip_row(
                "symbolization", case, RQ2_PROBLOG_VARIANT, "no runs on disk",
            ))

        scallop_row = _rq2_row_from_meta_runs(
            "symbolization", case, RQ2_SCALLOP_VARIANT,
            case_dir_ / RQ2_SCALLOP_VARIANT,
        )
        if scallop_row is not None:
            rows.append(scallop_row)

        vp_meta_dir = case_dir_ / RQ2_VPROBLOG_VARIANT
        vp_src = inputs_root / case / "vproblog"
        vp_row = None
        if vp_meta_dir.is_dir():
            vp_row = _rq2_row_from_meta_runs(
                "symbolization", case, RQ2_VPROBLOG_VARIANT, vp_meta_dir,
            )
        if vp_row is not None:
            rows.append(vp_row)
        elif not vp_src.is_dir():
            rows.append(_rq2_skip_row(
                "symbolization", case, RQ2_VPROBLOG_VARIANT,
                "no vproblog/ in dis benchmark",
            ))
        else:
            rows.append(_rq2_skip_row(
                "symbolization", case, RQ2_VPROBLOG_VARIANT, "no runs on disk",
            ))
    return rows


def _collect_rq2_taint(base: Path) -> List[List[str]]:
    rows: List[List[str]] = []
    taint_base = base / "taint"
    runs_root = taint_base / "runs"
    if not runs_root.is_dir():
        return rows
    case_names = sorted(p.name for p in runs_root.iterdir() if p.is_dir())
    for case in case_names:
        case_runs_dir = runs_root / case
        souffle_row = _rq2_taint_souffle_row_from_disk(
            case, case_runs_dir / RQ2_SOUFFLE_VARIANT,
        )
        if souffle_row is not None:
            rows.append(souffle_row)

        pl_runs_dir = case_runs_dir / RQ2_PROBLOG_VARIANT
        pl_row = _rq2_row_from_meta_runs(
            "taint", case, RQ2_PROBLOG_VARIANT, pl_runs_dir,
        )
        if pl_row is not None:
            rows.append(pl_row)

        scallop_runs_dir = case_runs_dir / RQ2_SCALLOP_VARIANT
        scallop_row = _rq2_row_from_meta_runs(
            "taint", case, RQ2_SCALLOP_VARIANT, scallop_runs_dir,
        )
        if scallop_row is not None:
            rows.append(scallop_row)

        vp_runs_dir = case_runs_dir / RQ2_VPROBLOG_VARIANT
        vp_row = _rq2_row_from_meta_runs(
            "taint", case, RQ2_VPROBLOG_VARIANT, vp_runs_dir,
        )
        if vp_row is not None:
            rows.append(vp_row)
    return rows


def _write_rq2_summary(
    base: Path, rows: Sequence[Sequence[str]], logger: Logger
) -> Path:
    out_tsv = base / RQ2_RESULT_FILENAME
    base.mkdir(parents=True, exist_ok=True)
    with out_tsv.open("w", encoding="utf-8") as fh:
        fh.write("\t".join(RQ2_SUMMARY_HEADER) + "\n")
        for r in rows:
            fh.write("\t".join(str(x) for x in r) + "\n")
    logger.banner("FMCAD -RQ2 runtime summary")
    logger.info("\n" + _render_table(RQ2_SUMMARY_HEADER, rows))
    logger.info(f"Wrote {out_tsv}")
    return out_tsv


def cmd_rq2(args: argparse.Namespace) -> None:
    base = Path(args.rq2_base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    log_file = (
        Path(args.rq2_log_file)
        if args.rq2_log_file
        else (base / "fmcad_rq2.log")
    )
    log_file.unlink(missing_ok=True)
    logger = Logger(base, log_file, quiet=args.quiet)
    args._rq2_engines_set = _rq2_selected_engines(args)

    if args.rq2_collect:
        logger.banner("FMCAD -RQ2 --collect: aggregate existing run.meta.json only")
        logger.info(f"Output root: {base}")
        rows: List[List[str]] = []
        if not args.skip_sc:
            rows.extend(_collect_rq2_sidechannel(base))
        if not args.skip_taint:
            rows.extend(_collect_rq2_taint(base))
        if not args.skip_dis:
            rows.extend(_collect_rq2_symbolization(base))
        _write_rq2_summary(base, rows, logger)
        return

    logger.banner(
        "FMCAD -RQ2: souffle --det-opt -r + problog "
        "across sidechannel / taint / symbolization"
    )
    logger.info(f"Output root: {base}")
    mem_cap_str = (
        f"{args.mem_limit_mb} MiB"
        if args.mem_limit_mb and args.mem_limit_mb > 0
        else "disabled"
    )
    logger.info(
        f"Runs per case/engine: {args.runs}, "
        f"run timeout: {args.rq2_run_timeout}s, "
        f"mem cap: {mem_cap_str}"
    )
    engines_set = args._rq2_engines_set
    if engines_set != set(RQ2_ALL_ENGINES):
        ordered = [e for e in RQ2_ALL_ENGINES if e in engines_set]
        logger.info(f"Engines (filtered): {', '.join(ordered)}")

    rows: List[List[str]] = []
    if not args.skip_sc:
        rows.extend(_run_rq2_sidechannel(args, base, logger))
    if not args.skip_taint:
        rows.extend(_run_rq2_taint(args, base, logger))
    if not args.skip_dis:
        rows.extend(_run_rq2_symbolization(args, base, logger))

    _write_rq2_summary(base, rows, logger)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "FMCAD runner. Default (or -sc) generates each case and runs two "
            "engines — Souffle (rewrite+BDD) and ProbLog — under artifact/FMCADSC. "
            "Use --collect to rebuild the runtime summary table from existing "
            "output logs without re-running."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "-sc",
        dest="mode",
        action="store_const",
        const="sc",
        help="Generate + run Souffle rewrite and ProbLog per case (default).",
    )
    p.add_argument(
        "-dis",
        dest="mode",
        action="store_const",
        const="dis",
        help=(
            "Run the symbolization benchmark through two souffle engines "
            "(no-rewrite, implicit-rewrite) per case. Output lands under "
            "--dis-base-dir."
        ),
    )
    p.add_argument(
        "-RQ1",
        dest="mode",
        action="store_const",
        const="rq1",
        help=(
            "Run souffle with --det-opt --rewrite across all sidechannel, taint, "
            "and symbolization cases. Collect per-case derivation-graph "
            "node/edge counts and rewrite reduction rates into a single table."
        ),
    )
    p.add_argument(
        "-RQ3",
        dest="mode",
        action="store_const",
        const="rq3",
        help=(
            "Run souffle across sidechannel / taint / symbolization under two "
            "runtime configs (one compile): 01 (--det-opt), "
            "11 (--det-opt -r). Collects per-case/"
            "variant wall-clock runtime."
        ),
    )
    p.add_argument(
        "-RQ2",
        dest="mode",
        action="store_const",
        const="rq2",
        help=(
            "Run souffle (--det-opt -r) + problog across the selected "
            "benchmarks (sc / taint / disasm; default = all three). Default "
            "per-run timeout is 30 minutes; no per-process memory cap unless "
            "--mem-limit-mb is set. Use --skip-sc/--skip-taint/--skip-dis to "
            "subset categories."
        ),
    )
    p.add_argument("--collect", action="store_true",
                   help="Skip run; aggregate runtime table from run.meta.json / log file.")
    p.add_argument("--mem-limit-mb", type=int, default=DEFAULT_MEM_LIMIT_MB,
                   help="Per-subprocess virtual-memory cap in MiB applied via "
                        "RLIMIT_AS/RLIMIT_DATA to every spawned child "
                        "(souffle, problog, ...). 0 disables (default).")
    p.add_argument("--base-dir", default=str(DEFAULT_BASE_DIR),
                   help="Output root for generated cases and runs.")
    p.add_argument("--souffle-bin", default=DEFAULT_SOUFFLE_BIN,
                   help="Souffle compiler binary.")
    p.add_argument("--vlog-bin", default=str(DEFAULT_VLOG_BIN),
                   help="Vlog (vproblog) binary path. Defaults to $VLOG_BIN "
                        "or /opt/vproblog/src/vlog-beta-sdd/build/vlog.")
    p.add_argument("--source-dir", default=None,
                   help="SMT source directory (defaults to side_channel_benchmark/data/smt).")
    p.add_argument("case_tokens", nargs="*",
                   help="Case tokens like P1-P20 P3-P8 P11 (optional).")
    p.add_argument("--cases", "--case", dest="cases", default=None,
                   help="Case spec, e.g. '1-5,7'.")
    p.add_argument("--size", type=int, default=None,
                   help="Limit to the first N cases.")
    p.add_argument("--runs", type=int, default=5,
                   help="Runs per engine/variant.")
    p.add_argument("--cleanup", action="store_true",
                   help="Clean <base-dir> before generation.")
    p.add_argument("--skip-generate", action="store_true",
                   help="Skip the generate step (use cached programs).")
    p.add_argument("--generate-timeout", type=int, default=600,
                   help="Generate timeout (s).")
    p.add_argument("--compile-timeout", type=int, default=300,
                   help="Compile timeout (s) per case.")
    p.add_argument("--run-timeout", type=int, default=300,
                   help="Run timeout (s) per case.")
    p.add_argument("--force-compile", action="store_true",
                   help="Recompile even if the binary already exists.")
    p.add_argument("--souffle-arg", action="append",
                   help="Extra args passed to 'souffle' (repeatable).")
    p.add_argument("--include-dir", default=str(PSOUFFLE_ROOT / "src" / "include"),
                   help="Souffle include dir.")
    p.add_argument("--log-prefix", default="log.txt",
                   help="Souffle --logfile prefix.")
    p.add_argument("--run-arg", action="append",
                   help="Extra args passed to the compiled Souffle binary (repeatable).")
    p.add_argument("--log-file", default=None,
                   help="Log path (default: <base-dir>/fmcad_sc.log).")
    p.add_argument("--quiet", action="store_true",
                   help="Silence console progress (file logging remains).")
    # -dis flags ------------------------------------------------------------
    p.add_argument("--dis-base-dir", default=str(DEFAULT_DIS_BASE_DIR),
                   help="Output root for -dis (per-case engine subdirs land here).")
    p.add_argument("--dis-source-dl", default=str(DEFAULT_DIS_SOURCE_DL),
                   help="Symbolization Datalog program shared across cases.")
    p.add_argument("--dis-inputs-dir", default=str(DEFAULT_DIS_INPUTS_DIR),
                   help="Root directory containing one subdir per symbolization case "
                        "(uses <case>/input when present).")
    p.add_argument("--dis-cases", default=None,
                   help="Comma/space-separated case names to restrict -dis to "
                        "(default: every subdir under --dis-inputs-dir).")
    p.add_argument("--dis-run-timeout", type=int, default=120,
                   help="Per-engine, per-run timeout (s) for -dis.")
    p.add_argument("--dis-log-file", default=None,
                   help="Log path for -dis (default: <dis-base-dir>/fmcad_dis.log).")
    # -RQ1 flags -------------------------------------------------------------
    p.add_argument("--rq1-base-dir", default=str(DEFAULT_RQ1_BASE_DIR),
                   help="Output root for -RQ1 (per-category subdirs land here).")
    p.add_argument("--rq1-run-timeout", type=int, default=300,
                   help="Per-run souffle timeout (s) for -RQ1.")
    p.add_argument("--rq1-log-file", default=None,
                   help="Log path for -RQ1 (default: <rq1-base-dir>/fmcad_rq1.log).")
    p.add_argument("--rq1-collect", action="store_true",
                   help="Re-aggregate existing RQ1 logs under --rq1-base-dir without re-running anything.")
    p.add_argument("--taint-bundle", default=str(DEFAULT_TAINT_BUNDLE),
                   help="Taint dataset root for -RQ1/-RQ3/-RQ2. New layout expects "
                        "<root>/programs/<stage>.dl + <root>/stages.txt + "
                        "<root>/<case>/input/; legacy layout (with pipeline/cases) "
                        "is also accepted.")
    p.add_argument("--taint-cases", default=None,
                   help="Comma/space-separated taint case names to restrict to "
                        "(default: every <case>/ subdir under --taint-bundle).")
    p.add_argument("--skip-sc", action="store_true",
                   help="In -RQ1/-RQ2/-RQ3 mode, skip the side-channel category.")
    p.add_argument("--skip-taint", action="store_true",
                   help="In -RQ1/-RQ2/-RQ3 mode, skip the taint category.")
    p.add_argument("--skip-dis", action="store_true",
                   help="In -RQ1/-RQ2/-RQ3 mode, skip the symbolization category.")
    p.add_argument("--only", default=None,
                   help="In -RQ1/-RQ2/-RQ3 mode, run ONLY the listed categories "
                        "(comma/space-separated; values: sc, taint, dis). "
                        "Equivalent to setting --skip-* on every other category. "
                        "Example: --only taint  /  --only sc,taint.")
    # -RQ3 flags -------------------------------------------------------------
    p.add_argument("--rq3-base-dir", default=str(DEFAULT_RQ3_BASE_DIR),
                   help="Output root for -RQ3 (per-category subdirs land here).")
    p.add_argument("--rq3-run-timeout", type=int, default=RQ3_DEFAULT_TIMEOUT,
                   help="Per-variant, per-run souffle timeout (s) for -RQ3 "
                        "(default 1800 = 30 minutes; no per-process mem cap "
                        "unless --mem-limit-mb is set).")
    p.add_argument("--rq3-log-file", default=None,
                   help="Log path for -RQ3 (default: <rq3-base-dir>/fmcad_rq3.log).")
    p.add_argument("--rq3-collect", action="store_true",
                   help="Re-aggregate existing RQ3 run.meta.json under --rq3-base-dir "
                        "without re-running anything. Includes a RandVars column "
                        "(pre-rewrite count) scraped from each run's "
                        "output/log.txt_*.json: FC_WMC_HYBRID.rand_vars_before_rewrite "
                        "for -r runs, FORWARD_COMPILATION.rand_vars for non-r runs.")
    p.add_argument("--rq3-split-ablation", action="store_true",
                   help="Use the upstream 11_split/11_nosplit ablation instead "
                        "of rewrite off/on. Requires an external compiler "
                        "whose generated runtime supports --split-mode, or "
                        "use with --rq3-collect for existing split-ablation logs.")
    # -RQ2 flags -------------------------------------------------------------
    p.add_argument("--rq2-base-dir", default=str(DEFAULT_RQ2_BASE_DIR),
                   help="Output root for -RQ2 (per-category subdirs land here).")
    p.add_argument("--rq2-run-timeout", type=int, default=RQ2_DEFAULT_TIMEOUT,
                   help="Per-engine, per-run timeout (s) for -RQ2 "
                        "(default 1800 = 30 minutes).")
    p.add_argument("--rq2-log-file", default=None,
                   help="Log path for -RQ2 (default: <rq2-base-dir>/fmcad_rq2.log).")
    p.add_argument("--rq2-collect", action="store_true",
                   help="Re-aggregate existing RQ2 run.meta.json under --rq2-base-dir "
                        "without re-running anything.")
    p.add_argument("--rq2-engines", default=None,
                   help="In -RQ2 mode, run ONLY the listed engines "
                        "(comma/space-separated; values: souffle, problog, "
                        "vproblog, scallop). "
                        "Default = all four. Example: --rq2-engines scallop.")
    p.add_argument("--resume", action="store_true",
                   help="In -RQ1/-RQ2/-RQ3 live mode, skip a (case, variant) cell when it "
                        "already has at least --runs successful run.meta.json files on "
                        "disk. Lets an interrupted run pick up where it left off.")
    # RQ2-specific dataset overrides — default to the reorganised
    # taint/ + symbolization/ layout (post 2026-04-25 cleanup) so -RQ2 runs
    # without the old --taint-bundle / --dis-* flags pointing at deleted dirs.
    p.add_argument("--rq2-taint-bundle", default=str(DEFAULT_TAINT_BUNDLE),
                   help="Taint dataset root for -RQ2. New layout expects "
                        "<root>/programs/<stage>.dl + <root>/stages.txt + "
                        "<root>/<case>/input/. Falls back to legacy --taint-bundle "
                        "shape (with pipeline/cases) when programs/ is absent.")
    p.add_argument("--rq2-taint-cases", default=None,
                   help="Comma/space-separated taint case names for -RQ2 "
                        "(overrides --taint-cases when set).")
    p.add_argument("--rq2-dis-source-dl", default=str(DEFAULT_RQ2_DIS_SOURCE_DL),
                   help="Symbolization Datalog program for -RQ2 (default points "
                        "at symbolization/symbolization.dl in the new layout).")
    p.add_argument("--rq2-dis-inputs-dir", default=str(DEFAULT_RQ2_DIS_INPUTS_DIR),
                   help="Symbolization inputs root for -RQ2 (default symbolization/; "
                        "treats subdirs containing input/ as cases).")
    p.add_argument("--rq2-dis-cases", default=None,
                   help="Comma/space-separated symbolization case names for -RQ2 "
                        "(overrides --dis-cases when set).")
    p.add_argument("--rq2-sc-base", default=None,
                   help="Existing side-channel workspace dir (each subdir Pn/ "
                        "ships compute.souffle.dl + compute.problog.dl + input/). "
                        "When set, RQ2 reuses it as the sc base and forces "
                        "--skip-generate (no SMT regeneration). Use this when "
                        "the SMT source is gone but a previous -sc workspace "
                        "(e.g. side_channel_full/) is still around.")
    # Presets ---------------------------------------------------------------
    p.add_argument("--smoke", action="store_true",
                   help="Smoke preset: -sc on P1,P3 with 1 run, output under "
                        "artifact/smoke/.")
    p.add_argument("--representative", action="store_true",
                   help="Representative preset: -RQ2 on 5 cases per benchmark "
                        "(sc/taint/dis) with 1 run, output under "
                        "artifact/representative/.")
    return p


SMOKE_BASE_DIR = SCRIPT_ROOT / "artifact" / "smoke"
SMOKE_SC_CASES = "1,3"
REPRESENTATIVE_BASE_DIR = SCRIPT_ROOT / "artifact" / "representative"
REPRESENTATIVE_SC_CASES = "1,3,4,5,6"
REPRESENTATIVE_TAINT_CASES = "and-roc,andors-trail,angulo,app-018,app-ca7"
REPRESENTATIVE_DIS_CASES = "bison,cluster,flex,gawk,gcc11"


def _apply_preset_flag(args: argparse.Namespace) -> None:
    """Translate --smoke / --representative into concrete mode + filter args.

    Both presets force --runs=1 and pin the output directory. They are
    mutually exclusive and override --base-dir / --rq2-base-dir / --cases /
    --rq2-taint-cases / --rq2-dis-cases."""
    smoke = getattr(args, "smoke", False)
    repr_ = getattr(args, "representative", False)
    if smoke and repr_:
        raise SystemExit("--smoke and --representative are mutually exclusive.")
    if smoke:
        args.mode = "sc"
        args.base_dir = str(SMOKE_BASE_DIR)
        args.cases = SMOKE_SC_CASES
        args.case_tokens = []
        args.runs = 1
    elif repr_:
        args.mode = "rq2"
        args.rq2_base_dir = str(REPRESENTATIVE_BASE_DIR)
        args.cases = REPRESENTATIVE_SC_CASES
        args.case_tokens = []
        args.rq2_taint_cases = REPRESENTATIVE_TAINT_CASES
        args.rq2_dis_cases = REPRESENTATIVE_DIS_CASES
        args.runs = 1


_ONLY_ALIASES = {
    "sc": "sc", "sidechannel": "sc", "side_channel": "sc", "side-channel": "sc",
    "taint": "taint",
    "dis": "dis", "disasm": "dis", "symbolization": "dis", "symbol": "dis",
}


def _rq2_selected_engines(args: argparse.Namespace) -> set[str]:
    """Resolve --rq2-engines into a canonical engine set.

    Default (flag absent / empty) selects all four engines. Aliases:
    souffle_det_opt_r -> souffle, vlog -> vproblog, scli -> scallop.
    """
    raw = getattr(args, "rq2_engines", None)
    if not raw:
        return set(RQ2_ALL_ENGINES)
    tokens = [t.strip().lower() for t in raw.replace(",", " ").split() if t.strip()]
    if not tokens:
        return set(RQ2_ALL_ENGINES)
    selected: set[str] = set()
    for tok in tokens:
        canon = _RQ2_ENGINE_ALIASES.get(tok)
        if canon is None:
            raise SystemExit(
                f"--rq2-engines: unknown engine '{tok}'. "
                f"Valid: {', '.join(RQ2_ALL_ENGINES)}."
            )
        selected.add(canon)
    return selected


def _apply_only_flag(args: argparse.Namespace) -> None:
    """Translate --only into the existing --skip-* flags.

    --only foo[,bar] selects exactly those categories; every category not
    listed has its skip_* flag forced on. Conflicting --skip-X explicitly
    paired with --only X raises an error so the intent is unambiguous.
    Only meaningful for -RQ1/-RQ2/-RQ3; ignored otherwise."""
    raw = getattr(args, "only", None)
    if raw is None:
        return
    if args.mode not in ("rq1", "rq2", "rq3"):
        raise SystemExit("--only is only valid in -RQ1/-RQ2/-RQ3 mode.")
    tokens = [t.strip().lower() for t in raw.replace(",", " ").split() if t.strip()]
    if not tokens:
        raise SystemExit("--only requires at least one category (sc / taint / dis).")
    selected: set[str] = set()
    for tok in tokens:
        canon = _ONLY_ALIASES.get(tok)
        if canon is None:
            raise SystemExit(
                f"--only: unknown category '{tok}'. "
                "Valid: sc | taint | dis (aliases: sidechannel, disasm, symbolization)."
            )
        selected.add(canon)
    pairs = (("sc", "skip_sc"), ("taint", "skip_taint"), ("dis", "skip_dis"))
    for cat, attr in pairs:
        if cat in selected and getattr(args, attr):
            raise SystemExit(
                f"--only includes '{cat}' but --skip-{ 'sc' if cat=='sc' else 'taint' if cat=='taint' else 'dis' } is also set."
            )
        if cat not in selected:
            setattr(args, attr, True)


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.rq3_split_ablation:
        RQ3_VARIANT_SPECS[:] = RQ3_SPLIT_VARIANT_SPECS
        RQ3_SC_VARIANTS[:] = [
            Variant(name=name, input_dir="input", output_dir=f"output_rq3_{name}",
                    exe_name="compute_rq3", compile_extra=list(compile_flags),
                    run_extra=list(run_flags))
            for name, compile_flags, run_flags in RQ3_VARIANT_SPECS
        ]
        RQ3_VARIANT_NAMES[:] = [name for name, _, _ in RQ3_VARIANT_SPECS]
    _apply_preset_flag(args)
    _apply_only_flag(args)
    _install_mem_limit(getattr(args, "mem_limit_mb", DEFAULT_MEM_LIMIT_MB))
    if args.mode == "dis":
        cmd_dis(args)
    elif args.mode == "rq1":
        cmd_rq1(args)
    elif args.mode == "rq2":
        cmd_rq2(args)
    elif args.mode == "rq3":
        cmd_rq3(args)
    else:
        # Default: side-channel mode (souffle + problog).
        cmd_sc(args)


if __name__ == "__main__":
    main()
