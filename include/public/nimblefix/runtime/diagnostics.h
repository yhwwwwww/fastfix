#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nimblefix/base/status.h"
#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/trace.h"

namespace nimble::runtime {

/// Snapshot of engine-level health status for diagnostics consumers.
struct EngineHealthSnapshot
{
  std::uint32_t worker_count{ 0 };
  std::uint32_t registered_sessions{ 0 };
  std::uint64_t total_inbound_messages{ 0 };
  std::uint64_t total_outbound_messages{ 0 };
  std::uint64_t total_parse_failures{ 0 };
  std::uint64_t total_checksum_failures{ 0 };
  std::uint64_t total_resend_requests{ 0 };
  std::uint64_t total_gap_fills{ 0 };
  std::uint64_t uptime_ns{ 0 };
};

/// Abstract interface for receiving periodic diagnostics data.
///
/// Implement this to export metrics/trace to external systems (Prometheus,
/// Grafana, log files, monitoring dashboards, etc.)
class DiagnosticsSink
{
public:
  virtual ~DiagnosticsSink() = default;

  /// Called periodically with the latest metrics snapshot.
  virtual auto OnMetricsSnapshot(const RuntimeMetricsSnapshot& snapshot) -> void = 0;

  /// Called periodically with recent trace events since last flush.
  virtual auto OnTraceEvents(const std::vector<TraceEvent>& events) -> void = 0;

  /// Called periodically with an aggregated engine health summary.
  virtual auto OnEngineHealth(const EngineHealthSnapshot& health) -> void = 0;
};

/// Built-in sink that formats diagnostics as JSON strings.
///
/// Writes to a configurable output function (default: no-op).
/// Typical usage: provide a callback that writes to a file, stdout, or network.
class JsonDiagnosticsSink final : public DiagnosticsSink
{
public:
  using OutputCallback = std::function<void(std::string_view json)>;

  JsonDiagnosticsSink() = default;
  explicit JsonDiagnosticsSink(OutputCallback callback);

  auto OnMetricsSnapshot(const RuntimeMetricsSnapshot& snapshot) -> void override;
  auto OnTraceEvents(const std::vector<TraceEvent>& events) -> void override;
  auto OnEngineHealth(const EngineHealthSnapshot& health) -> void override;

  /// Set or replace the output callback.
  auto SetOutput(OutputCallback callback) -> void;

  /// Access the last emitted JSON for each category (useful for testing).
  [[nodiscard]] auto last_metrics_json() const -> std::string_view;
  [[nodiscard]] auto last_trace_json() const -> std::string_view;
  [[nodiscard]] auto last_health_json() const -> std::string_view;

private:
  OutputCallback output_;
  std::string last_metrics_;
  std::string last_trace_;
  std::string last_health_;
};

/// Built-in sink that formats diagnostics as human-readable text.
class TextDiagnosticsSink final : public DiagnosticsSink
{
public:
  using OutputCallback = std::function<void(std::string_view text)>;

  TextDiagnosticsSink() = default;
  explicit TextDiagnosticsSink(OutputCallback callback);

  auto OnMetricsSnapshot(const RuntimeMetricsSnapshot& snapshot) -> void override;
  auto OnTraceEvents(const std::vector<TraceEvent>& events) -> void override;
  auto OnEngineHealth(const EngineHealthSnapshot& health) -> void override;

  auto SetOutput(OutputCallback callback) -> void;

  [[nodiscard]] auto last_metrics_text() const -> std::string_view;
  [[nodiscard]] auto last_trace_text() const -> std::string_view;
  [[nodiscard]] auto last_health_text() const -> std::string_view;

private:
  OutputCallback output_;
  std::string last_metrics_;
  std::string last_trace_;
  std::string last_health_;
};

/// Manages periodic diagnostics flushing to registered sinks.
///
/// Typical lifecycle:
///   monitor.AddSink(my_sink);
///   monitor.Bind(engine.metrics(), engine.trace());
///   monitor.Start(std::chrono::seconds(5));
///   // ... engine runs ...
///   monitor.Stop();
class DiagnosticsMonitor
{
public:
  DiagnosticsMonitor();
  ~DiagnosticsMonitor();

  DiagnosticsMonitor(const DiagnosticsMonitor&) = delete;
  auto operator=(const DiagnosticsMonitor&) -> DiagnosticsMonitor& = delete;
  DiagnosticsMonitor(DiagnosticsMonitor&&) = delete;
  auto operator=(DiagnosticsMonitor&&) -> DiagnosticsMonitor& = delete;

  /// Register a diagnostics sink.
  auto AddSink(std::shared_ptr<DiagnosticsSink> sink) -> void;

  /// Remove a previously registered sink.
  auto RemoveSink(const DiagnosticsSink* sink) -> void;

  /// Bind to engine metrics and trace sources.
  auto Bind(const MetricsRegistry* metrics, const TraceRecorder* trace) -> void;

  /// Trigger an immediate flush to all registered sinks.
  auto FlushNow() -> void;

  /// Start periodic background flushing.
  auto Start(std::chrono::milliseconds interval) -> base::Status;

  /// Stop background flushing. Blocks until the background thread exits.
  auto Stop() -> void;

  /// Whether the monitor is currently running periodic flushes.
  [[nodiscard]] auto running() const -> bool;

private:
  auto FlushInternal() -> void;
  auto BackgroundLoop(std::chrono::milliseconds interval) -> void;

  mutable std::mutex mutex_;
  std::condition_variable stop_cv_;
  std::vector<std::shared_ptr<DiagnosticsSink>> sinks_;
  const MetricsRegistry* metrics_{ nullptr };
  const TraceRecorder* trace_{ nullptr };
  std::uint64_t boot_timestamp_ns_{ 0 };
  std::uint64_t last_trace_sequence_{ 0 };
  std::atomic<bool> running_{ false };
  std::atomic<bool> stop_requested_{ false };
  std::thread background_thread_;
};

// ─── Trace Dump Utilities ────────────────────────────────────────────────────

/// Format trace events as a JSON array string.
[[nodiscard]] auto
DumpTraceToJson(const std::vector<TraceEvent>& events) -> std::string;

/// Format trace events as human-readable text (one line per event).
[[nodiscard]] auto
DumpTraceToText(const std::vector<TraceEvent>& events) -> std::string;

/// Write trace events to a file in JSON format.
[[nodiscard]] auto
DumpTraceToFile(const std::vector<TraceEvent>& events, const std::filesystem::path& path) -> base::Status;

/// Build an EngineHealthSnapshot from a metrics snapshot.
[[nodiscard]] auto
BuildEngineHealth(const RuntimeMetricsSnapshot& snapshot, std::uint64_t uptime_ns) -> EngineHealthSnapshot;

} // namespace nimble::runtime
