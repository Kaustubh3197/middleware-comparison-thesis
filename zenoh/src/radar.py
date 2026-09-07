# radar.py — 20ms HB/OBJ/DET + Calibration (REQ -> ACK -> RESP) with single-line TXN log
#            + exact per-stream message counters and payload-byte totals
import argparse
import os
import struct
import threading
import time
import random

import zenoh
from zenoh import Config

from common import (
    now_ms, now_ms_u32, rand_u32,
    pack_heartbeat, PERIOD_MS, HEALTH_OK,
    pack_one_object, OBJ_SIZE, OBJECTS_PER_CYCLE,
    pack_detection_array, DETECTIONS_PER_CYCLE, DET_FMT, DET_SIZE,
    decode_calibration_request, encode_calibration_response, encode_calibration_ack,
    CalibrationResponse, CalibrationAck,
    calib_req_bcast_key, calib_resp_key, calib_ack_radar_key, radar_ready_key,
)

try:
    from zenoh import CongestionControl, Priority, Reliability
except Exception:
    CongestionControl = Priority = Reliability = None


OBJ_CATALOG = [
    {"code": 0, "name": "car",        "size": (450, 180, 150)},
    {"code": 1, "name": "truck",      "size": (1200, 250, 370)},
    {"code": 2, "name": "motorcycle", "size": (220, 80, 120)},
    {"code": 3, "name": "pedestrian", "size": (50, 50, 170)},
    {"code": 4, "name": "bicycle",    "size": (180, 60, 110)},
    {"code": 5, "name": "bus",        "size": (1300, 260, 320)},
]


def parse_args():
    p = argparse.ArgumentParser(description="Radar publisher (zenoh)")
    p.add_argument("--radar_id", required=True)
    p.add_argument("--adas_ip", default=os.environ.get("ADAS_IP", "127.0.0.10"))
    p.add_argument("--port", type=int, default=int(os.environ.get("PORT", "7447")))
    p.add_argument("--cycles", type=int, default=3000)
    p.add_argument("--run_for_s", type=int, default=int(os.environ.get("RUN_FOR_S", "60")))
    p.add_argument("--quiet", default=os.environ.get("ADAS_QUIET", "summary"))
    p.add_argument("--log_dir", default=os.environ.get("ADAS_LOG_DIR", None))
    p.add_argument("--out_dir", default=os.environ.get("ADAS_OUT_DIR", "."))

    # Calibration controls
    p.add_argument("--enable_calib", type=int, default=int(os.environ.get("CALIB", "1")))
    p.add_argument("--calib_ready_period", type=float, default=float(os.environ.get("CALIB_READY_PERIOD", "0.5")))
    p.add_argument("--calib_ms_min", type=int, default=int(os.environ.get("CALIB_MS_MIN", "80")))
    p.add_argument("--calib_ms_max", type=int, default=int(os.environ.get("CALIB_MS_MAX", "220")))
    args, _ = p.parse_known_args()

    if args.cycles < 1 or args.run_for_s < 1:
        raise ValueError("cycles and run_for_s must be positive")
    expected_cycles = (args.run_for_s * 1000) // PERIOD_MS
    if args.cycles != expected_cycles:
        print(f"Warning: cycles={args.cycles} != expected {expected_cycles} for run_for_s={args.run_for_s}")
    return args


def make_logger(mode):
    def log(msg):
        if mode == "off":
            print(msg, flush=True)
    return log


def make_session_and_publishers(radar_id, radar_ip, adas_ip, port):
    cfg = Config()
    cfg.insert_json5("listen/endpoints",  f'["tcp/{radar_ip}:{port}"]')
    cfg.insert_json5("connect/endpoints", f'["tcp/{adas_ip}:{port}"]')
    z = zenoh.open(cfg)

    hb_pub  = z.declare_publisher(f"adas/heartbeat/radar/{radar_id}")
    obj_pub = z.declare_publisher(f"adas/objects/radar/{radar_id}")
    det_pub = z.declare_publisher(f"adas/detections/radar/{radar_id}")

    calib_resp_pub      = z.declare_publisher(calib_resp_key(radar_id))
    calib_ack_radar_pub = z.declare_publisher(calib_ack_radar_key(radar_id))
    calib_ready_pub     = z.declare_publisher(radar_ready_key(radar_id))

    pubs = (hb_pub, obj_pub, det_pub, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub)
    if Reliability is not None:
        for p in pubs:
            try: p.reliability = Reliability.RELIABLE
            except Exception: pass
    if CongestionControl is not None:
        for p in pubs:
            try: p.congestion_control = CongestionControl.BLOCK
            except Exception: pass
    if Priority is not None:
        for p in pubs:
            try: p.priority = Priority.REAL_TIME
            except Exception: pass
    return z, hb_pub, obj_pub, det_pub, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub


