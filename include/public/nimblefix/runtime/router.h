#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/codec/raw_passthrough.h"

namespace nimble::runtime {

class Engine;

/// Comparison operator for expression-based routing.
enum class ExprOp : std::uint32_t
{
  kEqual = 0,   // tag == value
  kNotEqual,    // tag != value
  kLessThan,    // tag < value (numeric comparison)
  kGreaterThan, // tag > value (numeric comparison)
  kContains,    // tag value contains substring
  kRegex,       // tag value matches regex
  kHasTag,      // tag is present in message
};

/// Logical connector for combining expression nodes.
enum class ExprLogic : std::uint32_t
{
  kAnd = 0,
  kOr,
};

/// One comparison node in a routing expression.
struct ExprNode
{
  std::uint32_t tag{ 0 };
  ExprOp op{ ExprOp::kEqual };
  std::string value;     // the comparison value (empty for kHasTag)
  bool negated{ false }; // NOT prefix
};

/// A parsed routing expression: a list of comparison nodes combined with AND/OR.
/// Evaluation: nodes are evaluated left-to-right with their connectors.
/// Precedence: AND binds tighter than OR (standard).
/// For simplicity, this is a flat list without parentheses grouping.
struct RoutingExpression
{
  struct Clause
  {
    ExprNode node;
    ExprLogic connector{ ExprLogic::kAnd }; // connector to the NEXT clause
  };
  std::vector<Clause> clauses;
};

/// Parse a routing expression from a text string.
/// Format: "55==AAPL AND 49!=BUY" or "35==D OR 35==G" or "HAS(1128)"
/// Operators: ==, !=, <, >, CONTAINS, REGEX, HAS
/// Connectors: AND, OR
/// NOT prefix: NOT HAS(1128), NOT 55==AAPL
[[nodiscard]] auto
ParseRoutingExpression(std::string_view text) -> base::Result<RoutingExpression>;

/// Evaluate a routing expression against a raw pass-through message.
[[nodiscard]] auto
EvaluateExpression(const RoutingExpression& expr, const codec::RawPassThroughView& message) -> bool;

/// Load balancing mode for forwarding rules.
enum class LoadBalancingMode : std::uint32_t
{
  kNone = 0,   // Fixed target session
  kRoundRobin, // Rotate across target sessions
  kSticky,     // Stick to one session based on a key tag
};

/// Load balancer configuration for a routing rule.
struct LoadBalancerConfig
{
  LoadBalancingMode mode{ LoadBalancingMode::kNone };
  std::vector<std::uint64_t> target_sessions; // pool of target sessions
  std::uint32_t sticky_key_tag{ 0 };          // tag to use as sticky key (e.g., 49=SenderCompID)
};

/// One field transformation to apply before forwarding.
struct FieldTransform
{
  enum class Action : std::uint32_t
  {
    kSet = 0, // Set/overwrite a tag value
    kRemove,  // Remove a tag from the body
    kAdd,     // Add a tag if not present
    kCopy,    // Copy value from one tag to another
  };

  Action action{ Action::kSet };
  std::uint32_t tag{ 0 };
  std::string value;             // for kSet/kAdd
  std::uint32_t source_tag{ 0 }; // for kCopy
};

/// Message transformation specification.
struct MessageTransform
{
  std::vector<FieldTransform> field_transforms;
};

/// Apply field transformations to a raw FIX body and produce a modified body.
/// Returns the modified body as bytes.
[[nodiscard]] auto
ApplyTransform(const codec::RawPassThroughView& message,
               const MessageTransform& transform,
               std::vector<std::byte>* output_body) -> base::Status;

/// Criteria for matching a message to a routing rule.
enum class RoutingCriterion : std::uint32_t
{
  /// Route all messages regardless of content.
  kAll = 0,
  /// Route based on MsgType value.
  kMsgType,
  /// Route based on a custom predicate function.
  kCustom,
  /// Route based on a parsed tag-expression rule.
  kExpression,
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
  /// Parsed expression (when criterion == kExpression).
  RoutingExpression expression;
  /// Action when matched.
  RoutingAction action{ RoutingAction::kForward };
  /// Target session_id for forwarding (ignored for kDrop/kReject, or when predicate returns target).
  std::uint64_t target_session_id{ 0 };
  /// Optional load-balancer configuration for selecting a forward target.
  LoadBalancerConfig load_balancer;
  /// Optional field transformations applied before forwarding.
  MessageTransform transform;
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
  RoutingTable(const RoutingTable& other);
  auto operator=(const RoutingTable& other) -> RoutingTable&;
  RoutingTable(RoutingTable&& other) noexcept;
  auto operator=(RoutingTable&& other) noexcept -> RoutingTable&;

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
    const RoutingRule* matched_rule{ nullptr };
  };

  [[nodiscard]] auto Route(const codec::RawPassThroughView& message) const -> RouteResult;

  /// Number of rules in the table.
  [[nodiscard]] auto rule_count() const -> std::size_t;

private:
  struct LoadBalancerState
  {
    std::atomic<std::uint32_t> round_robin_index{ 0 };
    std::unordered_map<std::string, std::uint64_t> sticky_map; // key -> session_id
  };

  [[nodiscard]] auto SelectLoadBalancedTarget(const RoutingRule& rule,
                                              std::uint64_t fallback_target,
                                              const codec::RawPassThroughView& message) const -> std::uint64_t;

  std::vector<RoutingRule> rules_;
  RoutingAction default_action_{ RoutingAction::kDrop };
  std::uint64_t default_target_{ 0 };
  mutable std::mutex load_balancer_mutex_;
  mutable std::unordered_map<std::string, LoadBalancerState> load_balancer_states_; // rule_name -> state
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
