#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nimblefix/runtime/diagnostics.h"

namespace {

constexpr std::uint64_t kTraceTimestampNs = 1'777'550'400'123'456'789ULL;

[[nodiscard]] auto
ReadWholeFile(const std::filesystem::path& path) -> std::string
{
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

[[nodiscard]] auto
SampleTraceEvent(std::uint64_t sequence = 1U,
                 nimble::runtime::TraceEventKind kind = nimble::runtime::TraceEventKind::kConfigLoaded,
                 std::string_view text = "boot complete") -> nimble::runtime::TraceEvent
{
  nimble::runtime::TraceEvent event;
  event.sequence = sequence;
  event.timestamp_ns = kTraceTimestampNs + sequence;
  event.kind = kind;
  event.session_id = 42U;
  event.worker_id = 1U;
  event.arg0 = 7U;
  event.arg1 = 8U;
  const auto copy_size = std::min(text.size(), event.text.size() - 1U);
  std::copy_n(text.data(), copy_size, event.text.data());
  event.text[copy_size] = '\0';
  return event;
}

[[nodiscard]] auto
BuildMetricsSnapshot() -> nimble::runtime::RuntimeMetricsSnapshot
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(2U);
  REQUIRE(metrics.RegisterSession(1001U, 0U).ok());
  REQUIRE(metrics.RegisterSession(1002U, 1U).ok());
  REQUIRE(metrics.RecordInbound(1001U, false).ok());
  REQUIRE(metrics.RecordInbound(1002U, true).ok());
  REQUIRE(metrics.RecordOutbound(1001U, false).ok());
  REQUIRE(metrics.RecordOutbound(1002U, true).ok());
  auto* session_1001 = metrics.FindSession(1001U);
  auto* session_1002 = metrics.FindSession(1002U);
  REQUIRE(session_1001 != nullptr);
  REQUIRE(session_1002 != nullptr);
  session_1001->read_bytes.fetch_add(101U, std::memory_order_relaxed);
  session_1001->write_bytes.fetch_add(201U, std::memory_order_relaxed);
  session_1001->socket_poll_count.fetch_add(2U, std::memory_order_relaxed);
  session_1002->read_bytes.fetch_add(102U, std::memory_order_relaxed);
  session_1002->write_bytes.fetch_add(202U, std::memory_order_relaxed);
  session_1002->socket_poll_count.fetch_add(3U, std::memory_order_relaxed);
  REQUIRE(metrics.RecordParseFailure(1001U).ok());
  REQUIRE(metrics.RecordChecksumFailure(1002U).ok());
  REQUIRE(metrics.RecordResendRequest(1002U).ok());
  REQUIRE(metrics.RecordGapFill(1001U, 3U).ok());
  return metrics.Snapshot();
}

class CountingSink final : public nimble::runtime::DiagnosticsSink
{
public:
  auto OnMetricsSnapshot(const nimble::runtime::RuntimeMetricsSnapshot& snapshot) -> void override
  {
    ++metrics_count;
    last_worker_count = snapshot.workers.size();
  }

  auto OnTraceEvents(const std::vector<nimble::runtime::TraceEvent>& events) -> void override
  {
    ++trace_count;
    last_trace_count = events.size();
  }

  auto OnEngineHealth(const nimble::runtime::EngineHealthSnapshot& health) -> void override
  {
    ++health_count;
    last_registered_sessions = health.registered_sessions;
  }

  auto OnMessageBacklog(std::uint64_t session_id, std::uint64_t backlog_ms) -> void override
  {
    ++backlog_count;
    last_backlog_session_id = session_id;
    last_backlog_ms = backlog_ms;
  }

  std::atomic<std::uint32_t> metrics_count{ 0 };
  std::atomic<std::uint32_t> trace_count{ 0 };
  std::atomic<std::uint32_t> health_count{ 0 };
  std::atomic<std::uint32_t> backlog_count{ 0 };
  std::size_t last_worker_count{ 0U };
  std::size_t last_trace_count{ 0U };
  std::uint32_t last_registered_sessions{ 0U };
  std::uint64_t last_backlog_session_id{ 0U };
  std::uint64_t last_backlog_ms{ 0U };
};

} // namespace

