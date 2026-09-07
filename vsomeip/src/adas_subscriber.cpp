// adas_subscriber.cpp — Excel + full file logging of RX events (no CSV)
// Saves into shared per-run folder:
//   run/<YYYYMMDD_HHMMSS>/subscriber/
//     - adas_subscriber_<YYYYMMDD_HHMMSS>.log
//     - adas_summary_<YYYYMMDD_HHMMSS>.xlsx

#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <random>

// ===== Excel (libxlsxwriter) =====
#include <xlsxwriter.h>

// -------- vSomeIP IDs (HB/OBJ/DET) --------
#define HEARTBEAT_SERVICE_ID      0x1234
#define HEARTBEAT_INSTANCE_BASE   0x5678
#define HEARTBEAT_EVENTGROUP_ID   0x4465
#define HEARTBEAT_EVENT_ID        0x8778

#define OBJECTS_SERVICE_ID        0x2345
#define OBJECTS_INSTANCE_BASE     0x6789
#define OBJECTS_EVENTGROUP_ID     0x5566
#define OBJECTS_EVENT_ID          0x9889

#define DETECTIONS_SERVICE_ID     0x3456
#define DETECTIONS_INSTANCE_BASE  0x7890
#define DETECTIONS_EVENTGROUP_ID  0x6677
#define DETECTIONS_EVENT_ID       0xAABB

#define READY_METHOD_ID           0x7010

// -------- vSomeIP IDs (CALIB 4th use case) --------
#define CALIB_SERVICE_ID          0x4567
#define CALIB_INSTANCE_BASE       0x8A00
#define CALIB_EVENTGROUP_ID       0xCA1B
#define CALIB_EVENT_ID            0xCA01
#define RADAR_ACK_METHOD_ID       0xCA02
#define RADAR_RESP_METHOD_ID      0xCA03

// -------- Wire structs (HB/OBJ/DET) --------
// Match publisher: no wrappers; flat arrays for OBJ/DET; HB is 3×u32.
#pragma pack(push, 1)
static const int    MAX_OBJECTS    = 20;
static const int    MAX_DETECTIONS = 20;

struct Heartbeat {
    uint32_t ts_ms;
    uint32_t seq;
    uint32_t health;
}; // 12 B

struct RadarObject {
    uint32_t ts_ms;
    uint32_t seq;
    uint32_t pos_x, pos_y, pos_z;
    uint32_t vel_x, vel_y, vel_z;
    uint32_t acc_x, acc_y, acc_z;
    uint32_t size_x, size_y, size_z;
    uint8_t  type;
}; // 57 B

struct Detection {
    uint32_t ts_ms;
    uint32_t seq;
    uint32_t pos_x, pos_y, pos_z;
    uint32_t vel_x, vel_y, vel_z;
    uint32_t acc_x, acc_y, acc_z;
    uint32_t size_1, size_2, size_3, size_4;
    uint32_t size_5, size_6, size_7, size_8;
    uint8_t  type;
}; // 77 B
#pragma pack(pop)

constexpr size_t OBJ_PAYLOAD_BYTES = MAX_OBJECTS    * sizeof(RadarObject);  // 1140
constexpr size_t DET_PAYLOAD_BYTES = MAX_DETECTIONS * sizeof(Detection);    // 1540
static_assert(sizeof(Heartbeat)   == 12,  "Heartbeat must be 12 bytes");
static_assert(sizeof(RadarObject) == 57,  "RadarObject must be 57 bytes");
static_assert(sizeof(Detection)   == 77,  "Detection must be 77 bytes");
static_assert(OBJ_PAYLOAD_BYTES   == 1140, "OBJ payload must be 1140 B");
static_assert(DET_PAYLOAD_BYTES   == 1540, "DET payload must be 1540 B");

// -------- Calibration wire struct helpers (strict 4×u32, big-endian) --------
struct U32x4 { uint32_t d1, d2, d3, d4; };

