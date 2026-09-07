#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>        // for localtime_r/localtime_s
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <array>

// ---------------- SOME/IP IDs (base per service/instance) ----------------
#define HEARTBEAT_SERVICE_ID     0x1234
#define HEARTBEAT_INSTANCE_BASE  0x5678
#define HEARTBEAT_EVENTGROUP_ID  0x4465
#define HEARTBEAT_EVENT_ID       0x8778

#define OBJECTS_SERVICE_ID       0x2345
#define OBJECTS_INSTANCE_BASE    0x6789
#define OBJECTS_EVENTGROUP_ID    0x5566
#define OBJECTS_EVENT_ID         0x9889

#define DETECTIONS_SERVICE_ID    0x3456
#define DETECTIONS_INSTANCE_BASE 0x7890
#define DETECTIONS_EVENTGROUP_ID 0x6677
#define DETECTIONS_EVENT_ID      0xAABB

// Handshake method (on HB service)
#define READY_METHOD_ID          0x7010

// ---------------- Calibration (ECU service, radar client) ----------------
#define CALIB_SERVICE_ID         0x4567
#define CALIB_INSTANCE_BASE      0x8A00
#define CALIB_EVENTGROUP_ID      0xCA1B
#define CALIB_EVENT_ID           0xCA01   // ECU -> radars (1 Hz, 4×u32 BE)
#define RADAR_ACK_METHOD_ID      0xCA02   // radar -> ECU (req_id, radar_rx_ts, echo d3, echo d4)
#define RADAR_RESP_METHOD_ID     0xCA03   // radar -> ECU (req_id, radar_resp_ts, randC, randD)

// ---------------- Wire structs ----------------
#pragma pack(push, 1)
static const uint32_t HEALTH_OK = 0;
static const int MAX_OBJECTS = 20;
static const int MAX_DETECTIONS = 20;

// === Heartbeat is just 3×u32 (12 B) ===
struct Heartbeat {
    uint32_t ts_ms;
    uint32_t seq;
    uint32_t health;
};

// === Element types (must match Zenoh sizes) ===
struct RadarObject {
    uint32_t ts_ms;  // repeat per element (match Zenoh)
    uint32_t seq;    // repeat per element (match Zenoh)
    uint32_t pos_x, pos_y, pos_z;
    uint32_t vel_x, vel_y, vel_z;
    uint32_t acc_x, acc_y, acc_z;
    uint32_t size_x, size_y, size_z;
    uint8_t  type;
}; // 57 B

struct Detection {
    uint32_t ts_ms;  // repeat per element (match Zenoh)
    uint32_t seq;    // repeat per element (match Zenoh)
    uint32_t pos_x, pos_y, pos_z;
    uint32_t vel_x, vel_y, vel_z;
    uint32_t acc_x, acc_y, acc_z;
    uint32_t size_1, size_2, size_3, size_4;
    uint32_t size_5, size_6, size_7, size_8;
    uint8_t  type;
}; // 77 B
#pragma pack(pop)

// Payload sizes for flat arrays
constexpr size_t OBJ_PAYLOAD_BYTES = MAX_OBJECTS * sizeof(RadarObject);   // 1140
constexpr size_t DET_PAYLOAD_BYTES = MAX_DETECTIONS * sizeof(Detection);  // 1540

static_assert(sizeof(Heartbeat)   == 12,  "Heartbeat must be 12 bytes");
static_assert(sizeof(RadarObject) == 57,  "RadarObject must be 57 bytes");
static_assert(sizeof(Detection)   == 77,  "Detection must be 77 bytes");
static_assert(OBJ_PAYLOAD_BYTES   == 1140, "OBJ payload must be 1140 B");
static_assert(DET_PAYLOAD_BYTES   == 1540, "DET payload must be 1540 B");

// ---- Calibration strict 4×u32 (big-endian on the wire) ----
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

// ---------------- helpers ----------------
static inline uint32_t now_ms32() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(ms & 0xFFFFFFFFu);
}

// ---- Metrics (TX) ---------------------------------------------------------
static std::atomic<uint64_t> TX_PAYLOAD_HB{0},  TX_WIRE_HB{0};
static std::atomic<uint64_t> TX_PAYLOAD_OBJ{0}, TX_WIRE_OBJ{0};
static std::atomic<uint64_t> TX_PAYLOAD_DET{0}, TX_WIRE_DET{0};
static std::atomic<uint64_t> TX_PAYLOAD_CAL{0}, TX_WIRE_CAL{0}; // ACK+RESP

// ===== EXACT TX COUNTERS (messages) =====
static std::atomic<uint64_t> CNT_HB_SENT{0};
static std::atomic<uint64_t> CNT_OBJ_SENT{0};
static std::atomic<uint64_t> CNT_DET_SENT{0};
static std::atomic<uint64_t> CNT_CAL_ACK_SENT{0};
static std::atomic<uint64_t> CNT_CAL_RESP_SENT{0};

