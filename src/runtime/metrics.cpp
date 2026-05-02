#include "nimblefix/runtime/metrics.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace nimble::runtime {

namespace {

[[nodiscard]] auto
BucketUpperBoundNs(std::size_t bucket_index) -> std::uint64_t
{
  if (bucket_index + 1U >= kLatencyHistogramBucketCount) {
    return kLatencyHistogramOverflowLowerBoundNs;
  }
  return kLatencyHistogramFirstBucketUpperBoundNs << bucket_index;
}

[[nodiscard]] auto
BucketMidpointNs(std::size_t bucket_index) -> std::uint64_t
{
  if (bucket_index == 0U) {
    return kLatencyHistogramFirstBucketUpperBoundNs / 2U;
  }
  if (bucket_index + 1U >= kLatencyHistogramBucketCount) {
    return kLatencyHistogramOverflowLowerBoundNs;
  }
  const auto upper_bound = BucketUpperBoundNs(bucket_index);
  return upper_bound - (upper_bound / 4U);
}

[[nodiscard]] auto
MakeHistogramEntry(const LatencyHistogramSnapshot& histogram) -> RuntimeMetricsSnapshot::WorkerEntry::HistogramEntry
{
  return RuntimeMetricsSnapshot::WorkerEntry::HistogramEntry{
    .p50_ns = histogram.Percentile(50.0),
    .p90_ns = histogram.Percentile(90.0),
    .p99_ns = histogram.Percentile(99.0),
    .p999_ns = histogram.Percentile(99.9),
    .count = histogram.Count(),
    .sum_ns = histogram.SumNs(),
  };
}

[[nodiscard]] auto
MakeWorkerHistogramSnapshot(const WorkerMetrics& worker) -> RuntimeMetricsSnapshot::WorkerEntry::WorkerHistogramSnapshot
{
  return RuntimeMetricsSnapshot::WorkerEntry::WorkerHistogramSnapshot{
    .session_inbound_latency_ns = MakeHistogramEntry(worker.session_inbound_latency_ns.Snapshot()),
    .encode_latency_ns = MakeHistogramEntry(worker.encode_latency_ns.Snapshot()),
    .parse_latency_ns = MakeHistogramEntry(worker.parse_latency_ns.Snapshot()),
    .store_flush_latency_ns = MakeHistogramEntry(worker.store_flush_latency_ns.Snapshot()),
    .send_latency_ns = MakeHistogramEntry(worker.send_latency_ns.Snapshot()),
  };
}

[[nodiscard]] auto
NormalizePercentile(double p) -> double
{
  if (p <= 1.0) {
    return p * 100.0;
  }
  return p;
}

} // namespace

auto
LatencyHistogramSnapshot::Count() const -> std::uint64_t
{
  std::uint64_t total = 0U;
  for (const auto count : counts) {
    total += count;
  }
  return total;
}

auto
LatencyHistogramSnapshot::SumNs() const -> std::uint64_t
{
  std::uint64_t sum = 0U;
  for (std::size_t index = 0U; index < counts.size(); ++index) {
    sum += counts[index] * BucketMidpointNs(index);
  }
  return sum;
}

auto
LatencyHistogramSnapshot::Percentile(double p) const -> std::uint64_t
{
  const auto total = Count();
  if (total == 0U) {
    return 0U;
  }

  const auto clamped = std::clamp(NormalizePercentile(p), 0.0, 100.0);
  const auto target =
    std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::ceil((clamped / 100.0) * static_cast<double>(total))));
  std::uint64_t cumulative = 0U;
  for (std::size_t index = 0U; index < counts.size(); ++index) {
    cumulative += counts[index];
    if (cumulative >= target) {
      return BucketUpperBoundNs(index);
    }
  }
  return kLatencyHistogramOverflowLowerBoundNs;
}

auto
LatencyHistogram::Percentile(double p) const -> std::uint64_t
{
  return Snapshot().Percentile(p);
}

auto
LatencyHistogram::Snapshot() const -> LatencyHistogramSnapshot
{
  LatencyHistogramSnapshot snapshot;
  for (std::size_t index = 0U; index < buckets_.size(); ++index) {
    snapshot.counts[index] = buckets_[index].load(std::memory_order_relaxed);
  }
  return snapshot;
}

auto
LatencyHistogram::Reset() -> void
{
  for (auto& bucket : buckets_) {
    bucket.store(0U, std::memory_order_relaxed);
  }
}

auto
MetricsRegistry::Reset(std::uint32_t worker_count) -> void
{
  std::unique_lock lock(sessions_mutex_);
  workers_.clear();
  workers_.reserve(worker_count);
  for (std::uint32_t worker_id = 0; worker_id < worker_count; ++worker_id) {
    auto w = std::make_unique<WorkerMetrics>();
    w->worker_id = worker_id;
    workers_.push_back(std::move(w));
  }
  sessions_.clear();
}

