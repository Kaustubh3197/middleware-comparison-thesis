# common.py
# Shared structs, timing, (de)serialization, and topic helpers
import struct
import time
import random
from dataclasses import dataclass

# ---------------- Time helpers ----------------
def now_ms() -> int:
    return int(time.monotonic() * 1000)

def now_ms_u32() -> int:
    return int(time.time() * 1000) & 0xFFFFFFFF

def rand_u32() -> int:
    return random.getrandbits(32)

# ============================================================
# ================ Heartbeat / Objects / Detections ==========
# ============================================================

# ---- Heartbeat (little-endian) ----
HB_FMT = "<III"
HB_SIZE = struct.calcsize(HB_FMT)
HEALTH_OK = 0
PERIOD_MS = 20
E2E_MAX_MS = 5
JITTER_MAX_MS = 1

def pack_heartbeat(ts_ms: int, seq: int, health: int) -> bytes:
    return struct.pack(HB_FMT, ts_ms & 0xFFFFFFFF, seq & 0xFFFFFFFF, health & 0xFFFFFFFF)

def unpack_heartbeat(buf: bytes):
    if len(buf) != HB_SIZE:
        raise ValueError(f"Heartbeat size mismatch: {len(buf)} != {HB_SIZE}")
    ts_ms, seq, health = struct.unpack(HB_FMT, buf)
    return ts_ms, seq, health

def health_str(h: int) -> str:
    return "OK" if h == HEALTH_OK else str(h)

# ---- Objects (little-endian) ----
OBJ_FMT = "<" + "I"*14 + "B"
OBJ_SIZE = struct.calcsize(OBJ_FMT)
OBJECTS_PER_CYCLE = 20
OBJ_PERIOD_MS = 20
OBJ_E2E_MAX_MS = 25
OBJ_JITTER_MAX_MS = 5
OBJ_CONSEC_LOSS_FAULT = 3
# Convenience: full array size
OBJ_ARRAY_SIZE = OBJ_SIZE * OBJECTS_PER_CYCLE

def pack_one_object(ts_ms: int, seq: int,
                    pos_x=0, pos_y=0, pos_z=0,
                    vel_x=0, vel_y=0, vel_z=0,
                    acc_x=0, acc_y=0, acc_z=0,
                    size_x=0, size_y=0, size_z=0,
                    obj_type=0) -> bytes:
    fields = (
        ts_ms & 0xFFFFFFFF, seq & 0xFFFFFFFF,
        pos_x & 0xFFFFFFFF, pos_y & 0xFFFFFFFF, pos_z & 0xFFFFFFFF,
        vel_x & 0xFFFFFFFF, vel_y & 0xFFFFFFFF, vel_z & 0xFFFFFFFF,
        acc_x & 0xFFFFFFFF, acc_y & 0xFFFFFFFF, acc_z & 0xFFFFFFFF,
        size_x & 0xFFFFFFFF, size_y & 0xFFFFFFFF, size_z & 0xFFFFFFFF,
        obj_type & 0xFF
    )
    return struct.pack(OBJ_FMT, *fields)

def pack_object_array(ts_ms: int, seq: int) -> bytes:
    payload = bytearray(OBJ_SIZE * OBJECTS_PER_CYCLE)
    for i in range(OBJECTS_PER_CYCLE):
        payload[i*OBJ_SIZE:(i+1)*OBJ_SIZE] = pack_one_object(ts_ms, seq, obj_type=i)
    return bytes(payload)

def unpack_object_array(buf: bytes):
    expected = OBJ_SIZE * OBJECTS_PER_CYCLE
    if len(buf) != expected:
        raise ValueError(f"Object array size mismatch: {len(buf)} != {expected}")
    first = buf[0:OBJ_SIZE]
    ts_ms, seq, *_ = struct.unpack(OBJ_FMT, first)
    return ts_ms, seq

# ---- Detections (little-endian) ----
DET_FMT = "<" + "I"*19 + "B"
DET_SIZE = struct.calcsize(DET_FMT)
DETECTIONS_PER_CYCLE = 20
DET_PERIOD_MS = 20
DET_E2E_MAX_MS = 25
DET_JITTER_MAX_MS = 5
DET_CONSEC_LOSS_FAULT = 3
DET_KNOWN_TYPES = [1, 2, 3, 4]
DET_UNKNOWN_TYPES = [0, 255]
# Convenience: full array size
DET_ARRAY_SIZE = DET_SIZE * DETECTIONS_PER_CYCLE

def pack_one_detection(ts_ms: int, seq: int,
                       pos_x=0, pos_y=0, pos_z=0,
                       vel_x=0, vel_y=0, vel_z=0,
                       acc_x=0, acc_y=0, acc_z=0,
                       size_1=0, size_2=0, size_3=0, size_4=0,
                       size_5=0, size_6=0, size_7=0, size_8=0,
                       det_type=0) -> bytes:
    fields = (
        ts_ms & 0xFFFFFFFF, seq & 0xFFFFFFFF,
        pos_x & 0xFFFFFFFF, pos_y & 0xFFFFFFFF, pos_z & 0xFFFFFFFF,
        vel_x & 0xFFFFFFFF, vel_y & 0xFFFFFFFF, vel_z & 0xFFFFFFFF,
        acc_x & 0xFFFFFFFF, acc_y & 0xFFFFFFFF, acc_z & 0xFFFFFFFF,
        size_1 & 0xFFFFFFFF, size_2 & 0xFFFFFFFF, size_3 & 0xFFFFFFFF, size_4 & 0xFFFFFFFF,
        size_5 & 0xFFFFFFFF, size_6 & 0xFFFFFFFF, size_7 & 0xFFFFFFFF, size_8 & 0xFFFFFFFF,
        det_type & 0xFF
    )
    return struct.pack(DET_FMT, *fields)