static inline uint32_t bswap32(uint32_t x){
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t htobe32u(uint32_t x){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return bswap32(x);
#else
    return x;
#endif
}
static inline uint32_t be32tohu(uint32_t x){ return htobe32u(x); }

static inline std::array<uint8_t,16> pack_be(const U32x4 &v){
    std::array<uint8_t,16> b{};
    uint32_t w[4] = { htobe32u(v.d1), htobe32u(v.d2), htobe32u(v.d3), htobe32u(v.d4) };
    std::memcpy(b.data()+0,  &w[0], 4);
    std::memcpy(b.data()+4,  &w[1], 4);
    std::memcpy(b.data()+8,  &w[2], 4);
    std::memcpy(b.data()+12, &w[3], 4);
    return b;
}
static inline U32x4 unpack_be(const uint8_t* p, size_t n){
    U32x4 v{}; if (n<16) return v;
    uint32_t w[4];
    std::memcpy(&w[0], p+0, 4); std::memcpy(&w[1], p+4, 4);
    std::memcpy(&w[2], p+8, 4); std::memcpy(&w[3], p+12, 4);
    v.d1 = be32tohu(w[0]); v.d2 = be32tohu(w[1]); v.d3 = be32tohu(w[2]); v.d4 = be32tohu(w[3]);
    return v;
}

// -------- Fixed thresholds / window --------
static const uint32_t HB_PERIOD_MS   = 20, HB_E2E_MAX_MS = 5,  HB_JITTER_MAX_MS = 1;
static const uint32_t OBJ_PERIOD_MS  = 20, OBJ_E2E_MAX_MS = 25, OBJ_JITTER_MAX_MS = 5, OBJ_CONSEC_FAULT = 3;
static const uint32_t DET_PERIOD_MS  = 20, DET_E2E_MAX_MS = 25, DET_JITTER_MAX_MS = 5, DET_CONSEC_FAULT = 3;

// -------- Calibration timing --------
static const uint32_t CAL_PERIOD_MS  = 1000;

// -------- Run duration (CLI override) --------
static int RUN_FOR_S = 60;                 // configurable

// -------- Time helpers --------
static inline uint64_t now_ms64() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static inline std::string wall_iso_now() {
    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
static inline std::string make_stamp() {
    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm); // correct arg order
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// -------- Shared parent run folder helpers --------
static bool is_valid_stamp(const std::string &s) {
    if (s.size() != 15 || s[8] != '_') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8) continue;
        if (!::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}
static std::time_t parse_stamp_to_time_t(const std::string &s) {
    std::tm tm{};
    tm.tm_year = std::stoi(s.substr(0,4)) - 1900;
    tm.tm_mon  = std::stoi(s.substr(4,2)) - 1;
    tm.tm_mday = std::stoi(s.substr(6,2));
    tm.tm_hour = std::stoi(s.substr(9,2));
    tm.tm_min  = std::stoi(s.substr(11,2));
    tm.tm_sec  = std::stoi(s.substr(13,2));
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}
static std::string get_or_create_shared_parent_run_dir() {
    namespace fs = std::filesystem;
    static const uint64_t WINDOW_S = 15; // reuse if created within last 15s
    const fs::path base{"run"};
    try { fs::create_directories(base); } catch(...) {}

    std::string latest_name;
    std::time_t latest_tt = 0;
    try {
        for (const auto &e : fs::directory_iterator(base)) {
            if (!e.is_directory()) continue;
            const auto name = e.path().filename().string();
            if (!is_valid_stamp(name)) continue;
            std::time_t tt = parse_stamp_to_time_t(name);
            if (tt > latest_tt) { latest_tt = tt; latest_name = name; }
        }
    } catch(...) {}

    const std::time_t now_tt = std::time(nullptr);
    if (!latest_name.empty() && (now_tt - latest_tt) <= static_cast<std::time_t>(WINDOW_S))
        return (base / latest_name).string();

    const std::string stamp = make_stamp();
    const fs::path parent = base / stamp;
    try { fs::create_directories(parent); } catch(...) {}
    return parent.string();
}

// -------- Async logger (file only) --------
class AsyncFileLogger {
public:
    explicit AsyncFileLogger(const std::string &prefix)
    : done_(false) {
        path_ = prefix + "_" + make_stamp() + ".log";
        ofs_.open(path_, std::ios::out | std::ios::binary);
        worker_ = std::thread([this]{ run(); });
    }
    ~AsyncFileLogger() { stop(); }
    void stop() {
        bool expected = false;
        if (done_.compare_exchange_strong(expected, true)) {
            cv_.notify_one();
            if (worker_.joinable()) worker_.join();
            if (ofs_.is_open()) { ofs_.flush(); ofs_.close(); }
        }
    }
    void log(const std::string &line) {
        if (!ofs_.is_open()) return;
        {
            std::lock_guard<std::mutex> lk(mx_);
            q_.push(line);
        }
        cv_.notify_one();
    }
    const std::string& path() const { return path_; }

private:
    void run() {
        std::string batch;
        batch.reserve(64*1024);
        while (true) {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(25), [&]{ return done_.load() || !q_.empty(); });
            while (!q_.empty()) {
                batch.append(q_.front());
                batch.push_back('\n');
                q_.pop();
                if (batch.size() > (256*1024)) break;
            }
            bool is_done = done_.load();
            lk.unlock();

            if (!batch.empty()) {
                ofs_ << batch;
                ofs_.flush();
                batch.clear();
            }
            if (is_done && q_.empty()) break;
        }
    }

    std::atomic<bool> done_;
    std::ofstream ofs_;
    std::string path_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::queue<std::string> q_;
    std::thread worker_;
};

static std::shared_ptr<vsomeip::application> app;
static std::unique_ptr<AsyncFileLogger> g_log;
static inline void LOG(const std::string &s){ if (g_log) g_log->log(s); }

// -------- Utils --------
static inline const char* health_str(uint32_t h) {
    return (h == 0) ? "OK" : "BAD";
}
static inline int slot_from(vsomeip::service_t svc, vsomeip::instance_t inst) {
    uint16_t base = 0;
    if (svc == HEARTBEAT_SERVICE_ID)      base = HEARTBEAT_INSTANCE_BASE;
    else if (svc == OBJECTS_SERVICE_ID)   base = OBJECTS_INSTANCE_BASE;
    else if (svc == DETECTIONS_SERVICE_ID)base = DETECTIONS_INSTANCE_BASE;
    else                                  return -1;
    if (inst < base) return -1;
    int slot = static_cast<int>(inst - base);
    return (slot >= 0 && slot < 6) ? slot : -1;
}

// ====== BYTE METRICS (RX/TX) ===============================================
static inline uint64_t someip_hdr() { return 16; }
static inline uint64_t ip_udp_ovh() { return 20 + 8; }   // IPv4 + UDP
static inline uint64_t ip_tcp_ovh() { return 20 + 20; }  // IPv4 + TCP
static inline uint64_t wire_udp(uint64_t pl) { return pl + someip_hdr() + ip_udp_ovh(); }
static inline uint64_t wire_tcp(uint64_t pl) { return pl + someip_hdr() + ip_tcp_ovh(); }

// RX bytes (what ECU receives)
static std::atomic<uint64_t> RX_PAYLOAD_HB{0},  RX_WIRE_HB{0};
static std::atomic<uint64_t> RX_PAYLOAD_OBJ{0}, RX_WIRE_OBJ{0};
static std::atomic<uint64_t> RX_PAYLOAD_DET{0}, RX_WIRE_DET{0};
static std::atomic<uint64_t> RX_PAYLOAD_CAL{0}, RX_WIRE_CAL{0}; // ACK + RESP requests from radars

// TX bytes (what ECU sends) — only CAL responses here
static std::atomic<uint64_t> TX_PAYLOAD_CAL{0}, TX_WIRE_CAL{0};

// ====== NEW: EXACT MESSAGE COUNTS ==========================================
// RX counts (decoded & accepted messages only)
static std::atomic<uint64_t> CNT_RX_HB{0};
static std::atomic<uint64_t> CNT_RX_OBJ{0};
static std::atomic<uint64_t> CNT_RX_DET{0};
static std::atomic<uint64_t> CNT_RX_CAL_ACK{0};   // radar -> ECU (RADAR_ACK_METHOD)
static std::atomic<uint64_t> CNT_RX_CAL_RESP{0};  // radar -> ECU (RADAR_RESP_METHOD)

// TX counts (ECU replies to radar methods)
static std::atomic<uint64_t> CNT_TX_ECU_ACK_TO_ACK{0};   // ECU response to RADAR_ACK (no payload)
static std::atomic<uint64_t> CNT_TX_ECU_ACK_TO_RESP{0};  // ECU response to RADAR_RESP (16B payload)

// -------- State & aggregates --------
struct StreamAgg { uint64_t total=0, ok=0, viol=0, decode_err=0, faults=0, max_e2e=0, max_jitter=0; };
struct HBState   { bool primed=false; uint32_t last_seq=0; uint64_t last_rx_ms=0; };
struct ObjState  { bool primed=false; uint32_t last_seq=0; uint64_t last_rx_ms=0; uint32_t loss_streak=0; };
using DetState = ObjState;

static std::mutex g_mx;
static std::unordered_map<std::string, HBState>  hb_states;
static std::unordered_map<std::string, ObjState> obj_states;
static std::unordered_map<std::string, DetState> det_states;

static std::unordered_map<std::string, StreamAgg> AHB, AOBJ, ADET;

// per-radar fault counters for logging
static std::unordered_map<std::string, int> HB_FAULTS, OBJ_FAULTS, DET_FAULTS;

// sample rows for Excel
struct SampleRow {
    std::string stream;   // HB/OBJ/DET
    std::string radar;    // "1" etc
    uint32_t seq{};
    uint64_t ts_src_ms{};
    uint64_t ts_rx_ms{};
    uint64_t e2e_ms{};
    uint64_t jitter_ms{};
    std::string status;   // OK / VIOL / PRIMED / DECODE_ERR
    std::string wall_iso;
};
struct FaultRow {
    std::string stream;
    std::string radar;
    int32_t seq;          // -1 => blank
    std::string reason;
    int64_t e2e_ms;       // -1 => blank
    int64_t jitter_ms;    // -1 => blank
    std::string wall_iso;
};
static std::vector<SampleRow> HB_SAMPLES, OBJ_SAMPLES, DET_SAMPLES;
static std::vector<FaultRow> FAULTS;

// per-slot subs + READY
struct SlotSub {
    std::atomic<bool> hb{false};
    std::atomic<bool> obj{false};
    std::atomic<bool> det{false};
    std::atomic<bool> ready_sent{false};
    std::atomic<bool> ready_acked{false};
};
static SlotSub slots[6];

static std::atomic<bool> run_started{false};
static std::mutex g_sw_mx;
static std::string g_start_wall, g_end_wall;

// per-run directory (auto)
static std::string g_run_dir;        // run/<stamp>/subscriber
static std::string g_parent_dir;     // run/<stamp>
static std::string g_stamp;          // <stamp> to use in filenames

// ---- CALIB (ECU) state ----
static std::atomic<bool> calib_bcast_running{false};
static std::atomic<bool> g_stop{false};

// Excel rows
struct CalTxnRow {
    std::string radar;   // "1", "2", ...
    uint32_t req_id;
    uint32_t ecu_send_ms, radar_ack_rx_ms, radar_resp_ms, ecu_ack_ms;
    uint32_t req_d1, req_d2, req_d3, req_d4;
    uint32_t resp_C, resp_D;
};
static std::vector<CalTxnRow> CAL_ROWS;

// per-(slot, req_id) inflight map
struct CalInflight {
    uint32_t ecu_send_ms{};
    uint32_t req_d1{}, req_d2{}, req_d3{}, req_d4{};
    uint32_t radar_ack_rx_ms{};
    uint32_t radar_resp_ms{};
    uint32_t resp_C{}, resp_D{};
};
static std::mutex CAL_MX;
static std::unordered_map<uint64_t, CalInflight> CAL_INFLIGHT; // key = ((uint64_t)slot<<32)|req_id
static inline uint64_t cal_key(int slot, uint32_t req_id){
    return ( (uint64_t)(uint32_t)slot << 32 ) | (uint64_t)req_id;
}

// -------- Helpers to push rows safely --------
static inline void push_hb_sample(const std::string &rad, uint32_t seq, uint64_t ts_src, uint64_t ts_rx,
                                  uint64_t e2e, uint64_t jitter, const std::string &status, const std::string &wiso) {
    SampleRow r;
    r.stream = "HB"; r.radar = rad; r.seq = seq; r.ts_src_ms = ts_src; r.ts_rx_ms = ts_rx;
    r.e2e_ms = e2e; r.jitter_ms = jitter; r.status = status; r.wall_iso = wiso;
    HB_SAMPLES.push_back(r);
}
static inline void push_obj_sample(const std::string &rad, uint32_t seq, uint64_t ts_src, uint64_t ts_rx,
                                   uint64_t e2e, uint64_t jitter, const std::string &status, const std::string &wiso) {
    SampleRow r;
    r.stream = "OBJ"; r.radar = rad; r.seq = seq; r.ts_src_ms = ts_src; r.ts_rx_ms = ts_rx;
    r.e2e_ms = e2e; r.jitter_ms = jitter; r.status = status; r.wall_iso = wiso;
    OBJ_SAMPLES.push_back(r);
}
static inline void push_det_sample(const std::string &rad, uint32_t seq, uint64_t ts_src, uint64_t ts_rx,
                                   uint64_t e2e, uint64_t jitter, const std::string &status, const std::string &wiso) {
    SampleRow r;
    r.stream = "DET"; r.radar = rad; r.seq = seq; r.ts_src_ms = ts_src; r.ts_rx_ms = ts_rx;
    r.e2e_ms = e2e; r.jitter_ms = jitter; r.status = status; r.wall_iso = wiso;
    DET_SAMPLES.push_back(r);
}
static inline void push_fault(const std::string &stream, const std::string &rad, int32_t seq,
                              const std::string &reason, int64_t e2e, int64_t jitter, const std::string &wiso) {
    FaultRow f;
    f.stream = stream; f.radar = rad; f.seq = seq; f.reason = reason;
    f.e2e_ms = e2e; f.jitter_ms = jitter; f.wall_iso = wiso;
    FAULTS.push_back(f);
}

// -------- Start/subscribe/handlers --------
// --- REPLACE with this version (note: bigger STOP_GRACE_MS = 1200) ---
static void start_run_timer_after_ready() {
    bool expected = false;
    if (run_started.compare_exchange_strong(expected, true)) {
        {
            std::lock_guard<std::mutex> lk(g_sw_mx);
            g_start_wall = wall_iso_now();
        }
        // Enough headroom for the last CAL RESP (ACK delay + RESP delay + scheduling jitter)
        static const int STOP_GRACE_MS = 1200;

        LOG("[ECU] RUN timer started (READY ack) — will stop in "
            + std::to_string(RUN_FOR_S) + "s + "
            + std::to_string(STOP_GRACE_MS) + "ms grace");

        std::thread([=]{
            std::this_thread::sleep_for(std::chrono::seconds(RUN_FOR_S));
            std::this_thread::sleep_for(std::chrono::milliseconds(STOP_GRACE_MS));
            if (app) app->stop();
        }).detach();
    }
}


static void subscribe_stream(vsomeip::service_t svc, vsomeip::instance_t inst,
                             vsomeip::eventgroup_t eg, vsomeip::event_t ev) {
    std::set<vsomeip::eventgroup_t> groups{ eg };
    app->request_event(svc, inst, ev, groups);
    app->subscribe(svc, inst, eg);
}

static void try_send_ready_for_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= 6) return;
    auto &S = slots[slot_idx];
    if (S.ready_sent.load()) return;
    if (!(S.hb.load() && S.obj.load() && S.det.load())) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto req = vsomeip::runtime::get()->create_request();
    req->set_service(HEARTBEAT_SERVICE_ID);
    req->set_instance(static_cast<vsomeip::instance_t>(HEARTBEAT_INSTANCE_BASE + slot_idx));
    req->set_method(READY_METHOD_ID);
    app->send(req);
    S.ready_sent.store(true);
    {
        std::ostringstream os;
        os << "[ECU] READY sent (slot=" << slot_idx
           << " inst=0x" << std::hex << (HEARTBEAT_INSTANCE_BASE + slot_idx) << std::dec << ")";
        LOG(os.str());
    }
}

