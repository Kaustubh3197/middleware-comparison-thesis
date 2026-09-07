# adas_ecu.py — HB/OBJ/DET accounting + Calibration @1Hz with single-line TXN summaries
#                + exact per-stream payload byte counters (RX/TX) and message counts
import os
import time
import csv
import struct
import argparse
import threading
from datetime import datetime
from collections import defaultdict

import zenoh
from zenoh import Config

from common import (
    now_ms, now_ms_u32, rand_u32,
    unpack_heartbeat, PERIOD_MS, E2E_MAX_MS, JITTER_MAX_MS,
    unpack_object_array, OBJ_PERIOD_MS, OBJ_E2E_MAX_MS, OBJ_JITTER_MAX_MS, OBJ_CONSEC_LOSS_FAULT,
    health_str,
    unpack_detection_array, DET_PERIOD_MS, DET_E2E_MAX_MS, DET_JITTER_MAX_MS, DET_CONSEC_LOSS_FAULT,
    CalibrationRequest, CalibrationAck,
    encode_calibration_request, decode_calibration_response, encode_calibration_ack, decode_calibration_ack,
    calib_req_bcast_key, calib_resp_key, calib_ack_radar_key, calib_ack_ecu_key, radar_ready_key,
)

try:
    from zenoh import Reliability
except Exception:
    Reliability = None

STOP_ENABLED = False
WAIT_SLACK_S = 5
RUN_FOR_S_DEFAULT = int(os.environ.get("RUN_FOR_S", "60"))
CALIB_PERIOD_MS = 1000  # 1 Hz


# ---------------- Args ----------------
def parse_args():
    p = argparse.ArgumentParser(description="ADAS ECU (zenoh) — summary + calibration 1Hz (simplified)")
    p.add_argument("--adas_ip", default=os.environ.get("ADAS_IP", "127.0.0.10"))
    p.add_argument("--port", type=int, default=int(os.environ.get("PORT", "7447")))
    p.add_argument("--out_dir", default=os.environ.get("ADAS_OUT_DIR", "."))
    p.add_argument("--quiet", default=os.environ.get("ADAS_QUIET", "summary"))
    p.add_argument("--cycles", type=int, default=None)
    p.add_argument("--run_for_s", type=int, default=RUN_FOR_S_DEFAULT)
    p.add_argument("--log_dir", default=os.environ.get("ADAS_LOG_DIR", None))
    p.add_argument("--radar_count", type=int, default=1)

    # Calibration
    p.add_argument("--enable_calib", type=int, default=int(os.environ.get("CALIB", "1")))
    p.add_argument("--calib_duration_s", type=float, default=None)
    p.add_argument("--calib_drain_s", type=float, default=float(os.environ.get("CALIB_DRAIN_S", "5")))
    p.add_argument("--calib_warmup_s", type=float, default=float(os.environ.get("CALIB_WARMUP_S", "0")))
    args, _ = p.parse_known_args()

    if args.radar_count < 1 or args.run_for_s < 1:
        raise ValueError("radar_count and run_for_s must be positive")
    if args.cycles is None:
        args.cycles = (args.run_for_s * 1000) // PERIOD_MS
    elif args.cycles < 1:
        raise ValueError("cycles must be positive")
    if args.calib_duration_s is None:
        args.calib_duration_s = float(args.run_for_s)
    return args


# ---------------- Quiet logger ----------------
def normalize_quiet(s):
    s = (s or "summary").strip().lower()
    if s in ("1", "true", "on", "yes"):
        return "summary"
    if s in ("0", "false", "no"):
        return "off"
    return s if s in ("off", "summary", "silent") else "summary"

def make_logger(mode):
    def log_detail(msg):
        if mode == "off":
            print(msg, flush=True)
    def log_summary(msg):
        if mode in ("off", "summary"):
            print(msg, flush=True)
    return log_detail, log_summary


# ---------------- Aggregation for HB/OBJ/DET ----------------
def agg_row():
    return {"total": 0, "ok": 0, "viol": 0, "decode_err": 0, "faults": 0, "max_e2e": 0, "max_jitter": 0}

agg = {"HB": defaultdict(agg_row), "OBJ": defaultdict(agg_row), "DET": defaultdict(agg_row)}
fault_events = []
samples = {"HB": [], "OBJ": [], "DET": []}

def wall_iso(ts=None):
    if ts is None:
        ts = time.time()
    return datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


