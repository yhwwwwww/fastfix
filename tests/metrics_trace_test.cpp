#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string_view>

#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/trace.h"

TEST_CASE("metrics-trace", "[metrics-trace]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(2U);
  REQUIRE(metrics.RegisterSession(1001U, 1U).ok());
  auto* session_metrics = metrics.FindSession(1001U);
  REQUIRE(session_metrics != nullptr);
  REQUIRE(session_metrics->read_bytes.load(std::memory_order_relaxed) == 0U);
  REQUIRE(session_metrics->write_bytes.load(std::memory_order_relaxed) == 0U);
  REQUIRE(session_metrics->socket_poll_count.load(std::memory_order_relaxed) == 0U);
  session_metrics->read_bytes.fetch_add(128U, std::memory_order_relaxed);
  session_metrics->read_bytes.fetch_add(64U, std::memory_order_relaxed);
  session_metrics->write_bytes.fetch_add(256U, std::memory_order_relaxed);
  session_metrics->write_bytes.fetch_add(32U, std::memory_order_relaxed);
  session_metrics->socket_poll_count.fetch_add(1U, std::memory_order_relaxed);
  session_metrics->socket_poll_count.fetch_add(2U, std::memory_order_relaxed);
  REQUIRE(metrics.RecordInbound(1001U, false).ok());
  REQUIRE(metrics.RecordOutbound(1001U, true).ok());
  REQUIRE(metrics.RecordResendRequest(1001U).ok());
  REQUIRE(metrics.RecordGapFill(1001U, 3U).ok());
  REQUIRE(metrics.RecordParseFailure(1001U).ok());
  REQUIRE(metrics.RecordChecksumFailure(1001U).ok());
  REQUIRE(metrics.UpdateOutboundQueueDepth(1001U, 4U).ok());
  REQUIRE(metrics.ObserveStoreFlushLatency(1001U, 1234U).ok());
  REQUIRE(metrics.RecordPlainConnection(1U).ok());
  REQUIRE(metrics.RecordTlsHandshake(1U, true, 2500U, true).ok());
  REQUIRE(metrics.RecordTlsHandshake(1U, false, 500U, false).ok());

  const auto snapshot = metrics.Snapshot();
  REQUIRE(snapshot.workers.size() == 2U);
  REQUIRE(snapshot.sessions.size() == 1U);
  REQUIRE(snapshot.sessions[0].inbound_messages == 1U);
  REQUIRE(snapshot.sessions[0].outbound_messages == 1U);
  REQUIRE(snapshot.sessions[0].read_bytes == 192U);
  REQUIRE(snapshot.sessions[0].write_bytes == 288U);
  REQUIRE(snapshot.sessions[0].socket_poll_count == 3U);
  REQUIRE(snapshot.sessions[0].admin_messages == 1U);
  REQUIRE(snapshot.sessions[0].resend_requests == 1U);
  REQUIRE(snapshot.sessions[0].gap_fills == 3U);
  REQUIRE(snapshot.sessions[0].parse_failures == 1U);
  REQUIRE(snapshot.sessions[0].checksum_failures == 1U);
  REQUIRE(snapshot.sessions[0].outbound_queue_depth == 4U);
  REQUIRE(snapshot.sessions[0].last_store_flush_latency_ns == 1234U);
  REQUIRE(snapshot.workers[1].registered_sessions == 1U);
  REQUIRE(snapshot.workers[1].outbound_queue_depth == 4U);
  REQUIRE(snapshot.workers[1].plain_connections == 1U);
  REQUIRE(snapshot.workers[1].tls_connections == 1U);
  REQUIRE(snapshot.workers[1].tls_handshake_successes == 1U);
  REQUIRE(snapshot.workers[1].tls_handshake_failures == 1U);
  REQUIRE(snapshot.workers[1].tls_handshake_latency_ns == 3000U);
  REQUIRE(snapshot.workers[1].tls_session_resumptions == 1U);
  REQUIRE(snapshot.workers[1].histograms.store_flush_latency_ns.count == 1U);
  REQUIRE(snapshot.workers[1].histograms.store_flush_latency_ns.p50_ns == 2048U);

  nimble::runtime::TraceRecorder trace;
  trace.Configure(nimble::runtime::TraceMode::kRing, 2U, 2U);
  trace.Record(nimble::runtime::TraceEventKind::kConfigLoaded, 0U, 0U, 1U, 1U, 0U, "boot");
  trace.Record(nimble::runtime::TraceEventKind::kProfileLoaded, 1001U, 0U, 2U, 1001U, 0U, "profile");
  trace.Record(nimble::runtime::TraceEventKind::kSessionRegistered, 1001U, 1U, 3U, 1001U, 0U, "session-a");
  trace.Record(nimble::runtime::TraceEventKind::kSessionEvent, 1002U, 1U, 4U, 7U, 8U, "session-b");
  trace.Record(nimble::runtime::TraceEventKind::kStoreEvent, 1003U, 0U, 5U, 9U, 10U, "store");

  const auto events = trace.Snapshot();
  REQUIRE(events.size() == 4U);
  REQUIRE(events[0].sequence == 2U);
  REQUIRE(events[0].kind == nimble::runtime::TraceEventKind::kProfileLoaded);
  REQUIRE(events[1].sequence == 3U);
  REQUIRE(events[2].sequence == 4U);
  REQUIRE(events[3].sequence == 5U);
  REQUIRE(std::string_view(events[3].text.data()) == "store");
}