// ===== CHANGED: start CAL broadcast only after first READY ack,
// and only send to slots that have ready_acked == true.
// --- REPLACE the whole start_cal_broadcast_if_needed() with this ---
static void start_cal_broadcast_if_needed() {
    if (!calib_bcast_running.exchange(true)) {
        std::thread([]{
            // We start sending immediately, then every CAL_PERIOD_MS with steady scheduling.
            // Exactly target_count sends (e.g., 60 for 60s @ 1Hz).
            const uint32_t target_count =
                static_cast<uint32_t>((RUN_FOR_S * 1000) / CAL_PERIOD_MS);

            std::mt19937 rng{ std::random_device{}() };

            // Use steady_clock for precise interval scheduling
            auto base = std::chrono::steady_clock::now();

            for (uint32_t k = 0; k < target_count && !g_stop.load(); ++k) {
                // Only send if at least one slot is READY-ACKed (should be, since we start after READY)
                bool any_ready = false;
                for (int s = 0; s < 6; ++s) {
                    if (slots[s].ready_acked.load()) { any_ready = true; break; }
                }

                if (any_ready) {
                    U32x4 req{};
                    req.d1 = (uint32_t)rng(); // req_id
                    req.d2 = (uint32_t)rng();
                    req.d3 = (uint32_t)rng();
                    req.d4 = (uint32_t)rng();

                    const uint32_t ecu_ts = (uint32_t)now_ms64();
                    auto bytes = pack_be(req);
                    bool sent_any = false;

                    for (int slot = 0; slot < 6; ++slot) {
                        if (!slots[slot].ready_acked.load()) continue;

                        const auto inst = static_cast<vsomeip::instance_t>(CALIB_INSTANCE_BASE + slot);

                        auto pl = vsomeip::runtime::get()->create_payload();
                        pl->set_data(bytes.data(), (uint32_t)bytes.size());
                        app->notify(CALIB_SERVICE_ID, inst, CALIB_EVENT_ID, pl);

                        {
                            std::lock_guard<std::mutex> lk(CAL_MX);
                            auto &inf = CAL_INFLIGHT[cal_key(slot, req.d1)];
                            inf.ecu_send_ms = ecu_ts;
                            inf.req_d1 = req.d1; inf.req_d2 = req.d2; inf.req_d3 = req.d3; inf.req_d4 = req.d4;
                        }
                        sent_any = true;
                    }

                    if (sent_any) {
                        std::ostringstream os;
                        os << "[CAL][SEND] #" << (k+1)
                           << " req_id=" << req.d1 << " ecu_ts=" << ecu_ts
                           << " data=(" << req.d1 << "," << req.d2 << "," << req.d3 << "," << req.d4 << ")";
                        LOG(os.str());
                    }
                }

                // Precise sleep-until for the next tick (immediate send for k=0)
                const auto next_deadline = base + std::chrono::milliseconds((k + 1) * CAL_PERIOD_MS);
                std::this_thread::sleep_until(next_deadline);
            }
        }).detach();
    }
}

