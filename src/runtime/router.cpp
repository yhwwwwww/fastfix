#include "nimblefix/runtime/router.h"

#include "nimblefix/codec/fix_tags.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nimble::runtime {

namespace {

struct FieldSpan
{
  std::size_t field_begin{ 0 };
  std::size_t value_begin{ 0 };
  std::size_t value_end{ 0 };
  std::size_t field_end{ 0 };
};

[[nodiscard]] auto
ToUpperAscii(std::string_view text) -> std::string
{
  std::string upper(text);
  for (auto& ch : upper) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
  }
  return upper;
}

[[nodiscard]] auto
ParseUint32Strict(std::string_view text, std::uint32_t* out) -> bool
{
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::uint32_t value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last || value == 0U) {
    return false;
  }
  *out = value;
  return true;
}

[[nodiscard]] auto
ParseDoubleStrict(std::string_view text, double* out) -> bool
{
  if (out == nullptr || text.empty()) {
    return false;
  }
  double value = 0.0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last) {
    return false;
  }
  *out = value;
  return true;
}

[[nodiscard]] auto
BytesToStringView(std::span<const std::byte> bytes) -> std::string_view
{
  if (bytes.empty()) {
    return {};
  }
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] auto
FindField(std::string_view body, std::uint32_t wanted_tag) -> std::optional<FieldSpan>
{
  std::size_t field_begin = 0U;
  while (field_begin < body.size()) {
    auto field_end_without_delim = field_begin;
    while (field_end_without_delim < body.size() && body[field_end_without_delim] != codec::kFixSoh) {
      ++field_end_without_delim;
    }

    const auto field_end =
      field_end_without_delim < body.size() ? field_end_without_delim + 1U : field_end_without_delim;
    const auto field = body.substr(field_begin, field_end_without_delim - field_begin);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && equals > 0U) {
      std::uint32_t tag = 0U;
      if (ParseUint32Strict(field.substr(0U, equals), &tag) && tag == wanted_tag) {
        return FieldSpan{
          .field_begin = field_begin,
          .value_begin = field_begin + equals + 1U,
          .value_end = field_end_without_delim,
          .field_end = field_end,
        };
      }
    }

    if (field_end_without_delim == body.size()) {
      break;
    }
    field_begin = field_end;
  }
  return std::nullopt;
}

[[nodiscard]] auto
ExtractTagValue(std::span<const std::byte> bytes, std::uint32_t tag) -> std::optional<std::string>
{
  const auto body = BytesToStringView(bytes);
  const auto span = FindField(body, tag);
  if (!span.has_value()) {
    return std::nullopt;
  }
  return std::string(body.substr(span->value_begin, span->value_end - span->value_begin));
}

[[nodiscard]] auto
KnownHeaderTagValue(const codec::RawPassThroughView& message, std::uint32_t tag) -> std::optional<std::string>
{
  using namespace codec::tags;
  switch (tag) {
    case kBeginString:
      if (!message.begin_string.empty()) {
        return std::string(message.begin_string);
      }
      break;
    case kMsgType:
      if (!message.msg_type.empty()) {
        return std::string(message.msg_type);
      }
      break;
    case kMsgSeqNum:
      if (message.msg_seq_num != 0U) {
        return std::to_string(message.msg_seq_num);
      }
      break;
    case kPossDupFlag:
      if (message.poss_dup) {
        return std::string("Y");
      }
      break;
    case kSenderCompID:
      if (!message.sender_comp_id.empty()) {
        return std::string(message.sender_comp_id);
      }
      break;
    case kSenderSubID:
      if (!message.sender_sub_id.empty()) {
        return std::string(message.sender_sub_id);
      }
      break;
    case kSendingTime:
      if (!message.sending_time.empty()) {
        return std::string(message.sending_time);
      }
      break;
    case kTargetCompID:
      if (!message.target_comp_id.empty()) {
        return std::string(message.target_comp_id);
      }
      break;
    case kTargetSubID:
      if (!message.target_sub_id.empty()) {
        return std::string(message.target_sub_id);
      }
      break;
    case kPossResend:
      if (message.poss_resend) {
        return std::string("Y");
      }
      break;
    case kOnBehalfOfCompID:
      if (!message.on_behalf_of_comp_id.empty()) {
        return std::string(message.on_behalf_of_comp_id);
      }
      break;
    case kOrigSendingTime:
      if (!message.orig_sending_time.empty()) {
        return std::string(message.orig_sending_time);
      }
      break;
    case kDeliverToCompID:
      if (!message.deliver_to_comp_id.empty()) {
        return std::string(message.deliver_to_comp_id);
      }
      break;
    case kApplVerID:
      if (!message.appl_ver_id.empty()) {
        return std::string(message.appl_ver_id);
      }
      break;
    case kDefaultApplVerID:
      if (!message.default_appl_ver_id.empty()) {
        return std::string(message.default_appl_ver_id);
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

[[nodiscard]] auto
ExtractMessageTagValue(const codec::RawPassThroughView& message, std::uint32_t tag) -> std::optional<std::string>
{
  if (auto value = KnownHeaderTagValue(message, tag); value.has_value()) {
    return value;
  }
  if (auto value = ExtractTagValue(message.raw_body, tag); value.has_value()) {
    return value;
  }
  return ExtractTagValue(message.raw_message, tag);
}

[[nodiscard]] auto
Tokenize(std::string_view text) -> std::vector<std::string_view>
{
  std::vector<std::string_view> tokens;
  std::size_t pos = 0U;
  while (pos < text.size()) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
      ++pos;
    }
    if (pos >= text.size()) {
      break;
    }
    auto end = pos;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' && text[end] != '\r') {
      ++end;
    }
    tokens.push_back(text.substr(pos, end - pos));
    pos = end;
  }
  return tokens;
}

