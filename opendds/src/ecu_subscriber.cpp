#include "AdasZenohCompatTypeSupportImpl.h"

#include <dds/DCPS/Service_Participant.h>
#include <dds/DCPS/Marked_Default_Qos.h>
#include <dds/DCPS/WaitSet.h>
#include <dds/DdsDcpsInfrastructureC.h>

#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;
using namespace std::chrono;

#ifdef USE_XLSXWRITER
#include <xlsxwriter.h>
#endif

// ---------------- time helpers ----------------
static inline uint32_t now_ms32() {
  auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  return static_cast<uint32_t>(ms & 0xFFFFFFFFu);
}
static inline uint64_t now_ms64() {
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------- thresholds ----------------
static const uint32_t HB_PERIOD_MS = 20, HB_E2E_MAX_MS = 5,  HB_JITTER_MAX_MS = 1;
static const uint32_t OBJ_PERIOD_MS = 20, OBJ_E2E_MAX_MS = 25, OBJ_JITTER_MAX_MS = 5, OBJ_CONSEC_FAULT = 3;
static const uint32_t DET_PERIOD_MS = 20, DET_E2E_MAX_MS = 25, DET_JITTER_MAX_MS = 5, DET_CONSEC_FAULT = 3;

static const int CALIB_PERIOD_MS = 1000;

// ---------------- stream state/agg ----------------
struct StreamAgg { uint64_t total=0, ok=0, viol=0, decode_err=0, faults=0, max_e2e=0, max_jitter=0; };
struct BaseState  {
  bool primed=false;
  uint32_t last_seq=0;
  uint64_t last_rx_ms=0;
  uint64_t last_src_ms=0;   // source ts_ms for jitter
  int loss_streak=0;
};
using HBState = BaseState;
using ObjState = BaseState;
using DetState = BaseState;

// ---------------- load metrics (payload-only) ----------------
struct Meter { uint64_t count=0, bytes=0; };
struct Metrics {
  struct { Meter hb, obj, det, cal_ack, cal_resp, ready; } rx;
  struct { Meter cal_req, cal_ack; } tx;  // cal_ack = ECU→radar
} M;
static inline void add(Meter& m, uint64_t bytes) { ++m.count; m.bytes += bytes; }

// ---- zenoh-style per-message payload size helpers (payload only; ignore strings) ----
static inline uint64_t sz_payload(const Adas::Heartbeat&) { return 12; } // ts(4)+seq(4)+health(4)
static inline uint64_t sz_payload(const Adas::ObjectsMsg& x) {
  const uint64_t per_obj = 57;
  return (uint64_t)x.objects.length() * per_obj;
}
static inline uint64_t sz_payload(const Adas::DetectionsMsg& x) {
  const uint64_t per_det = 77;
  return (uint64_t)x.detections.length() * per_det;
}
static inline uint64_t sz_payload(const Adas::CalibrationRequest&)  { return 16; } // 4×u32
static inline uint64_t sz_payload(const Adas::CalibrationAck&)      { return 16; }
static inline uint64_t sz_payload(const Adas::CalibrationResponse&) { return 16; }
static inline uint64_t sz_payload(const Adas::CalibrationReady&)    { return 16; }

// ---------------- log helpers ----------------
static void ensure_dir(const std::string& dir) {
  struct stat st; if (stat(dir.c_str(), &st) != 0) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
  }
}
static std::string ts_for_filename() {
  std::time_t t = std::time(nullptr);
  std::tm tm;
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32]; std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
  return buf;
}
static std::string ts_human(time_t t) {
  std::tm tm;
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
  return buf;
}

// ---------------- Excel helpers ----------------
#ifdef USE_XLSXWRITER
struct ExcelOut {
  lxw_workbook*  wb = nullptr;
  lxw_worksheet* sh_hb = nullptr;
  lxw_worksheet* sh_obj = nullptr;
  lxw_worksheet* sh_det = nullptr;
  lxw_worksheet* sh_summary = nullptr;
  lxw_worksheet* sh_info = nullptr;

  // Calibration
  lxw_worksheet* sh_calib = nullptr;
  lxw_worksheet* sh_calib_txn = nullptr;

  uint32_t row_hb  = 1, row_obj = 1, row_det = 1, row_sum = 1;
  uint32_t row_cal = 1, row_txn = 1;

  bool ok = false;
  std::string path;
};

static ExcelOut excel_open(const std::string& path) {
  ExcelOut X; X.path = path;
  X.wb = workbook_new(path.c_str());
  if (!X.wb) return X;

  // Data sheets
  X.sh_hb  = workbook_add_worksheet(X.wb, "HB");
  X.sh_obj = workbook_add_worksheet(X.wb, "OBJ");
  X.sh_det = workbook_add_worksheet(X.wb, "DET");
  X.sh_summary = workbook_add_worksheet(X.wb, "SUMMARY");
  X.sh_info    = workbook_add_worksheet(X.wb, "INFO");

  // Calibration sheets
  X.sh_calib     = workbook_add_worksheet(X.wb, "Calibration");
  X.sh_calib_txn = workbook_add_worksheet(X.wb, "Calibration Txns");

  // Headers
  auto hdr_common = [](lxw_worksheet* sh){
    worksheet_write_string(sh, 0, 0, "radar_id",  NULL);
    worksheet_write_string(sh, 0, 1, "seq",       NULL);
    worksheet_write_string(sh, 0, 2, "ts_src_ms", NULL);
    worksheet_write_string(sh, 0, 3, "ts_rx_ms",  NULL);
    worksheet_write_string(sh, 0, 4, "e2e_ms",    NULL);
    worksheet_write_string(sh, 0, 5, "jitter_ms", NULL);
    worksheet_write_string(sh, 0, 6, "status",    NULL);
  };
  hdr_common(X.sh_hb);
  hdr_common(X.sh_obj);
  hdr_common(X.sh_det);

  // SUMMARY header
  worksheet_write_string(X.sh_summary, 0, 0, "stream",       NULL);
  worksheet_write_string(X.sh_summary, 0, 1, "radar_id",     NULL);
  worksheet_write_number(X.sh_summary, 0, 2, (double)0, NULL); // headers as numbers require init
  worksheet_write_string(X.sh_summary, 0, 2, "total",        NULL);
  worksheet_write_string(X.sh_summary, 0, 3, "ok",           NULL);
  worksheet_write_string(X.sh_summary, 0, 4, "viol",         NULL);
  worksheet_write_string(X.sh_summary, 0, 5, "decode_err",   NULL);
  worksheet_write_string(X.sh_summary, 0, 6, "faults",       NULL);
  worksheet_write_string(X.sh_summary, 0, 7, "max_e2e_ms",   NULL);
  worksheet_write_string(X.sh_summary, 0, 8, "max_jitter_ms",NULL);
  worksheet_write_string(X.sh_summary, 0, 9, "ok %",         NULL);

  // INFO header
  worksheet_write_string(X.sh_info, 0, 0, "key",   NULL);
  worksheet_write_string(X.sh_info, 0, 1, "value", NULL);

  // Calibration summary header
  worksheet_write_string(X.sh_calib, 0, 0, "Radar", NULL);
  worksheet_write_number(X.sh_calib, 0, 1, (double)0, NULL);
  worksheet_write_string(X.sh_calib, 0, 1, "ACK(recv)", NULL);
  worksheet_write_string(X.sh_calib, 0, 2, "RESP(recv)", NULL);
  worksheet_write_string(X.sh_calib, 0, 3, "ECU-ACK(sent)", NULL);
  worksheet_write_string(X.sh_calib, 0, 4, "ReadySeen", NULL);

  // Calibration Txns header
  const char* H[] = {
    "Radar","ReqID","ECU_send_ms","Radar_ACK_rx_ms","Radar_RESP_ms","ECU_ACK_ms",
    "Req_d1","Req_d2","Req_d3","Req_d4","Resp_C","Resp_D",
    "L1_ECU->ACK(ms)","L2_ACK->RESP(ms)","L3_RESP->ECU(ms)"
  };
  for (int i=0;i<15;++i) worksheet_write_string(X.sh_calib_txn, 0, i, H[i], NULL);

  // column widths
  worksheet_set_column(X.sh_hb,      0, 6, 16.0, NULL);
  worksheet_set_column(X.sh_obj,     0, 6, 16.0, NULL);
  worksheet_set_column(X.sh_det,     0, 6, 16.0, NULL);
  worksheet_set_column(X.sh_summary, 0, 9, 16.0, NULL);
  worksheet_set_column(X.sh_info,    0, 1, 32.0, NULL);
  worksheet_set_column(X.sh_calib,   0, 4, 18.0, NULL);
  worksheet_set_column(X.sh_calib_txn, 0, 14, 18.0, NULL);

  X.ok = true;
  return X;
}