static void on_ready_resp(const std::shared_ptr<vsomeip::message>& resp) {
    const auto inst = resp->get_instance();
    int slot_idx = slot_from(resp->get_service(), inst);
    if (resp->get_return_code() == vsomeip::return_code_e::E_OK && slot_idx >= 0) {
        slots[slot_idx].ready_acked.store(true);
        {
            std::ostringstream os;
            os << "[ECU] READY acked by radar on inst=0x" << std::hex << inst << std::dec
               << " (slot " << slot_idx << ")";
            LOG(os.str());
        }

        // >>> start run AFTER READY (not on first HB/OBJ/DET RX)
        start_run_timer_after_ready();

        start_cal_broadcast_if_needed();
    }
}


static void on_availability(vsomeip::service_t svc, vsomeip::instance_t inst, bool available) {
    if (!available) return;
    const int slot_idx = slot_from(svc, inst);
    if (slot_idx < 0) return;

    if (svc == HEARTBEAT_SERVICE_ID) {
        subscribe_stream(svc, inst, HEARTBEAT_EVENTGROUP_ID, HEARTBEAT_EVENT_ID);
        slots[slot_idx].hb.store(true);
        { std::ostringstream os; os << "[ECU][HB] Service [" << std::hex << HEARTBEAT_SERVICE_ID
                                    << "." << inst << std::dec << "] is available."; LOG(os.str()); }
    } else if (svc == OBJECTS_SERVICE_ID) {
        subscribe_stream(svc, inst, OBJECTS_EVENTGROUP_ID, OBJECTS_EVENT_ID);
        slots[slot_idx].obj.store(true);
        { std::ostringstream os; os << "[ECU][OBJ] Service [" << std::hex << OBJECTS_SERVICE_ID
                                    << "." << inst << std::dec << "] is available."; LOG(os.str()); }
    } else if (svc == DETECTIONS_SERVICE_ID) {
        subscribe_stream(svc, inst, DETECTIONS_EVENTGROUP_ID, DETECTIONS_EVENT_ID);
        slots[slot_idx].det.store(true);
        { std::ostringstream os; os << "[ECU][DET] Service [" << std::hex << DETECTIONS_SERVICE_ID
                                    << "." << inst << std::dec << "] is available."; LOG(os.str()); }
    }
    try_send_ready_for_slot(slot_idx);
}

// -------- Handlers (HB/OBJ/DET) --------
static void handle_hb(const std::shared_ptr<vsomeip::message>& m) {
    const auto pl = m->get_payload();
    const auto len = pl ? pl->get_length() : 0U;
    const auto ts_rx = now_ms64();
    const auto wiso = wall_iso_now();
    const int  slot = slot_from(m->get_service(), m->get_instance());
    const std::string rid = (slot >= 0) ? std::to_string(slot+1) : std::string("UNKNOWN");

    if (len != sizeof(Heartbeat)) {
        std::lock_guard<std::mutex> lk(g_mx);
        AHB[rid].decode_err++;
        push_hb_sample(rid, 0, 0, ts_rx, 0, 0, "DECODE_ERR", wiso);
        LOG("[ADAS][RX][HB][" + rid + "] ❌ decode error");
        return;
    }

    // ==== METRICS (UDP) ====
    RX_PAYLOAD_HB += sizeof(Heartbeat);
    RX_WIRE_HB    += wire_udp(sizeof(Heartbeat));
    CNT_RX_HB.fetch_add(1, std::memory_order_relaxed);

    const auto *hb = reinterpret_cast<const Heartbeat*>(pl->get_data());
    const uint64_t e2e = ts_rx - static_cast<uint64_t>(hb->ts_ms);

    std::lock_guard<std::mutex> lk(g_mx);
    auto &st = hb_states[rid];
    if (!st.primed) {
        st.primed = true; st.last_seq = hb->seq; st.last_rx_ms = ts_rx;
        push_hb_sample(rid, hb->seq, hb->ts_ms, ts_rx, e2e, 0, "PRIMED", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][HB][" << rid << "] 🔄 primed seq=" << hb->seq
           << " ts_src=" << hb->ts_ms << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms | health=" << health_str(hb->health);
        LOG(os.str());
        return;
    }

    const uint64_t delta  = ts_rx - st.last_rx_ms;
    const uint64_t jitter = (delta > HB_PERIOD_MS) ? (delta - HB_PERIOD_MS) : (HB_PERIOD_MS - delta);
    st.last_seq = hb->seq; st.last_rx_ms = ts_rx;

    auto &ag = AHB[rid];
    ag.total++;
    ag.max_e2e    = std::max<uint64_t>(ag.max_e2e, e2e);
    ag.max_jitter = std::max<uint64_t>(ag.max_jitter, jitter);

    if (e2e > HB_E2E_MAX_MS || jitter > HB_JITTER_MAX_MS) {
        ag.viol++; ag.faults++; HB_FAULTS[rid]++;
        push_hb_sample(rid, hb->seq, hb->ts_ms, ts_rx, e2e, jitter, "VIOL", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][HB][" << rid << "] ❌ E2E:" << e2e << "ms | jitter:" << jitter
           << "ms (seq=" << hb->seq << ") | health=" << health_str(hb->health)
           << " | fault_count=" << HB_FAULTS[rid];
        LOG(os.str());
    } else {
        ag.ok++;
        push_hb_sample(rid, hb->seq, hb->ts_ms, ts_rx, e2e, jitter, "OK", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][HB][" << rid << "] ✅ seq=" << hb->seq
           << " ts_src=" << hb->ts_ms << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms | jitter=" << jitter
           << "ms | health=" << health_str(hb->health);
        LOG(os.str());
    }
}