static inline uint64_t someip_hdr() { return 16; }
static inline uint64_t ip_udp_ovh() { return 20 + 8; }   // IPv4 + UDP
static inline uint64_t ip_tcp_ovh() { return 20 + 20; }  // IPv4 + TCP
static inline uint64_t wire_udp(uint64_t pl) { return pl + someip_hdr() + ip_udp_ovh(); }
static inline uint64_t wire_tcp(uint64_t pl) { return pl + someip_hdr() + ip_tcp_ovh(); }

// ---------------- async file logger (low overhead) ----------------
class AsyncFileLogger {
public:
    explicit AsyncFileLogger(const std::string &prefix)
    : done_(false) {
        const std::string path = prefix + "_" + ts_file() + ".log";
        ofs_.open(path, std::ios::out | std::ios::binary);
        filepath_ = path;
        worker_ = std::thread([this]{ run(); });
    }

    ~AsyncFileLogger() { stop(); }

    void stop() {
        bool expected = false;
        if (done_.compare_exchange_strong(expected, true)) {
            {
                std::lock_guard<std::mutex> lk(mx_);
            }
            cv_.notify_one();
            if (worker_.joinable()) worker_.join();
            if (ofs_.is_open()) { ofs_.flush(); ofs_.close(); }
        }
    }

    void log_line(const std::string &line) {
        if (!ofs_.is_open()) return;
        {
            std::lock_guard<std::mutex> lk(mx_);
            q_.push(line);
        }
        cv_.notify_one();
    }

    const std::string &filepath() const { return filepath_; }

    static std::string ts_file() {
        using clock = std::chrono::system_clock;
        const std::time_t t = clock::to_time_t(clock::now());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return buf;
    }

private:
    void run() {
        std::string batch;
        batch.reserve(64 * 1024);
        while (true) {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(25), [&]{ return done_.load() || !q_.empty(); });
            if (!q_.empty()) {
                while (!q_.empty()) {
                    batch.append(q_.front());
                    batch.push_back('\n');
                    q_.pop();
                    if (batch.size() > (256 * 1024)) break;
                }
            }
            bool is_done = done_.load();
            lk.unlock();

            if (!batch.empty()) {
                ofs_ << batch;
                batch.clear();
            }
            if (is_done && q_.empty()) break;
        }
    }

    std::atomic<bool> done_;
    std::ofstream ofs_;
    std::string filepath_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::queue<std::string> q_;
    std::thread worker_;
};

// ---------------- Globals ----------------
static std::shared_ptr<vsomeip::application> g_app;
static std::shared_ptr<vsomeip::runtime> g_rt;
static std::mutex g_notify_mx;

// per-run directory & session log
static std::string g_run_dir; // will be run/<stamp>/publisher
static std::unique_ptr<AsyncFileLogger> g_session_log;
static inline void SLOG(const std::string &s) { if (g_session_log) g_session_log->log_line(s); }

// READY coordination
static std::mutex g_ready_mx;
static std::condition_variable g_ready_cv;
static int g_slots_total = 1;
static std::atomic<int> g_slots_ready{0};
static std::vector<std::shared_ptr<std::atomic<bool>>> g_slot_is_ready;
static std::unordered_map<vsomeip::instance_t, int> g_inst_to_slot;

// per-slot radar loggers (for use in event handlers)
static std::shared_ptr<AsyncFileLogger> g_radar_log[6];

// safe notify (serialize)
static inline void safe_notify(vsomeip::service_t s,
                               vsomeip::instance_t i,
                               vsomeip::event_t e,
                               const std::shared_ptr<vsomeip::payload> &pl) {
    std::lock_guard<std::mutex> lk(g_notify_mx);
    g_app->notify(s, i, e, pl);
    std::this_thread::yield();
}

// ---------------- timestamp helpers ----------------
static std::string make_stamp() {
    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

static bool is_valid_stamp(const std::string &s) {
    if (s.size() != 15 || s[8] != '_') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8) continue;
        if (!::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

static std::time_t parse_stamp_to_time_t(const std::string &s) {
    // s = YYYYMMDD_HHMMSS
    std::tm tm{};
    tm.tm_year = std::stoi(s.substr(0,4)) - 1900;
    tm.tm_mon  = std::stoi(s.substr(4,2)) - 1;
    tm.tm_mday = std::stoi(s.substr(6,2));
    tm.tm_hour = std::stoi(s.substr(9,2));
    tm.tm_min  = std::stoi(s.substr(11,2));
    tm.tm_sec  = std::stoi(s.substr(13,2));
    tm.tm_isdst = -1;
    return std::mktime(&tm); // local time
}

static std::string get_or_create_shared_parent_run_dir() {
    namespace fs = std::filesystem;
    static const uint64_t WINDOW_S = 15;

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
            if (tt > latest_tt) {
                latest_tt = tt;
                latest_name = name;
            }
        }
    } catch(...) {}

    const std::time_t now_tt = std::time(nullptr);
    if (!latest_name.empty() && (now_tt - latest_tt) <= static_cast<std::time_t>(WINDOW_S)) {
        return (base / latest_name).string();
    }

    const std::string stamp = make_stamp();
    const fs::path parent = base / stamp;
    try { fs::create_directories(parent); } catch(...) {}
    return parent.string();
}