TEST_CASE("latency histogram snapshots percentiles", "[metrics-trace]")
{
  nimble::runtime::LatencyHistogram histogram;
  histogram.Observe(0U);
  histogram.Observe(63U);
  histogram.Observe(64U);
  histogram.Observe(127U);
  histogram.Observe(128U);
  histogram.Observe(1'048'576U);

  const auto snapshot = histogram.Snapshot();
  REQUIRE(snapshot.Count() == 6U);
  REQUIRE(snapshot.Percentile(0.50) == 128U);
  REQUIRE(snapshot.Percentile(50.0) == 128U);
  REQUIRE(snapshot.Percentile(0.90) == 1'048'576U);
  REQUIRE(snapshot.Percentile(99.9) == 1'048'576U);
  REQUIRE(histogram.Percentile(50.0) == 128U);
  REQUIRE(snapshot.SumNs() > 0U);

  histogram.Reset();
  const auto reset_snapshot = histogram.Snapshot();
  REQUIRE(reset_snapshot.Count() == 0U);
  REQUIRE(reset_snapshot.Percentile(99.0) == 0U);
}

TEST_CASE("worker metrics snapshot includes latency histogram percentiles", "[metrics-trace]")
{
  nimble::runtime::MetricsRegistry metrics;
  metrics.Reset(1U);
  REQUIRE(metrics.RegisterSession(1001U, 0U).ok());
  REQUIRE(metrics.ObserveStoreFlushLatency(1001U, 200U).ok());
  REQUIRE(metrics.ObserveStoreFlushLatency(1001U, 900U).ok());
  auto* worker = metrics.FindWorker(0U);
  REQUIRE(worker != nullptr);
  worker->session_inbound_latency_ns.Observe(90U);
  worker->session_inbound_latency_ns.Observe(2'000U);
  worker->encode_latency_ns.Observe(300U);
  worker->parse_latency_ns.Observe(70U);
  worker->send_latency_ns.Observe(1'500U);

  const auto snapshot = metrics.Snapshot();
  REQUIRE(snapshot.workers.size() == 1U);
  const auto& histograms = snapshot.workers[0].histograms;
  REQUIRE(histograms.session_inbound_latency_ns.count == 2U);
  REQUIRE(histograms.session_inbound_latency_ns.p50_ns == 128U);
  REQUIRE(histograms.session_inbound_latency_ns.p90_ns == 2048U);
  REQUIRE(histograms.encode_latency_ns.count == 1U);
  REQUIRE(histograms.encode_latency_ns.p99_ns == 512U);
  REQUIRE(histograms.parse_latency_ns.count == 1U);
  REQUIRE(histograms.parse_latency_ns.p999_ns == 128U);
  REQUIRE(histograms.store_flush_latency_ns.count == 2U);
  REQUIRE(histograms.store_flush_latency_ns.p50_ns == 256U);
  REQUIRE(histograms.store_flush_latency_ns.p90_ns == 1024U);
  REQUIRE(histograms.send_latency_ns.count == 1U);
  REQUIRE(histograms.send_latency_ns.sum_ns > 0U);
}