static void handle_obj(const std::shared_ptr<vsomeip::message>& m) {
    const auto pl = m->get_payload();
    const auto len = pl ? pl->get_length() : 0U;
    const auto ts_rx = now_ms64();
    const auto wiso = wall_iso_now();
    const int  slot = slot_from(m->get_service(), m->get_instance());
    const std::string rid = (slot >= 0) ? std::to_string(slot+1) : std::string("UNKNOWN");

    if (len != OBJ_PAYLOAD_BYTES) {
        std::lock_guard<std::mutex> lk(g_mx);
        auto &st = obj_states[rid];
        st.loss_streak++;
        AOBJ[rid].decode_err++;
        push_obj_sample(rid, 0, 0, ts_rx, 0, 0, "DECODE_ERR", wiso);
        if (st.loss_streak >= OBJ_CONSEC_FAULT) {
            AOBJ[rid].faults++; OBJ_FAULTS[rid]++; st.loss_streak = 0;
            push_fault("OBJ", rid, -1, "decode x3", -1, -1, wiso);
            LOG("[ADAS][RX][OBJ][" + rid + "] ❌ FAULT (decode x3)");
        } else {
            LOG("[ADAS][RX][OBJ][" + rid + "] ⚠️ decode error");
        }
        return;
    }

    // ==== METRICS (TCP) ====
    RX_PAYLOAD_OBJ += OBJ_PAYLOAD_BYTES;
    RX_WIRE_OBJ    += wire_tcp(OBJ_PAYLOAD_BYTES);
    CNT_RX_OBJ.fetch_add(1, std::memory_order_relaxed);

    const auto *arr = reinterpret_cast<const RadarObject*>(pl->get_data());
    const uint32_t ts_src = arr[0].ts_ms;
    const uint32_t seq    = arr[0].seq;
    const uint64_t e2e = ts_rx - static_cast<uint64_t>(ts_src);

    std::lock_guard<std::mutex> lk(g_mx);
    auto &st = obj_states[rid];
    if (!st.primed) {
        st.primed = true; st.last_seq = seq; st.last_rx_ms = ts_rx; st.loss_streak = 0;
        push_obj_sample(rid, seq, ts_src, ts_rx, e2e, 0, "PRIMED", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][OBJ][" << rid << "] 🔄 primed seq=" << seq
           << " ts_src=" << ts_src << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms";
        LOG(os.str());
        return;
    }

    const uint64_t delta  = ts_rx - st.last_rx_ms;
    const uint64_t jitter = (delta > OBJ_PERIOD_MS) ? (delta - OBJ_PERIOD_MS) : (OBJ_PERIOD_MS - delta);
    st.last_seq = seq; st.last_rx_ms = ts_rx;

    auto &ag = AOBJ[rid];
    ag.total++;
    ag.max_e2e    = std::max<uint64_t>(ag.max_e2e, e2e);
    ag.max_jitter = std::max<uint64_t>(ag.max_jitter, jitter);

    if (e2e > OBJ_E2E_MAX_MS || jitter > OBJ_JITTER_MAX_MS) {
        st.loss_streak++; ag.viol++;
        push_obj_sample(rid, seq, ts_src, ts_rx, e2e, jitter, "VIOL", wiso);
        {
            std::ostringstream os;
            os << "[ADAS][RX][OBJ][" << rid << "] ⚠️ E2E:" << e2e << "ms | jitter:" << jitter
               << "ms (seq=" << seq << ", streak=" << st.loss_streak << ")";
            LOG(os.str());
        }
        if (st.loss_streak >= OBJ_CONSEC_FAULT) {
            ag.faults++; OBJ_FAULTS[rid]++; st.loss_streak = 0;
            push_fault("OBJ", rid, (int32_t)seq, "3 consecutive violations", (int64_t)e2e, (int64_t)jitter, wiso);
            std::ostringstream os;
            os << "[ADAS][RX][OBJ][" << rid << "] ❌ FAULT (3 consecutive violations) | fault_count=" << OBJ_FAULTS[rid];
            LOG(os.str());
        }
    } else {
        st.loss_streak = 0; ag.ok++;
        push_obj_sample(rid, seq, ts_src, ts_rx, e2e, jitter, "OK", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][OBJ][" << rid << "] ✅ seq=" << seq
           << " ts_src=" << ts_src << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms | jitter=" << jitter << "ms";
        LOG(os.str());
    }
}

static void handle_det(const std::shared_ptr<vsomeip::message>& m) {

    const auto pl = m->get_payload();
    const auto len = pl ? pl->get_length() : 0U;
    const auto ts_rx = now_ms64();
    const auto wiso = wall_iso_now();
    const int  slot = slot_from(m->get_service(), m->get_instance());
    const std::string rid = (slot >= 0) ? std::to_string(slot+1) : std::string("UNKNOWN");

    if (len != DET_PAYLOAD_BYTES) {
        std::lock_guard<std::mutex> lk(g_mx);
        auto &st = det_states[rid];
        st.loss_streak++;
        ADET[rid].decode_err++;
        push_det_sample(rid, 0, 0, ts_rx, 0, 0, "DECODE_ERR", wiso);
        if (st.loss_streak >= DET_CONSEC_FAULT) {
            ADET[rid].faults++; DET_FAULTS[rid]++; st.loss_streak = 0;
            push_fault("DET", rid, -1, "decode x3", -1, -1, wiso);
            LOG("[ADAS][RX][DET][" + rid + "] ❌ FAULT (decode x3)");
        } else {
            LOG("[ADAS][RX][DET][" + rid + "] ⚠️ decode error");
        }
        return;
    }

    // ==== METRICS (TCP) ====
    RX_PAYLOAD_DET += DET_PAYLOAD_BYTES;
    RX_WIRE_DET    += wire_tcp(DET_PAYLOAD_BYTES);
    CNT_RX_DET.fetch_add(1, std::memory_order_relaxed);

    const auto *arr = reinterpret_cast<const Detection*>(pl->get_data());
    const uint32_t ts_src = arr[0].ts_ms;
    const uint32_t seq    = arr[0].seq;
    const uint64_t e2e = ts_rx - static_cast<uint64_t>(ts_src);

    std::lock_guard<std::mutex> lk(g_mx);
    auto &st = det_states[rid];
    if (!st.primed) {
        st.primed = true; st.last_seq = seq; st.last_rx_ms = ts_rx; st.loss_streak = 0;
        push_det_sample(rid, seq, ts_src, ts_rx, e2e, 0, "PRIMED", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][DET][" << rid << "] 🔄 primed seq=" << seq
           << " ts_src=" << ts_src << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms";
        LOG(os.str());
        return;
    }

    const uint64_t delta  = ts_rx - st.last_rx_ms;
    const uint64_t jitter = (delta > DET_PERIOD_MS) ? (delta - DET_PERIOD_MS) : (DET_PERIOD_MS - delta);
    st.last_seq = seq; st.last_rx_ms = ts_rx;

    auto &ag = ADET[rid];
    ag.total++;
    ag.max_e2e    = std::max<uint64_t>(ag.max_e2e, e2e);
    ag.max_jitter = std::max<uint64_t>(ag.max_jitter, jitter);

    const bool violated = (e2e > DET_E2E_MAX_MS) || (jitter > DET_JITTER_MAX_MS);
    if (violated) {
        st.loss_streak++; ag.viol++;
        push_det_sample(rid, seq, ts_src, ts_rx, e2e, jitter, "VIOL", wiso);
        {
            std::ostringstream os;
            os << "[ADAS][RX][DET][" << rid << "] ⚠️ E2E:" << e2e << "ms | jitter:" << jitter
               << "ms (seq=" << seq << ", streak=" << st.loss_streak << ")";
            LOG(os.str());
        }
        if (st.loss_streak >= DET_CONSEC_FAULT) {
            ag.faults++; DET_FAULTS[rid]++; st.loss_streak = 0;
            push_fault("DET", rid, (int32_t)seq, "3 consecutive violations", (int64_t)e2e, (int64_t)jitter, wiso);
            std::ostringstream os;
            os << "[ADAS][RX][DET][" << rid << "] ❌ FAULT (3 consecutive violations) | fault_count=" << DET_FAULTS[rid];
            LOG(os.str());
        }
    } else {
        st.loss_streak = 0; ag.ok++;
        push_det_sample(rid, seq, ts_src, ts_rx, e2e, jitter, "OK", wiso);
        std::ostringstream os;
        os << "[ADAS][RX][DET][" << rid << "] ✅ seq=" << seq
           << " ts_src=" << ts_src << " ts_rx=" << ts_rx
           << " e2e=" << e2e << "ms | jitter=" << jitter << "ms";
        LOG(os.str());
    }
}

// -------- CALIB method handlers --------
static void handle_radar_ack(const std::shared_ptr<vsomeip::message> &req){
    const auto inst = req->get_instance();
    const int slot = (int)inst - (int)CALIB_INSTANCE_BASE;
    const auto pl = req->get_payload();

    if (!pl || pl->get_length()!=16 || slot<0 || slot>=6) {
        auto resp = vsomeip::runtime::get()->create_response(req);
        resp->set_return_code(vsomeip::return_code_e::E_NOT_OK);
        app->send(resp);
        return;
    }

    // ==== METRICS (TCP RX) ====
    RX_PAYLOAD_CAL += 16;
    RX_WIRE_CAL    += wire_tcp(16);
    CNT_RX_CAL_ACK.fetch_add(1, std::memory_order_relaxed);

    const U32x4 ack = unpack_be(pl->get_data(), pl->get_length());
    const uint32_t req_id = ack.d1;
    const uint32_t radar_rx_ts = ack.d2;

    {
        std::lock_guard<std::mutex> lk(CAL_MX);
        auto it = CAL_INFLIGHT.find(cal_key(slot, req_id));
        if (it != CAL_INFLIGHT.end()) it->second.radar_ack_rx_ms = radar_rx_ts;
    }

    auto resp = vsomeip::runtime::get()->create_response(req);
    resp->set_return_code(vsomeip::return_code_e::E_OK);
    app->send(resp);
    CNT_TX_ECU_ACK_TO_ACK.fetch_add(1, std::memory_order_relaxed); // empty ECU reply
}

