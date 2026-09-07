#include "AdasZenohCompatTypeSupportImpl.h"

#include <dds/DCPS/Service_Participant.h>
#include <dds/DCPS/Marked_Default_Qos.h>
#include <dds/DCPS/WaitSet.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <random>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdint>

using namespace std;
using namespace std::chrono;

// -------- time helpers --------
static inline uint32_t now_ms32() {
  auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  return static_cast<uint32_t>(ms & 0xFFFFFFFFu);
}

// -------- match helper --------
static bool wait_for_match(DDS::DataWriter_ptr dw, int want = 1, int timeout_ms = 8000,
                           bool verbose=false, const char* name="") {
  DDS::PublicationMatchedStatus st{};
  auto deadline = steady_clock::now() + milliseconds(timeout_ms);
  while (steady_clock::now() < deadline) {
    if (dw->get_publication_matched_status(st) == DDS::RETCODE_OK) {
      if (st.current_count >= want) {
        if (verbose) std::cout << "[PUBLISHER][" << name << "] matched readers=" << st.current_count << "\n";
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (verbose) std::cerr << "[PUBLISHER][" << name << "] match timeout — matched=" << st.current_count << "\n";
  return st.current_count >= want;
}

// --- tiny helpers for log files
static void ensure_logs_dir(const std::string& dir) {
  struct stat st{};
  if (stat(dir.c_str(), &st) != 0) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
  }
}

static std::string timestamp_for_filename() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
  return buf;
}

// ---------- payload metering (bytes-only) ----------
// NOTE: zenoh-style "payload-only" sizes: ignore radar_id string.
//       These constants reproduce the zenoh totals.
struct Meter { uint64_t count=0, bytes=0; };
struct Metrics {
  struct { Meter hb, obj, det, cal_ack, cal_resp, ready; } tx;
  struct { Meter cal_req, ecu_ack; } rx;
} M;

static inline void add(Meter& m, uint64_t bytes) { ++m.count; m.bytes += bytes; }

// --- Zenoh-style payload size calculators ---
static inline uint64_t sz_payload(const Adas::Heartbeat&) {
  // ts_ms(4) + seq(4) + health(4)
  return 12;
}

// ObjectsMsg: 57 B per object (zenoh: no header counted)
static inline uint64_t sz_payload(const Adas::ObjectsMsg& x) {
  const uint64_t per_obj = 57;
  return static_cast<uint64_t>(x.objects.length()) * per_obj;
}

// DetectionsMsg: 77 B per detection (zenoh: no header counted)
static inline uint64_t sz_payload(const Adas::DetectionsMsg& x) {
  const uint64_t per_det = 77;
  return static_cast<uint64_t>(x.detections.length()) * per_det;
}

// Calibration frames: 4 × u32 = 16 B
static inline uint64_t sz_payload(const Adas::CalibrationRequest&)  { return 16; }
static inline uint64_t sz_payload(const Adas::CalibrationAck&)      { return 16; }
static inline uint64_t sz_payload(const Adas::CalibrationResponse&) { return 16; }
static inline uint64_t sz_payload(const Adas::CalibrationReady&)    { return 16; }

// ---------- local send/recv counters for publisher-side summary ----------
struct StreamSend { uint64_t hb=0, obj=0, det=0; } Send;
struct CalCounts { uint64_t ready_sent=0, ack_sent=0, resp_sent=0, req_recv=0, ecu_ack_recv=0; } Cal;

int main(int argc, char* argv[]) {
  std::string radar_id = "1";
  int PERIOD_MS = 20;
  int RUN_FOR_S = 60;
  bool verbose = false;
  bool print_summary = false;

  int calib_ms_min = 80;
  int calib_ms_max = 220;
  double ready_period_s = 0.5;

  // NEW: log directory selection
  std::string log_dir;
  if (const char* env = std::getenv("LOG_DIR")) log_dir = env;
  for (int i=1;i<argc;++i) {
    std::string a = argv[i];
    if (a=="--radar-id" && i+1<argc) radar_id = argv[++i];
    else if (a=="--period-ms" && i+1<argc) PERIOD_MS = std::max(1, atoi(argv[++i]));
    else if (a=="--run-for" && i+1<argc) RUN_FOR_S = std::max(1, atoi(argv[++i]));
    else if (a=="--verbose") verbose = true;
    else if (a=="--summary") print_summary = true;
    else if (a=="--log-dir" && i+1<argc) log_dir = argv[++i];
    else if (a=="--calib-ms-min" && i+1<argc) calib_ms_min = std::max(0, atoi(argv[++i]));
    else if (a=="--calib-ms-max" && i+1<argc) calib_ms_max = std::max(0, atoi(argv[++i]));
  }
  if (log_dir.empty()) log_dir = "logs";
  ensure_logs_dir(log_dir);

  // Prepare log file
  const std::string run_ts = timestamp_for_filename();
  const std::string log_path = log_dir + "/radar" + radar_id + "_" + run_ts + ".log";
  std::ofstream log(log_path.c_str(), std::ios::out | std::ios::trunc);
  if (!log) { std::cerr << "[Radar_" << radar_id << "] ERROR opening log file: " << log_path << "\n"; return 2; }

  DDS::DomainParticipantFactory_var dpf = TheParticipantFactoryWithArgs(argc, argv);
  DDS::DomainParticipant_var dp = dpf->create_participant(42, PARTICIPANT_QOS_DEFAULT, 0, 0);
  if (!dp) { std::cerr << "Failed to create DomainParticipant\n"; return 1; }

  // Types + topics
  Adas::HeartbeatTypeSupport_var ts_hb = new Adas::HeartbeatTypeSupportImpl;
  Adas::ObjectsMsgTypeSupport_var ts_obj = new Adas::ObjectsMsgTypeSupportImpl;
  Adas::DetectionsMsgTypeSupport_var ts_det = new Adas::DetectionsMsgTypeSupportImpl;

  Adas::CalibrationRequestTypeSupport_var  ts_creq  = new Adas::CalibrationRequestTypeSupportImpl;
  Adas::CalibrationResponseTypeSupport_var ts_cresp = new Adas::CalibrationResponseTypeSupportImpl;
  Adas::CalibrationAckTypeSupport_var      ts_cack  = new Adas::CalibrationAckTypeSupportImpl;
  Adas::CalibrationReadyTypeSupport_var    ts_cready= new Adas::CalibrationReadyTypeSupportImpl;

  ts_hb->register_type(dp, ""); ts_obj->register_type(dp, ""); ts_det->register_type(dp, "");
  ts_creq->register_type(dp, ""); ts_cresp->register_type(dp, ""); ts_cack->register_type(dp, ""); ts_cready->register_type(dp, "");

  DDS::Topic_var t_hb  = dp->create_topic("adas/heartbeat",  ts_hb->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_obj = dp->create_topic("adas/objects",    ts_obj->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_det = dp->create_topic("adas/detections", ts_det->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);

  // Calibration topics
  DDS::Topic_var t_cal_req   = dp->create_topic("adas/calib/request",        ts_creq->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_resp  = dp->create_topic("adas/calib/response",       ts_cresp->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ack_r = dp->create_topic("adas/calib/ack/radar",      ts_cack->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ready = dp->create_topic("adas/calib/ready",          ts_cready->get_type_name(),TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ack_e = dp->create_topic("adas/calib/ack/ecu",        ts_cack->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);

  DDS::Publisher_var pub = dp->create_publisher(PUBLISHER_QOS_DEFAULT, 0, 0);
  DDS::Subscriber_var sub = dp->create_subscriber(SUBSCRIBER_QOS_DEFAULT, 0, 0);

  // Writer QoS
  // --- HB: BEST_EFFORT, shallow history, lower priority
  DDS::DataWriterQos hb_qos; pub->get_default_datawriter_qos(hb_qos);
  hb_qos.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;
  hb_qos.reliability.max_blocking_time.sec = 0;
  hb_qos.reliability.max_blocking_time.nanosec = 50 * 1000 * 1000;

  // NEW: keep recent samples for late-joiners
  hb_qos.durability.kind = DDS::TRANSIENT_LOCAL_DURABILITY_QOS;

  hb_qos.history.kind = DDS::KEEP_LAST_HISTORY_QOS;
  hb_qos.history.depth = 64; // was 32

  // bump limits so depth is actually attainable
  hb_qos.resource_limits.max_samples = 512;              // was 256
  hb_qos.resource_limits.max_instances = 8;
  hb_qos.resource_limits.max_samples_per_instance = 512; // was 256

  hb_qos.latency_budget.duration.sec = 0;
  hb_qos.latency_budget.duration.nanosec = 1 * 1000 * 1000;
  hb_qos.transport_priority.value = 64;


  // --- RELIABLE streams (OBJ/DET + calib): higher priority, lean limits
  DDS::DataWriterQos rel_wq; pub->get_default_datawriter_qos(rel_wq);
  rel_wq.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;

  // NEW: keep recent samples for late-joiners
  rel_wq.durability.kind = DDS::TRANSIENT_LOCAL_DURABILITY_QOS;

  rel_wq.history.kind = DDS::KEEP_LAST_HISTORY_QOS;
  rel_wq.history.depth = 64; // was 16

  // bump limits to accommodate the deeper history
  rel_wq.resource_limits.max_samples = 2048;               // was 1024
  rel_wq.resource_limits.max_instances = 64;               // was 32
  rel_wq.resource_limits.max_samples_per_instance = 256;   // was 128

  rel_wq.latency_budget.duration.sec = 0;
  rel_wq.latency_budget.duration.nanosec = 5 * 1000 * 1000;
  rel_wq.transport_priority.value = 32;


  // Reader QoS for calib request & ECU-ACK
  DDS::DataReaderQos rel_rq; sub->get_default_datareader_qos(rel_rq);
  rel_rq.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;
  rel_rq.history.kind = DDS::KEEP_LAST_HISTORY_QOS; rel_rq.history.depth = 16;
  rel_rq.resource_limits.max_samples = 1024;
  rel_rq.resource_limits.max_instances = 64;
  rel_rq.resource_limits.max_samples_per_instance = 128;
  rel_rq.latency_budget.duration.sec = 0; rel_rq.latency_budget.duration.nanosec = 5 * 1000 * 1000;

  DDS::DataWriter_var hb_dw  = pub->create_datawriter(t_hb,  hb_qos, 0, 0);
  DDS::DataWriter_var obj_dw = pub->create_datawriter(t_obj, rel_wq, 0, 0);
  DDS::DataWriter_var det_dw = pub->create_datawriter(t_det, rel_wq, 0, 0);

  // Calibration writers
  DDS::DataWriter_var cal_resp_dw  = pub->create_datawriter(t_cal_resp,  rel_wq, 0, 0);
  DDS::DataWriter_var cal_ack_dw   = pub->create_datawriter(t_cal_ack_r, rel_wq, 0, 0);
  DDS::DataWriter_var cal_ready_dw = pub->create_datawriter(t_cal_ready, rel_wq, 0, 0);

  Adas::HeartbeatDataWriter_var     hb_w  = Adas::HeartbeatDataWriter    ::_narrow(hb_dw);
  Adas::ObjectsMsgDataWriter_var    obj_w = Adas::ObjectsMsgDataWriter   ::_narrow(obj_dw);
  Adas::DetectionsMsgDataWriter_var det_w = Adas::DetectionsMsgDataWriter::_narrow(det_dw);

  Adas::CalibrationResponseDataWriter_var resp_w = Adas::CalibrationResponseDataWriter::_narrow(cal_resp_dw);
  Adas::CalibrationAckDataWriter_var      ack_w  = Adas::CalibrationAckDataWriter     ::_narrow(cal_ack_dw);
  Adas::CalibrationReadyDataWriter_var    ready_w= Adas::CalibrationReadyDataWriter   ::_narrow(cal_ready_dw);

  // Subscriber for requests (from ECU) and ECU-ACK
  DDS::DataReader_var cal_req_dr = sub->create_datareader(t_cal_req, rel_rq, 0, 0);
  Adas::CalibrationRequestDataReader_var req_r = Adas::CalibrationRequestDataReader::_narrow(cal_req_dr);

  DDS::DataReader_var cal_ack_ecu_dr = sub->create_datareader(t_cal_ack_e, rel_rq, 0, 0);
  Adas::CalibrationAckDataReader_var ack_ecu_r = Adas::CalibrationAckDataReader::_narrow(cal_ack_ecu_dr);

  // small catalog and RNG
  struct Cat { uint32_t code; const char* name; uint32_t L,W,H; };
  vector<Cat> CAT = {
    {0,"car",450,180,150}, {1,"truck",1200,250,370}, {2,"motorcycle",220,80,120},
    {3,"pedestrian",50,50,170}, {4,"bicycle",180,60,110}, {5,"bus",1300,260,320}
  };
  std::mt19937 rng(static_cast<uint32_t>(std::hash<string>{}(radar_id)));
  std::uniform_int_distribution<uint32_t> U300(0,300), U100(0,100), U10(0,10), U200(0,200), U200pos(1,200), U32;

  // Header
  log << "[Radar_" << radar_id << "] Publishing every " << PERIOD_MS
      << " ms (STOP-free, " << RUN_FOR_S << "s run)\n";

  if (verbose) std::cout << "[Radar_" << radar_id << "] waiting for ECU to match...\n";
  (void)wait_for_match(hb_dw,  1, 8000, verbose, "HB");
  (void)wait_for_match(obj_dw, 1, 8000, verbose, "OBJ");
  (void)wait_for_match(det_dw, 1, 8000, verbose, "DET");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Calibration READY beacon thread
  std::atomic<bool> beacons{true};
  std::thread ready_thr([&](){
    while (beacons.load()) {
      Adas::CalibrationReady rd; rd.radar_id = radar_id.c_str();
      rd.d1 = now_ms32(); rd.d2 = U32(rng); rd.d3 = U32(rng); rd.d4 = U32(rng);
      ready_w->write(rd, DDS::HANDLE_NIL);
      add(M.tx.ready, sz_payload(rd));
      ++Cal.ready_sent;
      std::this_thread::sleep_for(std::chrono::milliseconds((int)(ready_period_s*1000)));
    }
  });

  // Request handler (drain with WaitSet)
  DDS::ReadCondition_var rc_req   = req_r    ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::ReadCondition_var rc_ack_e = ack_ecu_r->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::WaitSet ws;
  ws.attach_condition(rc_req);
  ws.attach_condition(rc_ack_e);

  uint32_t seq = 0;
  const int OBJECTS_PER_CYCLE = 20, DETECTIONS_PER_CYCLE = 20;

  // -------- Pre-allocate and reuse payload buffers (reduce per-cycle allocs) --------
  Adas::ObjectsMsg om; 
  om.radar_id = radar_id.c_str(); 
  om.objects.length(OBJECTS_PER_CYCLE);

  Adas::DetectionsMsg dm; 
  dm.radar_id = radar_id.c_str(); 
  dm.detections.length(DETECTIONS_PER_CYCLE);

  auto next_t = steady_clock::now();
  auto end_at = next_t + seconds(RUN_FOR_S);
  uint64_t sent = 0;
  const uint32_t HEALTH_OK = 0;

  // Radar work thread pool (simple: spawn a thread per req)
  std::mutex log_mx;

  auto handle_req = [&](const Adas::CalibrationRequest& req){
    // If addressed (non-broadcast), enforce match to our radar_id
    if (req.radar_id.in() && std::string(req.radar_id.in()).size() && std::string(req.radar_id.in()) != radar_id) {
      return;
    }

    // ACK immediately (radar -> ECU)
    Adas::CalibrationAck ack;
    ack.radar_id = radar_id.c_str();
    ack.d1 = req.d1;
    ack.d2 = now_ms32();   // radar rx ts
    ack.d3 = req.d3;
    ack.d4 = req.d4;
    ack_w->write(ack, DDS::HANDLE_NIL);
    add(M.tx.cal_ack, sz_payload(ack));
    ++Cal.ack_sent;

    // Simulate work
    int lo = std::min(calib_ms_min, calib_ms_max);
    int hi = std::max(calib_ms_min, calib_ms_max);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(lo,hi)(rng)));

    // RESP (radar -> ECU)
    Adas::CalibrationResponse r;
    r.radar_id = radar_id.c_str();
    r.d1 = req.d1;           // echo req id
    r.d2 = now_ms32();       // radar resp ts
    r.d3 = U32(rng);
    r.d4 = U32(rng);
    resp_w->write(r, DDS::HANDLE_NIL);
    add(M.tx.cal_resp, sz_payload(r));
    ++Cal.resp_sent;

    // Single-line TXN log
    std::lock_guard<std::mutex> g(log_mx);
    log << "[CAL][TXN][RADAR " << radar_id << "] req=" << req.d1
        << " rx_ts=" << ack.d2
        << " resp_ts=" << r.d2
        << " req_data=(" << req.d1 << "," << req.d2 << "," << req.d3 << "," << req.d4 << ")"
        << " resp_data=(" << r.d3 << "," << r.d4 << ")\n";
  };

  // Main loop
  while (steady_clock::now() < end_at) {
    // Drain calibration requests + ECU-ACKs (non-blocking wait up to 1ms)
    DDS::ConditionSeq active;
    DDS::Duration_t to = {0, 1 * 1000 * 1000};
    if (ws.wait(active, to) == DDS::RETCODE_OK) {
      for (CORBA::ULong ai=0; ai<active.length(); ++ai) {
        if (active[ai] == rc_req) {
          Adas::CalibrationRequestSeq data; DDS::SampleInfoSeq info;
          while (true) {
            DDS::ReturnCode_t rc = req_r->take_w_condition(data, info, 64, rc_req);
            if (rc == DDS::RETCODE_NO_DATA) break;
            if (rc != DDS::RETCODE_OK) break;

            for (CORBA::ULong i=0;i<data.length();++i) {
              if (!info[i].valid_data) continue;
              const Adas::CalibrationRequest& req = data[i];
              add(M.rx.cal_req, sz_payload(req));       // RX metering
              ++Cal.req_recv;
              std::thread(handle_req, req).detach();    // handle in background
            }
            req_r->return_loan(data, info);
            if (data.length() < 64) break;
          }
        } else if (active[ai] == rc_ack_e) {
          // count ECU -> radar ACKs
          Adas::CalibrationAckSeq data; DDS::SampleInfoSeq info;
          while (true) {
            DDS::ReturnCode_t rc = ack_ecu_r->take_w_condition(data, info, 64, rc_ack_e);
            if (rc == DDS::RETCODE_NO_DATA) break;
            if (rc != DDS::RETCODE_OK) break;
            for (CORBA::ULong i=0;i<data.length();++i) {
              if (!info[i].valid_data) continue;
              add(M.rx.ecu_ack, sz_payload(data[i]));
              ++Cal.ecu_ack_recv;
            }
            ack_ecu_r->return_loan(data, info);
            if (data.length() < 64) break;
          }
        }
      }
    }

    // 20ms cycle publishers (HB/OBJ/DET)
    ++seq;
    const uint32_t ts = now_ms32();

    // Heartbeat
    {
      Adas::Heartbeat hb; hb.radar_id = radar_id.c_str(); hb.ts_ms = ts; hb.seq = seq; hb.health = HEALTH_OK;
      hb_w->write(hb, DDS::HANDLE_NIL);
      add(M.tx.hb, sz_payload(hb));
      ++Send.hb;
      log << "[TX][HB][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ health=OK\n";
    }

    // Objects (reuse buffer)
    om.ts_ms = ts; om.seq = seq;
    for (int i=0;i<OBJECTS_PER_CYCLE;++i) {
      const auto& c = CAT[i % CAT.size()];
      Adas::RadarObject o;
      o.pos_x = 200 + 5 * i + (seq % 10);
      o.pos_y = static_cast<int32_t>(-50 + 4 * (i % 5));
      o.pos_z = 0;
      o.vel_x = 15 + i; o.vel_y = 0; o.vel_z = 0;
      o.acc_x = 0; o.acc_y = 0; o.acc_z = 0;
      o.size_x = c.L; o.size_y = c.W; o.size_z = c.H;
      o.type = static_cast<unsigned char>(c.code);
      om.objects[i] = o;
    }
    obj_w->write(om, DDS::HANDLE_NIL);
    add(M.tx.obj, sz_payload(om));
    ++Send.obj;
    log << "[TX][OBJ][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ objs=" << OBJECTS_PER_CYCLE << "\n";
    for (int i=0;i<OBJECTS_PER_CYCLE;++i) {
      const auto& ob = om.objects[i];
      const auto& c = CAT[i % CAT.size()];
      log << "[TX][OBJ][" << radar_id << "] seq=" << seq << " ts_ms=" << ts
          << " | Object " << (i+1) << ": type=" << c.name << " (code=" << (int)c.code << "), "
          << "size=" << ob.size_x << "x" << ob.size_y << "x" << ob.size_z << " cm, "
          << "pos=(" << ob.pos_x << "," << ob.pos_y << "," << ob.pos_z << ") cm, "
          << "vel=(" << ob.vel_x << "," << ob.vel_y << "," << ob.vel_z << ") cm/s\n";
    }

    // Detections (reuse buffer)
    dm.ts_ms = ts; dm.seq = seq;
    for (int i=0;i<DETECTIONS_PER_CYCLE;++i) {
      Adas::Detection d;
      d.pos_x = U300(rng); d.pos_y = U100(rng); d.pos_z = U300(rng);
      d.vel_x = U10(rng);  d.vel_y = U10(rng);  d.vel_z = U10(rng);
      d.acc_x = U200(rng); d.acc_y = U200(rng); d.acc_z = U200(rng);
      d.size_1 = U200pos(rng); d.size_2 = U200pos(rng); d.size_3 = U200pos(rng); d.size_4 = U200pos(rng);
      d.size_5 = U200pos(rng); d.size_6 = U200pos(rng); d.size_7 = U200pos(rng); d.size_8 = U200pos(rng);
      d.type = 0;
      dm.detections[i] = d;
    }
    det_w->write(dm, DDS::HANDLE_NIL);
    add(M.tx.det, sz_payload(dm));
    ++Send.det;
    log << "[TX][DET][" << radar_id << "] seq=" << seq << " ts_ms=" << ts << " | ✅ dets=" << DETECTIONS_PER_CYCLE << "\n";
    for (int i=0;i<DETECTIONS_PER_CYCLE;++i) {
      const auto& d = dm.detections[i];
      log << "[TX][DET][" << radar_id << "] seq=" << seq << " ts_ms=" << ts
          << " | Detection " << (i+1) << ": type=unknown (code=0), sizes=["
          << d.size_1 << "," << d.size_2 << "," << d.size_3 << "," << d.size_4 << ","
          << d.size_5 << "," << d.size_6 << "," << d.size_7 << "," << d.size_8 << "], "
          << "pos=(" << d.pos_x << "," << d.pos_y << "," << d.pos_z << ") cm, "
          << "vel=(" << d.vel_x << "," << d.vel_y << "," << d.vel_z << ") cm/s\n";
    }

    ++sent;
    next_t += milliseconds(PERIOD_MS);
    std::this_thread::sleep_until(next_t);
  }

  // Clean up reader conditions
  ws.detach_condition(rc_req);
  ws.detach_condition(rc_ack_e);
  req_r->delete_readcondition(rc_req);
  ack_ecu_r->delete_readcondition(rc_ack_e);

  // Acknowledge all reliable writers
  DDS::Duration_t to{2,0};
  hb_w->wait_for_acknowledgments(to);
  obj_w->wait_for_acknowledgments(to);
  det_w->wait_for_acknowledgments(to);
  ack_w->wait_for_acknowledgments(to);
  resp_w->wait_for_acknowledgments(to);
  ready_w->wait_for_acknowledgments(to);

  beacons = false;
  if (ready_thr.joinable()) ready_thr.join();

  log << "[Radar_" << radar_id << "] Sent " << sent
      << " cycles @ " << PERIOD_MS << "ms in " << RUN_FOR_S
      << "s (seq 1.." << static_cast<uint32_t>(sent) << ").\n";

  // ---- terminal summaries ----
  auto bytes_to_mbps = [](uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return 0.0;
    return (bytes * 8.0) / seconds / 1e6;
  };
  uint64_t tx_total_bytes =
      M.tx.hb.bytes + M.tx.obj.bytes + M.tx.det.bytes +
      M.tx.cal_ack.bytes + M.tx.cal_resp.bytes + M.tx.ready.bytes;
  uint64_t rx_total_bytes = M.rx.cal_req.bytes + M.rx.ecu_ack.bytes;
  double run_secs = RUN_FOR_S;

  if (print_summary) {
    // 1) BYTE summary (payload only)
    std::cout << "\n[ADAS][METRICS][RADAR " << radar_id << "] ===== BYTE SUMMARY (payload only) =====\n";
    std::cout << "TX:\n";
    std::cout << "  HB:        count=" << M.tx.hb.count        << " payload=" << M.tx.hb.bytes        << " B\n";
    std::cout << "  OBJ:       count=" << M.tx.obj.count       << " payload=" << M.tx.obj.bytes       << " B\n";
    std::cout << "  DET:       count=" << M.tx.det.count       << " payload=" << M.tx.det.bytes       << " B\n";
    std::cout << "  CAL-ACK:   count=" << M.tx.cal_ack.count   << " payload=" << M.tx.cal_ack.bytes   << " B\n";
    std::cout << "  CAL-RESP:  count=" << M.tx.cal_resp.count  << " payload=" << M.tx.cal_resp.bytes  << " B\n";
    std::cout << "  READY:     count=" << M.tx.ready.count     << " payload=" << M.tx.ready.bytes     << " B\n";
    std::cout << "  TX TOTAL:  payload=" << tx_total_bytes << " B\n";
    std::cout << "  TX avg payload bitrate≈ " << std::fixed << std::setprecision(3)
              << bytes_to_mbps(tx_total_bytes, run_secs) << " Mbit/s\n";
    std::cout << "RX:\n";
    std::cout << "  CAL-REQ(broadcast): count=" << M.rx.cal_req.count << " payload=" << M.rx.cal_req.bytes << " B\n";
    std::cout << "  ECU-ACK:            count=" << M.rx.ecu_ack.count << " payload=" << M.rx.ecu_ack.bytes << " B\n";
    std::cout << "  RX TOTAL:  payload=" << rx_total_bytes << " B\n";
    std::cout << "  RX avg payload bitrate≈ " << std::fixed << std::setprecision(3)
              << bytes_to_mbps(rx_total_bytes, run_secs) << " Mbit/s\n";

    // 2) Stream send summary (publisher-side)
    std::cout << "\n[ADAS][SUMMARY] Streams (HB/OBJ/DET) over " << RUN_FOR_S << "s\n";
    std::cout << "  Radar_" << radar_id << " [HB ] Sent=" << Send.hb  << "\n";
    std::cout << "  Radar_" << radar_id << " [OBJ] Sent=" << Send.obj << " objs/packet=" << 20 << "\n";
    std::cout << "  Radar_" << radar_id << " [DET] Sent=" << Send.det << " dets/packet=" << 20 << "\n";

    // 3) Calibration counts (publisher-side perspective)
    std::cout << "[ADAS][SUMMARY] Calibration counts (RADAR side)\n";
    std::cout << "  Radar_" << radar_id
              << " READY(sent)=" << Cal.ready_sent
              << " ACK(sent)="   << Cal.ack_sent
              << " RESP(sent)="  << Cal.resp_sent
              << " CAL-REQ(recv)=" << Cal.req_recv
              << " ECU-ACK(recv)=" << Cal.ecu_ack_recv
              << "\n";
  }

  log.flush();
  dp->delete_contained_entities(); dpf->delete_participant(dp);
  TheServiceParticipant->shutdown();
  return 0;
}