def safe_put(pub, payload):
    """Best-effort put within the 20ms budget. Returns True if a put succeeded."""
    try:
        pub.put(payload)
        return True
    except Exception:
        t_end = time.monotonic() + (PERIOD_MS - 1) / 1000.0
        while time.monotonic() < t_end:
            try:
                pub.put(payload)
                return True
            except Exception:
                time.sleep(0.0005)
        return False


# ===== Counters & byte totals (thread-safe) =================================
_CMX = threading.Lock()
CTR = {
    # TX successes
    "tx_hb": 0,          "tx_hb_bytes": 0,
    "tx_obj": 0,         "tx_obj_bytes": 0,
    "tx_det": 0,         "tx_det_bytes": 0,
    "tx_cal_ack": 0,     "tx_cal_ack_bytes": 0,
    "tx_cal_resp": 0,    "tx_cal_resp_bytes": 0,
    "tx_ready": 0,       "tx_ready_bytes": 0,
    # RX
    "rx_cal_req": 0,     "rx_cal_req_bytes": 0,
    # Attempts (optional, for visibility)
    "attempt_hb": 0,
    "attempt_obj": 0,
    "attempt_det": 0,
}

def _add(key, val=1):
    with _CMX:
        CTR[key] = CTR.get(key, 0) + val


def _start_calibration_handlers(z, radar_id, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub, lf, args):
    stop_ev = threading.Event()

    # Presence beacon (READY ping to ECU)
    def ready_beacon():
        # 16 bytes payload: 4×u32 BE
        while not stop_ev.is_set():
            try:
                payload = struct.pack(">IIII", now_ms_u32(), rand_u32(), rand_u32(), rand_u32())
                calib_ready_pub.put(payload)
                _add("tx_ready", 1)
                _add("tx_ready_bytes", len(payload))
            except Exception:
                pass
            time.sleep(args.calib_ready_period)

    threading.Thread(target=ready_beacon, daemon=True).start()

    def handle_req(req_bytes):
        try:
            # count CAL-REQ (RX)
            _add("rx_cal_req", 1)
            _add("rx_cal_req_bytes", len(req_bytes))

            req = decode_calibration_request(req_bytes)

            # RADAR-ACK (16 bytes)
            radar_rx_ts = now_ms_u32()
            ack = CalibrationAck(
                calibration_req_data_1=req.calibration_req_data_1,
                calibration_req_data_2=radar_rx_ts,
                calibration_req_data_3=req.calibration_req_data_3,
                calibration_req_data_4=req.calibration_req_data_4,
            )
            ack_bytes = encode_calibration_ack(ack)
            calib_ack_radar_pub.put(ack_bytes)
            _add("tx_cal_ack", 1)
            _add("tx_cal_ack_bytes", len(ack_bytes))

            # Work → RADAR-RESP (16 bytes)
            lo, hi = sorted((args.calib_ms_min, args.calib_ms_max))
            dur_ms = random.randint(lo, hi)
            time.sleep(dur_ms / 1000.0)
            resp = CalibrationResponse(
                calibration_req_data_1=req.calibration_req_data_1,
                calibration_req_data_2=now_ms_u32(),
                calibration_req_data_3=rand_u32(),
                calibration_req_data_4=rand_u32(),
            )
            resp_bytes = encode_calibration_response(resp)
            calib_resp_pub.put(resp_bytes)
            _add("tx_cal_resp", 1)
            _add("tx_cal_resp_bytes", len(resp_bytes))

            # Single compact line (no 'work=')
            lf.write(
                f"[CAL][TXN][RADAR {radar_id}] req={req.calibration_req_data_1} "
                f"rx_ts={radar_rx_ts} resp_ts={resp.calibration_req_data_2} "
                f"req_data=({req.calibration_req_data_1},{req.calibration_req_data_2},"
                f"{req.calibration_req_data_3},{req.calibration_req_data_4}) "
                f"resp_data=({resp.calibration_req_data_3},{resp.calibration_req_data_4})\n"
            )

        except Exception as e:
            lf.write(f"[ERR][CAL][{radar_id}] {e}\n")

    def on_req(sample):
        try:
            raw = sample.payload.to_bytes() if hasattr(sample.payload, "to_bytes") else bytes(sample.payload)
            threading.Thread(target=handle_req, args=(raw,), daemon=True).start()
        except Exception as e:
            lf.write(f"[ERR][CAL_SUB][{radar_id}] {e}\n")

    sub = z.declare_subscriber(calib_req_bcast_key(), on_req)
    if Reliability is not None:
        try: sub.reliability = Reliability.RELIABLE
        except Exception: pass

    def cleanup():
        stop_ev.set()
        try: sub.undeclare()
        except Exception: pass

    return cleanup


