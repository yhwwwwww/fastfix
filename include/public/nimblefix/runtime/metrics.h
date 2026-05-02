#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "nimblefix/base/status.h"

namespace nimble::runtime {

inline constexpr std::size_t kLatencyHistogramBucketCount = 16U;
inline constexpr std::uint64_t kLatencyHistogramFirstBucketUpperBoundNs = 64U;
inline constexpr std::uint64_t kLatencyHistogramOverflowLowerBoundNs = 1'048'576U;

struct LatencyHistogramSnapshot
{
  std::array<std::uint64_t, kLatencyHistogramBucketCount> counts{};

  [[nodiscard]] auto Count() const -> std::uint64_t;
  [[nodiscard]] auto SumNs() const -> std::uint64_t;
  [[nodiscard]] auto Percentile(double p) const -> std::uint64_t;
};

class LatencyHistogram
{
public:
  auto Observe(std::uint64_t value_ns) -> void
  {
    buckets_[BucketIndex(value_ns)].fetch_add(1U, std::memory_order_relaxed);
  }

  [[nodiscard]] auto Percentile(double p) const -> std::uint64_t;
  [[nodiscard]] auto Snapshot() const -> LatencyHistogramSnapshot;
  auto Reset() -> void;

private:
  [[nodiscard]] static auto BucketIndex(std::uint64_t value_ns) -> std::size_t
  {
    if (value_ns < kLatencyHistogramFirstBucketUpperBoundNs) {
      return 0U;
    }
#if defined(__GNUC__) || defined(__clang__)
    const auto highest_bit = 63U - static_cast<unsigned>(__builtin_clzll(value_ns));
#else
    auto shifted_value = value_ns;
    unsigned highest_bit = 0U;
    while (shifted_value >>= 1U) {
      ++highest_bit;
    }
#endif
    const auto bucket = static_cast<std::size_t>(highest_bit - 5U);
    return bucket < kLatencyHistogramBucketCount ? bucket : kLatencyHistogramBucketCount - 1U;
  }

  std::array<std::atomic<std::uint64_t>, kLatencyHistogramBucketCount> buckets_{};
};

struct SessionMetrics
{
  std::uint64_t session_id{ 0 };
  std::uint32_t worker_id{ 0 };
  std::atomic<std::uint64_t> inbound_messages{ 0 };
  std::atomic<std::uint64_t> outbound_messages{ 0 };
  std::atomic<std::uint64_t> read_bytes{ 0 };
  std::atomic<std::uint64_t> write_bytes{ 0 };
  std::atomic<std::uint64_t> socket_poll_count{ 0 };
  std::atomic<std::uint64_t> admin_messages{ 0 };
  std::atomic<std::uint64_t> resend_requests{ 0 };
  std::atomic<std::uint64_t> gap_fills{ 0 };
  std::atomic<std::uint64_t> parse_failures{ 0 };
  std::atomic<std::uint64_t> checksum_failures{ 0 };
  std::atomic<std::uint32_t> outbound_queue_depth{ 0 };
  std::atomic<std::uint64_t> last_store_flush_latency_ns{ 0 };
};

struct WorkerMetrics
{
  std::uint32_t worker_id{ 0 };
  std::atomic<std::uint64_t> registered_sessions{ 0 };
  std::atomic<std::uint64_t> inbound_messages{ 0 };
  std::atomic<std::uint64_t> outbound_messages{ 0 };
  std::atomic<std::uint64_t> admin_messages{ 0 };
  std::atomic<std::uint64_t> resend_requests{ 0 };
  std::atomic<std::uint64_t> gap_fills{ 0 };
  std::atomic<std::uint64_t> parse_failures{ 0 };
  std::atomic<std::uint64_t> checksum_failures{ 0 };
  std::atomic<std::uint64_t> outbound_queue_depth{ 0 };
  std::atomic<std::uint64_t> last_store_flush_latency_ns{ 0 };
  std::atomic<std::uint64_t> plain_connections{ 0 };
  std::atomic<std::uint64_t> tls_connections{ 0 };
  std::atomic<std::uint64_t> tls_handshake_successes{ 0 };
  std::atomic<std::uint64_t> tls_handshake_failures{ 0 };
  std::atomic<std::uint64_t> tls_handshake_latency_ns{ 0 };
  std::atomic<std::uint64_t> tls_session_resumptions{ 0 };

  // Steady-state breakdown timing (nanoseconds, relaxed stores from worker
  // thread).
  std::atomic<std::uint64_t> poll_wait_ns{ 0 };
  std::atomic<std::uint64_t> recv_dispatch_ns{ 0 };
  std::atomic<std::uint64_t> app_callback_ns{ 0 };
  std::atomic<std::uint64_t> timer_process_ns{ 0 };
  std::atomic<std::uint64_t> send_ns{ 0 };
  std::atomic<std::uint64_t> poll_iterations{ 0 };

