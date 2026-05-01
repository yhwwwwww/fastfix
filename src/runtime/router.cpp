#include "nimblefix/runtime/router.h"

#include <algorithm>
#include <utility>

namespace nimble::runtime {

auto
RoutingTable::AddRule(RoutingRule rule) -> void
{
  const auto insert_at =
    std::upper_bound(rules_.begin(), rules_.end(), rule.priority, [](const auto priority, const auto& entry) {
      return priority < entry.priority;
    });
  rules_.insert(insert_at, std::move(rule));
}

auto
RoutingTable::RemoveRule(std::string_view name) -> void
{
  rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const auto& rule) { return rule.name == name; }),
               rules_.end());
}

auto
RoutingTable::Clear() -> void
{
  rules_.clear();
}

auto
RoutingTable::SetDefaultAction(RoutingAction action) -> void
{
  default_action_ = action;
}

auto
RoutingTable::SetDefaultTarget(std::uint64_t session_id) -> void
{
  default_target_ = session_id;
}

auto
RoutingTable::Route(const codec::RawPassThroughView& message) const -> RouteResult
{
  for (const auto& rule : rules_) {
    auto matched = false;
    auto target_session_id = rule.target_session_id;

    switch (rule.criterion) {
      case RoutingCriterion::kAll:
        matched = true;
        break;
      case RoutingCriterion::kMsgType:
        matched = message.msg_type == rule.msg_type;
        break;
      case RoutingCriterion::kCustom:
        if (rule.predicate) {
          target_session_id = rule.predicate(message);
          matched = target_session_id != 0U;
        }
        break;
    }

    if (matched) {
      return RouteResult{
        .action = rule.action,
        .target_session_id = rule.action == RoutingAction::kForward ? target_session_id : 0U,
        .matched_rule_name = rule.name,
      };
    }
  }

  return RouteResult{
    .action = default_action_,
    .target_session_id = default_action_ == RoutingAction::kForward ? default_target_ : 0U,
    .matched_rule_name = {},
  };
}

auto
RoutingTable::rule_count() const -> std::size_t
{
  return rules_.size();
}

auto
ForwardMessage(const codec::RawPassThroughView& inbound,
               const RoutingTable& table,
               const codec::ForwardingOptions& forwarding_options,
               codec::EncodeBuffer* buffer) -> base::Result<ForwardResult>
{
  if (buffer == nullptr) {
    return base::Status::InvalidArgument("encode buffer is null");
  }

  ForwardResult result;
  const auto route = table.Route(inbound);
  switch (route.action) {
    case RoutingAction::kDrop:
      ++result.dropped;
      buffer->clear();
      return result;
    case RoutingAction::kReject:
      ++result.rejected;
      buffer->clear();
      return result;
    case RoutingAction::kForward:
      break;
  }

  if (route.target_session_id == 0U) {
    ++result.failed;
    return base::Status::InvalidArgument("routing rule selected forward without a target session");
  }

  const auto status = codec::EncodeForwarded(inbound, forwarding_options, buffer);
  if (!status.ok()) {
    ++result.failed;
    return status;
  }

  ++result.forwarded;
  return result;
}

} // namespace nimble::runtime
