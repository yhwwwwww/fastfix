#include "nimblefix/runtime/connection_strategy.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace nimble::runtime {

namespace {

constexpr std::uint64_t kReconnectJitterSeedMask = 0xFFFF'FFFFULL;
constexpr std::uint32_t kReconnectJitterDivisor = 4U;

auto
ReconnectJitterLimit(std::uint32_t backoff_ms) -> std::uint32_t
{
  return backoff_ms / kReconnectJitterDivisor;
}

auto
RandomJitter(std::uint32_t max_jitter_ms) -> std::uint32_t
{
  if (max_jitter_ms == 0U) {
    return 0U;
  }
  thread_local std::mt19937 rng(
    static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) &
                               kReconnectJitterSeedMask));
  return std::uniform_int_distribution<std::uint32_t>(0, max_jitter_ms)(rng);
}

auto
EffectiveFailureCount(const ConnectionContext& context, std::uint32_t attempt_count) -> std::uint32_t
{
  return context.consecutive_failures == 0U ? attempt_count : context.consecutive_failures;
}

auto
ShouldGiveUp(const ConnectionContext& context, std::uint32_t attempt_count, std::uint32_t max_retries) -> bool
{
  return max_retries != 0U && EffectiveFailureCount(context, attempt_count) >= max_retries;
}

} // namespace

ExponentialBackoffStrategy::ExponentialBackoffStrategy(std::uint32_t initial_ms,
                                                       std::uint32_t max_ms,
                                                       std::uint32_t max_retries)
  : initial_ms_(initial_ms)
  , max_ms_(max_ms)
  , max_retries_(max_retries)
{
}

auto
ExponentialBackoffStrategy::OnDisconnected(const ConnectionContext& context) -> ConnectionDecision
{
  if (ShouldGiveUp(context, attempt_count_, max_retries_)) {
    return ConnectionDecision{ .give_up = true };
  }

  const auto previous_backoff = current_backoff_ms_ == 0U ? initial_ms_ : current_backoff_ms_;
  const auto next_backoff = current_backoff_ms_ == 0U
                              ? initial_ms_
                              : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                  static_cast<std::uint64_t>(current_backoff_ms_) * 2U, max_ms_));
  const auto jitter = RandomJitter(ReconnectJitterLimit(previous_backoff));
  current_backoff_ms_ = next_backoff;
  ++attempt_count_;

  return ConnectionDecision{
    .delay = std::chrono::milliseconds{ static_cast<std::int64_t>(next_backoff) + static_cast<std::int64_t>(jitter) },
  };
}

auto
ExponentialBackoffStrategy::OnConnected(const ConnectionContext& /*context*/) -> void
{
  current_backoff_ms_ = 0U;
  attempt_count_ = 0U;
}

AlwaysStartOnPrimaryStrategy::AlwaysStartOnPrimaryStrategy(std::uint32_t delay_ms, std::uint32_t max_retries)
  : delay_ms_(delay_ms)
  , max_retries_(max_retries)
{
}

auto
AlwaysStartOnPrimaryStrategy::OnDisconnected(const ConnectionContext& context) -> ConnectionDecision
{
  if (ShouldGiveUp(context, attempt_count_, max_retries_)) {
    return ConnectionDecision{ .give_up = true };
  }
  ++attempt_count_;
  return ConnectionDecision{
    .delay = std::chrono::milliseconds{ delay_ms_ },
    .next_endpoint = context.primary_endpoint,
  };
}

auto
AlwaysStartOnPrimaryStrategy::OnConnected(const ConnectionContext& /*context*/) -> void
{
  attempt_count_ = 0U;
}

RoundRobinStrategy::RoundRobinStrategy(std::uint32_t delay_ms, std::uint32_t max_retries)
  : delay_ms_(delay_ms)
  , max_retries_(max_retries)
{
}

auto
RoundRobinStrategy::OnDisconnected(const ConnectionContext& context) -> ConnectionDecision
{
  if (ShouldGiveUp(context, attempt_count_, max_retries_)) {
    return ConnectionDecision{ .give_up = true };
  }

  const auto endpoint_count = static_cast<std::uint32_t>(context.alternate_endpoints.size() + 1U);
  const auto selected_index = endpoint_count == 0U ? 0U : current_index_ % endpoint_count;
  auto endpoint = selected_index == 0U ? context.primary_endpoint : context.alternate_endpoints[selected_index - 1U];
  current_index_ = endpoint_count == 0U ? 0U : (selected_index + 1U) % endpoint_count;
  ++attempt_count_;

  return ConnectionDecision{
    .delay = std::chrono::milliseconds{ delay_ms_ },
    .next_endpoint = std::move(endpoint),
  };
}

auto
RoundRobinStrategy::OnConnected(const ConnectionContext& /*context*/) -> void
{
  attempt_count_ = 0U;
  current_index_ = 0U;
}

} // namespace nimble::runtime