  LatencyHistogram session_inbound_latency_ns;
  LatencyHistogram encode_latency_ns;
  LatencyHistogram parse_latency_ns;
  LatencyHistogram store_flush_latency_ns;
  LatencyHistogram send_latency_ns;
};

struct RuntimeMetricsSnapshot
{
  struct SessionEntry
  {
    std::uint64_t session_id{ 0 };
    std::uint32_t worker_id{ 0 };
    std::uint64_t inbound_messages{ 0 };
    std::uint64_t outbound_messages{ 0 };
    std::uint64_t read_bytes{ 0 };
    std::uint64_t write_bytes{ 0 };
    std::uint64_t socket_poll_count{ 0 };
    std::uint64_t admin_messages{ 0 };
    std::uint64_t resend_requests{ 0 };
    std::uint64_t gap_fills{ 0 };
    std::uint64_t parse_failures{ 0 };
    std::uint64_t checksum_failures{ 0 };
    std::uint32_t outbound_queue_depth{ 0 };
    std::uint64_t last_store_flush_latency_ns{ 0 };
  };

  struct WorkerEntry
  {
    struct HistogramEntry
    {
      std::uint64_t p50_ns{ 0 };
      std::uint64_t p90_ns{ 0 };
      std::uint64_t p99_ns{ 0 };
      std::uint64_t p999_ns{ 0 };
      std::uint64_t count{ 0 };
      std::uint64_t sum_ns{ 0 };
    };

    struct WorkerHistogramSnapshot
    {
      HistogramEntry session_inbound_latency_ns;
      HistogramEntry encode_latency_ns;
      HistogramEntry parse_latency_ns;
      HistogramEntry store_flush_latency_ns;
      HistogramEntry send_latency_ns;
    };

    std::uint32_t worker_id{ 0 };
    std::uint64_t registered_sessions{ 0 };
    std::uint64_t inbound_messages{ 0 };
    std::uint64_t outbound_messages{ 0 };
    std::uint64_t admin_messages{ 0 };
    std::uint64_t resend_requests{ 0 };
    std::uint64_t gap_fills{ 0 };
    std::uint64_t parse_failures{ 0 };
    std::uint64_t checksum_failures{ 0 };
    std::uint64_t outbound_queue_depth{ 0 };
    std::uint64_t last_store_flush_latency_ns{ 0 };
    std::uint64_t poll_wait_ns{ 0 };
    std::uint64_t recv_dispatch_ns{ 0 };
    std::uint64_t app_callback_ns{ 0 };
    std::uint64_t timer_process_ns{ 0 };
    std::uint64_t send_ns{ 0 };
    std::uint64_t poll_iterations{ 0 };
    std::uint64_t plain_connections{ 0 };
    std::uint64_t tls_connections{ 0 };
    std::uint64_t tls_handshake_successes{ 0 };
    std::uint64_t tls_handshake_failures{ 0 };
    std::uint64_t tls_handshake_latency_ns{ 0 };
    std::uint64_t tls_session_resumptions{ 0 };
    WorkerHistogramSnapshot histograms;
  };

  std::vector<WorkerEntry> workers;
  std::vector<SessionEntry> sessions;
};

class MetricsRegistry
{
public:
  auto Reset(std::uint32_t worker_count) -> void;
  auto RegisterSession(std::uint64_t session_id, std::uint32_t worker_id) -> base::Status;

  auto RecordInbound(std::uint64_t session_id, bool is_admin) -> base::Status;
  auto RecordOutbound(std::uint64_t session_id, bool is_admin) -> base::Status;
  auto RecordResendRequest(std::uint64_t session_id) -> base::Status;
  auto RecordGapFill(std::uint64_t session_id, std::uint64_t count) -> base::Status;
  auto RecordParseFailure(std::uint64_t session_id) -> base::Status;
  auto RecordChecksumFailure(std::uint64_t session_id) -> base::Status;
  auto UpdateOutboundQueueDepth(std::uint64_t session_id, std::uint32_t depth) -> base::Status;
  auto ObserveStoreFlushLatency(std::uint64_t session_id, std::uint64_t latency_ns) -> base::Status;
  auto RecordPlainConnection(std::uint32_t worker_id) -> base::Status;
  auto RecordTlsHandshake(std::uint32_t worker_id, bool success, std::uint64_t latency_ns, bool session_reused)
    -> base::Status;

  [[nodiscard]] auto Snapshot() const -> RuntimeMetricsSnapshot;
  [[nodiscard]] auto FindSession(std::uint64_t session_id) const -> const SessionMetrics*;
  [[nodiscard]] auto FindSession(std::uint64_t session_id) -> SessionMetrics*;
  // Workers are allocated during Reset()/Boot() and then only read
  // concurrently.
  [[nodiscard]] auto FindWorker(std::uint32_t worker_id) const -> const WorkerMetrics*;
  // Mutable worker pointers are stable after Reset(); callers only mutate the
  // atomics they own.
  [[nodiscard]] auto FindWorker(std::uint32_t worker_id) -> WorkerMetrics*;

private:
  auto FindMutableSession(std::uint64_t session_id) -> SessionMetrics*;
  auto FindMutableWorker(std::uint32_t worker_id) -> WorkerMetrics*;

  mutable std::shared_mutex sessions_mutex_;
  std::vector<std::unique_ptr<WorkerMetrics>> workers_;
  std::unordered_map<std::uint64_t, std::unique_ptr<SessionMetrics>> sessions_;
};

} // namespace nimble::runtime
