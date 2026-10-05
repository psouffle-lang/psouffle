#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Shared helpers for side-channel benchmark CLIs.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple, Union

SIDE_CHANNEL_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SIDE_CHANNEL_ROOT.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

CASE_PREFIX = "P"
CaseId = Union[int, str]


def _to_text(x):
    if x is None:
        return ""
    if isinstance(x, (bytes, bytearray)):
        try:
            return x.decode("utf-8", errors="replace")
        except Exception:
            return x.decode(errors="replace")
    return x


class Logger:
    def __init__(self, base_dir: Path, log_file: Optional[Path], quiet: bool = False, verbose: bool = False):
        self.base_dir = base_dir
        self.path = log_file if log_file else (base_dir / "operation.log")
        self.quiet = quiet
        self.verbose = verbose
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def _write(self, level: str, msg: str):
        line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {level:<5} {msg}"
        with open(self.path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if not self.quiet:
            print(line, flush=True)

    def info(self, msg: str):  self._write("INFO", msg)
    def warn(self, msg: str):  self._write("WARN", msg)
    def error(self, msg: str): self._write("ERROR", msg)
    def debug(self, msg: str):
        if self.verbose:
            self._write("DEBUG", msg)

    def banner(self, title: str):
        sep = "=" * max(10, min(78, len(title) + 10))
        self.info(sep)
        self.info(title)
        self.info(sep)


def _case_sort_key(case: CaseId) -> Tuple[str, int, str]:
    name = case_name(case)
    match = re.fullmatch(r"([A-Za-z]+)(\d+)", name)
    if match:
        return (match.group(1), int(match.group(2)), name)
    return (name, 0, name)


def _parse_case_token(token: str) -> CaseId:
    match = re.fullmatch(r"([A-Za-z]+)(\d+)", token)
    if match:
        return f"{match.group(1).upper()}{int(match.group(2))}"
    try:
        v = int(token)
    except ValueError:
        raise argparse.ArgumentTypeError(f"Invalid case value in --cases: '{token}'")
    if v < 1:
        raise argparse.ArgumentTypeError(f"Case must be >= 1: '{token}'")
    return v


def parse_cases_spec(spec: Optional[str], default_cases: Optional[Iterable[CaseId]] = None) -> List[CaseId]:
    if not spec:
        return sorted(set(default_cases), key=_case_sort_key) if default_cases else []
    out: set[CaseId] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_str, hi_str = part.split("-", 1)
            lo_match = re.fullmatch(r"([A-Za-z]+)?(\d+)", lo_str)
            hi_match = re.fullmatch(r"([A-Za-z]+)?(\d+)", hi_str)
            if not lo_match or not hi_match:
                raise argparse.ArgumentTypeError(f"Invalid range in --cases: '{part}'")
            lo_prefix = (lo_match.group(1) or "").upper()
            hi_prefix = (hi_match.group(1) or lo_prefix).upper()
            if lo_prefix != hi_prefix:
                raise argparse.ArgumentTypeError(f"Mixed case prefixes in --cases range: '{part}'")
            lo = int(lo_match.group(2))
            hi = int(hi_match.group(2))
            if lo < 1 or hi < lo:
                raise argparse.ArgumentTypeError(f"Invalid range bounds in --cases: '{part}'")
            for v in range(lo, hi + 1):
                out.add(f"{lo_prefix}{v}" if lo_prefix else v)
        else:
            out.add(_parse_case_token(part))
    return sorted(out, key=_case_sort_key)


def case_name(n: CaseId) -> str:
    if isinstance(n, str):
        return n.upper()
    return f"{CASE_PREFIX}{n}"


def case_dir(base_dir: Path, n: CaseId) -> Path:
    return base_dir / case_name(n)


def ensure_dirs(*paths: Path) -> None:
    for p in paths:
        p.mkdir(parents=True, exist_ok=True)


def time_cmd(cmd: Sequence[str], cwd: Optional[Path], timeout: int, feed: Optional[str] = None) -> Tuple[int, float, str, str]:
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd, cwd=str(cwd) if cwd else None,
            input=feed, text=True,
            capture_output=True, timeout=timeout
        )
        elapsed = time.perf_counter() - t0
        return proc.returncode, elapsed, _to_text(proc.stdout), _to_text(proc.stderr)
    except subprocess.TimeoutExpired as e:
        return 124, timeout, _to_text(e.stdout), _to_text(e.stderr) + "TIMEOUT after {}s".format(timeout)
    except FileNotFoundError as e:
        return 127, 0.0, "", "NOTFOUND: {}".format(e)
    except Exception as e:
        return 1, 0.0, "", "ERROR: {}".format(e)


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
                out[key.strip()] = float(val)
            except ValueError:
                continue
    return out


def _normalize_prob_map(prob_map: Dict[str, float]) -> Dict[str, float]:
    """Normalize probability map keys (case-insensitive) for comparison."""
    return {k.upper(): v for k, v in prob_map.items()}


def compare_prob_maps(a: Dict[str, float], b: Dict[str, float], tol: float = 1e-6) -> Tuple[bool, int, float]:
    a_norm = _normalize_prob_map(a)
    b_norm = _normalize_prob_map(b)
    keys = set(a_norm.keys()) | set(b_norm.keys())
    mismatches = 0
    max_delta = 0.0
    for k in keys:
        va = a_norm.get(k, 0.0)
        vb = b_norm.get(k, 0.0)
        d = abs(va - vb)
        if d > tol:
            mismatches += 1
            if d > max_delta:
                max_delta = d
    return mismatches == 0, mismatches, max_delta
