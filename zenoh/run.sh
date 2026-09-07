#!/usr/bin/env bash
set -euo pipefail
set +m

# --- config ---
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ADAS_IP="${ADAS_IP:-127.0.0.10}"
PORT="${PORT:-7447}"
RADAR_COUNT="${RADAR_COUNT:-1}"
RUN_FOR_S="${RUN_FOR_S:-60}"
PERIOD_MS=20
CYCLES=$(( (RUN_FOR_S * 1000) / PERIOD_MS ))
OUT_DIR="${OUT_DIR:-$REPO_DIR/output}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RUN_OUT_DIR="$OUT_DIR/$TIMESTAMP"
LOG_DIR="$RUN_OUT_DIR/logs"
OPEN_RESULT="${OPEN_RESULT:-1}"
PID_FILE="$RUN_OUT_DIR/.radar_pids"

# --- calibration knobs (on by default) ---
CALIB="${CALIB:-1}"
CALIB_DURATION_S="${CALIB_DURATION_S:-$RUN_FOR_S}"
CALIB_DRAIN_S="${CALIB_DRAIN_S:-5}"
CALIB_WARMUP_S="${CALIB_WARMUP_S:-5}"
CALIB_READY_PERIOD="${CALIB_READY_PERIOD:-0.5}"
CALIB_MS_MIN="${CALIB_MS_MIN:-80}"
CALIB_MS_MAX="${CALIB_MS_MAX:-220}"

# --- validation ---
[[ -f "$REPO_DIR/radar.py" ]] || { echo "Error: radar.py not found"; exit 1; }
[[ -f "$REPO_DIR/adas_ecu.py" ]] || { echo "Error: adas_ecu.py not found"; exit 1; }
[[ "$RADAR_COUNT" =~ ^[0-9]+$ && "$RADAR_COUNT" -ge 1 ]] || { echo "Error: RADAR_COUNT must be a positive integer"; exit 1; }
[[ "$RUN_FOR_S" =~ ^[0-9]+$ && "$RUN_FOR_S" -ge 1 ]] || { echo "Error: RUN_FOR_S must be a positive integer"; exit 1; }
[[ $CYCLES -ge 1 ]] || { echo "Error: Calculated CYCLES invalid ($CYCLES)"; exit 1; }

mkdir -p "$RUN_OUT_DIR" "$LOG_DIR"
: > "$PID_FILE"

cleanup() {
  set +e
  if [[ -s "$PID_FILE" ]]; then
    while read -r pid; do kill -TERM "$pid" 2>/dev/null || true; done < "$PID_FILE"
    rm -f "$PID_FILE"
  fi
  pkill -f "python3 .*radar\.py" 2>/dev/null || true
  pkill -f "python3 .*adas_ecu\.py" 2>/dev/null || true
  set -e
}
trap cleanup EXIT INT TERM

# make sure no leftovers are running
pkill -f "python3 .*adas_ecu\.py" 2>/dev/null || true
pkill -f "python3 .*radar\.py" 2>/dev/null || true

echo "[launcher] output dir: $RUN_OUT_DIR"

# --- ECU FIRST (background, prints to terminal) ---
echo "[launcher] starting ECU …"
python3 "$REPO_DIR/adas_ecu.py" \
  --adas_ip "$ADAS_IP" --port "$PORT" \
  --out_dir "$RUN_OUT_DIR" --cycles "$CYCLES" \
  --radar_count "$RADAR_COUNT" --run_for_s "$RUN_FOR_S" \
  --quiet summary --log_dir "$LOG_DIR" \
  --enable_calib "$CALIB" \
  --calib_duration_s "$CALIB_DURATION_S" \
  --calib_drain_s "$CALIB_DRAIN_S" \
  --calib_warmup_s "$CALIB_WARMUP_S" &
ECU_PID=$!

# give ECU subscribers a brief head-start so first frames aren't lost
sleep 1.5

# --- then RADARS ---
echo "[launcher] starting ${RADAR_COUNT} radars (RUN_FOR_S=${RUN_FOR_S}s, CYCLES=${CYCLES}, CALIB=${CALIB}) …"
for i in $(seq 1 "$RADAR_COUNT"); do
  (
    cd "$REPO_DIR"
    nohup python3 "radar.py" \
      --radar_id "$i" --adas_ip "$ADAS_IP" --port "$PORT" \
      --cycles "$CYCLES" --run_for_s "$RUN_FOR_S" \
      --quiet silent --out_dir "$RUN_OUT_DIR" --log_dir "$LOG_DIR" \
      --enable_calib "$CALIB" \
      --calib_ready_period "$CALIB_READY_PERIOD" \
      --calib_ms_min "$CALIB_MS_MIN" \
      --calib_ms_max "$CALIB_MS_MAX" \
      >/dev/null 2>&1 & echo $! >> "$PID_FILE"; disown
  )
  # tiny stagger helps zenoh discovery under load
  sleep 0.05
done

# --- wait for ECU to complete (collects cycles & writes reports) ---
ECU_STATUS=0
set +e
wait "$ECU_PID"
ECU_STATUS=$?
set -e

if [[ "$OPEN_RESULT" == "1" ]]; then
  # prefer Excel; if missing, try CSV
  LATEST_XLSX="$(ls -1t "$RUN_OUT_DIR"/adas_run_summary_*.xlsx 2>/dev/null | head -n 1 || true)"
  if [[ -n "$LATEST_XLSX" ]]; then
    if [[ "$(uname)" == "Darwin" ]]; then open "$LATEST_XLSX" >/dev/null 2>&1 &
    elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$LATEST_XLSX" >/dev/null 2>&1 &
    fi
  else
    LATEST_CSV="$(ls -1t "$RUN_OUT_DIR"/adas_run_summary_*_summary.csv 2>/dev/null | head -n 1 || true)"
    if [[ -n "$LATEST_CSV" ]]; then
      if [[ "$(uname)" == "Darwin" ]]; then open "$LATEST_CSV" >/dev/null 2>&1 &
      elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$LATEST_CSV" >/dev/null 2>&1 &
      fi
    fi
  fi
fi

echo "[launcher] logs: $LOG_DIR"
echo "[launcher] reports: $RUN_OUT_DIR"
exit "$ECU_STATUS"