static void excel_write_row(lxw_worksheet* sh, uint32_t& row,
                            const std::string& rid, uint32_t seq,
                            uint64_t ts_src, uint64_t ts_rx,
                            uint64_t e2e, uint64_t jitter,
                            const std::string& status) {
  worksheet_write_string (sh, row, 0, rid.c_str(), NULL);
  worksheet_write_number (sh, row, 1, seq, NULL);
  worksheet_write_number (sh, row, 2, (double)ts_src, NULL);
  worksheet_write_number (sh, row, 3, (double)ts_rx, NULL);
  worksheet_write_number (sh, row, 4, (double)e2e, NULL);
  worksheet_write_number (sh, row, 5, (double)jitter, NULL);
  worksheet_write_string (sh, row, 6, status.c_str(), NULL);
  row++;
}

static void excel_write_summary_rows(ExcelOut& X,
                                     const std::unordered_map<std::string, StreamAgg>& Mx,
                                     const char* stream) {
  if (!X.ok) return;
  for (const auto& kv : Mx) {
    const auto& rid = kv.first; const auto& a = kv.second;
    double ok_percent = a.total ? (100.0 * (double)a.ok / (double)a.total) : 0.0;
    worksheet_write_string (X.sh_summary, X.row_sum, 0, stream,      NULL);
    worksheet_write_string (X.sh_summary, X.row_sum, 1, rid.c_str(), NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 2, (double)a.total, NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 3, (double)a.ok,    NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 4, (double)a.viol,  NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 5, (double)a.decode_err, NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 6, (double)a.faults,     NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 7, (double)a.max_e2e,    NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 8, (double)a.max_jitter, NULL);
    worksheet_write_number (X.sh_summary, X.row_sum, 9, ok_percent, NULL);
    X.row_sum++;
  }
}

static void excel_write_calib_summary_row(ExcelOut& X,
                                          const std::string& rid,
                                          int ack_recv, int resp_recv, int ecu_ack_sent, bool ready) {
  if (!X.ok) return;
  worksheet_write_string (X.sh_calib, X.row_cal, 0, ("Radar_" + rid).c_str(), NULL);
  worksheet_write_number (X.sh_calib, X.row_cal, 1, ack_recv, NULL);
  worksheet_write_number (X.sh_calib, X.row_cal, 2, resp_recv, NULL);
  worksheet_write_number (X.sh_calib, X.row_cal, 3, ecu_ack_sent, NULL);
  worksheet_write_string (X.sh_calib, X.row_cal, 4, ready ? "yes" : "no", NULL);
  X.row_cal++;
}

static void excel_write_calib_txn(ExcelOut& X, const std::string& rid,
                                  uint32_t req_id,
                                  uint32_t ecu_send_ts, uint32_t radar_ack_rx_ts,
                                  uint32_t radar_resp_ts, uint32_t ecu_ack_ts,
                                  uint32_t req_d1, uint32_t req_d2, uint32_t req_d3, uint32_t req_d4,
                                  uint32_t resp_c, uint32_t resp_d,
                                  int l1, int l2, int l3) {
  if (!X.ok) return;
  int r = X.row_txn;
  worksheet_write_string (X.sh_calib_txn, r,  0, ("Radar_" + rid).c_str(), NULL);
  worksheet_write_number (X.sh_calib_txn, r,  1, req_id, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  2, ecu_send_ts, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  3, radar_ack_rx_ts, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  4, radar_resp_ts, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  5, ecu_ack_ts, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  6, req_d1, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  7, req_d2, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  8, req_d3, NULL);
  worksheet_write_number (X.sh_calib_txn, r,  9, req_d4, NULL);
  worksheet_write_number (X.sh_calib_txn, r, 10, resp_c, NULL);
  worksheet_write_number (X.sh_calib_txn, r, 11, resp_d, NULL);
  worksheet_write_number (X.sh_calib_txn, r, 12, l1, NULL);
  worksheet_write_number (X.sh_calib_txn, r, 13, l2, NULL);
  worksheet_write_number (X.sh_calib_txn, r, 14, l3, NULL);
  X.row_txn++;
}

static void excel_write_info_kv(lxw_worksheet* sh, uint32_t& row,
                                const std::string& k, const std::string& v) {
  worksheet_write_string(sh, row, 0, k.c_str(), NULL);
  worksheet_write_string(sh, row, 1, v.c_str(), NULL);
  row++;
}
static void excel_write_info_kv_num(lxw_worksheet* sh, uint32_t& row,
                                    const std::string& k, double v) {
  worksheet_write_string(sh, row, 0, k.c_str(), NULL);
  worksheet_write_number(sh, row, 1, v, NULL);
  row++;
}