TEST_CASE("DiagnosticsMonitor flush produces snapshots", "[diagnostics]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(1U);
  REQUIRE(metrics.RegisterSession(1001U, 0U).ok());
  REQUIRE(metrics.RecordInbound(1001U, false).ok());

  nimble::runtime::TraceRecorder trace;
  trace.Configure(nimble::runtime::TraceMode::kRing, 8U, 1U);
  trace.Record(nimble::runtime::TraceEventKind::kConfigLoaded, 0U, 0U, kTraceTimestampNs, 1U, 2U, "boot");

  auto sink = std::make_shared<nimble::runtime::JsonDiagnosticsSink>();
  nimble::runtime::DiagnosticsMonitor monitor;
  monitor.AddSink(sink);
  monitor.Bind(&metrics, &trace);
  monitor.FlushNow();

  REQUIRE(sink->last_metrics_json().find("\"type\":\"metrics\"") != std::string_view::npos);
  REQUIRE(sink->last_metrics_json().find("\"worker_id\":0") != std::string_view::npos);
  REQUIRE(sink->last_metrics_json().find("\"session_id\":1001") != std::string_view::npos);
  REQUIRE(sink->last_metrics_json().find("\"read_bytes\":0") != std::string_view::npos);
  REQUIRE(sink->last_metrics_json().find("\"write_bytes\":0") != std::string_view::npos);
  REQUIRE(sink->last_metrics_json().find("\"socket_poll_count\":0") != std::string_view::npos);
  REQUIRE(sink->last_trace_json().find("\"type\":\"trace\"") != std::string_view::npos);
  REQUIRE(sink->last_trace_json().find("\"kind\":\"config_loaded\"") != std::string_view::npos);
  REQUIRE(sink->last_health_json().find("\"type\":\"health\"") != std::string_view::npos);
  REQUIRE(sink->last_health_json().find("\"registered_sessions\":1") != std::string_view::npos);

  monitor.FlushNow();
  REQUIRE(sink->last_trace_json().find("\"events\":[]") != std::string_view::npos);
}

TEST_CASE("TextDiagnosticsSink formatting", "[diagnostics]")
{
  const auto snapshot = BuildMetricsSnapshot();
  const std::vector<nimble::runtime::TraceEvent> events{ SampleTraceEvent() };
  const auto health = nimble::runtime::BuildEngineHealth(snapshot, 99U);

  std::vector<std::string> emitted;
  nimble::runtime::TextDiagnosticsSink sink([&emitted](std::string_view text) { emitted.emplace_back(text); });
  sink.OnMetricsSnapshot(snapshot);
  sink.OnTraceEvents(events);
  sink.OnEngineHealth(health);

  REQUIRE(emitted.size() == 3U);
  REQUIRE(sink.last_metrics_text().find("metrics workers=2 sessions=2") != std::string_view::npos);
  REQUIRE(sink.last_metrics_text().find("session id=1001 worker=0") != std::string_view::npos);
  REQUIRE(sink.last_metrics_text().find("read_bytes=101") != std::string_view::npos);
  REQUIRE(sink.last_metrics_text().find("write_bytes=201") != std::string_view::npos);
  REQUIRE(sink.last_metrics_text().find("socket_poll_count=2") != std::string_view::npos);
  REQUIRE(sink.last_trace_text().find("[000001]") != std::string_view::npos);
  REQUIRE(sink.last_trace_text().find("config_loaded") != std::string_view::npos);
  REQUIRE(sink.last_trace_text().find("text=\"boot complete\"") != std::string_view::npos);
  REQUIRE(sink.last_health_text().find("health worker_count=2 registered_sessions=2") != std::string_view::npos);
}

TEST_CASE("diagnostics backlog callback", "[diagnostics-backlog]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(1U);
  constexpr std::uint64_t session_id = 1001U;
  REQUIRE(metrics.RegisterSession(session_id, 0U).ok());

  auto sink = std::make_shared<CountingSink>();
  nimble::runtime::DiagnosticsMonitor monitor;
  monitor.AddSink(sink);
  monitor.Bind(&metrics, nullptr);

  monitor.NotifyMessageBacklog(session_id, 6000U);
  REQUIRE(sink->backlog_count.load() == 1U);
  REQUIRE(sink->last_backlog_session_id == session_id);
  REQUIRE(sink->last_backlog_ms == 6000U);

  monitor.NotifyMessageBacklog(session_id, 7000U);
  REQUIRE(sink->backlog_count.load() == 2U);
  REQUIRE(sink->last_backlog_session_id == session_id);
  REQUIRE(sink->last_backlog_ms == 7000U);
}

