#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimblefix/base/result.h"

namespace nimble::runtime {

class Engine;

/// Type of warmup action to perform.
enum class WarmupAction : std::uint32_t
{
  /// Encode a message using the profile's encode path.
  kEncode = 0,
  /// Parse a raw FIX message through the codec.
  kParse,
  /// Encode then parse (full round-trip warmup).
  kRoundTrip,
  /// Touch profile dictionary pages (madvise/prefetch).
  kTouchProfile,
  /// Run the session send path without actual network I/O.
  kDrySend,
};

/// One warmup step in a warmup sequence.
struct WarmupStep
{
  /// Action to perform.
  WarmupAction action{ WarmupAction::kEncode };
  /// Profile ID to use for encode/parse steps.
  std::uint64_t profile_id{ 0 };
  /// MsgType to encode (for kEncode/kRoundTrip/kDrySend).
  std::string msg_type;
  /// Raw FIX bytes to parse (for kParse/kRoundTrip). If empty, a synthetic
  /// message is generated from msg_type.
  std::vector<std::byte> raw_message;
  /// Number of iterations to repeat this step.
  std::uint32_t iterations{ 1 };
};

/// Configuration for an engine warmup sequence.
struct WarmupConfig
{
  /// Ordered list of warmup steps.
  std::vector<WarmupStep> steps;
  /// Total time budget for warmup. Steps are skipped if time expires.
  std::chrono::milliseconds time_budget{ std::chrono::milliseconds{ 5000 } };
  /// Whether to report per-step timing in the result.
  bool report_timing{ true };
};

/// Result of one warmup step execution.
struct WarmupStepResult
{
  WarmupAction action{};
  std::string msg_type;
  std::uint32_t iterations_completed{ 0 };
  std::chrono::nanoseconds elapsed{};
  bool skipped{ false };
  std::string skip_reason;
};

/// Result of a complete warmup sequence.
struct WarmupResult
{
  /// Per-step results.
  std::vector<WarmupStepResult> steps;
  /// Total elapsed time for the warmup.
  std::chrono::nanoseconds total_elapsed{};
  /// Number of steps completed (not skipped).
  std::uint32_t steps_completed{ 0 };
  /// Number of steps skipped (time budget exceeded or errors).
  std::uint32_t steps_skipped{ 0 };
  /// Whether the warmup completed within the time budget.
  bool within_budget{ true };
  /// Human-readable summary.
  [[nodiscard]] auto summary() const -> std::string;
};

/// Execute a warmup sequence against a booted engine.
///
/// Warmup exercises the codec, dictionary lookup, and encode/parse hot paths
/// without actual network I/O. This is used to warm CPU caches, TLBs, and
/// branch predictors before production traffic arrives.
///
/// Precondition: Engine must be booted.
///
/// \param engine Booted engine with loaded profiles.
/// \param config Warmup configuration.
/// \return WarmupResult with timing and completion information.
[[nodiscard]] auto
RunWarmup(Engine& engine, const WarmupConfig& config) -> base::Result<WarmupResult>;

/// Create a default warmup config that exercises all loaded profiles.
///
/// Generates encode + parse steps for each common MsgType in each loaded profile.
[[nodiscard]] auto
DefaultWarmupConfig(const Engine& engine, std::uint32_t iterations_per_step = 100) -> WarmupConfig;

} // namespace nimble::runtime