static void handle_radar_resp(const std::shared_ptr<vsomeip::message> &req){
    const auto inst = req->get_instance();
    const int slot = (int)inst - (int)CALIB_INSTANCE_BASE;
    const auto pl = req->get_payload();

    if (!pl || pl->get_length()!=16 || slot<0 || slot>=6) {
        auto resp = vsomeip::runtime::get()->create_response(req);
        resp->set_return_code(vsomeip::return_code_e::E_NOT_OK);
        app->send(resp);
        return;
    }

    // ==== METRICS (TCP RX) ====
    RX_PAYLOAD_CAL += 16;
    RX_WIRE_CAL    += wire_tcp(16);
    CNT_RX_CAL_RESP.fetch_add(1, std::memory_order_relaxed);

    const U32x4 r = unpack_be(pl->get_data(), pl->get_length());
    const uint32_t req_id = r.d1;
    const uint32_t radar_resp_ts = r.d2;
    const uint32_t randC = r.d3, randD = r.d4;

    uint32_t ecu_rx_ts = (uint32_t)now_ms64();

    // Reply payload = ECU-ACK: {req_id, ecu_rx_ts, echo C, echo D}
    U32x4 ack{ req_id, ecu_rx_ts, randC, randD };
    auto resp_pl = vsomeip::runtime::get()->create_payload();
    auto bytes = pack_be(ack);
    resp_pl->set_data(bytes.data(), (uint32_t)bytes.size());

    {
        std::lock_guard<std::mutex> lk(CAL_MX);
        auto it = CAL_INFLIGHT.find(cal_key(slot, req_id));
        if (it != CAL_INFLIGHT.end()) {
            it->second.radar_resp_ms = radar_resp_ts;
            it->second.resp_C = randC; it->second.resp_D = randD;

            CalTxnRow row{};
            row.radar = std::to_string(slot+1);
            row.req_id = req_id;
            row.ecu_send_ms     = it->second.ecu_send_ms;
            row.radar_ack_rx_ms = it->second.radar_ack_rx_ms;
            row.radar_resp_ms   = it->second.radar_resp_ms;
            row.ecu_ack_ms      = ecu_rx_ts;
            row.req_d1 = it->second.req_d1;
            row.req_d2 = it->second.req_d2;
            row.req_d3 = it->second.req_d3;
            row.req_d4 = it->second.req_d4;
            row.resp_C = randC; row.resp_D = randD;
            CAL_ROWS.push_back(row);

            CAL_INFLIGHT.erase(it);
        }
    }

    auto resp = vsomeip::runtime::get()->create_response(req);
    resp->set_payload(resp_pl);
    resp->set_return_code(vsomeip::return_code_e::E_OK);
    app->send(resp);
    CNT_TX_ECU_ACK_TO_RESP.fetch_add(1, std::memory_order_relaxed);

    // ==== METRICS (TCP TX: ECU response with 16B payload) ====
    TX_PAYLOAD_CAL += 16;
    TX_WIRE_CAL    += wire_tcp(16);
}


// -------- Excel writer (.xlsx with HB/OBJ/DET + Calibration Txns + Info) --------
[[maybe_unused]]static int radar_key(const std::string &rid) {
    bool all_digits = !rid.empty() && std::all_of(rid.begin(), rid.end(), ::isdigit);
    if (!all_digits) return 1000000000; // UNKNOWN -> last
    return std::stoi(rid);
}