TEST_CASE("diagnostics backlog sink formatting", "[diagnostics-backlog]")
{
  std::vector<std::string> json_emitted;
  nimble::runtime::JsonDiagnosticsSink json_sink(
    [&json_emitted](std::string_view json) { json_emitted.emplace_back(json); });
  json_sink.OnMessageBacklog(42U, 6000U);

  REQUIRE(json_emitted.size() == 1U);
  REQUIRE(json_sink.last_backlog_json() == R"({"session_id":42,"backlog_ms":6000})");
  REQUIRE(json_emitted.front() == json_sink.last_backlog_json());

  std::vector<std::string> text_emitted;
  nimble::runtime::TextDiagnosticsSink text_sink(
    [&text_emitted](std::string_view text) { text_emitted.emplace_back(text); });
  text_sink.OnMessageBacklog(42U, 6000U);

  REQUIRE(text_emitted.size() == 1U);
  REQUIRE(text_sink.last_backlog_text() == "backlog session_id=42 backlog_ms=6000\n");
  REQUIRE(text_emitted.front() == text_sink.last_backlog_text());
}

TEST_CASE("Diagnostics metrics sinks include session network counters", "[diagnostics]")
{
  const auto snapshot = BuildMetricsSnapshot();

  nimble::runtime::JsonDiagnosticsSink json_sink;
  json_sink.OnMetricsSnapshot(snapshot);
  REQUIRE(json_sink.last_metrics_json().find("\"read_bytes\":101") != std::string_view::npos);
  REQUIRE(json_sink.last_metrics_json().find("\"write_bytes\":201") != std::string_view::npos);
  REQUIRE(json_sink.last_metrics_json().find("\"socket_poll_count\":2") != std::string_view::npos);

  nimble::runtime::TextDiagnosticsSink text_sink;
  text_sink.OnMetricsSnapshot(snapshot);
  REQUIRE(text_sink.last_metrics_text().find("read_bytes=101") != std::string_view::npos);
  REQUIRE(text_sink.last_metrics_text().find("write_bytes=201") != std::string_view::npos);
  REQUIRE(text_sink.last_metrics_text().find("socket_poll_count=2") != std::string_view::npos);
}

TEST_CASE("DumpTraceToJson", "[diagnostics]")
{
  const std::vector<nimble::runtime::TraceEvent> events{
    SampleTraceEvent(1U, nimble::runtime::TraceEventKind::kConfigLoaded, "boot \"ok\""),
    SampleTraceEvent(2U, nimble::runtime::TraceEventKind::kPendingConnectionRegistered, "pending\\conn"),
  };

  const auto json = nimble::runtime::DumpTraceToJson(events);
  REQUIRE(json.front() == '[');
  REQUIRE(json.back() == ']');
  REQUIRE(json.find("\"sequence\":1") != std::string::npos);
  REQUIRE(json.find("\"timestamp_ns\":1777550400123456790") != std::string::npos);
  REQUIRE(json.find("\"kind\":\"config_loaded\"") != std::string::npos);
  REQUIRE(json.find("\"kind\":\"pending_connection_registered\"") != std::string::npos);
  REQUIRE(json.find("\"text\":\"boot \\\"ok\\\"\"") != std::string::npos);
  REQUIRE(json.find("\"text\":\"pending\\\\conn\"") != std::string::npos);
}

TEST_CASE("DumpTraceToText", "[diagnostics]")
{
  const std::vector<nimble::runtime::TraceEvent> events{
    SampleTraceEvent(1U, nimble::runtime::TraceEventKind::kConfigLoaded, "boot complete"),
    SampleTraceEvent(2U, nimble::runtime::TraceEventKind::kProfileLoaded, ""),
  };

  const auto text = nimble::runtime::DumpTraceToText(events);
  REQUIRE(text.find("[000001] 2026-04-30T12:00:00.123456790Z config_loaded") != std::string::npos);
  REQUIRE(text.find("sess=42") != std::string::npos);
  REQUIRE(text.find("worker=1") != std::string::npos);
  REQUIRE(text.find("arg0=7 arg1=8 text=\"boot complete\"") != std::string::npos);
  REQUIRE(text.find("[000002] 2026-04-30T12:00:00.123456791Z profile_loaded") != std::string::npos);
  REQUIRE(text.find("text=\"\"") != std::string::npos);
}

