#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nimble::runtime {

/// Endpoint address for connection attempts.
struct ConnectionEndpoint
{
  std::string host;
  std::uint16_t port{ 0 };

  [[nodiscard]] friend auto operator==(const ConnectionEndpoint&, const ConnectionEndpoint&) -> bool = default;
};

/// Result of a strategy decision after a disconnect or failed connect.
struct ConnectionDecision
{
  /// Delay before next connection attempt.
  std::chrono::milliseconds delay{ 0 };
  /// Which endpoint to connect to next. If empty, use the original endpoint.
  std::optional<ConnectionEndpoint> next_endpoint;
  /// Whether to give up (no more retries).
  bool give_up{ false };
};

/// Context passed to the strategy for decision-making.
struct ConnectionContext
{
  std::uint64_t session_id{ 0 };
  std::uint32_t consecutive_failures{ 0 };
  std::chrono::milliseconds last_connected_duration{ 0 };
  /// The original configured endpoint.
  ConnectionEndpoint primary_endpoint;
  /// Additional endpoints (if configured).
  std::vector<ConnectionEndpoint> alternate_endpoints;
};

/// Abstract interface for controlling reconnect behavior.
///
/// The initiator calls OnDisconnected() after a transport close or connect
/// failure. The returned ConnectionDecision tells the initiator when and
/// where to reconnect next.
class ConnectionStrategy
{
public:
  virtual ~ConnectionStrategy() = default;

  /// Called when a connection attempt fails or an established session disconnects.
  [[nodiscard]] virtual auto OnDisconnected(const ConnectionContext& context) -> ConnectionDecision = 0;

  /// Called when a connection is successfully established. Resets internal state.
  virtual auto OnConnected(const ConnectionContext& context) -> void = 0;
};

/// Exponential backoff with jitter (wraps the existing NimbleFIX behavior).
class ExponentialBackoffStrategy final : public ConnectionStrategy
{
public:
  explicit ExponentialBackoffStrategy(std::uint32_t initial_ms = 1000,
                                      std::uint32_t max_ms = 30000,
                                      std::uint32_t max_retries = 0);

  [[nodiscard]] auto OnDisconnected(const ConnectionContext& context) -> ConnectionDecision override;
  auto OnConnected(const ConnectionContext& context) -> void override;

private:
  std::uint32_t initial_ms_;
  std::uint32_t max_ms_;
  std::uint32_t max_retries_;
  std::uint32_t current_backoff_ms_{ 0 };
  std::uint32_t attempt_count_{ 0 };
};

/// Always reconnect to the primary endpoint with a fixed delay.
class AlwaysStartOnPrimaryStrategy final : public ConnectionStrategy
{
public:
  explicit AlwaysStartOnPrimaryStrategy(std::uint32_t delay_ms = 1000, std::uint32_t max_retries = 0);

  [[nodiscard]] auto OnDisconnected(const ConnectionContext& context) -> ConnectionDecision override;
  auto OnConnected(const ConnectionContext& context) -> void override;

private:
  std::uint32_t delay_ms_;
  std::uint32_t max_retries_;
  std::uint32_t attempt_count_{ 0 };
};

/// Cycle through primary + alternate endpoints in round-robin order.
class RoundRobinStrategy final : public ConnectionStrategy
{
public:
  explicit RoundRobinStrategy(std::uint32_t delay_ms = 1000, std::uint32_t max_retries = 0);

  [[nodiscard]] auto OnDisconnected(const ConnectionContext& context) -> ConnectionDecision override;
  auto OnConnected(const ConnectionContext& context) -> void override;

private:
  std::uint32_t delay_ms_;
  std::uint32_t max_retries_;
  std::uint32_t attempt_count_{ 0 };
  std::uint32_t current_index_{ 0 };
};

} // namespace nimble::runtime