static bool write_excel_xlsx(const std::string &xlsx_path,
                             const std::unordered_map<std::string, StreamAgg> &AHB_,
                             const std::unordered_map<std::string, StreamAgg> &AOBJ_,
                             const std::unordered_map<std::string, StreamAgg> &ADET_,
                             const std::vector<SampleRow> &HB_,
                             const std::vector<SampleRow> &OBJ_,
                             const std::vector<SampleRow> &DET_,
                             const std::vector<CalTxnRow>  &CAL_,
                             const std::string &start_wall,
                             const std::string &end_wall) {
    lxw_workbook *wb = workbook_new(xlsx_path.c_str());
    if (!wb) return false;

    // formats
    lxw_format *fmt_hdr = workbook_add_format(wb); format_set_bold(fmt_hdr);
    lxw_format *fmt_pct = workbook_add_format(wb); format_set_num_format(fmt_pct, "0.0%");
    lxw_format *fmt_int = workbook_add_format(wb); format_set_num_format(fmt_int, "0");

    // SUMMARY
    lxw_worksheet *ws = workbook_add_worksheet(wb, "Summary");
    const char* H[] = {"Radar","Stream","Total","OK","Violations","DecodeErr","Faults","MaxE2E(ms)","MaxJitter(ms)","OK%"};
    for (int c=0;c<10;++c) worksheet_write_string(ws, 0, c, H[c], fmt_hdr);

    // derive radar list (union)
    std::vector<std::string> rids;
    rids.reserve(AHB_.size()+AOBJ_.size()+ADET_.size());
    auto add_rids = [&](const auto &M){ for (const auto &kv : M) rids.push_back(kv.first); };
    add_rids(AHB_); add_rids(AOBJ_); add_rids(ADET_);
    std::sort(rids.begin(), rids.end(), [](const std::string&a,const std::string&b){
        auto isdigits = [](const std::string&s){ return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); };
        int ka = isdigits(a) ? std::stoi(a) : 1000000000;
        int kb = isdigits(b) ? std::stoi(b) : 1000000000;
        return ka<kb || (ka==kb && a<b);
    });
    rids.erase(std::unique(rids.begin(), rids.end()), rids.end());

    auto write_stream = [&](const char* stream, const auto &M, int &row){
        for (const auto &rid : rids) {
            auto it = M.find(rid);
            StreamAgg a{};
            if (it != M.end()) a = it->second;
            double ok_pct = (a.total ? (double)a.ok / (double)a.total : 0.0);
            std::string radar_name = "Radar_" + rid;

            worksheet_write_string(ws, row, 0, radar_name.c_str(), nullptr);
            worksheet_write_string(ws, row, 1, stream, nullptr);
            worksheet_write_number(ws, row, 2, (double)a.total, fmt_int);
            worksheet_write_number(ws, row, 3, (double)a.ok, fmt_int);
            worksheet_write_number(ws, row, 4, (double)a.viol, fmt_int);
            worksheet_write_number(ws, row, 5, (double)a.decode_err, fmt_int);
            worksheet_write_number(ws, row, 6, (double)a.faults, fmt_int);
            worksheet_write_number(ws, row, 7, (double)a.max_e2e, fmt_int);
            worksheet_write_number(ws, row, 8, (double)a.max_jitter, fmt_int);
            worksheet_write_number(ws, row, 9, ok_pct, fmt_pct);
            ++row;
        }
    };

    int r = 1;
    write_stream("HB",  AHB_,  r);
    write_stream("OBJ", AOBJ_, r);
    write_stream("DET", ADET_, r);
    worksheet_freeze_panes(ws, 1, 0);

    // helper to write sample sheets
    auto write_samples = [&](const char* tab, const std::vector<SampleRow> &rows){
        lxw_worksheet *w = workbook_add_worksheet(wb, tab);
        const char* HH[] = {"Wall Time","Radar","Seq","ts_src(ms:monotonic)","ts_rx(ms:monotonic)","E2E(ms)","Jitter(ms)","Status"};
        for (int c=0;c<8;++c) worksheet_write_string(w, 0, c, HH[c], fmt_hdr);
        int rr = 1;
        for (const auto &row : rows) {
            std::string radar_name = "Radar_" + row.radar;
            worksheet_write_string(w, rr, 0, row.wall_iso.c_str(), nullptr);
            worksheet_write_string(w, rr, 1, radar_name.c_str(), nullptr);
            worksheet_write_number(w, rr, 2, (double)row.seq, fmt_int);
            worksheet_write_number(w, rr, 3, (double)row.ts_src_ms, fmt_int);
            worksheet_write_number(w, rr, 4, (double)row.ts_rx_ms, fmt_int);
            worksheet_write_number(w, rr, 5, (double)row.e2e_ms, fmt_int);
            worksheet_write_number(w, rr, 6, (double)row.jitter_ms, fmt_int);
            worksheet_write_string(w, rr, 7, row.status.c_str(), nullptr);
            ++rr;
        }
        worksheet_freeze_panes(w, 1, 0);
    };

    write_samples("HB Samples",  HB_);
    write_samples("OBJ Samples", OBJ_);
    write_samples("DET Samples", DET_);

    // ---- Calibration Txns sheet ----
    {
        lxw_worksheet *w = workbook_add_worksheet(wb, "Calibration Txns");
        const char* HC[] = {
            "Radar","ReqID","ECU_send_ms","Radar_ACK_rx_ms","Radar_RESP_ms","ECU_ACK_ms",
            "Req_d1","Req_d2","Req_d3","Req_d4","Resp_C","Resp_D",
            "L1_ECU->ACK(ms)","L2_ACK->RESP(ms)","L3_RESP->ECU(ms)"
        };
        for(int c=0;c<15;++c) worksheet_write_string(w, 0, c, HC[c], fmt_hdr);

        int rr = 1;
        for (const auto &row : CAL_) {
            std::string radar_name = "Radar_" + row.radar;
            worksheet_write_string(w, rr, 0, radar_name.c_str(), nullptr);
            worksheet_write_number(w, rr, 1, (double)row.req_id, fmt_int);
            worksheet_write_number(w, rr, 2, (double)row.ecu_send_ms, fmt_int);
            worksheet_write_number(w, rr, 3, (double)row.radar_ack_rx_ms, fmt_int);
            worksheet_write_number(w, rr, 4, (double)row.radar_resp_ms, fmt_int);
            worksheet_write_number(w, rr, 5, (double)row.ecu_ack_ms, fmt_int);

            worksheet_write_number(w, rr, 6, (double)row.req_d1, fmt_int);
            worksheet_write_number(w, rr, 7, (double)row.req_d2, fmt_int);
            worksheet_write_number(w, rr, 8, (double)row.req_d3, fmt_int);
            worksheet_write_number(w, rr, 9, (double)row.req_d4, fmt_int);
            worksheet_write_number(w, rr,10, (double)row.resp_C, fmt_int);
            worksheet_write_number(w, rr,11, (double)row.resp_D, fmt_int);

            const int32_t L1 = (int32_t)(row.radar_ack_rx_ms - row.ecu_send_ms);
            const int32_t L2 = (int32_t)(row.radar_resp_ms   - row.radar_ack_rx_ms);
            const int32_t L3 = (int32_t)(row.ecu_ack_ms      - row.radar_resp_ms);
            worksheet_write_number(w, rr,12, (double)L1, fmt_int);
            worksheet_write_number(w, rr,13, (double)L2, fmt_int);
            worksheet_write_number(w, rr,14, (double)L3, fmt_int);
            ++rr;
        }
        worksheet_freeze_panes(w, 1, 0);
    }

    // INFO
    lxw_worksheet *wi = workbook_add_worksheet(wb, "Info");
    worksheet_write_string(wi, 0, 0, "Key", fmt_hdr);
    worksheet_write_string(wi, 0, 1, "Value", fmt_hdr);

    int ir = 1;
    auto kv = [&](const char* k, const std::string &v){ worksheet_write_string(wi, ir, 0, k, nullptr); worksheet_write_string(wi, ir, 1, v.c_str(), nullptr); ++ir; };
    auto ki = [&](const char* k, uint32_t v){ worksheet_write_string(wi, ir, 0, k, nullptr); worksheet_write_number(wi, ir, 1, (double)v, fmt_int); ++ir; };
    kv("Run Duration (s)", std::to_string(RUN_FOR_S));
    kv("Start (wall)", start_wall.empty() ? "-" : start_wall);
    kv("End (wall)",   end_wall.empty()   ? "-" : end_wall);
    kv("Notes", "ts_src/ts_rx are MONOTONIC ms (used for E2E/jitter)");

    ki("HB period (ms)", HB_PERIOD_MS);
    ki("HB E2E<= (ms)",  HB_E2E_MAX_MS);
    ki("HB jitter<= (ms)", HB_JITTER_MAX_MS);

    ki("OBJ period (ms)", OBJ_PERIOD_MS);
    ki("OBJ E2E<= (ms)",  OBJ_E2E_MAX_MS);
    ki("OBJ jitter<= (ms)", OBJ_JITTER_MAX_MS);
    ki("OBJ consec for fault", OBJ_CONSEC_FAULT);

    ki("DET period (ms)", DET_PERIOD_MS);
    ki("DET E2E<= (ms)",  DET_E2E_MAX_MS);
    ki("DET jitter<= (ms)", DET_JITTER_MAX_MS);
    ki("DET consec for fault", DET_CONSEC_FAULT);

    ki("CAL period (ms)", CAL_PERIOD_MS);

    workbook_close(wb);
    return true;
}


