#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/codec/raw_passthrough.h"

namespace nimble::runtime {

class Engine;

/// Criteria for matching a message to a routing rule.
enum class RoutingCriterion : std::uint32_t
{
  /// Route all messages regardless of content.
  kAll = 0,
  /// Route based on MsgType value.
  kMsgType,
  /// Route based on a custom predicate function.
  kCustom,
};

/// Action to take when a routing rule matches.
enum class RoutingAction : std::uint32_t
{
  /// Forward the message to the target session(s).
  kForward = 0,
  /// Drop the message silently.
  kDrop,
  /// Reject the message back to the sender.
  kReject,
};

/// Predicate function for custom routing decisions.
/// Returns the target session_id to forward to, or 0 to indicate no match.
using RoutingPredicate = std::function<std::uint64_t(const codec::RawPassThroughView& message)>;

/// One routing rule in a routing table.
struct RoutingRule
{
  /// Human-readable name for this rule (for logging/diagnostics).
  std::string name;
  /// Criterion type for matching.
  RoutingCriterion criterion{ RoutingCriterion::kAll };
  /// MsgType value to match (when criterion == kMsgType).
  std::string msg_type;
  /// Custom predicate (when criterion == kCustom).
  RoutingPredicate predicate;
  /// Action when matched.
  RoutingAction action{ RoutingAction::kForward };
  /// Target session_id for forwarding (ignored for kDrop/kReject, or when predicate returns target).
  std::uint64_t target_session_id{ 0 };
  /// Priority (lower = higher priority). First matching rule wins.
  std::uint32_t priority{ 100 };
};

/// Routing table that maps inbound messages to outbound targets.
///
/// Rules are evaluated in priority order (ascending). First match wins.
/// If no rule matches, the default action applies.
class RoutingTable
{
public:
  RoutingTable() = default;

  /// Add a rule to the routing table.
  auto AddRule(RoutingRule rule) -> void;

  /// Remove all rules with the given name.
  auto RemoveRule(std::string_view name) -> void;

  /// Clear all rules.
  auto Clear() -> void;

  /// Set the default action for messages that match no rule.
  auto SetDefaultAction(RoutingAction action) -> void;

  /// Set the default target for messages that match no rule (when default action is kForward).
  auto SetDefaultTarget(std::uint64_t session_id) -> void;

  /// Evaluate the routing table against an inbound message.
  ///
  /// Returns the action and target session_id.
  struct RouteResult
  {
    RoutingAction action{ RoutingAction::kDrop };
    std::uint64_t target_session_id{ 0 };
    std::string_view matched_rule_name;
  };

  [[nodiscard]] auto Route(const codec::RawPassThroughView& message) const -> RouteResult;

  /// Number of rules in the table.
  [[nodiscard]] auto rule_count() const -> std::size_t;

private:
  std::vector<RoutingRule> rules_;
  RoutingAction default_action_{ RoutingAction::kDrop };
  std::uint64_t default_target_{ 0 };
};

/// Configuration for a session-level forwarding bridge.
struct ForwardingBridgeConfig
{
  /// Source session_id (inbound messages from this session are routed).
  std::uint64_t source_session_id{ 0 };
  /// Routing table for this bridge.
  RoutingTable routing_table;
  /// Whether to preserve PossDupFlag from inbound to outbound.
  bool preserve_poss_dup{ true };
  /// Whether to set OnBehalfOfCompID on forwarded messages.
  bool set_on_behalf_of{ true };
};

/// Result of a forwarding operation.
struct ForwardResult
{
  /// Number of messages successfully forwarded.
  std::uint32_t forwarded{ 0 };
  /// Number of messages dropped.
  std::uint32_t dropped{ 0 };
  /// Number of messages rejected.
  std::uint32_t rejected{ 0 };
  /// Number of messages that failed to forward (encoding/routing errors).
  std::uint32_t failed{ 0 };
};

/// Forward a single raw inbound message according to a routing table.
///
/// This is the core primitive for building FIX proxies. It:
/// 1. Evaluates the routing table against the inbound message
/// 2. Re-encodes the message with forwarding options (new sender/target/seq)
/// 3. Returns the forwarding result
///
/// The caller is responsible for:
/// - Obtaining the raw message from the inbound session
/// - Sending the re-encoded frame on the target session
///
/// \param inbound Decoded raw pass-through view of the inbound message.
/// \param table Routing table to evaluate.
/// \param forwarding_options Base forwarding options (sender/target/seq for the downstream session).
/// \param buffer Encode buffer to write the forwarded frame into.
/// \return ForwardResult with one message counted.
[[nodiscard]] auto
ForwardMessage(const codec::RawPassThroughView& inbound,
               const RoutingTable& table,
               const codec::ForwardingOptions& forwarding_options,
               codec::EncodeBuffer* buffer) -> base::Result<ForwardResult>;

} // namespace nimble::runtime