// READY request handler
static void on_ready(const std::shared_ptr<vsomeip::message> &req) {
    const auto inst = req->get_instance();
    int slot = -1;
    if (auto it = g_inst_to_slot.find(inst); it != g_inst_to_slot.end()) slot = it->second;

    if (slot >= 0 && slot < g_slots_total) {
        bool expected = false;
        if (g_slot_is_ready[slot]->compare_exchange_strong(expected, true)) {
            int rc = ++g_slots_ready;
            std::ostringstream os;
            os << "[PUBLISHER] READY received for slot " << slot << " — " << rc << "/" << g_slots_total;
            SLOG(os.str());
        }
    }

    auto resp = vsomeip::runtime::get()->create_response(req);
    resp->set_return_code(vsomeip::return_code_e::E_OK);
    g_app->send(resp);

    if (g_slots_ready.load() >= g_slots_total) {
        SLOG("[PUBLISHER] All slots READY — starting ALL publishers");
        g_ready_cv.notify_all();
    }
}

// ------------- CLI helpers -------------
static std::vector<std::string> split_csv(const std::string &csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
    return out;
}

// tolerate glued flags like --period-ms-20 or --run-for-15
static void try_parse_glued_flag(const std::string &a, const char *key, int &dst) {
    std::string prefix = std::string(key) + "-";
    if (a.rfind(prefix, 0) == 0 && a.size() > prefix.size()) {
        dst = std::max(1, atoi(a.c_str() + prefix.size()));
    }
}

// ---- CAL state per slot ----
struct CalState {
    std::atomic<bool> calib_avail{false};
};
static CalState g_cal[6];
static int CAL_DELAY_MIN_MS = 80;
static int CAL_DELAY_MAX_MS = 220;

