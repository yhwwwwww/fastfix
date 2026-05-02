#include "nimblefix/runtime/diagnostics.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace nimble::runtime {

namespace {

inline constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

[[nodiscard]] auto
NowUnixNs() -> std::uint64_t
{
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return count <= 0 ? 0U : static_cast<std::uint64_t>(count);
}

[[nodiscard]] auto
TraceKindToString(TraceEventKind kind) -> std::string_view
{
  switch (kind) {
    case TraceEventKind::kConfigLoaded:
      return "config_loaded";
    case TraceEventKind::kProfileLoaded:
      return "profile_loaded";
    case TraceEventKind::kSessionRegistered:
      return "session_registered";
    case TraceEventKind::kPendingConnectionRegistered:
      return "pending_connection_registered";
    case TraceEventKind::kSessionEvent:
      return "session_event";
    case TraceEventKind::kStoreEvent:
      return "store_event";
    case TraceEventKind::kScheduleEvent:
      return "schedule_event";
  }
  return "unknown";
}

[[nodiscard]] auto
TraceText(const TraceEvent& event) -> std::string_view
{
  std::size_t size = 0U;
  while (size < event.text.size() && event.text[size] != '\0') {
    ++size;
  }
  return std::string_view(event.text.data(), size);
}

auto
AppendJsonString(std::string& out, std::string_view value) -> void
{
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const auto raw_ch : value) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (ch < 0x20U) {
          out.append("\\u00");
          out.push_back(kHex[(ch >> 4U) & 0x0FU]);
          out.push_back(kHex[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

template<class Value>
auto
AppendNumberField(std::string& out, std::string_view name, Value value, bool trailing_comma = true) -> void
{
  out.push_back('"');
  out.append(name);
  out.append("\":");
  out.append(std::to_string(value));
  if (trailing_comma) {
    out.push_back(',');
  }
}

auto
AppendSessionJson(std::string& out, const RuntimeMetricsSnapshot::SessionEntry& session) -> void
{
  out.push_back('{');
  AppendNumberField(out, "session_id", session.session_id);
  AppendNumberField(out, "worker_id", session.worker_id);
  AppendNumberField(out, "inbound_messages", session.inbound_messages);
  AppendNumberField(out, "outbound_messages", session.outbound_messages);
  AppendNumberField(out, "admin_messages", session.admin_messages);
  AppendNumberField(out, "resend_requests", session.resend_requests);
  AppendNumberField(out, "gap_fills", session.gap_fills);
  AppendNumberField(out, "parse_failures", session.parse_failures);
  AppendNumberField(out, "checksum_failures", session.checksum_failures);
  AppendNumberField(out, "outbound_queue_depth", session.outbound_queue_depth);
  AppendNumberField(out, "last_store_flush_latency_ns", session.last_store_flush_latency_ns, false);
  out.push_back('}');
}

auto
AppendHistogramEntryJson(std::string& out,
                         std::string_view name,
                         const RuntimeMetricsSnapshot::WorkerEntry::HistogramEntry& histogram,
                         bool trailing_comma = true) -> void
{
  out.push_back('"');
  out.append(name);
  out.append("\":{");
  AppendNumberField(out, "p50_ns", histogram.p50_ns);
  AppendNumberField(out, "p90_ns", histogram.p90_ns);
  AppendNumberField(out, "p99_ns", histogram.p99_ns);
  AppendNumberField(out, "p999_ns", histogram.p999_ns);
  AppendNumberField(out, "count", histogram.count);
  AppendNumberField(out, "sum_ns", histogram.sum_ns, false);
  out.push_back('}');
  if (trailing_comma) {
    out.push_back(',');
  }
}

auto
AppendWorkerHistogramsJson(std::string& out,
                           const RuntimeMetricsSnapshot::WorkerEntry::WorkerHistogramSnapshot& histograms) -> void
{
  out.append("\"histograms\":{");
  AppendHistogramEntryJson(out, "session_inbound_latency_ns", histograms.session_inbound_latency_ns);
  AppendHistogramEntryJson(out, "encode_latency_ns", histograms.encode_latency_ns);
  AppendHistogramEntryJson(out, "parse_latency_ns", histograms.parse_latency_ns);
  AppendHistogramEntryJson(out, "store_flush_latency_ns", histograms.store_flush_latency_ns);
  AppendHistogramEntryJson(out, "send_latency_ns", histograms.send_latency_ns, false);
  out.push_back('}');
}

auto
AppendWorkerJson(std::string& out, const RuntimeMetricsSnapshot::WorkerEntry& worker) -> void
{
  out.push_back('{');
  AppendNumberField(out, "worker_id", worker.worker_id);
  AppendNumberField(out, "registered_sessions", worker.registered_sessions);
  AppendNumberField(out, "inbound_messages", worker.inbound_messages);
  AppendNumberField(out, "outbound_messages", worker.outbound_messages);
  AppendNumberField(out, "admin_messages", worker.admin_messages);
  AppendNumberField(out, "resend_requests", worker.resend_requests);
  AppendNumberField(out, "gap_fills", worker.gap_fills);
  AppendNumberField(out, "parse_failures", worker.parse_failures);
  AppendNumberField(out, "checksum_failures", worker.checksum_failures);
  AppendNumberField(out, "outbound_queue_depth", worker.outbound_queue_depth);
  AppendNumberField(out, "last_store_flush_latency_ns", worker.last_store_flush_latency_ns);
  AppendNumberField(out, "poll_wait_ns", worker.poll_wait_ns);
  AppendNumberField(out, "recv_dispatch_ns", worker.recv_dispatch_ns);
  AppendNumberField(out, "app_callback_ns", worker.app_callback_ns);
  AppendNumberField(out, "timer_process_ns", worker.timer_process_ns);
  AppendNumberField(out, "send_ns", worker.send_ns);
  AppendNumberField(out, "poll_iterations", worker.poll_iterations);
  AppendNumberField(out, "plain_connections", worker.plain_connections);
  AppendNumberField(out, "tls_connections", worker.tls_connections);
  AppendNumberField(out, "tls_handshake_successes", worker.tls_handshake_successes);
  AppendNumberField(out, "tls_handshake_failures", worker.tls_handshake_failures);
  AppendNumberField(out, "tls_handshake_latency_ns", worker.tls_handshake_latency_ns);
  AppendNumberField(out, "tls_session_resumptions", worker.tls_session_resumptions);
  AppendWorkerHistogramsJson(out, worker.histograms);
  out.push_back('}');
}

[[nodiscard]] auto
MetricsToJson(const RuntimeMetricsSnapshot& snapshot) -> std::string
{
  std::string out;
  out.reserve(128U + (snapshot.workers.size() * 512U) + (snapshot.sessions.size() * 256U));
  out.append("{\"type\":\"metrics\",\"workers\":[");
  for (std::size_t index = 0U; index < snapshot.workers.size(); ++index) {
    if (index != 0U) {
      out.push_back(',');
    }
    AppendWorkerJson(out, snapshot.workers[index]);
  }
  out.append("],\"sessions\":[");
  for (std::size_t index = 0U; index < snapshot.sessions.size(); ++index) {
    if (index != 0U) {
      out.push_back(',');
    }
    AppendSessionJson(out, snapshot.sessions[index]);
  }
  out.append("]}");
  return out;
}

[[nodiscard]] auto
HealthToJson(const EngineHealthSnapshot& health) -> std::string
{
  std::string out;
  out.reserve(256U);
  out.append("{\"type\":\"health\",");
  AppendNumberField(out, "worker_count", health.worker_count);
  AppendNumberField(out, "registered_sessions", health.registered_sessions);
  AppendNumberField(out, "total_inbound_messages", health.total_inbound_messages);
  AppendNumberField(out, "total_outbound_messages", health.total_outbound_messages);
  AppendNumberField(out, "total_parse_failures", health.total_parse_failures);
  AppendNumberField(out, "total_checksum_failures", health.total_checksum_failures);
  AppendNumberField(out, "total_resend_requests", health.total_resend_requests);
  AppendNumberField(out, "total_gap_fills", health.total_gap_fills);
  AppendNumberField(out, "uptime_ns", health.uptime_ns, false);
  out.push_back('}');
  return out;
}

[[nodiscard]] auto
MetricsToText(const RuntimeMetricsSnapshot& snapshot) -> std::string
{
  std::ostringstream out;
  out << "metrics workers=" << snapshot.workers.size() << " sessions=" << snapshot.sessions.size() << '\n';
  const auto append_histogram = [&out](std::string_view name,
                                       const RuntimeMetricsSnapshot::WorkerEntry::HistogramEntry& histogram) {
    out << ' ' << name << "_count=" << histogram.count << ' ' << name << "_sum_ns=" << histogram.sum_ns << ' ' << name
        << "_p50_ns=" << histogram.p50_ns << ' ' << name << "_p90_ns=" << histogram.p90_ns << ' ' << name
        << "_p99_ns=" << histogram.p99_ns << ' ' << name << "_p999_ns=" << histogram.p999_ns;
  };
  for (const auto& worker : snapshot.workers) {
    out << "worker id=" << worker.worker_id << " registered_sessions=" << worker.registered_sessions
        << " inbound_messages=" << worker.inbound_messages << " outbound_messages=" << worker.outbound_messages
        << " parse_failures=" << worker.parse_failures << " checksum_failures=" << worker.checksum_failures
        << " resend_requests=" << worker.resend_requests << " gap_fills=" << worker.gap_fills;
    append_histogram("session_inbound_latency_ns", worker.histograms.session_inbound_latency_ns);
    append_histogram("encode_latency_ns", worker.histograms.encode_latency_ns);
    append_histogram("parse_latency_ns", worker.histograms.parse_latency_ns);
    append_histogram("store_flush_latency_ns", worker.histograms.store_flush_latency_ns);
    append_histogram("send_latency_ns", worker.histograms.send_latency_ns);
    out << '\n';
  }
  for (const auto& session : snapshot.sessions) {
    out << "session id=" << session.session_id << " worker=" << session.worker_id
        << " inbound_messages=" << session.inbound_messages << " outbound_messages=" << session.outbound_messages
        << " parse_failures=" << session.parse_failures << " checksum_failures=" << session.checksum_failures
        << " resend_requests=" << session.resend_requests << " gap_fills=" << session.gap_fills << '\n';
  }
  return out.str();
}

[[nodiscard]] auto
HealthToText(const EngineHealthSnapshot& health) -> std::string
{
  std::ostringstream out;
  out << "health worker_count=" << health.worker_count << " registered_sessions=" << health.registered_sessions
      << " total_inbound_messages=" << health.total_inbound_messages
      << " total_outbound_messages=" << health.total_outbound_messages
      << " total_parse_failures=" << health.total_parse_failures
      << " total_checksum_failures=" << health.total_checksum_failures
      << " total_resend_requests=" << health.total_resend_requests << " total_gap_fills=" << health.total_gap_fills
      << " uptime_ns=" << health.uptime_ns << '\n';
  return out.str();
}

[[nodiscard]] auto
FormatTimestampNs(std::uint64_t timestamp_ns) -> std::string
{
  const auto seconds = static_cast<std::time_t>(timestamp_ns / kNanosecondsPerSecond);
  const auto nanos = timestamp_ns % kNanosecondsPerSecond;
  std::tm time_parts{};
#if defined(_WIN32)
  gmtime_s(&time_parts, &seconds);
#else
  if (gmtime_r(&seconds, &time_parts) == nullptr) {
    time_parts = std::tm{};
  }
#endif

  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << (time_parts.tm_year + 1900) << '-' << std::setw(2)
      << (time_parts.tm_mon + 1) << '-' << std::setw(2) << time_parts.tm_mday << 'T' << std::setw(2)
      << time_parts.tm_hour << ':' << std::setw(2) << time_parts.tm_min << ':' << std::setw(2) << time_parts.tm_sec
      << '.' << std::setw(9) << nanos << 'Z';
  return out.str();
}

auto
AppendTraceEventJson(std::string& out, const TraceEvent& event) -> void
{
  out.push_back('{');
  AppendNumberField(out, "sequence", event.sequence);
  AppendNumberField(out, "timestamp_ns", event.timestamp_ns);
  out.append("\"kind\":");
  AppendJsonString(out, TraceKindToString(event.kind));
  out.push_back(',');
  AppendNumberField(out, "session_id", event.session_id);
  AppendNumberField(out, "worker_id", event.worker_id);
  AppendNumberField(out, "arg0", event.arg0);
  AppendNumberField(out, "arg1", event.arg1);
  out.append("\"text\":");
  AppendJsonString(out, TraceText(event));
  out.push_back('}');
}

auto
AppendTraceEventText(std::ostringstream& out, const TraceEvent& event) -> void
{
  out << '[' << std::setfill('0') << std::setw(6) << event.sequence << "] " << FormatTimestampNs(event.timestamp_ns)
      << ' ' << std::setfill(' ') << std::left << std::setw(30) << TraceKindToString(event.kind)
      << " sess=" << std::setw(5) << event.session_id << " worker=" << std::setw(3) << event.worker_id
      << " arg0=" << event.arg0 << " arg1=" << event.arg1 << " text=\"";
  const auto text = TraceText(event);
  for (const auto ch : text) {
    if (ch == '"' || ch == '\\') {
      out << '\\';
    }
    out << ch;
  }
  out << "\"\n" << std::right;
}

} // namespace

JsonDiagnosticsSink::JsonDiagnosticsSink(OutputCallback callback)
  : output_(std::move(callback))
{
}

auto
JsonDiagnosticsSink::OnMetricsSnapshot(const RuntimeMetricsSnapshot& snapshot) -> void
{
  last_metrics_ = MetricsToJson(snapshot);
  if (output_) {
    output_(last_metrics_);
  }
}

auto
JsonDiagnosticsSink::OnTraceEvents(const std::vector<TraceEvent>& events) -> void
{
  last_trace_.clear();
  last_trace_.reserve(32U + (events.size() * 160U));
  last_trace_.append("{\"type\":\"trace\",\"events\":");
  last_trace_.append(DumpTraceToJson(events));
  last_trace_.push_back('}');
  if (output_) {
    output_(last_trace_);
  }
}

auto
JsonDiagnosticsSink::OnEngineHealth(const EngineHealthSnapshot& health) -> void
{
  last_health_ = HealthToJson(health);
  if (output_) {
    output_(last_health_);
  }
}

auto
JsonDiagnosticsSink::OnMessageBacklog(std::uint64_t session_id, std::uint64_t backlog_ms) -> void
{
  last_backlog_json_.clear();
  last_backlog_json_.reserve(48U);
  last_backlog_json_.push_back('{');
  AppendNumberField(last_backlog_json_, "session_id", session_id);
  AppendNumberField(last_backlog_json_, "backlog_ms", backlog_ms, false);
  last_backlog_json_.push_back('}');
  if (output_) {
    output_(last_backlog_json_);
  }
}

auto
JsonDiagnosticsSink::SetOutput(OutputCallback callback) -> void
{
  output_ = std::move(callback);
}

auto
JsonDiagnosticsSink::last_metrics_json() const -> std::string_view
{
  return last_metrics_;
}

auto
JsonDiagnosticsSink::last_trace_json() const -> std::string_view
{
  return last_trace_;
}

auto
JsonDiagnosticsSink::last_health_json() const -> std::string_view
{
  return last_health_;
}

auto
JsonDiagnosticsSink::last_backlog_json() const -> std::string_view
{
  return last_backlog_json_;
}

TextDiagnosticsSink::TextDiagnosticsSink(OutputCallback callback)
  : output_(std::move(callback))
{
}

auto
TextDiagnosticsSink::OnMetricsSnapshot(const RuntimeMetricsSnapshot& snapshot) -> void
{
  last_metrics_ = MetricsToText(snapshot);
  if (output_) {
    output_(last_metrics_);
  }
}

auto
TextDiagnosticsSink::OnTraceEvents(const std::vector<TraceEvent>& events) -> void
{
  last_trace_ = DumpTraceToText(events);
  if (output_) {
    output_(last_trace_);
  }
}

auto
TextDiagnosticsSink::OnEngineHealth(const EngineHealthSnapshot& health) -> void
{
  last_health_ = HealthToText(health);
  if (output_) {
    output_(last_health_);
  }
}

auto
TextDiagnosticsSink::OnMessageBacklog(std::uint64_t session_id, std::uint64_t backlog_ms) -> void
{
  std::ostringstream out;
  out << "backlog session_id=" << session_id << " backlog_ms=" << backlog_ms << '\n';
  last_backlog_text_ = out.str();
  if (output_) {
    output_(last_backlog_text_);
  }
}

auto
TextDiagnosticsSink::SetOutput(OutputCallback callback) -> void
{
  output_ = std::move(callback);
}

auto
TextDiagnosticsSink::last_metrics_text() const -> std::string_view
{
  return last_metrics_;
}

auto
TextDiagnosticsSink::last_trace_text() const -> std::string_view
{
  return last_trace_;
}

auto
TextDiagnosticsSink::last_health_text() const -> std::string_view
{
  return last_health_;
}

auto
TextDiagnosticsSink::last_backlog_text() const -> std::string_view
{
  return last_backlog_text_;
}

DiagnosticsMonitor::DiagnosticsMonitor() = default;

DiagnosticsMonitor::~DiagnosticsMonitor()
{
  Stop();
}

auto
DiagnosticsMonitor::AddSink(std::shared_ptr<DiagnosticsSink> sink) -> void
{
  if (sink == nullptr) {
    return;
  }
  std::lock_guard lock(mutex_);
  sinks_.push_back(std::move(sink));
}

auto
DiagnosticsMonitor::RemoveSink(const DiagnosticsSink* sink) -> void
{
  std::lock_guard lock(mutex_);
  sinks_.erase(
    std::remove_if(sinks_.begin(), sinks_.end(), [sink](const auto& candidate) { return candidate.get() == sink; }),
    sinks_.end());
}

auto
DiagnosticsMonitor::Bind(const MetricsRegistry* metrics, const TraceRecorder* trace) -> void
{
  std::lock_guard lock(mutex_);
  metrics_ = metrics;
  trace_ = trace;
  last_trace_sequence_ = 0U;
}

auto
DiagnosticsMonitor::FlushNow() -> void
{
  FlushInternal();
}

auto
DiagnosticsMonitor::NotifyMessageBacklog(std::uint64_t session_id, std::uint64_t backlog_ms) -> void
{
  std::lock_guard lock(mutex_);
  for (const auto& sink : sinks_) {
    if (sink == nullptr) {
      continue;
    }
    sink->OnMessageBacklog(session_id, backlog_ms);
  }
}

auto
DiagnosticsMonitor::Start(std::chrono::milliseconds interval) -> base::Status
{
  if (interval.count() <= 0) {
    return base::Status::InvalidArgument("diagnostics interval must be positive");
  }

  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return base::Status::Busy("diagnostics monitor is already running");
  }

  {
    std::lock_guard lock(mutex_);
    boot_timestamp_ns_ = NowUnixNs();
    last_trace_sequence_ = 0U;
    stop_requested_.store(false, std::memory_order_release);
  }

  try {
    background_thread_ = std::thread(&DiagnosticsMonitor::BackgroundLoop, this, interval);
  } catch (const std::system_error& error) {
    running_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    return base::Status::IoError(std::string("unable to start diagnostics monitor thread: ") + error.what());
  }

  return base::Status::Ok();
}

auto
DiagnosticsMonitor::Stop() -> void
{
  stop_requested_.store(true, std::memory_order_release);
  stop_cv_.notify_all();
  if (background_thread_.joinable()) {
    background_thread_.join();
  }
  running_.store(false, std::memory_order_release);
}

auto
DiagnosticsMonitor::running() const -> bool
{
  return running_.load(std::memory_order_acquire);
}

auto
DiagnosticsMonitor::FlushInternal() -> void
{
  std::lock_guard lock(mutex_);

  RuntimeMetricsSnapshot metrics_snapshot;
  const auto has_metrics = metrics_ != nullptr;
  if (has_metrics) {
    metrics_snapshot = metrics_->Snapshot();
  }

  std::vector<TraceEvent> new_trace_events;
  const auto has_trace = trace_ != nullptr;
  if (has_trace) {
    const auto trace_snapshot = trace_->Snapshot();
    new_trace_events.reserve(trace_snapshot.size());
    auto max_sequence = last_trace_sequence_;
    for (const auto& event : trace_snapshot) {
      if (event.sequence <= last_trace_sequence_) {
        continue;
      }
      new_trace_events.push_back(event);
      max_sequence = std::max(max_sequence, event.sequence);
    }
    last_trace_sequence_ = max_sequence;
  }

  const auto now_ns = NowUnixNs();
  const auto uptime_ns = boot_timestamp_ns_ == 0U || now_ns < boot_timestamp_ns_ ? 0U : now_ns - boot_timestamp_ns_;
  const auto health = BuildEngineHealth(metrics_snapshot, uptime_ns);

  for (const auto& sink : sinks_) {
    if (sink == nullptr) {
      continue;
    }
    if (has_metrics) {
      sink->OnMetricsSnapshot(metrics_snapshot);
    }
    if (has_trace) {
      sink->OnTraceEvents(new_trace_events);
    }
    if (has_metrics) {
      sink->OnEngineHealth(health);
    }
  }
}

auto
DiagnosticsMonitor::BackgroundLoop(std::chrono::milliseconds interval) -> void
{
  std::unique_lock lock(mutex_);
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (stop_cv_.wait_for(lock, interval, [this] { return stop_requested_.load(std::memory_order_acquire); })) {
      break;
    }
    lock.unlock();
    FlushInternal();
    lock.lock();
  }
  running_.store(false, std::memory_order_release);
}

auto
DumpTraceToJson(const std::vector<TraceEvent>& events) -> std::string
{
  std::string out;
  out.reserve(2U + (events.size() * 160U));
  out.push_back('[');
  for (std::size_t index = 0U; index < events.size(); ++index) {
    if (index != 0U) {
      out.push_back(',');
    }
    AppendTraceEventJson(out, events[index]);
  }
  out.push_back(']');
  return out;
}

auto
DumpTraceToText(const std::vector<TraceEvent>& events) -> std::string
{
  std::ostringstream out;
  for (const auto& event : events) {
    AppendTraceEventText(out, event);
  }
  return out.str();
}

auto
DumpTraceToFile(const std::vector<TraceEvent>& events, const std::filesystem::path& path) -> base::Status
{
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error) {
      return base::Status::IoError("unable to create diagnostics trace directory '" + parent.string() +
                                   "': " + create_error.message());
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return base::Status::IoError("unable to open diagnostics trace file '" + path.string() + "'");
  }
  const auto json = DumpTraceToJson(events);
  out.write(json.data(), static_cast<std::streamsize>(json.size()));
  if (!out) {
    return base::Status::IoError("unable to write diagnostics trace file '" + path.string() + "'");
  }
  return base::Status::Ok();
}

auto
BuildEngineHealth(const RuntimeMetricsSnapshot& snapshot, std::uint64_t uptime_ns) -> EngineHealthSnapshot
{
  EngineHealthSnapshot health;
  health.worker_count = static_cast<std::uint32_t>(snapshot.workers.size());
  health.uptime_ns = uptime_ns;
  for (const auto& worker : snapshot.workers) {
    health.registered_sessions += static_cast<std::uint32_t>(worker.registered_sessions);
    health.total_inbound_messages += worker.inbound_messages;
    health.total_outbound_messages += worker.outbound_messages;
    health.total_parse_failures += worker.parse_failures;
    health.total_checksum_failures += worker.checksum_failures;
    health.total_resend_requests += worker.resend_requests;
    health.total_gap_fills += worker.gap_fills;
  }
  return health;
}

} // namespace nimble::runtime