def main():
    args = parse_args()
    radar_id = str(args.radar_id)

    try:
        rid_octet = max(1, min(254, int(radar_id)))
    except Exception:
        rid_octet = 1
    radar_ip = f"127.0.1.{rid_octet}"

    log = make_logger(args.quiet)

    log_dir = args.log_dir or "."
    os.makedirs(log_dir, exist_ok=True)
    log_path = os.path.join(log_dir, f"radar_{radar_id}.log")

    z, hb_pub, obj_pub, det_pub, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub = make_session_and_publishers(
        radar_id, radar_ip, args.adas_ip, args.port
    )

    calib_cleanup = None
    try:
        with open(log_path, "w", buffering=1) as lf:
            lf.write(f"[Radar_{radar_id}] Publishing every {PERIOD_MS} ms (STOP-free, {args.run_for_s}s run)\n")

            if args.enable_calib:
                calib_cleanup = _start_calibration_handlers(
                    z, radar_id, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub, lf, args
                )

            period_s = PERIOD_MS / 1000.0
            next_t = time.monotonic()

            for seq in range(1, args.cycles + 1):
                ts_ms = now_ms()

                # ================= Heartbeat =================
                hb_bytes = pack_heartbeat(ts_ms, seq, health=HEALTH_OK)
                _add("attempt_hb", 1)
                ok_hb = safe_put(hb_pub, hb_bytes)
                if ok_hb:
                    _add("tx_hb", 1)
                    _add("tx_hb_bytes", len(hb_bytes))
                lf.write(f"[TX][HB][{radar_id}] seq={seq} ts_ms={ts_ms} | {'✅' if ok_hb else '🔴 SEND-FAIL'}\n")

                # ================= Objects ===================
                payload = bytearray(OBJ_SIZE * OBJECTS_PER_CYCLE)
                obj_preview = []
                for i in range(OBJECTS_PER_CYCLE):
                    cat = OBJ_CATALOG[i % len(OBJ_CATALOG)]
                    size_x, size_y, size_z = cat["size"]
                    pos_x = 200 + 5*i + (seq % 10)
                    pos_y = -50 + 4*(i % 5)
                    pos_z = 0
                    vel_x, vel_y, vel_z = 15 + i, 0, 0
                    obj_bytes = pack_one_object(
                        ts_ms, seq, pos_x, pos_y, pos_z, vel_x, vel_y, vel_z,
                        0, 0, 0, size_x, size_y, size_z, cat["code"]
                    )
                    start = i * OBJ_SIZE
                    payload[start:start+OBJ_SIZE] = obj_bytes
                    obj_preview.append(
                        f"Object {i+1}: type={cat['name']} (code={cat['code']}), "
                        f"size={size_x}x{size_y}x{size_z} cm, pos=({pos_x},{pos_y},{pos_z}) cm, "
                        f"vel=({vel_x},{vel_y},{vel_z}) cm/s"
                    )
                _add("attempt_obj", 1)
                ok_obj = safe_put(obj_pub, bytes(payload))
                if ok_obj:
                    _add("tx_obj", 1)
                    _add("tx_obj_bytes", len(payload))
                lf.write(f"[TX][OBJ][{radar_id}] seq={seq} ts_ms={ts_ms} | {'✅' if ok_obj else '🔴 SEND-FAIL'} objs={OBJECTS_PER_CYCLE}\n")
                for line in obj_preview:
                    lf.write(f"[TX][OBJ][{radar_id}] seq={seq} ts_ms={ts_ms} | {line}\n")

                # ================= Detections =================
                det_payload = bytearray(pack_detection_array(ts_ms, seq))
                # set type=0 for all detections
                for i in range(DETECTIONS_PER_CYCLE):
                    det_payload[i * DET_SIZE + (DET_SIZE - 1)] = 0
                _add("attempt_det", 1)
                ok_det = safe_put(det_pub, bytes(det_payload))
                if ok_det:
                    _add("tx_det", 1)
                    _add("tx_det_bytes", len(det_payload))
                lf.write(f"[TX][DET][{radar_id}] seq={seq} ts_ms={ts_ms} | {'✅' if ok_det else '🔴 SEND-FAIL'} dets={DETECTIONS_PER_CYCLE}\n")

                for i in range(DETECTIONS_PER_CYCLE):
                    vals = struct.unpack_from(DET_FMT, det_payload, i * DET_SIZE)
                    pos_x, pos_y, pos_z = vals[2], vals[3], vals[4]
                    vel_x, vel_y, vel_z = vals[5], vals[6], vals[7]
                    sizes_all = vals[11:19]
                    lf.write(
                        f"[TX][DET][{radar_id}] seq={seq} ts_ms={ts_ms} | "
                        f"Detection {i+1}: type=unknown (code={vals[19]}), "
                        f"sizes=[{','.join(map(str,sizes_all))}], "
                        f"pos=({pos_x},{pos_y},{pos_z}) cm, vel=({vel_x},{vel_y},{vel_z}) cm/s\n"
                    )

                # pacing
                next_t += period_s
                sleep_s = next_t - time.monotonic()
                if sleep_s > 0:
                    time.sleep(sleep_s)
                else:
                    next_t = time.monotonic()

            # ===== Final summaries (to log and stdout) =====
            with _CMX:
                c = dict(CTR)  # snapshot

            tx_tot_bytes = c["tx_hb_bytes"] + c["tx_obj_bytes"] + c["tx_det_bytes"] + c["tx_cal_ack_bytes"] + c["tx_cal_resp_bytes"] + c["tx_ready_bytes"]
            secs = max(1, args.run_for_s)
            def rate(n): return f"{(n / secs):.2f}/s"

            counts = (
                "[RADAR][COUNTS] ===== MESSAGE COUNT SUMMARY =====\n"
                f"TX: HB={c['tx_hb']} ({rate(c['tx_hb'])})  "
                f"OBJ={c['tx_obj']} ({rate(c['tx_obj'])})  "
                f"DET={c['tx_det']} ({rate(c['tx_det'])})\n"
                f"CAL: ACK={c['tx_cal_ack']} ({rate(c['tx_cal_ack'])})  "
                f"RESP={c['tx_cal_resp']} ({rate(c['tx_cal_resp'])})  "
                f"READY={c['tx_ready']} ({rate(c['tx_ready'])})\n"
                f"RX CAL: REQ={c['rx_cal_req']} ({rate(c['rx_cal_req'])})\n"
                f"ATTEMPTS: HB={c['attempt_hb']} OBJ={c['attempt_obj']} DET={c['attempt_det']}\n"
            )

            bytes_sum = (
                "[RADAR][BYTES] ===== PAYLOAD BYTE SUMMARY (exact payload only) =====\n"
                f"TX: HB={c['tx_hb_bytes']} B  OBJ={c['tx_obj_bytes']} B  DET={c['tx_det_bytes']} B  "
                f"CAL_ACK={c['tx_cal_ack_bytes']} B  CAL_RESP={c['tx_cal_resp_bytes']} B  READY={c['tx_ready_bytes']} B\n"
                f"    TX TOTAL payload={tx_tot_bytes} B  (~{(8*tx_tot_bytes)/1e6/ secs:.3f} Mbit/s avg payload)\n"
                f"RX: CAL_REQ={c['rx_cal_req_bytes']} B\n"
            )

            print(counts + bytes_sum, flush=True)
            lf.write(counts)
            lf.write(bytes_sum)

    finally:
        if calib_cleanup:
            try: calib_cleanup()
            except Exception: pass
        for pub in (hb_pub, obj_pub, det_pub, calib_resp_pub, calib_ack_radar_pub, calib_ready_pub):
            try: pub.undeclare()
            except Exception: pass
        z.close()
        log(f"[Radar_{radar_id}] Done.")


if __name__ == "__main__":
    main()