// -------- main --------
int main(int argc, char* argv[]) {
    // CLI: only --run-for
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a == "--run-for" && i+1 < argc) RUN_FOR_S = std::max(1, atoi(argv[++i]));
    }

    // Shared parent folder, then /subscriber
    const std::string parent = get_or_create_shared_parent_run_dir();
    try { std::filesystem::create_directories(parent + "/subscriber"); } catch(...) {}
    g_parent_dir = parent;
    g_run_dir    = parent + "/subscriber";
    g_stamp      = std::filesystem::path(parent).filename().string();

    auto rt = vsomeip::runtime::get();
    app = rt->create_application("adas_subscriber");
    if (!app->init()) { std::cerr << "Failed to init application\n"; return 2; }

    // log inside subscriber subfolder
    g_log.reset(new AsyncFileLogger(g_run_dir + "/adas_subscriber"));

    // --- Offer CALIB service (server) on all slots at startup + event ---
    for (int slot = 0; slot < 6; ++slot) {
        const vsomeip::instance_t cinst = static_cast<vsomeip::instance_t>(CALIB_INSTANCE_BASE + slot);
        app->offer_service(CALIB_SERVICE_ID, cinst);
        std::set<vsomeip::eventgroup_t> eg{ CALIB_EVENTGROUP_ID };
        app->offer_event(CALIB_SERVICE_ID, cinst, CALIB_EVENT_ID, eg,
                         vsomeip_v3::event_type_e::ET_EVENT,
                         std::chrono::milliseconds::zero(),
                         false, true, nullptr,
                         vsomeip_v3::reliability_type_e::RT_RELIABLE);
    }

    // NOTE: we NO LONGER start the CAL broadcaster here.
    // It will start after the first READY ack arrives (see on_ready_resp).

    // ANY_INSTANCE and handlers (HB/OBJ/DET)static void handle_radar_ack
    app->request_service(HEARTBEAT_SERVICE_ID, vsomeip::ANY_INSTANCE);
    app->request_service(OBJECTS_SERVICE_ID,   vsomeip::ANY_INSTANCE);
    app->request_service(DETECTIONS_SERVICE_ID,vsomeip::ANY_INSTANCE);

    app->register_availability_handler(HEARTBEAT_SERVICE_ID, vsomeip::ANY_INSTANCE, on_availability);
    app->register_availability_handler(OBJECTS_SERVICE_ID,   vsomeip::ANY_INSTANCE, on_availability);
    app->register_availability_handler(DETECTIONS_SERVICE_ID,vsomeip::ANY_INSTANCE, on_availability);

    app->register_message_handler(HEARTBEAT_SERVICE_ID, vsomeip::ANY_INSTANCE, READY_METHOD_ID, on_ready_resp);
    app->register_message_handler(HEARTBEAT_SERVICE_ID, vsomeip::ANY_INSTANCE, HEARTBEAT_EVENT_ID, handle_hb);
    app->register_message_handler(OBJECTS_SERVICE_ID,   vsomeip::ANY_INSTANCE, OBJECTS_EVENT_ID,   handle_obj);
    app->register_message_handler(DETECTIONS_SERVICE_ID,vsomeip::ANY_INSTANCE, DETECTIONS_EVENT_ID, handle_det);

    // CALIB method handlers (ECU server)
    app->register_message_handler(CALIB_SERVICE_ID, vsomeip::ANY_INSTANCE, RADAR_ACK_METHOD_ID,  handle_radar_ack);
    app->register_message_handler(CALIB_SERVICE_ID, vsomeip::ANY_INSTANCE, RADAR_RESP_METHOD_ID, handle_radar_resp);

    app->start(); // stops after RUN_FOR_S (on first RX)

    g_stop.store(true); // stop broadcaster thread after start() returns

    {
        std::lock_guard<std::mutex> lk(g_sw_mx);
        g_end_wall = wall_iso_now();
    }

    // Compact KPI summary
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto printOne = [](const std::string& stream, const std::unordered_map<std::string, StreamAgg>& A) {
            for (const auto& kv : A) {
                const auto& rid = kv.first; const auto& a = kv.second;
                double okpct = a.total ? (100.0 * double(a.ok) / double(a.total)) : 0.0;
                std::cout << "  Radar_" << rid << " [" << stream << "]: "
                          << "Total=" << a.total << " OK=" << a.ok << " Viol=" << a.viol
                          << " DecodeErr=" << a.decode_err << " Faults=" << a.faults
                          << " MaxE2E(ms)=" << a.max_e2e << " MaxJitter(ms)=" << a.max_jitter
                          << " OK%=" << std::fixed << std::setprecision(3) << okpct << "\n";
            }
        };
        std::cout << "\n[ADAS] ===== SUMMARY (" << RUN_FOR_S << "s, STOP-free) =====\n";
        printOne("HB",  AHB);
        printOne("OBJ", AOBJ);
        printOne("DET", ADET);
    }

    // ===== BYTE SUMMARY =====
    {
        uint64_t rx_pl_hb  = RX_PAYLOAD_HB.load();
        uint64_t rx_pl_obj = RX_PAYLOAD_OBJ.load();
        uint64_t rx_pl_det = RX_PAYLOAD_DET.load();
        uint64_t rx_pl_cal = RX_PAYLOAD_CAL.load();
        uint64_t rx_pl_tot = rx_pl_hb + rx_pl_obj + rx_pl_det + rx_pl_cal;

        uint64_t rx_wi_hb  = RX_WIRE_HB.load();
        uint64_t rx_wi_obj = RX_WIRE_OBJ.load();
        uint64_t rx_wi_det = RX_WIRE_DET.load();
        uint64_t rx_wi_cal = RX_WIRE_CAL.load();
        uint64_t rx_wi_tot = rx_wi_hb + rx_wi_obj + rx_wi_det + rx_wi_cal;

        uint64_t tx_pl_cal = TX_PAYLOAD_CAL.load();
        uint64_t tx_wi_cal = TX_WIRE_CAL.load();

        double secs = std::max(1, RUN_FOR_S);
        double rx_mbit_s = (rx_wi_tot * 8.0) / (secs * 1e6);
        double tx_mbit_s = (tx_wi_cal * 8.0) / (secs * 1e6);

        auto line = [](const char* n, uint64_t pl, uint64_t wi){
            std::ostringstream os;
            os << "  " << n << ": payload=" << pl << " B, wire≈" << wi << " B";
            return os.str();
        };

        std::ostringstream sum;
        sum << "[ADAS][METRICS] ===== BYTE SUMMARY (approx, IPv4; SOME/IP hdr=16B) =====\n"
            << "RX:\n"
            << line("HB (UDP)",  rx_pl_hb,  rx_wi_hb)  << "\n"
            << line("OBJ(TCP)",  rx_pl_obj, rx_wi_obj) << "\n"
            << line("DET(TCP)",  rx_pl_det, rx_wi_det) << "\n"
            << line("CAL(TCP)",  rx_pl_cal, rx_wi_cal) << "\n"
            << "  RX TOTAL: payload=" << rx_pl_tot << " B, wire≈" << rx_wi_tot << " B\n"
            << "  RX avg bitrate≈ " << std::fixed << std::setprecision(3) << rx_mbit_s << " Mbit/s\n"
            << "TX (ECU→radar, CAL responses):\n"
            << line("CAL(TCP)",  tx_pl_cal, tx_wi_cal) << "\n"
            << "  TX avg bitrate≈ " << std::fixed << std::setprecision(3) << tx_mbit_s << " Mbit/s\n";

        std::cout << "\n" << sum.str();
        LOG(sum.str());
    }

    // ===== NEW: MESSAGE COUNT SUMMARY =====
    {
        const uint64_t n_rx_hb   = CNT_RX_HB.load();
        const uint64_t n_rx_obj  = CNT_RX_OBJ.load();
        const uint64_t n_rx_det  = CNT_RX_DET.load();
        const uint64_t n_rx_ack  = CNT_RX_CAL_ACK.load();
        const uint64_t n_rx_resp = CNT_RX_CAL_RESP.load();

        const uint64_t n_tx_ecu_ack    = CNT_TX_ECU_ACK_TO_ACK.load();
        const uint64_t n_tx_ecu_ack16  = CNT_TX_ECU_ACK_TO_RESP.load();

        const double secs = std::max(1, RUN_FOR_S);
        auto r = [&](uint64_t n){ return n / secs; };

        std::ostringstream os;
        os << "[ADAS][COUNTS] ===== MESSAGE COUNT SUMMARY =====\n"
           << "RX: HB=" << n_rx_hb   << " (" << std::fixed << std::setprecision(2) << r(n_rx_hb)   << "/s)"
           << "  OBJ=" << n_rx_obj  << " (" << r(n_rx_obj)  << "/s)"
           << "  DET=" << n_rx_det  << " (" << r(n_rx_det)  << "/s)\n"
           << "RX CAL: ACK=" << n_rx_ack << " (" << r(n_rx_ack) << "/s)"
           << "  RESP=" << n_rx_resp << " (" << r(n_rx_resp) << "/s)\n"
           << "TX CAL replies (ECU): to-ACK(empty)=" << n_tx_ecu_ack
           << "  to-RESP(16B)=" << n_tx_ecu_ack16 << "\n";

        std::cout << os.str();
        LOG(os.str());
    }

    // Write Excel
    const std::string xlsx  = g_run_dir + "/adas_summary_" + g_stamp + ".xlsx";
    if (write_excel_xlsx(xlsx, AHB, AOBJ, ADET, HB_SAMPLES, OBJ_SAMPLES, DET_SAMPLES, CAL_ROWS, g_start_wall, g_end_wall)) {
        std::cout << "[ADAS] Excel written: " << xlsx << "\n";
    } else {
        std::cout << "[ADAS] Excel writer unavailable — skipping .xlsx\n";
    }

    std::string log_path = g_log ? g_log->path() : "";
    if (g_log) g_log->stop();
    if (!log_path.empty()) std::cout << "[LOG] File saved: " << log_path << "\n";
    std::cout << "[ADAS] Parent run folder: " << g_parent_dir << "\n";
    std::cout << "[ADAS] Subscriber folder: " << g_run_dir << "\n";
    return 0;
}