[[nodiscard]] auto
ParseHasToken(std::string_view token, std::uint32_t* tag) -> bool
{
  const auto upper = ToUpperAscii(token);
  if (upper.size() < 6U || upper.rfind("HAS(", 0U) != 0U || upper.back() != ')') {
    return false;
  }
  return ParseUint32Strict(token.substr(4U, token.size() - 5U), tag);
}

[[nodiscard]] auto
ParseOperatorToken(std::string_view token) -> std::optional<ExprOp>
{
  const auto upper = ToUpperAscii(token);
  if (upper == "==") {
    return ExprOp::kEqual;
  }
  if (upper == "!=") {
    return ExprOp::kNotEqual;
  }
  if (upper == "<") {
    return ExprOp::kLessThan;
  }
  if (upper == ">") {
    return ExprOp::kGreaterThan;
  }
  if (upper == "CONTAINS") {
    return ExprOp::kContains;
  }
  if (upper == "REGEX") {
    return ExprOp::kRegex;
  }
  if (upper == "HAS") {
    return ExprOp::kHasTag;
  }
  return std::nullopt;
}

[[nodiscard]] auto
FindInlineComparisonOperator(std::string_view token, std::size_t* op_pos, ExprOp* op) -> bool
{
  if (op_pos == nullptr || op == nullptr) {
    return false;
  }

  const std::pair<std::string_view, ExprOp> operators[]{
    { "==", ExprOp::kEqual },
    { "!=", ExprOp::kNotEqual },
    { "<", ExprOp::kLessThan },
    { ">", ExprOp::kGreaterThan },
  };
  for (const auto& entry : operators) {
    const auto found = token.find(entry.first);
    if (found != std::string_view::npos) {
      *op_pos = found;
      *op = entry.second;
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto
ParseClause(const std::vector<std::string_view>& tokens, std::size_t* index) -> base::Result<ExprNode>
{
  if (index == nullptr || *index >= tokens.size()) {
    return base::Status::InvalidArgument("routing expression expected a clause");
  }

  auto negated = false;
  if (ToUpperAscii(tokens[*index]) == "NOT") {
    negated = true;
    ++(*index);
    if (*index >= tokens.size()) {
      return base::Status::InvalidArgument("routing expression has NOT without a comparison");
    }
  }

  std::uint32_t tag = 0U;
  if (ParseHasToken(tokens[*index], &tag)) {
    ++(*index);
    return ExprNode{ .tag = tag, .op = ExprOp::kHasTag, .value = {}, .negated = negated };
  }

  if (ToUpperAscii(tokens[*index]) == "HAS") {
    ++(*index);
    if (*index >= tokens.size()) {
      return base::Status::InvalidArgument("HAS operator requires a tag");
    }
    auto tag_token = tokens[*index];
    if (tag_token.size() >= 2U && tag_token.front() == '(' && tag_token.back() == ')') {
      tag_token = tag_token.substr(1U, tag_token.size() - 2U);
    }
    if (!ParseUint32Strict(tag_token, &tag)) {
      return base::Status::InvalidArgument("HAS operator requires a numeric tag");
    }
    ++(*index);
    return ExprNode{ .tag = tag, .op = ExprOp::kHasTag, .value = {}, .negated = negated };
  }

  std::size_t op_pos = 0U;
  ExprOp op = ExprOp::kEqual;
  if (FindInlineComparisonOperator(tokens[*index], &op_pos, &op)) {
    const auto token = tokens[*index];
    const auto op_width = (op == ExprOp::kEqual || op == ExprOp::kNotEqual) ? 2U : 1U;
    if (!ParseUint32Strict(token.substr(0U, op_pos), &tag)) {
      return base::Status::InvalidArgument("routing expression comparison requires a numeric tag");
    }
    auto value = token.substr(op_pos + op_width);
    ++(*index);
    if (value.empty() && *index < tokens.size()) {
      value = tokens[*index];
      ++(*index);
    }
    if (value.empty()) {
      return base::Status::InvalidArgument("routing expression comparison requires a value");
    }
    return ExprNode{ .tag = tag, .op = op, .value = std::string(value), .negated = negated };
  }

  if (!ParseUint32Strict(tokens[*index], &tag)) {
    return base::Status::InvalidArgument("routing expression comparison requires a numeric tag");
  }
  ++(*index);
  if (*index >= tokens.size()) {
    return base::Status::InvalidArgument("routing expression comparison requires an operator");
  }

  auto parsed_op = ParseOperatorToken(tokens[*index]);
  if (!parsed_op.has_value() || *parsed_op == ExprOp::kHasTag) {
    return base::Status::InvalidArgument("routing expression comparison has an invalid operator");
  }
  op = *parsed_op;
  ++(*index);
  if (*index >= tokens.size()) {
    return base::Status::InvalidArgument("routing expression comparison requires a value");
  }

  const auto value = tokens[*index];
  ++(*index);
  return ExprNode{ .tag = tag, .op = op, .value = std::string(value), .negated = negated };
}

[[nodiscard]] auto
EvaluateNode(const ExprNode& node, const codec::RawPassThroughView& message) -> bool
{
  const auto value = ExtractMessageTagValue(message, node.tag);
  auto matched = false;

  if (node.op == ExprOp::kHasTag) {
    matched = value.has_value();
  } else if (value.has_value()) {
    switch (node.op) {
      case ExprOp::kEqual:
        matched = *value == node.value;
        break;
      case ExprOp::kNotEqual:
        matched = *value != node.value;
        break;
      case ExprOp::kLessThan: {
        double lhs = 0.0;
        double rhs = 0.0;
        matched = ParseDoubleStrict(*value, &lhs) && ParseDoubleStrict(node.value, &rhs) && lhs < rhs;
        break;
      }
      case ExprOp::kGreaterThan: {
        double lhs = 0.0;
        double rhs = 0.0;
        matched = ParseDoubleStrict(*value, &lhs) && ParseDoubleStrict(node.value, &rhs) && lhs > rhs;
        break;
      }
      case ExprOp::kContains:
        matched = value->find(node.value) != std::string::npos;
        break;
      case ExprOp::kRegex:
        try {
          matched = std::regex_match(*value, std::regex(node.value));
        } catch (const std::regex_error&) {
          matched = false;
        }
        break;
      case ExprOp::kHasTag:
        break;
    }
  }

  return node.negated ? !matched : matched;
}

auto
AppendField(std::string* body, std::uint32_t tag, std::string_view value) -> void
{
  if (body == nullptr) {
    return;
  }
  if (!body->empty() && body->back() != codec::kFixSoh) {
    body->push_back(codec::kFixSoh);
  }
  body->append(std::to_string(tag));
  body->push_back('=');
  body->append(value);
  body->push_back(codec::kFixSoh);
}

auto
SetField(std::string* body, std::uint32_t tag, std::string_view value) -> void
{
  if (body == nullptr) {
    return;
  }
  const auto span = FindField(*body, tag);
  if (!span.has_value()) {
    AppendField(body, tag, value);
    return;
  }
  body->replace(span->value_begin, span->value_end - span->value_begin, value);
}

auto
RemoveField(std::string* body, std::uint32_t tag) -> void
{
  if (body == nullptr) {
    return;
  }
  const auto span = FindField(*body, tag);
  if (!span.has_value()) {
    return;
  }
  body->erase(span->field_begin, span->field_end - span->field_begin);
}

} // namespace

auto
ParseRoutingExpression(std::string_view text) -> base::Result<RoutingExpression>
{
  const auto tokens = Tokenize(text);
  if (tokens.empty()) {
    return base::Status::InvalidArgument("routing expression is empty");
  }

  RoutingExpression expression;
  std::size_t index = 0U;
  while (index < tokens.size()) {
    auto parsed_node = ParseClause(tokens, &index);
    if (!parsed_node.ok()) {
      return parsed_node.status();
    }

    RoutingExpression::Clause clause{ .node = std::move(parsed_node).value(), .connector = ExprLogic::kAnd };
    expression.clauses.push_back(std::move(clause));

    if (index >= tokens.size()) {
      break;
    }

    const auto connector = ToUpperAscii(tokens[index]);
    if (connector == "AND") {
      expression.clauses.back().connector = ExprLogic::kAnd;
    } else if (connector == "OR") {
      expression.clauses.back().connector = ExprLogic::kOr;
    } else {
      return base::Status::InvalidArgument("routing expression expected AND or OR connector");
    }
    ++index;
    if (index >= tokens.size()) {
      return base::Status::InvalidArgument("routing expression connector is missing a following clause");
    }
  }

  for (const auto& clause : expression.clauses) {
    if (clause.node.op == ExprOp::kRegex) {
      try {
        [[maybe_unused]] const auto compiled = std::regex(clause.node.value);
      } catch (const std::regex_error&) {
        return base::Status::InvalidArgument("routing expression contains an invalid regex");
      }
    }
  }

  return expression;
}

auto
EvaluateExpression(const RoutingExpression& expr, const codec::RawPassThroughView& message) -> bool
{
  if (expr.clauses.empty()) {
    return false;
  }

  auto current_and_group = EvaluateNode(expr.clauses.front().node, message);
  auto result = false;
  for (std::size_t index = 0U; index + 1U < expr.clauses.size(); ++index) {
    const auto next_value = EvaluateNode(expr.clauses[index + 1U].node, message);
    if (expr.clauses[index].connector == ExprLogic::kAnd) {
      current_and_group = current_and_group && next_value;
    } else {
      result = result || current_and_group;
      current_and_group = next_value;
    }
  }
  return result || current_and_group;
}

auto
ApplyTransform(const codec::RawPassThroughView& message,
               const MessageTransform& transform,
               std::vector<std::byte>* output_body) -> base::Status
{
  if (output_body == nullptr) {
    return base::Status::InvalidArgument("output body is null");
  }
  if (!message.valid) {
    return base::Status::InvalidArgument("message view is not valid");
  }

  std::string body(BytesToStringView(message.raw_body));
  for (const auto& field_transform : transform.field_transforms) {
    if (field_transform.tag == 0U) {
      return base::Status::InvalidArgument("field transform tag must be non-zero");
    }

    switch (field_transform.action) {
      case FieldTransform::Action::kSet:
        SetField(&body, field_transform.tag, field_transform.value);
        break;
      case FieldTransform::Action::kRemove:
        RemoveField(&body, field_transform.tag);
        break;
      case FieldTransform::Action::kAdd:
        if (!FindField(body, field_transform.tag).has_value()) {
          AppendField(&body, field_transform.tag, field_transform.value);
        }
        break;
      case FieldTransform::Action::kCopy: {
        if (field_transform.source_tag == 0U) {
          return base::Status::InvalidArgument("copy transform source tag must be non-zero");
        }
        std::optional<std::string> source_value;
        if (const auto source_span = FindField(body, field_transform.source_tag); source_span.has_value()) {
          source_value = body.substr(source_span->value_begin, source_span->value_end - source_span->value_begin);
        } else {
          source_value = ExtractMessageTagValue(message, field_transform.source_tag);
        }
        if (source_value.has_value()) {
          SetField(&body, field_transform.tag, *source_value);
        }
        break;
      }
    }
  }

  output_body->clear();
  output_body->reserve(body.size());
  for (const auto ch : body) {
    output_body->push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return base::Status::Ok();
}

RoutingTable::RoutingTable(const RoutingTable& other)
{
  std::lock_guard lock(other.load_balancer_mutex_);
  rules_ = other.rules_;
  default_action_ = other.default_action_;
  default_target_ = other.default_target_;
}

auto
RoutingTable::operator=(const RoutingTable& other) -> RoutingTable&
{
  if (this == &other) {
    return *this;
  }

  std::scoped_lock lock(load_balancer_mutex_, other.load_balancer_mutex_);
  rules_ = other.rules_;
  default_action_ = other.default_action_;
  default_target_ = other.default_target_;
  load_balancer_states_.clear();
  return *this;
}

RoutingTable::RoutingTable(RoutingTable&& other) noexcept
{
  std::lock_guard lock(other.load_balancer_mutex_);
  rules_ = std::move(other.rules_);
  default_action_ = other.default_action_;
  default_target_ = other.default_target_;
  other.load_balancer_states_.clear();
}

auto
RoutingTable::operator=(RoutingTable&& other) noexcept -> RoutingTable&
{
  if (this == &other) {
    return *this;
  }

  std::scoped_lock lock(load_balancer_mutex_, other.load_balancer_mutex_);
  rules_ = std::move(other.rules_);
  default_action_ = other.default_action_;
  default_target_ = other.default_target_;
  load_balancer_states_.clear();
  other.load_balancer_states_.clear();
  return *this;
}

auto
RoutingTable::AddRule(RoutingRule rule) -> void
{
  std::lock_guard lock(load_balancer_mutex_);
  const auto insert_at =
    std::upper_bound(rules_.begin(), rules_.end(), rule.priority, [](const auto priority, const auto& entry) {
      return priority < entry.priority;
    });
  rules_.insert(insert_at, std::move(rule));
}

auto
RoutingTable::RemoveRule(std::string_view name) -> void
{
  std::lock_guard lock(load_balancer_mutex_);
  rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const auto& rule) { return rule.name == name; }),
               rules_.end());
  load_balancer_states_.erase(std::string(name));
}

