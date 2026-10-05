#!/usr/bin/env bash
# vproblog-only T2 taint driver. Mirrors _t2_run_vproblog_taint_stages from
# FMCAD.py (vlog mat per stage, sum per-case wall-clock, stop on failure).
#
# Usage:
#   VLOG_BIN=/path/to/vlog OUT=/abs/out/dir TIMEOUT=900 \
#     scripts/run_t2_taint_vproblog.sh [case ...]

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VLOG_BIN="${VLOG_BIN:-/opt/vproblog/src/vlog-beta-sdd/build/vlog}"
OUT="${OUT:-$ROOT/artifact/runs/t2_taint_vp_$(date +%Y%m%d_%H%M%S)}"
TIMEOUT="${TIMEOUT:-900}"

if [ ! -x "$VLOG_BIN" ]; then
  echo "vlog binary missing: $VLOG_BIN" >&2; exit 2
fi

mapfile -t STAGES < <(grep -v '^[[:space:]]*\(#\|$\)' "$ROOT/taint/stages.txt")
if [ "${#STAGES[@]}" -eq 0 ]; then
  echo "no stages in $ROOT/taint/stages.txt" >&2; exit 2
fi

if [ "$#" -gt 0 ]; then
  CASES=( "$@" )
else
  mapfile -t CASES < <(
    for d in "$ROOT"/taint/*/vproblog; do
      [ -d "$d" ] && basename "$(dirname "$d")"
    done | sort
  )
fi

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
SUMMARY="$OUT/T2result_vproblog.tsv"
LOG="$OUT/run.log"
: >"$SUMMARY"
: >"$LOG"
printf 'category\tcase\tengine\truns\tavg_s\tmin_s\tmax_s\tstatus\tnote\n' >>"$SUMMARY"

export LD_LIBRARY_PATH="$(dirname "$VLOG_BIN"):${LD_LIBRARY_PATH:-}"

log() { printf '[%s] %s\n' "$(date +%F\ %T)" "$*" | tee -a "$LOG"; }

log "vproblog-only T2 taint: $(date)"
log "vlog=$VLOG_BIN  timeout=${TIMEOUT}s  out=$OUT"
log "stages: ${STAGES[*]}"
log "cases (${#CASES[@]}): ${CASES[*]}"

idx=0
for case in "${CASES[@]}"; do
  idx=$((idx + 1))
  vp_root="$ROOT/taint/$case/vproblog"
  if [ ! -d "$vp_root" ]; then
    log "[$idx/${#CASES[@]}] $case  SKIP (no vproblog dir)"
    printf 'taint\t%s\tvproblog\t0\t0\t0\t0\tSKIP\tno vproblog dir\n' "$case" >>"$SUMMARY"
    continue
  fi
  case_out="$OUT/$case"
  mkdir -p "$case_out"
  total=0
  status="OK"
  note=""
  log "[$idx/${#CASES[@]}] $case  start"
  for stage in "${STAGES[@]}"; do
    src="$vp_root/$stage"
    if [ ! -d "$src" ]; then
      status="FAIL"; note="missing stage dir: $stage"
      log "  $stage  MISSING"
      break
    fi
    sd="$case_out/$stage"
    mkdir -p "$sd/storemat"
    sl="$sd/vlog.log"
    t0=$(date +%s.%N)
    (cd "$src" && timeout "${TIMEOUT}s" "$VLOG_BIN" mat \
      -e   "$src/edb.conf" \
      --rules     "$src/rules" \
      --prob_file "$src/mappings.csv" \
      --storemat_path "$sd/storemat" \
      --storemat_format csv \
      --rewriteMultihead true \
      --restrictedChase  false \
      --ignoreMagic      false \
      -l info) \
      >"$sl" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    el=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
    total=$(awk -v a="$total" -v b="$el" 'BEGIN{printf "%.2f", a+b}')
    if [ "$rc" -eq 0 ]; then
      log "  $stage  OK in ${el}s"
    else
      if [ "$rc" -eq 124 ]; then
        status="TIMEOUT"
      else
        status="FAIL"
      fi
      note="exit=$rc at $stage"
      log "  $stage  $status in ${el}s ($note)"
      break
    fi
  done
  log "[$idx/${#CASES[@]}] $case  $status total=${total}s ($note)"
  if [ "$status" = "OK" ]; then
    printf 'taint\t%s\tvproblog\t1\t%s\t%s\t%s\tOK\t\n' "$case" "$total" "$total" "$total" >>"$SUMMARY"
  else
    printf 'taint\t%s\tvproblog\t0\t0\t0\t0\t%s\t%s\n' "$case" "$status" "$note" >>"$SUMMARY"
  fi
done
log "done."