# ---------------- Excel/CSV reporting (no Calibration Events) ----------------
def write_excel(path, agg, fault_events, samples, run_info, calib=None):
    try:
        import xlsxwriter
    except Exception:
        return False

    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    wb = xlsxwriter.Workbook(path)
    fmt_hdr = wb.add_format({"bold": True})
    fmt_pct = wb.add_format({"num_format": "0.0%"})
    fmt_ms = wb.add_format({"num_format": "0"})

    # Summary HB/OBJ/DET
    ws = wb.add_worksheet("Summary")
    headers = ["Radar", "Stream", "Total", "OK", "Violations", "DecodeErr", "FaultEvents", "MaxE2E(ms)", "MaxJitter(ms)", "OK%"]
    for c, h in enumerate(headers):
        ws.write(0, c, h, fmt_hdr)
    r = 1
    for stream in ("HB", "OBJ", "DET"):
        for rid in sorted(run_info["expected_radars"]):
            a = agg[stream][rid]; total = a["total"]; okpct = (a["ok"]/total) if total else 0.0
            ws.write(r, 0, f"Radar_{rid}"); ws.write(r, 1, stream)
            ws.write_number(r, 2, a["total"], fmt_ms); ws.write_number(r, 3, a["ok"], fmt_ms)
            ws.write_number(r, 4, a["viol"], fmt_ms); ws.write_number(r, 5, a["decode_err"], fmt_ms)
            ws.write_number(r, 6, a["faults"], fmt_ms); ws.write_number(r, 7, a["max_e2e"], fmt_ms)
            ws.write_number(r, 8, a["max_jitter"], fmt_ms); ws.write_number(r, 9, okpct, fmt_pct)
            r += 1
    ws.freeze_panes(1, 0)

    # Fault events
    ws = wb.add_worksheet("Fault Events")
    headers = ["Wall Time", "Radar", "Stream", "Seq", "Reason", "E2E(ms)", "Jitter(ms)"]
    for c, h in enumerate(headers):
        ws.write(0, c, h, fmt_hdr)
    for i, ev in enumerate(fault_events, start=1):
        ws.write(i, 0, ev["wall_time_iso"]); ws.write(i, 1, f"Radar_{ev['radar_id']}"); ws.write(i, 2, ev["stream"])
        ws.write(i, 3, "" if ev["seq"] is None else ev["seq"]); ws.write(i, 4, ev["reason"])
        ws.write_number(i, 5, 0 if ev["e2e"] is None else ev["e2e"], fmt_ms)
        ws.write_number(i, 6, 0 if ev["jitter"] is None else ev["jitter"], fmt_ms)
    ws.freeze_panes(1, 0)

    # Samples
    def dump_samples(name, rows):
        ws = wb.add_worksheet(name)
        headers = ["Wall Time", "Radar", "Seq", "ts_src(ms:monotonic)", "ts_rx(ms:monotonic)", "E2E(ms)", "Jitter(ms)", "Status"]
        for c, h in enumerate(headers):
            ws.write(0, c, h, fmt_hdr)
        for i, row in enumerate(rows, start=1):
            rid, seq, ts_src, ts_rx, e2e, jit, st, wiso = row
            ws.write(i, 0, wiso or ""); ws.write(i, 1, f"Radar_{rid}"); ws.write(i, 2, "" if seq is None else seq)
            ws.write_number(i, 3, 0 if ts_src is None else ts_src, fmt_ms)
            ws.write_number(i, 4, 0 if ts_rx is None else ts_rx, fmt_ms)
            ws.write_number(i, 5, 0 if e2e is None else e2e, fmt_ms)
            ws.write_number(i, 6, 0 if jit is None else jit, fmt_ms)
            ws.write(i, 7, st)
        ws.freeze_panes(1, 0)

    dump_samples("HB Samples", samples["HB"])
    dump_samples("OBJ Samples", samples["OBJ"])
    dump_samples("DET Samples", samples["DET"])

    # Info
    ws = wb.add_worksheet("Info")
    info = [
        ("Stop Enabled", str(STOP_ENABLED)),
        ("Run Duration (s)", run_info["run_for_s"]),
        ("Start (wall)", run_info["start_wall_iso"]),
        ("End (wall)", run_info["end_wall_iso"]),
        ("ADAS endpoint", f"tcp/{run_info['adas_ip']}:{run_info['adas_port']}"),
        ("Expected Radars", ",".join(sorted(run_info["expected_radars"]))),
        ("HB period (ms)", PERIOD_MS),
        ("OBJ period (ms)", OBJ_PERIOD_MS),
        ("DET period (ms)", DET_PERIOD_MS),
        ("Calibration period (ms)", CALIB_PERIOD_MS),
    ]
    if calib:
        info.append(("Calibration total requests sent", calib["req_sent_total"]))
        info.append(("Calibration txn rows", len(calib.get("txns", []))))
    ws.write(0, 0, "Key", fmt_hdr); ws.write(0, 1, "Value", fmt_hdr)
    for i, (k, v) in enumerate(info, start=1):
        ws.write(i, 0, k); ws.write(i, 1, v)

    # Calibration (counts + txns)
    if calib:
        ws = wb.add_worksheet("Calibration")
        headers = ["Radar", "ACK(recv)", "RESP(recv)", "ECU-ACK(sent)", "ReadySeen"]
        for c, h in enumerate(headers):
            ws.write(0, c, h, fmt_hdr)
        r = 1
        for rid in sorted(calib["radar_ids"]):
            ws.write(r, 0, f"Radar_{rid}")
            ws.write_number(r, 1, calib["ack_radar_recv"].get(rid, 0))
            ws.write_number(r, 2, calib["resp_recv"].get(rid, 0))
            ws.write_number(r, 3, calib["ecu_ack_sent"].get(rid, 0))
            ws.write(r, 4, "yes" if calib["ready"].get(rid, False) else "no")
            r += 1

        # Calibration Txns (renamed middle latency)
        txns = calib.get("txns", [])
        ws2 = wb.add_worksheet("Calibration Txns")
        headers = [
            "Radar","ReqID",
            "ECU_send_ms","Radar_ACK_rx_ms","Radar_RESP_ms","ECU_ACK_ms",
            "Req_d1","Req_d2","Req_d3","Req_d4","Resp_C","Resp_D",
            "L1_ECU->ACK(ms)","L2_ACK->RESP(ms)","L3_RESP->ECU(ms)"
        ]
        for c, h in enumerate(headers):
            ws2.write(0, c, h, fmt_hdr)
        for i, t in enumerate(txns, start=1):
            ws2.write(i, 0, f"Radar_{t['radar']}"); ws2.write_number(i, 1, t["req_id"], fmt_ms)
            ws2.write_number(i, 2, t["ecu_send_ts"], fmt_ms)
            ws2.write_number(i, 3, t["radar_ack_rx_ts"], fmt_ms)
            ws2.write_number(i, 4, t["radar_resp_ts"], fmt_ms)
            ws2.write_number(i, 5, t["ecu_ack_ts"], fmt_ms)
            ws2.write_number(i, 6, t["req_d1"], fmt_ms); ws2.write_number(i, 7, t["req_d2"], fmt_ms)
            ws2.write_number(i, 8, t["req_d3"], fmt_ms); ws2.write_number(i, 9, t["req_d4"], fmt_ms)
            ws2.write_number(i,10, t["resp_c"], fmt_ms); ws2.write_number(i,11, t["resp_d"], fmt_ms)
            ws2.write_number(i,12, t["l1"], fmt_ms); ws2.write_number(i,13, t["l2"], fmt_ms); ws2.write_number(i,14, t["l3"], fmt_ms)

    wb.close()
    return True