auto
RoutingTable::Clear() -> void
{
  std::lock_guard lock(load_balancer_mutex_);
  rules_.clear();
  load_balancer_states_.clear();
}

auto
RoutingTable::SetDefaultAction(RoutingAction action) -> void
{
  std::lock_guard lock(load_balancer_mutex_);
  default_action_ = action;
}

auto
RoutingTable::SetDefaultTarget(std::uint64_t session_id) -> void
{
  std::lock_guard lock(load_balancer_mutex_);
  default_target_ = session_id;
}

auto
RoutingTable::Route(const codec::RawPassThroughView& message) const -> RouteResult
{
  std::lock_guard lock(load_balancer_mutex_);
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
      case RoutingCriterion::kExpression:
        matched = EvaluateExpression(rule.expression, message);
        break;
    }

    if (matched) {
      if (rule.action == RoutingAction::kForward) {
        target_session_id = SelectLoadBalancedTarget(rule, target_session_id, message);
      }
      return RouteResult{
        .action = rule.action,
        .target_session_id = rule.action == RoutingAction::kForward ? target_session_id : 0U,
        .matched_rule_name = rule.name,
        .matched_rule = &rule,
      };
    }
  }

  return RouteResult{
    .action = default_action_,
    .target_session_id = default_action_ == RoutingAction::kForward ? default_target_ : 0U,
    .matched_rule_name = {},
    .matched_rule = nullptr,
  };
}

