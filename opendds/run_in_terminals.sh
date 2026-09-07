#!/usr/bin/env bash
set -euo pipefail

# -------------------------------------------------------
# Multi-radar runner: 1x ECU + Nx RADARs, synchronized T0
# Everything logs to build/logs/run_<timestamp> (no terminals pop up)
#
# Optional env knobs:
#   START_DELAY=5        # seconds from now to start all RADARs
#   ECU_PREWARM=0        # ECU starts this many seconds before T0 (0 = together)
#   RADAR_IDS="1 2 3"    # which radar IDs to launch
#   ECU_ARGS="--run-for 60 --summary"   # extra args for ECU
#   RADAR_ARGS="--run-for 60 --summary" # extra args for all radars
#   INI=/path/to/rtps.ini  BIN=/path/to/build
#   VERBOSE=1             # print composed args (optional)
#   DRAIN_MS=1500         # bounded drain after run window (ms). Set 0 to disable.
# -------------------------------------------------------

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INI="${INI:-$ROOT/rtps.ini}"
BIN="${BIN:-$ROOT/build}"
VERBOSE="${VERBOSE:-0}"
DRAIN_MS="${DRAIN_MS:-1500}"

# timing (seconds)
START_DELAY=${START_DELAY:-5}
ECU_PREWARM=${ECU_PREWARM:-0}

# pass-through args (keep --summary by default for ECU)
ECU_ARGS="${ECU_ARGS:-}"
RADAR_ARGS="${RADAR_ARGS:-}"
ECU_ARGS="${ECU_ARGS} --summary"

# Try to infer RUN_FOR_S from ECU_ARGS (needed for optional drain)
RUN_FOR_S=60
if [[ "$ECU_ARGS" =~ --run-for[[:space:]]+([0-9]+) ]]; then
  RUN_FOR_S="${BASH_REMATCH[1]}"
fi

# radars
RADAR_IDS_STR="${RADAR_IDS:-1 2 3 4 5 6}"
read -r -a RADAR_IDS <<< "$RADAR_IDS_STR"

# logs
STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_ROOT="$ROOT/build/logs"
RUN_LOG_DIR="$LOG_ROOT/run_$STAMP"
mkdir -p "$RUN_LOG_DIR"

ECU_LOG="$RUN_LOG_DIR/ecu_${STAMP}.log"
declare -a RAD_LOGS=()

# mark start so we can detect Excel created during this run
START_MARKER="$RUN_LOG_DIR/.start"
: > "$START_MARKER"

# try to stop any stale processes (best effort)
pkill -f "radar_publisher --radar-id" 2>/dev/null || true
pkill -f "ecu_subscriber" 2>/dev/null || true

# absolute start time
T0=$(( $(date +%s) + START_DELAY ))
ECU_WAIT=$(( START_DELAY - ECU_PREWARM )); (( ECU_WAIT < 0 )) && ECU_WAIT=0

echo "[RUN] Using INI: $INI"
echo "[RUN] BIN dir   : $BIN"
echo "[RUN] Logs      : $RUN_LOG_DIR"
echo "[RUN] ECU prewarm: ${ECU_PREWARM}s  | START_DELAY: ${START_DELAY}s"
if date -d @0 >/dev/null 2>&1; then
  echo "[RUN] T0 = ${T0} ($(date -d @${T0} '+%H:%M:%S'))"
else
  echo "[RUN] T0 = ${T0} ($(date -r ${T0} '+%H:%M:%S'))"
fi

# only print composed args if VERBOSE=1
if [[ "$VERBOSE" == "1" ]]; then
  echo "[RUN] Composed ECU args  : $ECU_ARGS"
  echo "[RUN] Composed RADAR args: $RADAR_ARGS"
fi
echo