static void excel_close(ExcelOut& X) {
  if (X.wb) workbook_close(X.wb);
  X = ExcelOut{};
}
#endif // USE_XLSXWRITER

// ---------------- helpers ----------------
static bool wait_for_match(DDS::DataReader_ptr dr, const char* name,
                           int want = 1, int timeout_ms = 8000, bool verbose=false) {
  DDS::SubscriptionMatchedStatus st{};
  auto deadline = steady_clock::now() + milliseconds(timeout_ms);
  while (steady_clock::now() < deadline) {
    if (dr->get_subscription_matched_status(st) == DDS::RETCODE_OK) {
      if (st.current_count >= want) {
        if (verbose) std::cout << "[ECU][" << name << "] matched publishers=" << st.current_count << "\n";
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (verbose) std::cerr << "[ECU][" << name << "] match timeout — matched=" << st.current_count << "\n";
  return st.current_count >= want;
}

// ---------------- Calibration bookkeeping ----------------
struct ReqMeta { uint32_t ecu_send_ts=0, d1=0, d2=0, d3=0, d4=0; };
struct CalTxn {
  std::string rid;
  uint32_t req_id=0;
  uint32_t ecu_send_ts=0, radar_ack_rx_ts=0, radar_resp_ts=0, ecu_ack_ts=0;
  uint32_t req_d1=0, req_d2=0, req_d3=0, req_d4=0;
  uint32_t resp_c=0, resp_d=0;
  bool have_ack=false, have_resp=false, have_send=false, have_ecu_ack=false;
};

int main(int argc, char* argv[]) {
  bool verbose = false;
  bool print_summary = false;
  std::string ini_path;
  std::string log_dir = "logs";   // default
  int RUN_FOR_S = 60;             // configurable via --run-for

  for (int i=1;i<argc;++i) {
    std::string a = argv[i];
    if (a=="--verbose") verbose = true;
    if (a=="--summary") print_summary = true;
    if (a=="--run-for" && i+1<argc) RUN_FOR_S = std::max(1, atoi(argv[++i]));
    if (a=="-DCPSConfigFile" && i+1<argc) ini_path = argv[i+1];
    const std::string k = "-DCPSConfigFile=";
    if (a.rfind(k,0)==0 && a.size()>k.size()) ini_path = a.substr(k.size());
    if (a=="--log-dir" && i+1<argc) log_dir = argv[++i];
  }

  ensure_dir(log_dir);
  const std::string run_ts = ts_for_filename();
  const std::string ecu_log_path = log_dir + "/ecu_" + run_ts + ".log";
  std::ofstream flog(ecu_log_path.c_str(), std::ios::out | std::ios::trunc);
  if (!flog) { std::cerr << "[ECU] ERROR opening log file: " << ecu_log_path << "\n"; return 2; }

#ifdef USE_XLSXWRITER
  const std::string xlsx_path = log_dir + "/adas_run_" + run_ts + ".xlsx";
  ExcelOut X = excel_open(xlsx_path);
  if (!X.ok) std::cerr << "[ECU] Failed to create Excel workbook: " << xlsx_path << "\n";
#endif

  time_t t_start_wall = std::time(nullptr);

  DDS::DomainParticipantFactory_var dpf = TheParticipantFactoryWithArgs(argc, argv);
  DDS::DomainParticipant_var dp = dpf->create_participant(42, PARTICIPANT_QOS_DEFAULT, 0, 0);
  if (!dp) { std::cerr << "DP create failed\n"; return 1; }

  // Register types + topics
  Adas::HeartbeatTypeSupport_var ts_hb = new Adas::HeartbeatTypeSupportImpl;
  Adas::ObjectsMsgTypeSupport_var ts_obj = new Adas::ObjectsMsgTypeSupportImpl;
  Adas::DetectionsMsgTypeSupport_var ts_det = new Adas::DetectionsMsgTypeSupportImpl;

  Adas::CalibrationRequestTypeSupport_var  ts_creq  = new Adas::CalibrationRequestTypeSupportImpl;
  Adas::CalibrationResponseTypeSupport_var ts_cresp = new Adas::CalibrationResponseTypeSupportImpl;
  Adas::CalibrationAckTypeSupport_var      ts_cack  = new Adas::CalibrationAckTypeSupportImpl;
  Adas::CalibrationReadyTypeSupport_var    ts_cready= new Adas::CalibrationReadyTypeSupportImpl;

  ts_hb->register_type(dp,""); ts_obj->register_type(dp,""); ts_det->register_type(dp,"");
  ts_creq->register_type(dp,""); ts_cresp->register_type(dp,"");
  ts_cack->register_type(dp,""); ts_cready->register_type(dp,"");

  // Classic topics
  DDS::Topic_var t_hb  = dp->create_topic("adas/heartbeat",  ts_hb->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_obj = dp->create_topic("adas/objects",    ts_obj->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_det = dp->create_topic("adas/detections", ts_det->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);

  // Calibration topics (shared)
  DDS::Topic_var t_cal_req   = dp->create_topic("adas/calib/request",        ts_creq->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_resp  = dp->create_topic("adas/calib/response",       ts_cresp->get_type_name(), TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ack_r = dp->create_topic("adas/calib/ack/radar",      ts_cack->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ack_e = dp->create_topic("adas/calib/ack/ecu",        ts_cack->get_type_name(),  TOPIC_QOS_DEFAULT, 0, 0);
  DDS::Topic_var t_cal_ready = dp->create_topic("adas/calib/ready",          ts_cready->get_type_name(),TOPIC_QOS_DEFAULT, 0, 0);

  DDS::Subscriber_var sub = dp->create_subscriber(SUBSCRIBER_QOS_DEFAULT, 0, 0);
  DDS::Publisher_var  pub = dp->create_publisher(PUBLISHER_QOS_DEFAULT, 0, 0);

  // Per-stream reader QoS

  // --- HB reader: RELIABLE + KEEP_ALL so nothing falls off the tail
  DDS::DataReaderQos hb_qos;  sub->get_default_datareader_qos(hb_qos);
  hb_qos.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;

  // Keep all samples and give enough headroom for 6×3000 frames
  hb_qos.history.kind = DDS::KEEP_ALL_HISTORY_QOS;
  hb_qos.resource_limits.max_samples = 100000;
  hb_qos.resource_limits.max_instances = 64;
  hb_qos.resource_limits.max_samples_per_instance = 4096;

  // Optional: receive pre-join samples from transient-local writers
  hb_qos.durability.kind = DDS::TRANSIENT_LOCAL_DURABILITY_QOS;

  hb_qos.latency_budget.duration.sec = 0;
  hb_qos.latency_budget.duration.nanosec = 1 * 1000 * 1000;


  // --- Reliable readers for OBJ/DET/Cal: KEEP_ALL to prevent tail-drop
  DDS::DataReaderQos rel_qos; sub->get_default_datareader_qos(rel_qos);
  rel_qos.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;

  rel_qos.history.kind = DDS::KEEP_ALL_HISTORY_QOS;
  rel_qos.resource_limits.max_samples = 100000;
  rel_qos.resource_limits.max_instances = 64;
  rel_qos.resource_limits.max_samples_per_instance = 4096;

  rel_qos.durability.kind = DDS::TRANSIENT_LOCAL_DURABILITY_QOS;

  rel_qos.latency_budget.duration.sec = 0;
  rel_qos.latency_budget.duration.nanosec = 5 * 1000 * 1000;



  // Writers QoS for calibration responses/acks (ECU→radar)
  DDS::DataWriterQos wq; pub->get_default_datawriter_qos(wq);
  wq.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;
  wq.history.kind = DDS::KEEP_LAST_HISTORY_QOS; wq.history.depth = 32;
  wq.transport_priority.value = 64; // higher than HB

  // Readers
  DDS::DataReader_var hb_dr  = sub->create_datareader(t_hb,  hb_qos, 0, 0);
  DDS::DataReader_var obj_dr = sub->create_datareader(t_obj, rel_qos, 0, 0);
  DDS::DataReader_var det_dr = sub->create_datareader(t_det, rel_qos, 0, 0);

  Adas::HeartbeatDataReader_var     hb_r  = Adas::HeartbeatDataReader::_narrow(hb_dr);
  Adas::ObjectsMsgDataReader_var    obj_r = Adas::ObjectsMsgDataReader ::_narrow(obj_dr);
  Adas::DetectionsMsgDataReader_var det_r = Adas::DetectionsMsgDataReader::_narrow(det_dr);

  // Calibration readers
  DDS::DataReader_var cal_ack_radar_dr = sub->create_datareader(t_cal_ack_r, rel_qos, 0, 0);
  DDS::DataReader_var cal_resp_dr      = sub->create_datareader(t_cal_resp,  rel_qos, 0, 0);
  DDS::DataReader_var cal_ready_dr     = sub->create_datareader(t_cal_ready, rel_qos, 0, 0);

  Adas::CalibrationAckDataReader_var      ack_radar_r = Adas::CalibrationAckDataReader::_narrow(cal_ack_radar_dr);
  Adas::CalibrationResponseDataReader_var resp_r      = Adas::CalibrationResponseDataReader::_narrow(cal_resp_dr);
  Adas::CalibrationReadyDataReader_var    ready_r     = Adas::CalibrationReadyDataReader::_narrow(cal_ready_dr);

  // Calibration writers
  DDS::DataWriter_var cal_req_dw     = pub->create_datawriter(t_cal_req,     wq, 0, 0);
  DDS::DataWriter_var cal_ack_ecu_dw = pub->create_datawriter(t_cal_ack_e,   wq, 0, 0);

  Adas::CalibrationRequestDataWriter_var req_w  = Adas::CalibrationRequestDataWriter::_narrow(cal_req_dw);
  Adas::CalibrationAckDataWriter_var     ack_w  = Adas::CalibrationAckDataWriter::_narrow(cal_ack_ecu_dw);

  // Match existing streams
  (void)wait_for_match(hb_dr,  "HB",  1, 8000, verbose);
  (void)wait_for_match(obj_dr, "OBJ", 1, 8000, verbose);
  (void)wait_for_match(det_dr, "DET", 1, 8000, verbose);

  // States and aggregations
  unordered_map<string, HBState>  hb_states;
  unordered_map<string, ObjState> obj_states;
  unordered_map<string, DetState> det_states;
  unordered_map<string, StreamAgg> AHB, AOBJ, ADET;

  // Calibration state
  std::mutex mx;
  unordered_map<uint32_t, ReqMeta> req_meta;           // by d1(req_id)
  map<pair<string,uint32_t>, CalTxn> tx_by_radar_req;  // (rid, req_id)
  unordered_map<string,int> ack_recv, resp_recv, ecu_ack_sent;
  unordered_map<string,bool> ready_seen;

  // Ready condition + WaitSet
  DDS::ReadCondition_var rc_hb    = hb_r   ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::ReadCondition_var rc_obj   = obj_r  ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::ReadCondition_var rc_det   = det_r  ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);

  DDS::ReadCondition_var rc_ack   = ack_radar_r->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::ReadCondition_var rc_resp  = resp_r     ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);
  DDS::ReadCondition_var rc_ready = ready_r    ->create_readcondition(DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);

  DDS::WaitSet ws;
  ws.attach_condition(rc_hb);
  ws.attach_condition(rc_obj);
  ws.attach_condition(rc_det);
  ws.attach_condition(rc_ack);
  ws.attach_condition(rc_resp);
  ws.attach_condition(rc_ready);

  const CORBA::Long MAX_BATCH = 64;

  // run window starts at FIRST RX, not process start
  auto end_at = steady_clock::now() + hours(24); // placeholder until first RX
  bool started_timer = false;

  bool first_rx_logged = false;
  time_t t_first_rx_wall = 0;

  // ---- calibration sender arming (NEW) ----
  static std::atomic<bool>    sender_go{false};
  static std::atomic<int64_t> sender_t0_ns{0}; // steady_clock::time_point as nanoseconds

  auto start_timer_if_needed = [&](){
    if (!started_timer) {
      const auto t0 = steady_clock::now();
      end_at = t0 + seconds(RUN_FOR_S);
      started_timer = true;

      // arm calibration sender exactly at FIRST_RX time
      sender_t0_ns.store(duration_cast<nanoseconds>(t0.time_since_epoch()).count(), std::memory_order_release);
      sender_go.store(true, std::memory_order_release);
    }
  };

  auto log_primed = [&](const char* tag, const std::string& rid, uint32_t seq, uint64_t ts_src, uint64_t ts_rx, uint64_t e2e, const char* extra="") {
    if (!first_rx_logged) {
      flog << "[ECU] RUN timer started (FIRST_RX) — will stop in " << RUN_FOR_S << "s\n";
      first_rx_logged = true;
      t_first_rx_wall = std::time(nullptr);
    }
    start_timer_if_needed();
    flog << "[ADAS][RX][" << tag << "][" << rid << "] 🔄 primed seq=" << seq
         << " ts_src=" << ts_src << " ts_rx=" << ts_rx << " e2e=" << e2e << "ms";
    if (extra && *extra) flog << " | " << extra;
    flog << "\n";
  };
  auto log_ok = [&](const char* tag, const std::string& rid, uint32_t seq, uint64_t ts_src, uint64_t ts_rx, uint64_t e2e, uint64_t jitter, const char* extra="") {
    flog << "[ADAS][RX][" << tag << "][" << rid << "] ✅ seq=" << seq
         << " ts_src=" << ts_src << " ts_rx=" << ts_rx << " e2e=" << e2e << "ms | jitter=" << jitter << "ms";
    if (extra && *extra) flog << " | " << extra;
    flog << "\n";
  };
  auto log_warn = [&](const char* tag, const std::string& rid, uint64_t e2e, uint64_t jitter, uint32_t seq, int streak=-1) {
    flog << "[ADAS][RX][" << tag << "][" << rid << "] ⚠️ E2E:" << e2e << "ms | jitter:" << jitter << "ms (seq=" << seq;
    if (streak >= 0) flog << ", streak=" << streak;
    flog << ")\n";
  };
  auto log_fault = [&](const char* tag, const std::string& rid, uint64_t e2e, uint64_t jitter, uint32_t seq, uint64_t& fault_count, bool hb_style=false) {
    if (hb_style) {
      flog << "[ADAS][RX][" << tag << "][" << rid << "] ❌ E2E:" << e2e << "ms | jitter:" << jitter
           << "ms (seq=" << seq << ") | health=OK | fault_count=" << ++fault_count << "\n";
    } else {
      flog << "[ADAS][RX][" << tag << "][" << rid << "] ❌ FAULT (3 consecutive violations) | fault_count=" << ++fault_count << "\n";
    }
  };

#ifdef USE_XLSXWRITER
  auto write_xls = [&](lxw_worksheet* sh, uint32_t& row,
                       const std::string& rid, uint32_t seq,
                       uint64_t ts_src, uint64_t ts_rx,
                       uint64_t e2e, uint64_t jitter,
                       const std::string& status){
    if (X.ok) excel_write_row(sh, row, rid, seq, ts_src, ts_rx, e2e, jitter, status);
  };
#endif

  // -------- HB/OBJ/DET drainers --------
  auto drain_hb = [&](){
    Adas::HeartbeatSeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = hb_r->take_w_condition(data, info, MAX_BATCH, rc_hb);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;
      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::Heartbeat& hb = data[i];
        add(M.rx.hb, sz_payload(hb));

        const string rid = hb.radar_id.in();
        const uint64_t rx = now_ms64(); const uint64_t e2e = rx - hb.ts_ms;

        auto& st = hb_states[rid]; auto& ag = AHB[rid];
        if (!st.primed) {
          st.primed=true; st.last_seq=hb.seq; st.last_rx_ms=rx; st.last_src_ms=hb.ts_ms; st.loss_streak=0;
          log_primed("HB", rid, hb.seq, hb.ts_ms, rx, e2e, "health=OK");
#ifdef USE_XLSXWRITER
          write_xls(X.sh_hb, X.row_hb, rid, hb.seq, hb.ts_ms, rx, e2e, 0, "PRIMED");
#endif
          continue;
        }

        // RX-spacing jitter (vSOME/IP): |Δ(RX inter-arrival) - period|
        const uint64_t delta  = rx - st.last_rx_ms;
        const uint64_t jitter = (delta > HB_PERIOD_MS) ? (delta - HB_PERIOD_MS) : (HB_PERIOD_MS - delta);
        st.last_seq = hb.seq; st.last_rx_ms = rx; st.last_src_ms = hb.ts_ms;

        ag.total++; ag.max_e2e=max<uint64_t>(ag.max_e2e,e2e); ag.max_jitter=max<uint64_t>(ag.max_jitter,jitter);

        if (e2e>HB_E2E_MAX_MS || jitter>HB_JITTER_MAX_MS) {
          ag.viol++; // increment faults happens inside log_fault for HB
          log_fault("HB", rid, e2e, jitter, hb.seq, ag.faults, true);
  #ifdef USE_XLSXWRITER
          write_xls(X.sh_hb, X.row_hb, rid, hb.seq, hb.ts_ms, rx, e2e, jitter, "VIOL");
  #endif
        } else {

          ag.ok++;
          log_ok("HB", rid, hb.seq, hb.ts_ms, rx, e2e, jitter, "health=OK");
#ifdef USE_XLSXWRITER
          write_xls(X.sh_hb, X.row_hb, rid, hb.seq, hb.ts_ms, rx, e2e, jitter, "OK");
#endif
        }
      }
      hb_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  auto drain_obj = [&](){
    Adas::ObjectsMsgSeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = obj_r->take_w_condition(data, info, MAX_BATCH, rc_obj);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;
      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::ObjectsMsg& om = data[i];
        add(M.rx.obj, sz_payload(om));

        const string rid = om.radar_id.in();
        const uint64_t rx = now_ms64(); const uint64_t e2e = rx - om.ts_ms;

        auto& st = obj_states[rid]; auto& ag = AOBJ[rid];
        if (!st.primed) {
          st.primed=true; st.last_seq=om.seq; st.last_rx_ms=rx; st.last_src_ms=om.ts_ms; st.loss_streak=0;
          log_primed("OBJ", rid, om.seq, om.ts_ms, rx, e2e);
#ifdef USE_XLSXWRITER
          write_xls(X.sh_obj, X.row_obj, rid, om.seq, om.ts_ms, rx, e2e, 0, "PRIMED");
#endif
          continue;
        }

        // RX-spacing jitter (vSOME/IP): |Δ(RX inter-arrival) - period|
        const uint64_t delta  = rx - st.last_rx_ms;
        const uint64_t jitter = (delta > OBJ_PERIOD_MS) ? (delta - OBJ_PERIOD_MS) : (OBJ_PERIOD_MS - delta);
        st.last_seq=om.seq; st.last_rx_ms=rx; st.last_src_ms=om.ts_ms;

        ag.total++; ag.max_e2e=max<uint64_t>(ag.max_e2e,e2e); ag.max_jitter=max<uint64_t>(ag.max_jitter,jitter);

        if (e2e>OBJ_E2E_MAX_MS || jitter>OBJ_JITTER_MAX_MS) {
          st.loss_streak++; ag.viol++;
          log_warn("OBJ", rid, e2e, jitter, om.seq, st.loss_streak);
          if (st.loss_streak>=OBJ_CONSEC_FAULT) {
            log_fault("OBJ", rid, e2e, jitter, om.seq, ag.faults, false);
            st.loss_streak = 0;
          }
#ifdef USE_XLSXWRITER
          write_xls(X.sh_obj, X.row_obj, rid, om.seq, om.ts_ms, rx, e2e, jitter, "VIOL");
#endif
        } else {
          st.loss_streak=0; ag.ok++;
          log_ok("OBJ", rid, om.seq, om.ts_ms, rx, e2e, jitter);
#ifdef USE_XLSXWRITER
          write_xls(X.sh_obj, X.row_obj, rid, om.seq, om.ts_ms, rx, e2e, jitter, "OK");
#endif
        }
      }
      obj_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  auto drain_det = [&](){
    Adas::DetectionsMsgSeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = det_r->take_w_condition(data, info, MAX_BATCH, rc_det);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;
      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::DetectionsMsg& dm = data[i];
        add(M.rx.det, sz_payload(dm));

        const string rid = dm.radar_id.in();
        const uint64_t rx = now_ms64(); const uint64_t e2e = rx - dm.ts_ms;

        auto& st = det_states[rid]; auto& ag = ADET[rid];
        if (!st.primed) {
          st.primed=true; st.last_seq=dm.seq; st.last_rx_ms=rx; st.last_src_ms=dm.ts_ms; st.loss_streak=0;
          log_primed("DET", rid, dm.seq, dm.ts_ms, rx, e2e);
#ifdef USE_XLSXWRITER
          write_xls(X.sh_det, X.row_det, rid, dm.seq, dm.ts_ms, rx, e2e, 0, "PRIMED");
#endif
          continue;
        }

        // RX-spacing jitter (vSOME/IP): |Δ(RX inter-arrival) - period|
        const uint64_t delta  = rx - st.last_rx_ms;
        const uint64_t jitter = (delta > DET_PERIOD_MS) ? (delta - DET_PERIOD_MS) : (DET_PERIOD_MS - delta);
        st.last_seq=dm.seq; st.last_rx_ms=rx; st.last_src_ms=dm.ts_ms;

        ag.total++; ag.max_e2e=max<uint64_t>(ag.max_e2e,e2e); ag.max_jitter=max<uint64_t>(ag.max_jitter,jitter);

        if (e2e>DET_E2E_MAX_MS || jitter>DET_JITTER_MAX_MS) {
          st.loss_streak++; ag.viol++;
          log_warn("DET", rid, e2e, jitter, dm.seq, st.loss_streak);
          if (st.loss_streak>=DET_CONSEC_FAULT) {
            log_fault("DET", rid, e2e, jitter, dm.seq, ag.faults, false);
            st.loss_streak = 0;
          }
#ifdef USE_XLSXWRITER
          write_xls(X.sh_det, X.row_det, rid, dm.seq, dm.ts_ms, rx, e2e, jitter, "VIOL");
#endif
        } else {
          st.loss_streak=0; ag.ok++;
          log_ok("DET", rid, dm.seq, dm.ts_ms, rx, e2e, jitter);
#ifdef USE_XLSXWRITER
          write_xls(X.sh_det, X.row_det, rid, dm.seq, dm.ts_ms, rx, e2e, jitter, "OK");
#endif
        }
      }
      det_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  // -------- Calibration drainers --------
  auto finalize_if_ready = [&](const std::string& rid, uint32_t req_id){
    std::lock_guard<std::mutex> g(mx);
    auto it = tx_by_radar_req.find({rid, req_id});
    if (it == tx_by_radar_req.end()) return;
    CalTxn& t = it->second;
    if (!t.ecu_send_ts || !t.radar_ack_rx_ts || !t.radar_resp_ts || !t.ecu_ack_ts) return;

    int l1 = (int)t.radar_ack_rx_ts - (int)t.ecu_send_ts;
    int l2 = (int)t.radar_resp_ts   - (int)t.radar_ack_rx_ts;
    int l3 = (int)t.ecu_ack_ts      - (int)t.radar_resp_ts;

    // Log single-line TXN
    flog << "[CAL][TXN] radar=" << rid << " req_id=" << req_id
         << " ECU->ACK=" << l1 << "ms, ACK->RESP=" << l2 << "ms, RESP->ECU=" << l3 << "ms "
         << "| req=(" << t.req_d1 << "," << t.req_d2 << "," << t.req_d3 << "," << t.req_d4 << ") "
         << "resp=(" << t.resp_c << "," << t.resp_d << ")\n";

#ifdef USE_XLSXWRITER
    excel_write_calib_txn(X, rid, req_id, t.ecu_send_ts, t.radar_ack_rx_ts, t.radar_resp_ts, t.ecu_ack_ts,
                          t.req_d1, t.req_d2, t.req_d3, t.req_d4, t.resp_c, t.resp_d,
                          l1, l2, l3);
#endif
    tx_by_radar_req.erase(it);
  };

  auto drain_ready = [&](){
    Adas::CalibrationReadySeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = ready_r->take_w_condition(data, info, MAX_BATCH, rc_ready);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;
      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::CalibrationReady& rd = data[i];
        add(M.rx.ready, sz_payload(rd));

        std::string rid = rd.radar_id.in();
        if (!ready_seen[rid]) {
          ready_seen[rid] = true;
          flog << "[READY] radar=" << rid << "\n";
        }
      }
      ready_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  auto drain_ack = [&](){
    Adas::CalibrationAckSeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = ack_radar_r->take_w_condition(data, info, MAX_BATCH, rc_ack);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;

      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::CalibrationAck& a = data[i];
        add(M.rx.cal_ack, sz_payload(a));
        const std::string rid = a.radar_id.in();
        ack_recv[rid]++;

        {
          std::lock_guard<std::mutex> g(mx);
          auto key = std::make_pair(rid, a.d1);
          CalTxn& t = tx_by_radar_req[key];
          t.rid = rid;
          t.req_id = a.d1;
          t.radar_ack_rx_ts = a.d2;

          auto rm_it = req_meta.find(a.d1);
          if (rm_it != req_meta.end()) {
            t.ecu_send_ts = rm_it->second.ecu_send_ts;
            t.req_d1 = rm_it->second.d1;
            t.req_d2 = rm_it->second.d2;
            t.req_d3 = rm_it->second.d3;
            t.req_d4 = rm_it->second.d4;
          }
        }

        finalize_if_ready(rid, a.d1);
      }
      ack_radar_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  auto drain_resp = [&](){
    Adas::CalibrationResponseSeq data; DDS::SampleInfoSeq info;
    while (true) {
      DDS::ReturnCode_t rc = resp_r->take_w_condition(data, info, MAX_BATCH, rc_resp);
      if (rc == DDS::RETCODE_NO_DATA) break;
      if (rc != DDS::RETCODE_OK) break;

      for (CORBA::ULong i=0;i<data.length();++i) {
        if (!info[i].valid_data) continue;
        const Adas::CalibrationResponse& r = data[i];
        add(M.rx.cal_resp, sz_payload(r));
        const std::string rid = r.radar_id.in();
        resp_recv[rid]++;

        // Immediately send ECU-ACK back to radar
        Adas::CalibrationAck ack;
        ack.radar_id = rid.c_str();
        ack.d1 = r.d1;                   // req id
        ack.d2 = now_ms32();             // ecu rx ts
        ack.d3 = r.d3;
        ack.d4 = r.d4;
        ack_w->write(ack, DDS::HANDLE_NIL);
        add(M.tx.cal_ack, sz_payload(ack));
        ecu_ack_sent[rid]++;

        {
          std::lock_guard<std::mutex> g(mx);
          auto key = std::make_pair(rid, r.d1);
          CalTxn& t = tx_by_radar_req[key];
          t.rid = rid;
          t.req_id = r.d1;
          t.radar_resp_ts = r.d2;
          t.resp_c = r.d3;
          t.resp_d = r.d4;
          t.ecu_ack_ts = ack.d2;

          auto rm_it = req_meta.find(r.d1);
          if (rm_it != req_meta.end()) {
            if (!t.ecu_send_ts) t.ecu_send_ts = rm_it->second.ecu_send_ts;
            t.req_d1 = rm_it->second.d1;
            t.req_d2 = rm_it->second.d2;
            t.req_d3 = rm_it->second.d3;
            t.req_d4 = rm_it->second.d4;
          }
        }

        finalize_if_ready(rid, r.d1);
      }
      resp_r->return_loan(data, info);
      if (data.length() < (CORBA::ULong)MAX_BATCH) break;
    }
  };

  // -------- Calibration 1 Hz sender (UPDATED: deterministic & bounded) --------
  std::atomic<bool> sender_running{true};
  std::thread sender([&](){
    std::mt19937 rng((uint32_t)std::random_device{}());

    // wait until we are armed at FIRST_RX
    while (sender_running.load(std::memory_order_acquire) && !sender_go.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!sender_running.load(std::memory_order_acquire)) return;

    // reconstruct t0 from atomic ns
    int64_t t0ns = sender_t0_ns.load(std::memory_order_acquire);
    auto t0 = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(t0ns));

    const auto period = std::chrono::milliseconds(CALIB_PERIOD_MS);
    const int target = (RUN_FOR_S * 1000) / CALIB_PERIOD_MS; // exact count (e.g., 60)
    auto next_deadline = t0 + period; // first tick at t0+1000ms
    int sent = 0;

    auto send_once = [&](){
      uint32_t d1 = (uint32_t)rng();
      uint32_t d2 = (uint32_t)rng();
      uint32_t d3 = (uint32_t)rng();
      uint32_t d4 = (uint32_t)rng();

      Adas::CalibrationRequest req;
      req.radar_id = "";  // broadcast
      req.d1 = d1; req.d2 = d2; req.d3 = d3; req.d4 = d4;

      uint32_t ecu_ts = now_ms32();
      req_w->write(req, DDS::HANDLE_NIL);
      add(M.tx.cal_req, sz_payload(req));

      {
        std::lock_guard<std::mutex> g(mx);
        ReqMeta rm; rm.ecu_send_ts = ecu_ts; rm.d1=d1; rm.d2=d2; rm.d3=d3; rm.d4=d4;
        req_meta[d1] = rm;
      }

      flog << "[CAL][SEND] req_id=" << d1 << " ecu_ts=" << ecu_ts
           << " data=(" << d1 << "," << d2 << "," << d3 << "," << d4 << ")\n";
    };

    while (sender_running.load(std::memory_order_acquire) && sent < target) {
      auto now = std::chrono::steady_clock::now();
      if (now < next_deadline) {
        auto sleep_for = std::min<std::chrono::milliseconds>(std::chrono::milliseconds(5),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(next_deadline - now));
        if (sleep_for.count() > 0) std::this_thread::sleep_for(sleep_for);
        continue;
      }
      // deadline reached (or late): send one
      send_once();
      ++sent;
      next_deadline += period;
    }
    // done; exit thread
  });

  // -------- Main wait loop (window ends exactly RUN_FOR_S after first RX) --------
  while (steady_clock::now() < end_at) 

  // ---- Final bounded drain to catch tail arrivals (~1.5s) ----
  {
    DDS::ConditionSeq active;
    DDS::Duration_t to = {0, 50 * 1000 * 1000}; // 50ms
    auto drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < drain_until) {
      if (ws.wait(active, to) == DDS::RETCODE_OK) {
        for (CORBA::ULong i=0; i<active.length(); ++i) {
          if (active[i] == rc_hb)       drain_hb();
          else if (active[i] == rc_obj) drain_obj();
          else if (active[i] == rc_det) drain_det();
          else if (active[i] == rc_ack) drain_ack();
          else if (active[i] == rc_resp) drain_resp();
          else if (active[i] == rc_ready) drain_ready();
        }
      }
    }
  }


  // cleanup conditions
  ws.detach_condition(rc_hb); ws.detach_condition(rc_obj); ws.detach_condition(rc_det);
  ws.detach_condition(rc_ack); ws.detach_condition(rc_resp); ws.detach_condition(rc_ready);

  hb_r->delete_readcondition(rc_hb); obj_r->delete_readcondition(rc_obj); det_r->delete_readcondition(rc_det);
  ack_radar_r->delete_readcondition(rc_ack); resp_r->delete_readcondition(rc_resp); ready_r->delete_readcondition(rc_ready);

  sender_running = false;
  if (sender.joinable()) sender.join();

#ifdef USE_XLSXWRITER
  if (X.ok) {
    // SUMMARY sheet
    excel_write_summary_rows(X, AHB,  "HB");
    excel_write_summary_rows(X, AOBJ, "OBJ");
    excel_write_summary_rows(X, ADET, "DET");

    // Calibration summary
    std::set<std::string> all_rids;
    for (auto& kv: ack_recv) all_rids.insert(kv.first);
    for (auto& kv: resp_recv) all_rids.insert(kv.first);
    for (auto& kv: ecu_ack_sent) all_rids.insert(kv.first);
    for (auto& kv: ready_seen) all_rids.insert(kv.first);
    for (const auto& rid : all_rids) {
      excel_write_calib_summary_row(
        X, rid,
        ack_recv[rid], resp_recv[rid], ecu_ack_sent[rid],
        ready_seen[rid]
      );
    }

    // INFO sheet
    uint32_t r = 1;
    time_t t_end_wall = std::time(nullptr);
    excel_write_info_kv(X.sh_info, r, "generated_at", ts_human(t_end_wall));
    excel_write_info_kv(X.sh_info, r, "run_seconds",  std::to_string(RUN_FOR_S));
    excel_write_info_kv(X.sh_info, r, "first_rx_at",  t_first_rx_wall ? ts_human(t_first_rx_wall) : "(no data)");
    excel_write_info_kv(X.sh_info, r, "ecu_log_path",  ecu_log_path);
    excel_write_info_kv(X.sh_info, r, "xlsx_path",     X.path);
    excel_write_info_kv(X.sh_info, r, "dcps_config",   ini_path.empty() ? "(not provided)" : ini_path);
    excel_write_info_kv_num(X.sh_info, r, "domain_id", 42);

    // thresholds
    excel_write_info_kv_num(X.sh_info, r, "HB_PERIOD_MS", HB_PERIOD_MS);
    excel_write_info_kv_num(X.sh_info, r, "HB_E2E_MAX_MS", HB_E2E_MAX_MS);
    excel_write_info_kv_num(X.sh_info, r, "HB_JITTER_MAX_MS", HB_JITTER_MAX_MS);

    excel_write_info_kv_num(X.sh_info, r, "OBJ_PERIOD_MS", OBJ_PERIOD_MS);
    excel_write_info_kv_num(X.sh_info, r, "OBJ_E2E_MAX_MS", OBJ_E2E_MAX_MS);
    excel_write_info_kv_num(X.sh_info, r, "OBJ_JITTER_MAX_MS", OBJ_JITTER_MAX_MS);
    excel_write_info_kv_num(X.sh_info, r, "OBJ_CONSEC_FAULT", OBJ_CONSEC_FAULT);

    excel_write_info_kv_num(X.sh_info, r, "DET_PERIOD_MS", DET_PERIOD_MS);
    excel_write_info_kv_num(X.sh_info, r, "DET_E2E_MAX_MS", DET_E2E_MAX_MS);
    excel_write_info_kv_num(X.sh_info, r, "DET_JITTER_MAX_MS", DET_JITTER_MAX_MS);
    excel_write_info_kv_num(X.sh_info, r, "DET_CONSEC_FAULT", DET_CONSEC_FAULT);

    excel_write_info_kv_num(X.sh_info, r, "CALIB_PERIOD_MS", CALIB_PERIOD_MS);

    excel_write_info_kv(X.sh_info, r, "notes",
      "Jitter = |Δ(RX inter-arrival) - period| (vSOME/IP style); HB violations counted as faults; OBJ/DET faults require 3 consecutive violations. "
      "Window starts at first RX and lasts exactly RUN_FOR_S seconds. Calibration sheets contain counts and per-request latencies L1/L2/L3.");

    excel_close(X);
  }
#endif

  // ---- terminal summaries ----
  auto bytes_to_mbps = [](uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return 0.0;
    return (bytes * 8.0) / seconds / 1e6;
  };

  uint64_t rx_total_bytes =
    M.rx.hb.bytes + M.rx.obj.bytes + M.rx.det.bytes +
    M.rx.cal_ack.bytes + M.rx.cal_resp.bytes + M.rx.ready.bytes;
  uint64_t tx_total_bytes = M.tx.cal_req.bytes + M.tx.cal_ack.bytes;
  double run_secs = RUN_FOR_S;

  if (print_summary) {
    // BYTE summary (payload only)
    std::cout << "\n[ADAS][METRICS] ===== BYTE SUMMARY (payload only) =====\n";
    std::cout << "RX:\n";
    std::cout << "  HB:        count=" << M.rx.hb.count        << " payload=" << M.rx.hb.bytes        << " B\n";
    std::cout << "  OBJ:       count=" << M.rx.obj.count       << " payload=" << M.rx.obj.bytes       << " B\n";
    std::cout << "  DET:       count=" << M.rx.det.count       << " payload=" << M.rx.det.bytes       << " B\n";
    std::cout << "  CAL-ACK:   count=" << M.rx.cal_ack.count   << " payload=" << M.rx.cal_ack.bytes   << " B\n";
    std::cout << "  CAL-RESP:  count=" << M.rx.cal_resp.count  << " payload=" << M.rx.cal_resp.bytes  << " B\n";
    std::cout << "  READY:     count=" << M.rx.ready.count     << " payload=" << M.rx.ready.bytes     << " B\n";
    std::cout << "  RX TOTAL:  payload=" << rx_total_bytes << " B\n";
    std::cout << "  RX avg payload bitrate≈ " << std::fixed << std::setprecision(3)
              << bytes_to_mbps(rx_total_bytes, run_secs) << " Mbit/s\n";

    std::cout << "TX:\n";
    std::cout << "  CAL-REQ(broadcast):   count=" << M.tx.cal_req.count << " payload=" << M.tx.cal_req.bytes << " B\n";
    std::cout << "  CAL-ACK(ECU→radar):   count=" << M.tx.cal_ack.count << " payload=" << M.tx.cal_ack.bytes << " B\n";
    std::cout << "  TX TOTAL:  payload=" << tx_total_bytes << " B\n";
    std::cout << "  TX avg payload bitrate≈ " << std::fixed << std::setprecision(3)
              << bytes_to_mbps(tx_total_bytes, run_secs) << " Mbit/s\n";

    // Stream QoS summary (OK/Viol/Faults etc.)
    std::cout << "\n[ADAS][SUMMARY] Streams (HB/OBJ/DET) over " << RUN_FOR_S << "s\n";
    auto printStream = [&](const char* tag, const unordered_map<string, StreamAgg>& MAP) {
      for (const auto& kv : MAP) {
        const auto& rid = kv.first; const auto& a = kv.second;
        double okp = a.total ? (100.0 * (double)a.ok / (double)a.total) : 0.0;
        std::cout << "  Radar_" << rid << " [" << tag << "]"
                  << " Total=" << a.total
                  << " OK=" << a.ok
                  << " Viol=" << a.viol
                  << " Faults=" << a.faults
                  << " MaxE2E=" << a.max_e2e << "ms"
                  << " MaxJitter=" << a.max_jitter << "ms"
                  << " OK%=" << std::setprecision(6) << okp << std::setprecision(6) << "\n";
      }
    };
    printStream("HB",  AHB);
    printStream("OBJ", AOBJ);
    printStream("DET", ADET);

    // Calibration counts per radar
    std::cout << "[ADAS][SUMMARY] Calibration counts per radar\n";
    std::set<std::string> all_rids;
    for (auto& kv: ack_recv) all_rids.insert(kv.first);
    for (auto& kv: resp_recv) all_rids.insert(kv.first);
    for (auto& kv: ecu_ack_sent) all_rids.insert(kv.first);
    for (auto& kv: ready_seen) all_rids.insert(kv.first);
    for (const auto& rid : all_rids) {
      std::cout << "  Radar_" << rid
                << " ACK(recv)=" << ack_recv[rid]
                << " RESP(recv)=" << resp_recv[rid]
                << " ECU-ACK(sent)=" << ecu_ack_sent[rid]
                << " ReadySeen=" << (ready_seen[rid] ? "yes" : "no")
                << "\n";
    }
  }

  flog.flush();
  dp->delete_contained_entities();
  dpf->delete_participant(dp);
  TheServiceParticipant->shutdown();
  return 0;
}