def write_csvs(prefix, agg, fault_events, samples, run_info, calib=None):
    d = os.path.dirname(prefix)
    if d:
        os.makedirs(d, exist_ok=True)

    with open(prefix+"_summary.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Radar", "Stream", "Total", "OK", "Violations", "DecodeErr", "FaultEvents", "MaxE2E(ms)", "MaxJitter(ms)", "OK%"])
        for stream in ("HB", "OBJ", "DET"):
            for rid in sorted(run_info["expected_radars"]):
                a = agg[stream][rid]; total = a["total"]; okpct = (a["ok"]/total) if total else 0.0
                w.writerow([f"Radar_{rid}", stream, a["total"], a["ok"], a["viol"], a["decode_err"], a["faults"], a["max_e2e"], a["max_jitter"], f"{okpct:.4f}"])

    with open(prefix+"_fault_events.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Wall Time", "Radar", "Stream", "Seq", "Reason", "E2E(ms)", "Jitter(ms)"])
        for ev in fault_events:
            w.writerow([ev["wall_time_iso"], f"Radar_{ev['radar_id']}", ev["stream"],
                        "" if ev["seq"] is None else ev["seq"], ev["reason"],
                        "" if ev["e2e"] is None else ev["e2e"], "" if ev["jitter"] is None else ev["jitter"]])

    def dump_samples(name, rows):
        with open(prefix+f"_{name}.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["Wall Time", "Radar", "Seq", "ts_src(ms:monotonic)", "ts_rx(ms:monotonic)", "E2E(ms)", "Jitter(ms)", "Status"])
            for rid, seq, ts_src, ts_rx, e2e, jit, st, wiso in rows:
                w.writerow([wiso or "", f"Radar_{rid}",
                            "" if seq is None else seq,
                            "" if ts_src is None else ts_src,
                            "" if ts_rx is None else ts_rx,
                            "" if e2e is None else e2e,
                            "" if jit is None else jit,
                            st])
    dump_samples("hb_samples", samples["HB"])
    dump_samples("obj_samples", samples["OBJ"])
    dump_samples("det_samples", samples["DET"])

    if calib:
        with open(prefix+"_calibration.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["TotalReqSent", calib["req_sent_total"]])
            w.writerow([])
            w.writerow(["Radar", "ACK(recv)", "RESP(recv)", "ECU-ACK(sent)", "ReadySeen"])
            for rid in sorted(calib["radar_ids"]):
                w.writerow([f"Radar_{rid}",
                            calib["ack_radar_recv"].get(rid, 0),
                            calib["resp_recv"].get(rid, 0),
                            calib["ecu_ack_sent"].get(rid, 0),
                            "yes" if calib["ready"].get(rid, False) else "no"])
        # Txns
        txns = calib.get("txns", [])
        with open(prefix+"_calibration_txns.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow([
                "Radar","ReqID","ECU_send_ms","Radar_ACK_rx_ms","Radar_RESP_ms","ECU_ACK_ms",
                "Req_d1","Req_d2","Req_d3","Req_d4","Resp_C","Resp_D",
                "L1_ECU->ACK(ms)","L2_ACK->RESP(ms)","L3_RESP->ECU(ms)"
            ])
            for t in txns:
                w.writerow([t["radar"], t["req_id"], t["ecu_send_ts"], t["radar_ack_rx_ts"],
                            t["radar_resp_ts"], t["ecu_ack_ts"], t["req_d1"], t["req_d2"],
                            t["req_d3"], t["req_d4"], t["resp_c"], t["resp_d"], t["l1"], t["l2"], t["l3"]])


def fmt_pct4(x): s = f"{x:.4f}".rstrip("0").rstrip("."); return s

def print_runtime_summary(log_summary, duration_s, expected_radars):
    log_summary("================= RUNTIME SUMMARY =================")
    log_summary(f"[ADAS] ===== SUMMARY ({duration_s}s, STOP-free) =====")
    for stream in ("HB", "OBJ", "DET"):
        for rid in sorted(expected_radars):
            a = agg[stream][rid]; total, ok, viol, de = a["total"], a["ok"], a["viol"], a["decode_err"]
            okpct = 100.0*ok/total if total else 0.0
            log_summary(
                f"  Radar_{rid} [{stream}]: Total={total} OK={ok} Viol={viol} "
                f"DecodeErr={de} Faults={a['faults']} MaxE2E(ms)={a['max_e2e']} "
                f"MaxJitter(ms)={a['max_jitter']} OK%={fmt_pct4(okpct)}"
            )


# ===== NEW: exact payload counters (messages + bytes) =======================
CTR = defaultdict(int)   # thread-safe enough with GIL for simple increments
def add(k, v=1): CTR[k] += v

def print_byte_summary(duration_s):
    # RX payload bytes
    rx_hb_b   = CTR["rx_hb_bytes"]
    rx_obj_b  = CTR["rx_obj_bytes"]
    rx_det_b  = CTR["rx_det_bytes"]
    rx_ack_b  = CTR["rx_cal_ack_bytes"]
    rx_resp_b = CTR["rx_cal_resp_bytes"]
    rx_ready_b= CTR["rx_ready_bytes"]
    rx_tot_b  = rx_hb_b + rx_obj_b + rx_det_b + rx_ack_b + rx_resp_b + rx_ready_b

    # TX payload bytes
    tx_req_b  = CTR["tx_cal_req_bytes"]
    tx_eack_b = CTR["tx_ecu_ack_bytes"]
    tx_tot_b  = tx_req_b + tx_eack_b

    secs = max(1, int(duration_s))
    rx_mbit = (rx_tot_b * 8.0) / (secs * 1e6)
    tx_mbit = (tx_tot_b * 8.0) / (secs * 1e6)

    def line(name, count, bytes_):
        return f"  {name}: count={count} payload={bytes_} B"

    print("\n[ADAS][METRICS] ===== BYTE SUMMARY (payload only) =====")
    print("RX:")
    print(line("HB",           CTR["rx_hb"],        rx_hb_b))
    print(line("OBJ",          CTR["rx_obj"],       rx_obj_b))
    print(line("DET",          CTR["rx_det"],       rx_det_b))
    print(line("CAL-ACK",      CTR["rx_cal_ack"],   rx_ack_b))
    print(line("CAL-RESP",     CTR["rx_cal_resp"],  rx_resp_b))
    print(line("READY(beacon)",CTR["rx_ready"],     rx_ready_b))
    print(f"  RX TOTAL: payload={rx_tot_b} B")
    print(f"  RX avg payload bitrate≈ {rx_mbit:.3f} Mbit/s")
    print("TX:")
    print(line("CAL-REQ(broadcast)", CTR["tx_cal_req"], tx_req_b))
    print(line("CAL-ACK(ECU→radar)", CTR["tx_ecu_ack"], tx_eack_b))
    print(f"  TX TOTAL: payload={tx_tot_b} B")
    print(f"  TX avg payload bitrate≈ {tx_mbit:.3f} Mbit/s\n")


# ---------------- Main ----------------
def main():
    args = parse_args()
    quiet_mode = normalize_quiet(args.quiet)
    _log_detail, log_summary = make_logger(quiet_mode)

    expected_radars = {str(i) for i in range(1, args.radar_count + 1)}

    # RX logfile
    log_dir = args.log_dir or "."
    os.makedirs(log_dir, exist_ok=True)
    rx_path = os.path.join(log_dir, "ecu_rx.log")
    rx = open(rx_path, "w", buffering=1)
    def rxlog(line):
        rx.write(line + "\n"); rx.flush()
        if quiet_mode == "off": print(line, flush=True)

    # Zenoh session
    cfg = Config()
    cfg.insert_json5("listen/endpoints", f'["tcp/{args.adas_ip}:{args.port}"]')
    z = zenoh.open(cfg)

    # ---------- HB/OBJ/DET subscribers ----------
    hb_states = defaultdict(lambda: type("HBState", (object,), {"last_recv_ms": None, "primed": False})())
    obj_states = defaultdict(lambda: type("ObjState", (object,), {"last_recv_ms": None, "primed": False, "loss_streak": 0})())
    det_states = defaultdict(lambda: type("DetState", (object,), {"last_recv_ms": None, "primed": False, "loss_streak": 0})())
    hb_faults = defaultdict(int); obj_faults = defaultdict(int); det_faults = defaultdict(int)

    def count_primed(stream, rid, seq, ts_src, ts_rx, e2e_max):
        a = agg[stream][rid]; e2e = ts_rx - ts_src
        a["total"] += 1; a["max_e2e"] = max(a["max_e2e"], e2e)
        if e2e > e2e_max:
            a["viol"] += 1; a["faults"] += 1
            fault_events.append({"stream": stream, "radar_id": rid, "seq": seq, "reason": "violation(prime)", "e2e": e2e, "jitter": None, "wall_time_iso": wall_iso()})
            samples[stream].append((rid, seq, ts_src, ts_rx, e2e, None, "VIOL_PRIME", wall_iso()))
        else:
            a["ok"] += 1
            samples[stream].append((rid, seq, ts_src, ts_rx, e2e, None, "OK_PRIME", wall_iso()))

    def hb_cb(sample):
        ts_rx = now_ms(); rid = str(sample.key_expr).split("/")[-1]
        if rid not in expected_radars: return
        raw = bytes(sample.payload); add("rx_hb", 1); add("rx_hb_bytes", len(raw))
        st = hb_states[rid]
        try:
            ts_src, seq, health = unpack_heartbeat(raw)
        except Exception as e:
            a = agg["HB"][rid]; a["total"] += 1; a["decode_err"] += 1; a["faults"] += 1
            rxlog(f"[ADAS][RX][HB][{rid}] ❌ decode error: {e}"); samples["HB"].append((rid, None, None, ts_rx, None, None, "DECODE_ERR", wall_iso()))
            return
        if not st.primed:
            st.primed = True; st.last_recv_ms = ts_rx
            count_primed("HB", rid, seq, ts_src, ts_rx, E2E_MAX_MS)
            rxlog(f"[ADAS][RX][HB][{rid}] 🔄 primed seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={ts_rx-ts_src}ms | health={health_str(health)}")
            return
        jitter = abs(ts_rx - st.last_recv_ms - PERIOD_MS); st.last_recv_ms = ts_rx
        a = agg["HB"][rid]; a["total"] += 1; e2e = ts_rx - ts_src
        a["max_e2e"] = max(a["max_e2e"], e2e); a["max_jitter"] = max(a["max_jitter"], jitter)
        if (e2e > E2E_MAX_MS) or (jitter > JITTER_MAX_MS):
            a["viol"] += 1; a["faults"] += 1; hb_faults[rid] += 1
            rxlog(f"[ADAS][RX][HB][{rid}] ❌ E2E:{e2e}ms | jitter:{jitter}ms (seq={seq}) | fault_count={hb_faults[rid]}")
            fault_events.append({"stream": "HB", "radar_id": rid, "seq": seq, "reason": "violation", "e2e": e2e, "jitter": jitter, "wall_time_iso": wall_iso()})
            samples["HB"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "VIOL", wall_iso()))
        else:
            a["ok"] += 1; rxlog(f"[ADAS][RX][HB][{rid}] ✅ seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={e2e}ms | jitter={jitter}ms")
            samples["HB"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "OK", wall_iso()))

    def obj_cb(sample):
        ts_rx = now_ms(); rid = str(sample.key_expr).split("/")[-1]
        if rid not in expected_radars: return
        raw = bytes(sample.payload); add("rx_obj", 1); add("rx_obj_bytes", len(raw))
        st = obj_states[rid]
        try:
            ts_src, seq = unpack_object_array(raw)
        except Exception as e:
            st.loss_streak += 1; a = agg["OBJ"][rid]; a["total"] += 1; a["decode_err"] += 1
            rxlog(f"[ADAS][RX][OBJ][{rid}] ⚠️ decode error (streak={st.loss_streak}): {e}")
            if st.loss_streak >= OBJ_CONSEC_LOSS_FAULT:
                a["faults"] += 1; st.loss_streak = 0
                rxlog(f"[ADAS][RX][OBJ][{rid}] ❌ FAULT (decode x{OBJ_CONSEC_LOSS_FAULT})")
                fault_events.append({"stream": "OBJ", "radar_id": rid, "seq": None, "reason": f"decode x{OBJ_CONSEC_LOSS_FAULT}", "e2e": None, "jitter": None, "wall_time_iso": wall_iso()})
            samples["OBJ"].append((rid, None, None, ts_rx, None, None, "DECODE_ERR", wall_iso())); return
        if not st.primed:
            st.primed = True; st.last_recv_ms = ts_rx; st.loss_streak = 0
            count_primed("OBJ", rid, seq, ts_src, ts_rx, OBJ_E2E_MAX_MS)
            rxlog(f"[ADAS][RX][OBJ][{rid}] 🔄 primed seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={ts_rx-ts_src}ms"); return
        jitter = abs(ts_rx - st.last_recv_ms - OBJ_PERIOD_MS); st.last_recv_ms = ts_rx
        a = agg["OBJ"][rid]; a["total"] += 1; e2e = ts_rx - ts_src
        a["max_e2e"] = max(a["max_e2e"], e2e); a["max_jitter"] = max(a["max_jitter"], jitter)
        if (e2e > OBJ_E2E_MAX_MS) or (jitter > OBJ_JITTER_MAX_MS):
            st.loss_streak += 1; a["viol"] += 1
            rxlog(f"[ADAS][RX][OBJ][{rid}] ⚠️ E2E:{e2e}ms | jitter:{jitter}ms (seq={seq}, streak={st.loss_streak})")
            samples["OBJ"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "VIOL", wall_iso()))
            if st.loss_streak >= OBJ_CONSEC_LOSS_FAULT:
                a["faults"] += 1; st.loss_streak = 0
                rxlog(f"[ADAS][RX][OBJ][{rid}] ❌ FAULT (3 consecutive violations)")
                fault_events.append({"stream": "OBJ", "radar_id": rid, "seq": seq, "reason": "3 consecutive violations", "e2e": e2e, "jitter": jitter, "wall_time_iso": wall_iso()})
        else:
            st.loss_streak = 0; a["ok"] += 1
            rxlog(f"[ADAS][RX][OBJ][{rid}] ✅ seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={e2e}ms | jitter={jitter}ms")
            samples["OBJ"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "OK", wall_iso()))

    def det_cb(sample):
        ts_rx = now_ms(); rid = str(sample.key_expr).split("/")[-1]
        if rid not in expected_radars: return
        raw = bytes(sample.payload); add("rx_det", 1); add("rx_det_bytes", len(raw))
        st = det_states[rid]
        try:
            ts_src, seq = unpack_detection_array(raw)
        except Exception as e:
            st.loss_streak += 1; a = agg["DET"][rid]; a["total"] += 1; a["decode_err"] += 1
            rxlog(f"[ADAS][RX][DET][{rid}] ⚠️ decode error (streak={st.loss_streak}): {e}")
            if st.loss_streak >= DET_CONSEC_LOSS_FAULT:
                a["faults"] += 1; st.loss_streak = 0
                rxlog(f"[ADAS][RX][DET][{rid}] ❌ FAULT (decode x{DET_CONSEC_LOSS_FAULT})")
                fault_events.append({"stream": "DET", "radar_id": rid, "seq": None, "reason": f"decode x{DET_CONSEC_LOSS_FAULT}", "e2e": None, "jitter": None, "wall_time_iso": wall_iso()})
            samples["DET"].append((rid, None, None, ts_rx, None, None, "DECODE_ERR", wall_iso())); return
        if not st.primed:
            st.primed = True; st.last_recv_ms = ts_rx; st.loss_streak = 0
            count_primed("DET", rid, seq, ts_src, ts_rx, DET_E2E_MAX_MS)
            rxlog(f"[ADAS][RX][DET][{rid}] 🔄 primed seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={ts_rx-ts_src}ms"); return
        jitter = abs(ts_rx - st.last_recv_ms - DET_PERIOD_MS); st.last_recv_ms = ts_rx
        a = agg["DET"][rid]; a["total"] += 1; e2e = ts_rx - ts_src
        a["max_e2e"] = max(a["max_e2e"], e2e); a["max_jitter"] = max(a["max_jitter"], jitter)
        if (e2e > DET_E2E_MAX_MS) or (jitter > DET_JITTER_MAX_MS):
            st.loss_streak += 1; a["viol"] += 1
            rxlog(f"[ADAS][RX][DET][{rid}] ⚠️ E2E:{e2e}ms | jitter:{jitter}ms (seq={seq}, streak={st.loss_streak})")
            samples["DET"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "VIOL", wall_iso()))
            if st.loss_streak >= DET_CONSEC_LOSS_FAULT:
                a["faults"] += 1; st.loss_streak = 0
                rxlog(f"[ADAS][RX][DET][{rid}] ❌ FAULT (3 consecutive violations)")
                fault_events.append({"stream": "DET", "radar_id": rid, "seq": seq, "reason": "3 consecutive violations", "e2e": e2e, "jitter": jitter, "wall_time_iso": wall_iso()})
        else:
            st.loss_streak = 0; a["ok"] += 1
            rxlog(f"[ADAS][RX][DET][{rid}] ✅ seq={seq} ts_src={ts_src} ts_rx={ts_rx} e2e={e2e}ms | jitter={jitter}ms")
            samples["DET"].append((rid, seq, ts_src, ts_rx, e2e, jitter, "OK", wall_iso()))

    hb_sub = z.declare_subscriber("adas/heartbeat/radar/*", hb_cb)
    obj_sub = z.declare_subscriber("adas/objects/radar/*", obj_cb)
    det_sub = z.declare_subscriber("adas/detections/radar/*", det_cb)
    if Reliability is not None:
        for s in (hb_sub, obj_sub, det_sub):
            try: s.reliability = Reliability.RELIABLE
            except Exception: pass

    # ---------- Calibration (1 Hz) with simplified TXN summaries ----------
    calib_summary = None
    if args.enable_calib:
        radar_ids = sorted(list(expected_radars))
        ready = {rid: False for rid in radar_ids}

        req_sent_total = 0
        ack_radar_recv = defaultdict(int)
        resp_recv = defaultdict(int)
        ecu_ack_sent = defaultdict(int)
        calib_txns  = []

        req_meta = {}           # req_id -> {ecu_send_ts, d1..d4}
        tx_by_radar_req = {}    # (rid, req_id) -> dict
        lock = threading.Lock()

        def finalize_if_ready(rid, req_id):
            key = (rid, req_id)
            with lock:
                t = tx_by_radar_req.get(key)
                if not t:
                    return
                fields = ("ecu_send_ts","radar_ack_rx_ts","radar_resp_ts","ecu_ack_ts",
                          "req_d1","req_d2","req_d3","req_d4","resp_c","resp_d")
                if not all(k in t for k in fields):
                    return
                l1 = int(t["radar_ack_rx_ts"] - t["ecu_send_ts"])
                l2 = int(t["radar_resp_ts"]   - t["radar_ack_rx_ts"])
                l3 = int(t["ecu_ack_ts"]      - t["radar_resp_ts"])
                t["l1"], t["l2"], t["l3"] = l1, l2, l3
                # Single, clearer line (no 'Work=')
                rxlog(
                    f"[CAL][TXN] radar={rid} req_id={req_id} "
                    f"ECU->ACK={l1}ms, ACK->RESP={l2}ms, RESP->ECU={l3}ms "
                    f"| req=({t['req_d1']},{t['req_d2']},{t['req_d3']},{t['req_d4']}) "
                    f"resp=({t['resp_c']},{t['resp_d']})"
                )
                calib_txns.append({
                    "radar": rid, "req_id": req_id,
                    "ecu_send_ts": t["ecu_send_ts"],
                    "radar_ack_rx_ts": t["radar_ack_rx_ts"],
                    "radar_resp_ts": t["radar_resp_ts"],
                    "ecu_ack_ts": t["ecu_ack_ts"],
                    "req_d1": t["req_d1"], "req_d2": t["req_d2"], "req_d3": t["req_d3"], "req_d4": t["req_d4"],
                    "resp_c": t["resp_c"], "resp_d": t["resp_d"],
                    "l1": l1, "l2": l2, "l3": l3,
                })
                tx_by_radar_req.pop(key, None)

        # Pubs
        calib_req_pub = z.declare_publisher(calib_req_bcast_key())
        ack_ecu_pubs = {}
        def get_ack_pub(rid):
            if rid not in ack_ecu_pubs:
                ack_ecu_pubs[rid] = z.declare_publisher(calib_ack_ecu_key(rid))
            return ack_ecu_pubs[rid]

        # Subs
        def on_ready(sample):
            rid = str(sample.key_expr).split("/")[-1]
            raw = bytes(sample.payload); add("rx_ready", 1); add("rx_ready_bytes", len(raw))
            if rid in ready and not ready[rid]:
                ready[rid] = True
                rxlog(f"[READY] radar={rid}")

        def on_radar_ack(sample):
            rid = str(sample.key_expr).split("/")[-1]
            raw = bytes(sample.payload); add("rx_cal_ack", 1); add("rx_cal_ack_bytes", len(raw))
            try:
                ack = decode_calibration_ack(raw)
                ack_radar_recv[rid] += 1
                with lock:
                    rm = req_meta.get(ack.calibration_req_data_1, {})
                    key = (rid, ack.calibration_req_data_1)
                    t = tx_by_radar_req.setdefault(key, {})
                    t["radar_ack_rx_ts"] = ack.calibration_req_data_2
                    if rm:
                        t["ecu_send_ts"] = rm["ecu_send_ts"]
                        t["req_d1"] = rm["d1"]; t["req_d2"] = rm["d2"]
                        t["req_d3"] = rm["d3"]; t["req_d4"] = rm["d4"]
                finalize_if_ready(rid, ack.calibration_req_data_1)
            except Exception as e:
                rxlog(f"[CAL][ACK-RADAR][ERROR] {e}")

        def on_radar_resp(sample):
            rid = str(sample.key_expr).split("/")[-1]
            raw = bytes(sample.payload); add("rx_cal_resp", 1); add("rx_cal_resp_bytes", len(raw))
            try:
                resp = decode_calibration_response(raw)
                resp_recv[rid] += 1
                ecu_rx_ts = now_ms_u32()
                # ECU-ACK immediately
                ack = CalibrationAck(
                    calibration_req_data_1=resp.calibration_req_data_1,
                    calibration_req_data_2=ecu_rx_ts,
                    calibration_req_data_3=resp.calibration_req_data_3,
                    calibration_req_data_4=resp.calibration_req_data_4,
                )
                ack_bytes = encode_calibration_ack(ack)
                get_ack_pub(rid).put(ack_bytes)
                ecu_ack_sent[rid] += 1
                add("tx_ecu_ack", 1); add("tx_ecu_ack_bytes", len(ack_bytes))

                with lock:
                    rm = req_meta.get(resp.calibration_req_data_1, {})
                    key = (rid, resp.calibration_req_data_1)
                    t = tx_by_radar_req.setdefault(key, {})
                    t["radar_resp_ts"] = resp.calibration_req_data_2
                    t["resp_c"] = resp.calibration_req_data_3
                    t["resp_d"] = resp.calibration_req_data_4
                    t["ecu_ack_ts"] = ecu_rx_ts
                    if rm and "ecu_send_ts" not in t:
                        t["ecu_send_ts"] = rm["ecu_send_ts"]
                        t["req_d1"] = rm["d1"]; t["req_d2"] = rm["d2"]
                        t["req_d3"] = rm["d3"]; t["req_d4"] = rm["d4"]

                finalize_if_ready(rid, resp.calibration_req_data_1)

            except Exception as e:
                rxlog(f"[CAL][RESP][ERROR] {e}")

        ready_sub = z.declare_subscriber(radar_ready_key("*"), on_ready)
        ack_sub   = z.declare_subscriber(calib_ack_radar_key("*"), on_radar_ack)
        resp_sub  = z.declare_subscriber(calib_resp_key("*"), on_radar_resp)
        if Reliability is not None:
            for s in (ready_sub, ack_sub, resp_sub):
                try: s.reliability = Reliability.RELIABLE
                except Exception: pass

        # 1 Hz broadcast sender (four random u32s; d1 is req_id)
        def calib_sender():
            nonlocal req_sent_total
            # best-effort warmup
            t0 = time.time()
            while (time.time() - t0) < args.calib_warmup_s and not all(ready.values()):
                time.sleep(0.01)

            period_s = CALIB_PERIOD_MS / 1000.0
            next_t = time.monotonic()
            end_at = time.monotonic() + args.calib_duration_s
            while time.monotonic() < end_at:
                d1, d2, d3, d4 = rand_u32(), rand_u32(), rand_u32(), rand_u32()
                req = CalibrationRequest(d1, d2, d3, d4)
                ecu_ts = now_ms_u32()
                try:
                    req_bytes = encode_calibration_request(req)
                    calib_req_pub.put(req_bytes)
                    add("tx_cal_req", 1); add("tx_cal_req_bytes", len(req_bytes))
                    with lock:
                        req_meta[d1] = {"ecu_send_ts": ecu_ts, "d1": d1, "d2": d2, "d3": d3, "d4": d4}
                    req_sent_total += 1
                    rxlog(f"[CAL][SEND] req_id={d1} ecu_ts={ecu_ts} data=({d1},{d2},{d3},{d4})")
                except Exception as e:
                    rxlog(f"[CAL][SEND][ERROR] {e}")

                next_t += period_s
                sleep_s = next_t - time.monotonic()
                if sleep_s > 0: time.sleep(sleep_s)
                else: next_t = time.monotonic()

        t_cal = threading.Thread(target=calib_sender, daemon=True)
        t_cal.start()

    # ---------- wait for HB/OBJ/DET cycles ----------
    def reached_cycles():
        target = args.cycles
        for stream in ("HB", "OBJ", "DET"):
            for rid in expected_radars:
                if agg[stream][rid]["total"] < target:
                    return False
        return True

    start_wall = wall_iso()
    end_at = time.monotonic() + args.run_for_s + WAIT_SLACK_S
    try:
        while time.monotonic() < end_at:
            if reached_cycles(): break
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        end_wall = wall_iso()
        if args.enable_calib:
            time.sleep(min(1.0, float(os.environ.get("CALIB_DRAIN_S", "5"))))
        for s in (hb_sub, obj_sub, det_sub):
            try: s.undeclare()
            except Exception: pass
        if args.enable_calib:
            for s in (ready_sub, ack_sub, resp_sub):
                try: s.undeclare()
                except Exception: pass
        z.close(); rx.close()

        print_runtime_summary(make_logger(quiet_mode)[1], args.run_for_s, expected_radars)
        print_byte_summary(args.run_for_s)

        # Build calibration summary payload for report
        calib_summary = None
        if args.enable_calib:
            calib_summary = {
                "radar_ids": expected_radars,
                "req_sent_total": req_sent_total,
                "ack_radar_recv": ack_radar_recv if 'ack_radar_recv' in locals() else {},
                "resp_recv": resp_recv if 'resp_recv' in locals() else {},
                "ecu_ack_sent": ecu_ack_sent if 'ecu_ack_sent' in locals() else {},
                "ready": ready if 'ready' in locals() else {},
                "txns": calib_txns if 'calib_txns' in locals() else [],
            }
            print("\n[ADAS][CALIB SUMMARY]")
            print(f"  total_requests_sent={calib_summary['req_sent_total']}")
            for rid in sorted(expected_radars):
                print(f"  radar={rid}: ack_recv={calib_summary['ack_radar_recv'].get(rid,0)} "
                      f"resp_recv={calib_summary['resp_recv'].get(rid,0)} "
                      f"ecu_ack_sent={calib_summary['ecu_ack_sent'].get(rid,0)} "
                      f"ready={'yes' if calib_summary['ready'].get(rid,False) else 'no'}")
            print(f"  txn_rows={len(calib_summary['txns'])}")

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = os.path.join(args.out_dir, f"adas_run_summary_{stamp}")
        run_info = {"start_wall_iso": start_wall, "end_wall_iso": end_wall,
                    "adas_ip": args.adas_ip, "adas_port": args.port,
                    "expected_radars": expected_radars, "run_for_s": args.run_for_s}
        if write_excel(base + ".xlsx", agg, fault_events, samples, run_info, calib_summary):
            print(f"[ADAS] Excel report written: {base}.xlsx")
        else:
            write_csvs(base, agg, fault_events, samples, run_info, calib_summary)
            print(f"[ADAS] CSVs written with prefix: {base}_*.csv / _info.txt")
        print("[ADAS] Stopped.")


if __name__ == "__main__":
    main()