auto
RoutingTable::rule_count() const -> std::size_t
{
  std::lock_guard lock(load_balancer_mutex_);
  return rules_.size();
}

auto
RoutingTable::SelectLoadBalancedTarget(const RoutingRule& rule,
                                       std::uint64_t fallback_target,
                                       const codec::RawPassThroughView& message) const -> std::uint64_t
{
  if (rule.load_balancer.mode == LoadBalancingMode::kNone || rule.load_balancer.target_sessions.empty()) {
    return fallback_target;
  }

  auto& state = load_balancer_states_[rule.name];
  const auto round_robin_target = [&state, &rule]() -> std::uint64_t {
    if (rule.load_balancer.target_sessions.empty()) {
      return 0U;
    }
    const auto index = state.round_robin_index.fetch_add(1U, std::memory_order_relaxed);
    return rule.load_balancer.target_sessions[index % rule.load_balancer.target_sessions.size()];
  };

  switch (rule.load_balancer.mode) {
    case LoadBalancingMode::kNone:
      return fallback_target;
    case LoadBalancingMode::kRoundRobin:
      return round_robin_target();
    case LoadBalancingMode::kSticky: {
      const auto sticky_key = ExtractMessageTagValue(message, rule.load_balancer.sticky_key_tag);
      if (!sticky_key.has_value() || sticky_key->empty()) {
        return round_robin_target();
      }
      if (const auto existing = state.sticky_map.find(*sticky_key); existing != state.sticky_map.end()) {
        return existing->second;
      }
      const auto selected = round_robin_target();
      state.sticky_map.emplace(*sticky_key, selected);
      return selected;
    }
  }

  return fallback_target;
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

  std::vector<std::byte> transformed_body;
  auto forwarded_view = inbound;
  if (route.matched_rule != nullptr && !route.matched_rule->transform.field_transforms.empty()) {
    const auto transform_status = ApplyTransform(inbound, route.matched_rule->transform, &transformed_body);
    if (!transform_status.ok()) {
      ++result.failed;
      return transform_status;
    }
    forwarded_view.raw_body = std::span<const std::byte>(transformed_body.data(), transformed_body.size());
  }

  const auto status = codec::EncodeForwarded(forwarded_view, forwarding_options, buffer);
  if (!status.ok()) {
    ++result.failed;
    return status;
  }

  ++result.forwarded;
  return result;
}

} // namespace nimble::runtime