auto
MetricsRegistry::FindMutableSession(std::uint64_t session_id) -> SessionMetrics*
{
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second.get();
}

auto
MetricsRegistry::FindMutableWorker(std::uint32_t worker_id) -> WorkerMetrics*
{
  if (worker_id >= workers_.size()) {
    return nullptr;
  }
  return workers_[worker_id].get();
}

auto
MetricsRegistry::RegisterSession(std::uint64_t session_id, std::uint32_t worker_id) -> base::Status
{
  if (session_id == 0) {
    return base::Status::InvalidArgument("metrics session_id must be positive");
  }
  std::unique_lock lock(sessions_mutex_);
  auto* worker = FindMutableWorker(worker_id);
  if (worker == nullptr) {
    return base::Status::NotFound("metrics worker_id is not registered");
  }
  auto s = std::make_unique<SessionMetrics>();
  s->session_id = session_id;
  s->worker_id = worker_id;
  if (!sessions_.emplace(session_id, std::move(s)).second) {
    return base::Status::AlreadyExists("metrics session already registered");
  }
  worker->registered_sessions.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordInbound(std::uint64_t session_id, bool is_admin) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->inbound_messages.fetch_add(1, std::memory_order_relaxed);
  worker->inbound_messages.fetch_add(1, std::memory_order_relaxed);
  if (is_admin) {
    session->admin_messages.fetch_add(1, std::memory_order_relaxed);
    worker->admin_messages.fetch_add(1, std::memory_order_relaxed);
  }
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordOutbound(std::uint64_t session_id, bool is_admin) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->outbound_messages.fetch_add(1, std::memory_order_relaxed);
  worker->outbound_messages.fetch_add(1, std::memory_order_relaxed);
  if (is_admin) {
    session->admin_messages.fetch_add(1, std::memory_order_relaxed);
    worker->admin_messages.fetch_add(1, std::memory_order_relaxed);
  }
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordResendRequest(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->resend_requests.fetch_add(1, std::memory_order_relaxed);
  worker->resend_requests.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordGapFill(std::uint64_t session_id, std::uint64_t count) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->gap_fills.fetch_add(count, std::memory_order_relaxed);
  worker->gap_fills.fetch_add(count, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordParseFailure(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->parse_failures.fetch_add(1, std::memory_order_relaxed);
  worker->parse_failures.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordChecksumFailure(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->checksum_failures.fetch_add(1, std::memory_order_relaxed);
  worker->checksum_failures.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::UpdateOutboundQueueDepth(std::uint64_t session_id, std::uint32_t depth) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  const auto old_depth = session->outbound_queue_depth.exchange(depth, std::memory_order_relaxed);
  worker->outbound_queue_depth.fetch_sub(old_depth, std::memory_order_relaxed);
  worker->outbound_queue_depth.fetch_add(depth, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::ObserveStoreFlushLatency(std::uint64_t session_id, std::uint64_t latency_ns) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->last_store_flush_latency_ns.store(latency_ns, std::memory_order_relaxed);
  worker->last_store_flush_latency_ns.store(latency_ns, std::memory_order_relaxed);
  worker->store_flush_latency_ns.Observe(latency_ns);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordPlainConnection(std::uint32_t worker_id) -> base::Status
{
  auto* worker = FindMutableWorker(worker_id);
  if (worker == nullptr) {
    return base::Status::NotFound("metrics worker_id is not registered");
  }
  worker->plain_connections.fetch_add(1U, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordTlsHandshake(std::uint32_t worker_id,
                                    bool success,
                                    std::uint64_t latency_ns,
                                    bool session_reused) -> base::Status
{
  auto* worker = FindMutableWorker(worker_id);
  if (worker == nullptr) {
    return base::Status::NotFound("metrics worker_id is not registered");
  }
  worker->tls_handshake_latency_ns.fetch_add(latency_ns, std::memory_order_relaxed);
  if (success) {
    worker->tls_connections.fetch_add(1U, std::memory_order_relaxed);
    worker->tls_handshake_successes.fetch_add(1U, std::memory_order_relaxed);
    if (session_reused) {
      worker->tls_session_resumptions.fetch_add(1U, std::memory_order_relaxed);
    }
  } else {
    worker->tls_handshake_failures.fetch_add(1U, std::memory_order_relaxed);
  }
  return base::Status::Ok();
}

auto
MetricsRegistry::Snapshot() const -> RuntimeMetricsSnapshot
{
  std::shared_lock lock(sessions_mutex_);
  RuntimeMetricsSnapshot snapshot;
  snapshot.workers.reserve(workers_.size());
  for (const auto& wp : workers_) {
    const auto& w = *wp;
    snapshot.workers.push_back(RuntimeMetricsSnapshot::WorkerEntry{
      .worker_id = w.worker_id,
      .registered_sessions = w.registered_sessions.load(std::memory_order_relaxed),
      .inbound_messages = w.inbound_messages.load(std::memory_order_relaxed),
      .outbound_messages = w.outbound_messages.load(std::memory_order_relaxed),
      .admin_messages = w.admin_messages.load(std::memory_order_relaxed),
      .resend_requests = w.resend_requests.load(std::memory_order_relaxed),
      .gap_fills = w.gap_fills.load(std::memory_order_relaxed),
      .parse_failures = w.parse_failures.load(std::memory_order_relaxed),
      .checksum_failures = w.checksum_failures.load(std::memory_order_relaxed),
      .outbound_queue_depth = w.outbound_queue_depth.load(std::memory_order_relaxed),
      .last_store_flush_latency_ns = w.last_store_flush_latency_ns.load(std::memory_order_relaxed),
      .poll_wait_ns = w.poll_wait_ns.load(std::memory_order_relaxed),
      .recv_dispatch_ns = w.recv_dispatch_ns.load(std::memory_order_relaxed),
      .app_callback_ns = w.app_callback_ns.load(std::memory_order_relaxed),
      .timer_process_ns = w.timer_process_ns.load(std::memory_order_relaxed),
      .send_ns = w.send_ns.load(std::memory_order_relaxed),
      .poll_iterations = w.poll_iterations.load(std::memory_order_relaxed),
      .plain_connections = w.plain_connections.load(std::memory_order_relaxed),
      .tls_connections = w.tls_connections.load(std::memory_order_relaxed),
      .tls_handshake_successes = w.tls_handshake_successes.load(std::memory_order_relaxed),
      .tls_handshake_failures = w.tls_handshake_failures.load(std::memory_order_relaxed),
      .tls_handshake_latency_ns = w.tls_handshake_latency_ns.load(std::memory_order_relaxed),
      .tls_session_resumptions = w.tls_session_resumptions.load(std::memory_order_relaxed),
      .histograms = MakeWorkerHistogramSnapshot(w),
    });
  }
  snapshot.sessions.reserve(sessions_.size());
  for (const auto& [_, sp] : sessions_) {
    const auto& s = *sp;
    snapshot.sessions.push_back(RuntimeMetricsSnapshot::SessionEntry{
      .session_id = s.session_id,
      .worker_id = s.worker_id,
      .inbound_messages = s.inbound_messages.load(std::memory_order_relaxed),
      .outbound_messages = s.outbound_messages.load(std::memory_order_relaxed),
      .read_bytes = s.read_bytes.load(std::memory_order_relaxed),
      .write_bytes = s.write_bytes.load(std::memory_order_relaxed),
      .socket_poll_count = s.socket_poll_count.load(std::memory_order_relaxed),
      .admin_messages = s.admin_messages.load(std::memory_order_relaxed),
      .resend_requests = s.resend_requests.load(std::memory_order_relaxed),
      .gap_fills = s.gap_fills.load(std::memory_order_relaxed),
      .parse_failures = s.parse_failures.load(std::memory_order_relaxed),
      .checksum_failures = s.checksum_failures.load(std::memory_order_relaxed),
      .outbound_queue_depth = static_cast<std::uint32_t>(s.outbound_queue_depth.load(std::memory_order_relaxed)),
      .last_store_flush_latency_ns = s.last_store_flush_latency_ns.load(std::memory_order_relaxed),
    });
  }
  std::sort(snapshot.sessions.begin(), snapshot.sessions.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.session_id < rhs.session_id;
  });
  return snapshot;
}

auto
MetricsRegistry::FindSession(std::uint64_t session_id) const -> const SessionMetrics*
{
  std::shared_lock lock(sessions_mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second.get();
}

auto
MetricsRegistry::FindSession(std::uint64_t session_id) -> SessionMetrics*
{
  std::shared_lock lock(sessions_mutex_);
  return FindMutableSession(session_id);
}

auto
MetricsRegistry::FindWorker(std::uint32_t worker_id) const -> const WorkerMetrics*
{
  if (worker_id >= workers_.size()) {
    return nullptr;
  }
  return workers_[worker_id].get();
}

auto
MetricsRegistry::FindWorker(std::uint32_t worker_id) -> WorkerMetrics*
{
  return FindMutableWorker(worker_id);
}

} // namespace nimble::runtime