# cleanup on exit / ctrl-c
pids=()
cleanup() {
  echo
  echo "[RUN] Cleaning up..."
  if ((${#pids[@]})); then
    for p in "${pids[@]}"; do
      if kill -0 "$p" 2>/dev/null; then
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
      fi
    done
  fi
}
trap cleanup EXIT INT TERM

# ---------- launch ECU ----------
(
  cd "$BIN"
  if date -d @0 >/dev/null 2>&1; then
    echo "[ECU] will start ~${ECU_PREWARM}s before T0=$T0 ($(date -d @${T0} '+%H:%M:%S'))"
  else
    echo "[ECU] will start ~${ECU_PREWARM}s before T0=$T0 ($(date -r ${T0} '+%H:%M:%S'))"
  fi
  sleep "$ECU_WAIT"
  echo "[ECU] START $(date '+%H:%M:%S')"
  LOG_DIR="$RUN_LOG_DIR" stdbuf -oL -eL ./ecu_subscriber \
    -DCPSConfigFile "$INI" \
    --log-dir "$RUN_LOG_DIR" $ECU_ARGS
  echo "[ECU] exited."
) | tee "$ECU_LOG" &
ECU_PID=$!
pids+=("$ECU_PID")

# ---------- launch each RADAR ----------
for RID in "${RADAR_IDS[@]}"; do
  RLOG="$RUN_LOG_DIR/radar${RID}_${STAMP}.log"
  RAD_LOGS+=("$RLOG")
  (
    cd "$BIN"
    if date -d @0 >/dev/null 2>&1; then
      echo "[RADAR $RID] armed. Will start at T0=$T0 ($(date -d @${T0} '+%H:%M:%S'))"
    else
      echo "[RADAR $RID] armed. Will start at T0=$T0 ($(date -r ${T0} '+%H:%M:%S'))"
    fi
    while (( $(date +%s) < T0 )); do sleep 0.05; done
    echo "[RADAR $RID] START $(date '+%H:%M:%S')"
    LOG_DIR="$RUN_LOG_DIR" stdbuf -oL -eL ./radar_publisher \
      -DCPSConfigFile "$INI" \
      --radar-id "$RID" --log-dir "$RUN_LOG_DIR" $RADAR_ARGS
    echo "[RADAR $RID] exited."
  ) | tee "$RLOG" &
  pids+=("$!")
done

# ---------- optional bounded drain (no Python) ----------
# If we could infer RUN_FOR_S and DRAIN_MS>0, wait until end of window and give ECU a short drain.
if (( RUN_FOR_S > 0 )) && (( DRAIN_MS > 0 )); then
  (
    STOP_AT=$(( T0 + RUN_FOR_S ))
    # wait until run window has elapsed
    while (( $(date +%s) < STOP_AT )); do sleep 0.2; done
    # fractional sleep for drain
    drain_s="$(awk "BEGIN{printf \"%.3f\", ${DRAIN_MS}/1000}")"
    sleep "$drain_s"
    # If ECU is still up (e.g., waiting), nudge it to stop cleanly.
    if kill -0 "$ECU_PID" 2>/dev/null; then
      echo "[RUN] Draining done, stopping ECU at $(date '+%H:%M:%S')"
      kill -INT "$ECU_PID" 2>/dev/null || true
    fi
  ) &
fi

# ---------- wait for everything ----------
for p in "${pids[@]}"; do
  wait "$p" || true
done

echo
echo "[RUN] Logs saved:"
echo "  ECU   -> $ECU_LOG"
for RLOG in "${RAD_LOGS[@]}"; do
  echo "  RADAR -> $RLOG"
done

# detect newest Excel produced during this run (in RUN_LOG_DIR only)
XLSX_FOUND=""
if command -v find >/dev/null 2>&1; then
  XLSX_FOUND="$(find "$RUN_LOG_DIR" -maxdepth 1 -name '*.xlsx' -newer "$START_MARKER" -print | sort | tail -n1 || true)"
fi
if [[ -n "$XLSX_FOUND" ]]; then
  echo "  XLSX  -> $XLSX_FOUND"
else
  echo "  XLSX  -> (none detected; was ECU built with USE_XLSXWRITER?)"
fi

echo "[RUN] Done."