TEST_CASE("DumpTraceToFile", "[diagnostics]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-diagnostics-test";
  std::filesystem::remove_all(temp_root);
  const auto path = temp_root / "trace.json";

  const std::vector<nimble::runtime::TraceEvent> events{ SampleTraceEvent() };
  const auto status = nimble::runtime::DumpTraceToFile(events, path);
  REQUIRE(status.ok());
  const auto contents = ReadWholeFile(path);
  REQUIRE(contents == nimble::runtime::DumpTraceToJson(events));
  REQUIRE(contents.find("\"kind\":\"config_loaded\"") != std::string::npos);

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("DiagnosticsMonitor periodic", "[diagnostics]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(1U);
  REQUIRE(metrics.RegisterSession(1001U, 0U).ok());

  nimble::runtime::TraceRecorder trace;
  trace.Configure(nimble::runtime::TraceMode::kRing, 8U, 1U);
  trace.Record(nimble::runtime::TraceEventKind::kSessionEvent, 1001U, 0U, kTraceTimestampNs, 1U, 2U, "event");

  auto sink = std::make_shared<CountingSink>();
  nimble::runtime::DiagnosticsMonitor monitor;
  monitor.AddSink(sink);
  monitor.Bind(&metrics, &trace);
  REQUIRE(monitor.Start(std::chrono::milliseconds{ 10 }).ok());
  REQUIRE(monitor.running());
  std::this_thread::sleep_for(std::chrono::milliseconds{ 45 });
  monitor.Stop();
  REQUIRE_FALSE(monitor.running());

  REQUIRE(sink->metrics_count.load() >= 2U);
  REQUIRE(sink->trace_count.load() >= 2U);
  REQUIRE(sink->health_count.load() >= 2U);
}

TEST_CASE("BuildEngineHealth aggregation", "[diagnostics]")
{
  const auto snapshot = BuildMetricsSnapshot();
  const auto health = nimble::runtime::BuildEngineHealth(snapshot, 12'345U);

  REQUIRE(health.worker_count == 2U);
  REQUIRE(health.registered_sessions == 2U);
  REQUIRE(health.total_inbound_messages == 2U);
  REQUIRE(health.total_outbound_messages == 2U);
  REQUIRE(health.total_parse_failures == 1U);
  REQUIRE(health.total_checksum_failures == 1U);
  REQUIRE(health.total_resend_requests == 1U);
  REQUIRE(health.total_gap_fills == 3U);
  REQUIRE(health.uptime_ns == 12'345U);
}

TEST_CASE("Multiple sinks", "[diagnostics]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(1U);
  REQUIRE(metrics.RegisterSession(1001U, 0U).ok());

  nimble::runtime::TraceRecorder trace;
  trace.Configure(nimble::runtime::TraceMode::kRing, 8U, 1U);
  trace.Record(nimble::runtime::TraceEventKind::kStoreEvent, 1001U, 0U, kTraceTimestampNs, 1U, 2U, "store");

  auto sink_a = std::make_shared<CountingSink>();
  auto sink_b = std::make_shared<CountingSink>();
  nimble::runtime::DiagnosticsMonitor monitor;
  monitor.AddSink(sink_a);
  monitor.AddSink(sink_b);
  monitor.Bind(&metrics, &trace);
  monitor.FlushNow();

  REQUIRE(sink_a->metrics_count.load() == 1U);
  REQUIRE(sink_b->metrics_count.load() == 1U);
  REQUIRE(sink_a->trace_count.load() == 1U);
  REQUIRE(sink_b->trace_count.load() == 1U);
  REQUIRE(sink_a->health_count.load() == 1U);
  REQUIRE(sink_b->health_count.load() == 1U);
  REQUIRE(sink_a->last_worker_count == sink_b->last_worker_count);
  REQUIRE(sink_a->last_trace_count == sink_b->last_trace_count);
  REQUIRE(sink_a->last_registered_sessions == sink_b->last_registered_sessions);
}