// Handle CAL_REQ event: send RADAR_ACK immediately, then RADAR_RESP after delay
static void on_cal_req(const std::shared_ptr<vsomeip::message> &ev) {
    const auto inst = ev->get_instance();
    auto it = g_inst_to_slot.find(inst);
    if (it == g_inst_to_slot.end()) return;
    const int slot = it->second;
    if (slot < 0 || slot >= 6) return;

    // --- NEW: ensure reliable serializer is ready before any method send
    {
        constexpr int kMaxWaitMs = 1500;
        constexpr int kStepMs    = 50;
        int waited = 0;
        while (!g_cal[slot].calib_avail.load() && waited < kMaxWaitMs) {
            if (g_radar_log[slot] && (waited % 250 == 0)) {
                g_radar_log[slot]->log_line("[CAL] waiting for reliable serializer...");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
            waited += kStepMs;
        }
        if (!g_cal[slot].calib_avail.load()) {
            if (g_radar_log[slot]) {
                g_radar_log[slot]->log_line("[CAL] reliable path not ready after wait; skipping this CAL_REQ");
            }
            return;
        }
    }

    const auto pl = ev->get_payload();
    if (!pl || pl->get_length() != 16) return;

    U32x4 req = unpack_be(pl->get_data(), pl->get_length());
    const uint32_t req_id      = req.d1;
    const uint32_t radar_rx_ts = now_ms32(); // actual RX time at radar

    // log: radar TX ACK
    if (g_radar_log[slot]) {
        std::ostringstream os;
        os << "[TX][CAL_ACK] req_id=" << req_id << " radar_rx_ts=" << radar_rx_ts
           << " echo=(" << req.d3 << "," << req.d4 << ")";
        g_radar_log[slot]->log_line(os.str());
    }

    // Send RADAR_ACK (RELIABLE)
    {
        U32x4 ack{ req_id, radar_rx_ts, req.d3, req.d4 };
        auto pl_ack = g_rt->create_payload();
        auto bytes  = pack_be(ack);
        pl_ack->set_data(bytes.data(), (uint32_t)bytes.size());

        auto reqm = g_rt->create_request();
        reqm->set_service(CALIB_SERVICE_ID);
        reqm->set_instance(inst);
        reqm->set_method(RADAR_ACK_METHOD_ID);
        reqm->set_payload(pl_ack);
        g_app->send(reqm);

        // metrics
        TX_PAYLOAD_CAL += 16;
        TX_WIRE_CAL    += wire_tcp(16);
        CNT_CAL_ACK_SENT.fetch_add(1, std::memory_order_relaxed);
    }

    // After random delay, send RADAR_RESP (RELIABLE)
    std::thread([slot, inst, req_id, radar_rx_ts, req]{
        std::mt19937 rng{ (uint32_t)(std::hash<int>{}(slot) ^ (uint32_t)now_ms32()) };
        std::uniform_int_distribution<int> D(CAL_DELAY_MIN_MS, CAL_DELAY_MAX_MS);
        int delay_ms = std::clamp(D(rng), CAL_DELAY_MIN_MS, CAL_DELAY_MAX_MS);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        // Double-check serializer still available (rarely flips back, but cheap)
        if (!g_cal[slot].calib_avail.load()) {
            if (g_radar_log[slot])
                g_radar_log[slot]->log_line("[CAL] reliable path lost before RESP; skipping RESP");
            return;
        }

        const uint32_t radar_resp_ts = now_ms32();
        const uint32_t randC = (uint32_t)rng();
        const uint32_t randD = (uint32_t)rng();

        if (g_radar_log[slot]) {
            std::ostringstream os1;
            os1 << "[TX][CAL_RESP] req_id=" << req_id << " radar_resp_ts=" << radar_resp_ts
                << " randC=" << randC << " randD=" << randD;
            g_radar_log[slot]->log_line(os1.str());

            std::ostringstream os2;
            os2 << "[CAL][TXN][RADAR " << (slot+1) << "] req=" << req_id
                << " rx_ts=" << radar_rx_ts
                << " resp_ts=" << radar_resp_ts
                << " req_data=(" << req.d1 << "," << req.d2 << "," << req.d3 << "," << req.d4 << ")"
                << " resp_data=(" << randC << "," << randD << ")";
            g_radar_log[slot]->log_line(os2.str());
        }

        U32x4 resp{ req_id, radar_resp_ts, randC, randD };
        auto pl_resp = g_rt->create_payload();
        auto bytes   = pack_be(resp);
        pl_resp->set_data(bytes.data(), (uint32_t)bytes.size());

        auto reqm = g_rt->create_request();
        reqm->set_service(CALIB_SERVICE_ID);
        reqm->set_instance(inst);
        reqm->set_method(RADAR_RESP_METHOD_ID);
        reqm->set_payload(pl_resp);
        g_app->send(reqm);

        // metrics
        TX_PAYLOAD_CAL += 16;
        TX_WIRE_CAL    += wire_tcp(16);
        CNT_CAL_RESP_SENT.fetch_add(1, std::memory_order_relaxed);
    }).detach();
}



int main(int argc, char* argv[]) {
    // ---- CLI ----
    std::string app_name = "radar_publisher";
    int SLOTS = 1;
    std::string radar_ids_csv = "1";
    int PERIOD_MS = 20;
    int RUN_FOR_S = 60;

    int detail_rate = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--app" && i + 1 < argc)            app_name = argv[++i];
        else if (a == "--slots" && i + 1 < argc)     SLOTS = std::max(1, atoi(argv[++i]));
        else if (a == "--radar-ids" && i + 1 < argc) radar_ids_csv = argv[++i];
        else if (a == "--period-ms" && i + 1 < argc) PERIOD_MS = std::max(1, atoi(argv[++i]));
        else if (a == "--run-for" && i + 1 < argc)   RUN_FOR_S = std::max(1, atoi(argv[++i]));
        else if (a == "--log-detail-rate" && i + 1 < argc) detail_rate = std::max(1, atoi(argv[++i]));
        else if (a == "--cal-delay-min-ms" && i+1 < argc) CAL_DELAY_MIN_MS = std::max(0, atoi(argv[++i]));
        else if (a == "--cal-delay-max-ms" && i+1 < argc) CAL_DELAY_MAX_MS = std::max(0, atoi(argv[++i]));
        else {
            // tolerate glued forms like --period-ms-20, --run-for-15
            try_parse_glued_flag(a, "--period-ms", PERIOD_MS);
            try_parse_glued_flag(a, "--run-for", RUN_FOR_S);
            try_parse_glued_flag(a, "--log-detail-rate", detail_rate);
            try_parse_glued_flag(a, "--cal-delay-min-ms", CAL_DELAY_MIN_MS);
            try_parse_glued_flag(a, "--cal-delay-max-ms", CAL_DELAY_MAX_MS);
        }
    }
    if (CAL_DELAY_MAX_MS < CAL_DELAY_MIN_MS) CAL_DELAY_MAX_MS = CAL_DELAY_MIN_MS;

    // ------- Shared parent folder -------
    const std::string parent = get_or_create_shared_parent_run_dir();

    // Publisher subfolder
    try { std::filesystem::create_directories(parent + "/publisher"); } catch(...) {}
    g_run_dir = parent + "/publisher";

    // Expand radar IDs list to SLOTS
    auto radar_ids = split_csv(radar_ids_csv);
    if ((int)radar_ids.size() < SLOTS) {
        for (int i = (int)radar_ids.size(); i < SLOTS; ++i) radar_ids.push_back(std::to_string(i + 1));
    } else if ((int)radar_ids.size() > SLOTS) {
        radar_ids.resize(SLOTS);
    }

    // session log (handshake / global)
    g_session_log.reset(new AsyncFileLogger(g_run_dir + "/publisher"));

    g_rt = vsomeip::runtime::get();
    if (!g_rt) { std::cerr << "Failed to get vsomeip runtime\n"; return 1; }

    g_app = g_rt->create_application(app_name);
    if (!g_app->init()) { std::cerr << "Failed to init vsomeip application\n"; return 2; }

    g_slots_total = SLOTS;

    g_slot_is_ready.clear();
    g_slot_is_ready.reserve(SLOTS);
    for (int i = 0; i < SLOTS; ++i)
        g_slot_is_ready.emplace_back(std::make_shared<std::atomic<bool>>(false));

    // CALIB availability handler: subscribe to CAL_REQ when ECU offers per-slot instance
    g_app->register_availability_handler(CALIB_SERVICE_ID, vsomeip::ANY_INSTANCE,
        [](vsomeip::service_t /*svc*/, vsomeip::instance_t inst, bool avail){
            auto it = g_inst_to_slot.find(inst);
            int slot = (it == g_inst_to_slot.end() ? -1 : it->second);
            if (!avail || slot < 0 || slot >= 6) return;

            // Subscribe to the CAL event right away (as you already did)
            {
                // Small delay so the reliable serializer can get created on the client side first
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                std::set<vsomeip::eventgroup_t> eg{ CALIB_EVENTGROUP_ID };
                g_app->request_event(CALIB_SERVICE_ID, inst, CALIB_EVENT_ID, eg);
                g_app->subscribe(CALIB_SERVICE_ID, inst, CALIB_EVENTGROUP_ID);
            }

            // Warm-up: give vSomeIP a moment to establish the reliable serializer
            std::thread([slot, inst]{
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                g_cal[slot].calib_avail.store(true);

                if (g_radar_log[slot]) {
                    std::ostringstream os;
                    os << "[Radar_" << (slot+1) << "] CALIB reliable path is READY on inst=0x"
                    << std::hex << inst << std::dec;
                    g_radar_log[slot]->log_line(os.str());
                }
            }).detach();
        }
    );

    // Register CAL_REQ event handler (global; route by instance->slot)
    g_app->register_message_handler(CALIB_SERVICE_ID, vsomeip::ANY_INSTANCE, CALIB_EVENT_ID, on_cal_req);

    // ---- Offer per-slot services/events & register READY handlers ----
    for (int slot = 0; slot < SLOTS; ++slot) {
        const std::string radar_id = radar_ids[slot];
        const vsomeip::instance_t hb_inst  = static_cast<vsomeip::instance_t>(HEARTBEAT_INSTANCE_BASE  + slot);
        const vsomeip::instance_t obj_inst = static_cast<vsomeip::instance_t>(OBJECTS_INSTANCE_BASE    + slot);
        const vsomeip::instance_t det_inst = static_cast<vsomeip::instance_t>(DETECTIONS_INSTANCE_BASE + slot);
        const vsomeip::instance_t cal_inst = static_cast<vsomeip::instance_t>(CALIB_INSTANCE_BASE      + slot);

        g_inst_to_slot[hb_inst]  = slot;
        g_inst_to_slot[obj_inst] = slot;
        g_inst_to_slot[det_inst] = slot;
        g_inst_to_slot[cal_inst] = slot;

        // per-radar logger
        g_radar_log[slot] = std::make_shared<AsyncFileLogger>(g_run_dir + "/radar_" + radar_id);

        g_app->offer_service(HEARTBEAT_SERVICE_ID,  hb_inst);
        g_app->offer_service(OBJECTS_SERVICE_ID,    obj_inst);
        g_app->offer_service(DETECTIONS_SERVICE_ID, det_inst);

        std::set<vsomeip::eventgroup_t> eg_hb{HEARTBEAT_EVENTGROUP_ID};
        std::set<vsomeip::eventgroup_t> eg_obj{OBJECTS_EVENTGROUP_ID};
        std::set<vsomeip::eventgroup_t> eg_det{DETECTIONS_EVENTGROUP_ID};

        g_app->offer_event(HEARTBEAT_SERVICE_ID, hb_inst, HEARTBEAT_EVENT_ID, eg_hb,
                           vsomeip_v3::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(),
                           false, true, nullptr, vsomeip_v3::reliability_type_e::RT_UNRELIABLE);

        g_app->offer_event(OBJECTS_SERVICE_ID, obj_inst, OBJECTS_EVENT_ID, eg_obj,
                           vsomeip_v3::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(),
                           false, true, nullptr, vsomeip_v3::reliability_type_e::RT_RELIABLE);

        g_app->offer_event(DETECTIONS_SERVICE_ID, det_inst, DETECTIONS_EVENT_ID, eg_det,
                           vsomeip_v3::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(),
                           false, true, nullptr, vsomeip_v3::reliability_type_e::RT_RELIABLE);

        g_app->register_message_handler(HEARTBEAT_SERVICE_ID, hb_inst, READY_METHOD_ID, on_ready);

        // IMPORTANT: request CALIB service (ECU server) so reliable serializer is created
        g_app->request_service(CALIB_SERVICE_ID, cal_inst);
    }

    // ---- Start per-slot workers (wait for READY-all) ----
    std::vector<std::thread> workers;
    workers.reserve(SLOTS);

    for (int slot = 0; slot < SLOTS; ++slot) {
        const std::string radar_id = radar_ids[slot];
        const vsomeip::instance_t hb_inst  = static_cast<vsomeip::instance_t>(HEARTBEAT_INSTANCE_BASE  + slot);
        const vsomeip::instance_t obj_inst = static_cast<vsomeip::instance_t>(OBJECTS_INSTANCE_BASE    + slot);
        const vsomeip::instance_t det_inst = static_cast<vsomeip::instance_t>(DETECTIONS_INSTANCE_BASE + slot);

        workers.emplace_back([=]() {
            auto LOG = [&](const std::string &s){ if (g_radar_log[slot]) g_radar_log[slot]->log_line(s); };

            {
                std::unique_lock<std::mutex> lk(g_ready_mx);
                g_ready_cv.wait(lk, [] { return g_slots_ready.load() >= g_slots_total; });
            }

            const int64_t phase_us = (int64_t)slot * (PERIOD_MS * 1000LL) / std::max(1, g_slots_total);
            if (phase_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(phase_us));

            {
                std::ostringstream os;
                os << "[Radar_" << radar_id << "] Publishing every " << PERIOD_MS
                   << " ms (STOP-free, " << RUN_FOR_S << "s run) | CAL work delay ["
                   << CAL_DELAY_MIN_MS << "," << CAL_DELAY_MAX_MS << "] ms";
                LOG(os.str());
            }

            struct Cat { uint32_t code; const char* name; uint32_t L, W, H; };
            std::vector<Cat> CAT = {
                {0,"car",450,180,150}, {1,"truck",1200,250,370}, {2,"motorcycle",220,80,120},
                {3,"pedestrian",50,50,170}, {4,"bicycle",180,60,110}, {5,"bus",1300,260,320}
            };
            auto type_name = [&](uint8_t code)->const char* {
                for (auto &c : CAT) if (c.code == code) return c.name;
                return "unknown";
            };

            std::mt19937 rng(static_cast<uint32_t>(std::hash<std::string>{}(radar_id)));
            std::uniform_int_distribution<uint32_t> U300(0,300), U100(0,100), U10(0,10);
            std::uniform_int_distribution<uint32_t> U200(0,200), U200pos(1,200);

            uint32_t seq = 0;
            uint64_t sent = 0;
            auto next_t = std::chrono::steady_clock::now();
            auto end_at = next_t + std::chrono::seconds(RUN_FOR_S);

            while (std::chrono::steady_clock::now() < end_at) {
                ++seq;
                const uint32_t ts = now_ms32();

                // ================= Heartbeat (12 B) =================
                Heartbeat hb{ ts, seq, HEALTH_OK };
                auto pl_hb = g_rt->create_payload();
                pl_hb->set_data(reinterpret_cast<const vsomeip::byte_t*>(&hb),
                                static_cast<uint32_t>(sizeof(hb)));
                safe_notify(HEARTBEAT_SERVICE_ID, hb_inst, HEARTBEAT_EVENT_ID, pl_hb);
                // ---- metrics (UDP)
                TX_PAYLOAD_HB += sizeof(hb);
                TX_WIRE_HB    += wire_udp(sizeof(hb));
                CNT_HB_SENT.fetch_add(1, std::memory_order_relaxed);

                // ================= Objects (flat 20×RadarObject = 1140 B) =================
                std::array<RadarObject, MAX_OBJECTS> obj_arr{};
                for (int i = 0; i < MAX_OBJECTS; ++i) {
                    const auto &c = CAT[i % CAT.size()];
                    int sx = 200 + 5 * i + (seq % 10);
                    int sy = -50 + 4 * (i % 5);
                    int sz = 0;
                    int vx = 15 + i, vy = 0, vz = 0;

                    RadarObject o{};
                    o.ts_ms = ts;
                    o.seq   = seq;
                    o.pos_x = (uint32_t)sx; o.pos_y = (uint32_t)sy; o.pos_z = (uint32_t)sz;
                    o.vel_x = (uint32_t)vx; o.vel_y = (uint32_t)vy; o.vel_z = (uint32_t)vz;
                    o.acc_x = 0; o.acc_y = 0; o.acc_z = 0;
                    o.size_x = c.L; o.size_y = c.W; o.size_z = c.H;
                    o.type = static_cast<uint8_t>(c.code);
                    obj_arr[i] = o;
                }
                auto pl_obj = g_rt->create_payload();
                pl_obj->set_data(reinterpret_cast<const vsomeip::byte_t*>(obj_arr.data()),
                                 static_cast<uint32_t>(OBJ_PAYLOAD_BYTES));
                safe_notify(OBJECTS_SERVICE_ID, obj_inst, OBJECTS_EVENT_ID, pl_obj);
                // ---- metrics (TCP)
                TX_PAYLOAD_OBJ += OBJ_PAYLOAD_BYTES;
                TX_WIRE_OBJ    += wire_tcp(OBJ_PAYLOAD_BYTES);
                CNT_OBJ_SENT.fetch_add(1, std::memory_order_relaxed);

                // ================= Detections (flat 20×Detection = 1540 B) =================
                std::array<Detection, MAX_DETECTIONS> det_arr{};
                struct DetLog { int px, py, pz, vx, vy, vz; uint32_t s[8]; uint8_t type; };
                DetLog detlog[MAX_DETECTIONS];

                for (int i = 0; i < MAX_DETECTIONS; ++i) {
                    Detection d{};
                    d.ts_ms = ts;
                    d.seq   = seq;

                    detlog[i].px = (int)U300(rng); detlog[i].py = (int)U100(rng); detlog[i].pz = (int)U300(rng);
                    detlog[i].vx = (int)U10(rng);  detlog[i].vy = (int)U10(rng);  detlog[i].vz = (int)U10(rng);

                    d.pos_x = (uint32_t)detlog[i].px; d.pos_y = (uint32_t)detlog[i].py; d.pos_z = (uint32_t)detlog[i].pz;
                    d.vel_x = (uint32_t)detlog[i].vx; d.vel_y = (uint32_t)detlog[i].vy; d.vel_z = (uint32_t)detlog[i].vz;

                    d.acc_x = U200(rng); d.acc_y = U200(rng); d.acc_z = U200(rng);
                    d.size_1 = detlog[i].s[0] = U200pos(rng);
                    d.size_2 = detlog[i].s[1] = U200pos(rng);
                    d.size_3 = detlog[i].s[2] = U200pos(rng);
                    d.size_4 = detlog[i].s[3] = U200pos(rng);
                    d.size_5 = detlog[i].s[4] = U200pos(rng);
                    d.size_6 = detlog[i].s[5] = U200pos(rng);
                    d.size_7 = detlog[i].s[6] = U200pos(rng);
                    d.size_8 = detlog[i].s[7] = U200pos(rng);
                    d.type = detlog[i].type = 0;

                    det_arr[i] = d;
                }

                auto pl_det = g_rt->create_payload();
                pl_det->set_data(reinterpret_cast<const vsomeip::byte_t*>(det_arr.data()),
                                 static_cast<uint32_t>(DET_PAYLOAD_BYTES));
                safe_notify(DETECTIONS_SERVICE_ID, det_inst, DETECTIONS_EVENT_ID, pl_det);
                // ---- metrics (TCP)
                TX_PAYLOAD_DET += DET_PAYLOAD_BYTES;
                TX_WIRE_DET    += wire_tcp(DET_PAYLOAD_BYTES);
                CNT_DET_SENT.fetch_add(1, std::memory_order_relaxed);

                // ============ Compact cycle markers ============
                {
                    std::ostringstream os;
                    os << "[TX][HB][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ health=OK";
                    LOG(os.str());
                }
                {
                    std::ostringstream os;
                    os << "[TX][OBJ][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ objs=" << MAX_OBJECTS;
                    LOG(os.str());
                }
                {
                    std::ostringstream os;
                    os << "[TX][DET][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ dets=" << MAX_DETECTIONS;
                    LOG(os.str());
                }

                // ============ Detailed logs ============
                if (detail_rate > 0 && (seq % detail_rate == 0)) {
                    for (int i = 0; i < MAX_OBJECTS; ++i) {
                        const auto &o = obj_arr[i];
                        const char* tname = type_name(o.type);
                        int sx = (int)o.pos_x;
                        int sy = (int)(int32_t)o.pos_y;
                        int sz = (int)o.pos_z;
                        int vx = (int)o.vel_x, vy = (int)o.vel_y, vz = (int)o.vel_z;

                        std::ostringstream os;
                        os << "[TX][OBJ][" << radar_id << "] seq=" << seq << " ts_ms=" << ts
                           << " | Object " << (i+1) << ": type=" << tname
                           << " (code=" << (int)o.type << "), size=" << o.size_x << "x" << o.size_y << "x" << o.size_z << " cm"
                           << ", pos=(" << sx << "," << sy << "," << sz << ") cm"
                           << ", vel=(" << vx << "," << vy << "," << vz << ") cm/s";
                        LOG(os.str());
                    }
                    for (int i = 0; i < MAX_DETECTIONS; ++i) {
                        const auto &d = det_arr[i];
                        std::ostringstream os;
                        os << "[TX][DET][" << radar_id << "] seq=" << seq << " ts_ms=" << ts
                           << " | Detection " << (i+1) << ": type=unknown (code=0), sizes=["
                           << d.size_1 << "," << d.size_2 << "," << d.size_3 << ","
                           << d.size_4 << "," << d.size_5 << "," << d.size_6 << ","
                           << d.size_7 << "," << d.size_8 << "], pos=("
                           << d.pos_x << "," << d.pos_y << "," << d.pos_z << ") cm, vel=("
                           << d.vel_x << "," << d.vel_y << "," << d.vel_z << ") cm/s";
                        LOG(os.str());
                    }
                }

                ++sent;
                next_t += std::chrono::milliseconds(PERIOD_MS);
                std::this_thread::sleep_until(next_t);
            }

            {
                std::ostringstream os;
                os << "[Radar_" << radar_id << "] Done. Sent " << (unsigned long long)sent
                   << " cycles @ " << PERIOD_MS << "ms in " << RUN_FOR_S
                   << "s (seq 1.." << (unsigned)seq << ").";
                LOG(os.str());
            }
        });
    }

    std::thread app_thread([] { g_app->start(); });

    for (auto &t : workers) t.join();

    // ---- Print TX METRICS summary ----
    {
        // exact counts
        const uint64_t n_hb  = CNT_HB_SENT.load();
        const uint64_t n_obj = CNT_OBJ_SENT.load();
        const uint64_t n_det = CNT_DET_SENT.load();
        const uint64_t n_ack = CNT_CAL_ACK_SENT.load();
        const uint64_t n_rsp = CNT_CAL_RESP_SENT.load();

        // bytes (already tracked)
        const uint64_t pl_hb  = TX_PAYLOAD_HB.load();
        const uint64_t pl_obj = TX_PAYLOAD_OBJ.load();
        const uint64_t pl_det = TX_PAYLOAD_DET.load();
        const uint64_t pl_cal = TX_PAYLOAD_CAL.load(); // ACK+RESP payload together
        const uint64_t pl_tot = pl_hb + pl_obj + pl_det + pl_cal;

        const uint64_t wi_hb  = TX_WIRE_HB.load();
        const uint64_t wi_obj = TX_WIRE_OBJ.load();
        const uint64_t wi_det = TX_WIRE_DET.load();
        const uint64_t wi_cal = TX_WIRE_CAL.load();
        const uint64_t wi_tot = wi_hb + wi_obj + wi_det + wi_cal;

        const double secs = std::max(1, RUN_FOR_S);
        const double mbit_s = (wi_tot * 8.0) / (secs * 1e6);

        auto line = [](const char* n, uint64_t cnt, uint64_t pl, uint64_t wi){
            std::ostringstream os;
            os << "  " << n << ": count=" << cnt
               << "  payload=" << pl << " B, wire≈" << wi << " B";
            return os.str();
        };

        std::ostringstream sum;
        sum << "[PUBLISHER][METRICS] ===== TX SUMMARY (" << RUN_FOR_S << "s) =====\n"
            << line("HB ", n_hb,  pl_hb,  wi_hb)  << "\n"
            << line("OBJ", n_obj, pl_obj, wi_obj) << "\n"
            << line("DET", n_det, pl_det, wi_det) << "\n"
            // break out CAL as ACK/RESP counts while keeping combined bytes
            << "  CAL: ACK_count=" << n_ack << "  RESP_count=" << n_rsp
            << "  payload=" << pl_cal << " B, wire≈" << wi_cal << " B\n"
            << "  TOTAL: payload=" << pl_tot << " B, wire≈" << wi_tot << " B\n"
            << "  Avg bitrate≈ " << std::fixed << std::setprecision(3) << mbit_s << " Mbit/s\n";

        std::cout << "\n" << sum.str();
        SLOG(sum.str());
    }

    g_app->stop();
    if (app_thread.joinable())
        app_thread.join();

    if (g_session_log) {
        std::string path = g_session_log->filepath();
        g_session_log->stop();
        if (!path.empty()) std::cout << "[LOG] Session file: " << path << "\n";
    }

    std::cout << "[PUBLISHER] Parent run folder: " << std::filesystem::path(g_run_dir).parent_path().string() << "\n";
    return 0;
}