def pack_detection_array(ts_ms: int, seq: int) -> bytes:
    payload = bytearray(DET_SIZE * DETECTIONS_PER_CYCLE)
    for i in range(DETECTIONS_PER_CYCLE):
        det_type = random.choice(DET_KNOWN_TYPES + DET_UNKNOWN_TYPES)
        payload[i*DET_SIZE:(i+1)*DET_SIZE] = pack_one_detection(
            ts_ms, seq,
            pos_x=random.randint(0, 300),
            pos_y=random.randint(0, 100),
            pos_z=random.randint(0, 300),
            vel_x=random.randint(0, 10),
            vel_y=random.randint(0, 10),
            vel_z=random.randint(0, 10),
            acc_x=random.randint(0, 200),
            acc_y=random.randint(0, 200),
            acc_z=random.randint(0, 200),
            size_1=random.randint(1, 200),
            size_2=random.randint(1, 200),
            size_3=random.randint(1, 200),
            size_4=random.randint(1, 200),
            size_5=random.randint(1, 200),
            size_6=random.randint(1, 200),
            size_7=random.randint(1, 200),
            size_8=random.randint(1, 200),
            det_type=det_type
        )
    return bytes(payload)

def unpack_detection_array(buf: bytes):
    expected = DET_SIZE * DETECTIONS_PER_CYCLE
    if len(buf) != expected:
        raise ValueError(f"Detection array size mismatch: {len(buf)} != {expected}")
    first = buf[0:DET_SIZE]
    ts_ms, seq, *_ = struct.unpack(DET_FMT, first)
    return ts_ms, seq

# ===================== Calibration (big-endian 4×u32) =====================
CAL_FMT = ">IIII"
CAL_SIZE = struct.calcsize(CAL_FMT)

@dataclass
class CalibrationRequest:
    calibration_req_data_1: int  # req_id
    calibration_req_data_2: int  # arbitrary u32 (we log ECU send ts separately)
    calibration_req_data_3: int  # randA
    calibration_req_data_4: int  # randB

@dataclass
class CalibrationResponse:
    calibration_req_data_1: int  # req_id (echo)
    calibration_req_data_2: int  # radar response timestamp (ms, u32)
    calibration_req_data_3: int  # randC
    calibration_req_data_4: int  # randD

@dataclass
class CalibrationAck:
    calibration_req_data_1: int  # req_id (echo)
    calibration_req_data_2: int  # sender timestamp (radar-rx-ts or ecu-rx-ts)
    calibration_req_data_3: int  # data1 (echoed)
    calibration_req_data_4: int  # data2 (echoed)

def encode_calibration_request(m: CalibrationRequest) -> bytes:
    return struct.pack(CAL_FMT, m.calibration_req_data_1, m.calibration_req_data_2,
                                 m.calibration_req_data_3, m.calibration_req_data_4)
def decode_calibration_request(raw: bytes) -> CalibrationRequest:
    if len(raw) != CAL_SIZE: raise ValueError(f"CalibrationRequest must be {CAL_SIZE} bytes, got {len(raw)}")
    d1,d2,d3,d4 = struct.unpack(CAL_FMT, raw)
    return CalibrationRequest(d1,d2,d3,d4)

def encode_calibration_response(m: CalibrationResponse) -> bytes:
    return struct.pack(CAL_FMT, m.calibration_req_data_1, m.calibration_req_data_2,
                                 m.calibration_req_data_3, m.calibration_req_data_4)
def decode_calibration_response(raw: bytes) -> CalibrationResponse:
    if len(raw) != CAL_SIZE: raise ValueError(f"CalibrationResponse must be {CAL_SIZE} bytes, got {len(raw)}")
    d1,d2,d3,d4 = struct.unpack(CAL_FMT, raw)
    return CalibrationResponse(d1,d2,d3,d4)

def encode_calibration_ack(m: CalibrationAck) -> bytes:
    return struct.pack(CAL_FMT, m.calibration_req_data_1, m.calibration_req_data_2,
                                 m.calibration_req_data_3, m.calibration_req_data_4)
def decode_calibration_ack(raw: bytes) -> CalibrationAck:
    if len(raw) != CAL_SIZE: raise ValueError(f"CalibrationAck must be {CAL_SIZE} bytes, got {len(raw)}")
    d1,d2,d3,d4 = struct.unpack(CAL_FMT, raw)
    return CalibrationAck(d1,d2,d3,d4)

def build_radar_ack_from_request(req: CalibrationRequest, radar_rx_ts_ms: int) -> CalibrationAck:
    return CalibrationAck(req.calibration_req_data_1, radar_rx_ts_ms,
                          req.calibration_req_data_3, req.calibration_req_data_4)

def build_ecu_ack_from_response(resp: CalibrationResponse, ecu_rx_ts_ms: int) -> CalibrationAck:
    return CalibrationAck(resp.calibration_req_data_1, ecu_rx_ts_ms,
                          resp.calibration_req_data_3, resp.calibration_req_data_4)

# Topics
def calib_req_bcast_key() -> str:           return "adas/calib/request"
def calib_resp_key(radar_id: str) -> str:   return f"adas/calib/response/{radar_id}"
def calib_ack_radar_key(radar_id: str) -> str: return f"adas/calib/ack/radar/{radar_id}"
def calib_ack_ecu_key(radar_id: str) -> str:   return f"adas/calib/ack/ecu/{radar_id}"
def radar_ready_key(radar_id: str) -> str:  return f"adas/calib/ready/{radar_id}"