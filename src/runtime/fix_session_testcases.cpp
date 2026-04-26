#include "nimblefix/runtime/fix_session_testcases.h"

#include "nimblefix/runtime/interop_harness.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

namespace nimble::runtime {

namespace {

constexpr char kCommentPrefix = '#';
constexpr char kFieldSeparator = '|';
constexpr std::uint64_t kOfficialFix42SessionId = 4201U;
constexpr std::uint64_t kOfficialFix44SessionId = 4401U;
constexpr std::uint64_t kOfficialFixT11SessionId = 1101U;

auto
Trim(std::string_view input) -> std::string_view
{
  std::size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  std::size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return input.substr(begin, end - begin);
}

auto
Split(std::string_view input, char delimiter) -> std::vector<std::string>
{
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= input.size()) {
    const auto end = input.find(delimiter, begin);
    if (end == std::string_view::npos) {
      parts.emplace_back(Trim(input.substr(begin)));
      break;
    }
    parts.emplace_back(Trim(input.substr(begin, end - begin)));
    begin = end + 1;
  }
  return parts;
}

auto
SplitLines(std::string_view text) -> std::vector<std::string>
{
  std::vector<std::string> lines;
  std::string current;
  for (const auto ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty() || text.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

auto
ParseRole(std::string_view token) -> base::Result<OfficialCaseRole>
{
  const auto value = Trim(token);
  if (value == "initiator") {
    return OfficialCaseRole::kInitiator;
  }
  if (value == "acceptor") {
    return OfficialCaseRole::kAcceptor;
  }
  if (value == "all") {
    return OfficialCaseRole::kAll;
  }
  return base::Status::InvalidArgument("unknown official case role");
}

auto
ParseRequirement(std::string_view token) -> base::Result<OfficialCaseRequirement>
{
  const auto value = Trim(token);
  if (value == "mandatory") {
    return OfficialCaseRequirement::kMandatory;
  }
  if (value == "optional") {
    return OfficialCaseRequirement::kOptional;
  }
  return base::Status::InvalidArgument("unknown official case requirement");
}

auto
ParseSupport(std::string_view token) -> base::Result<OfficialCaseSupport>
{
  const auto value = Trim(token);
  if (value == "mapped") {
    return OfficialCaseSupport::kMapped;
  }
  if (value == "unsupported") {
    return OfficialCaseSupport::kUnsupported;
  }
  if (value == "xfail") {
    return OfficialCaseSupport::kExpectedFail;
  }
  return base::Status::InvalidArgument("unknown official case support status");
}

auto
ParseVerificationStatus(std::string_view token) -> base::Result<OfficialCaseVerificationStatus>
{
  const auto value = Trim(token);
  if (value.empty() || value == "unspecified") {
    return OfficialCaseVerificationStatus::kUnspecified;
  }
  if (value == "partial") {
    return OfficialCaseVerificationStatus::kPartial;
  }
  if (value == "verified" || value == "official-verified") {
    return OfficialCaseVerificationStatus::kVerified;
  }
  return base::Status::InvalidArgument("unknown official case verification status");
}

auto
RoleText(OfficialCaseRole role) -> std::string_view
{
  switch (role) {
    case OfficialCaseRole::kInitiator:
      return "initiator";
    case OfficialCaseRole::kAcceptor:
      return "acceptor";
    case OfficialCaseRole::kAll:
      return "all";
  }
  return "all";
}

auto
RequirementText(OfficialCaseRequirement requirement) -> std::string_view
{
  switch (requirement) {
    case OfficialCaseRequirement::kMandatory:
      return "mandatory";
    case OfficialCaseRequirement::kOptional:
      return "optional";
  }
  return "mandatory";
}

auto
SupportText(OfficialCaseSupport support) -> std::string_view
{
  switch (support) {
    case OfficialCaseSupport::kMapped:
      return "mapped";
    case OfficialCaseSupport::kUnsupported:
      return "unsupported";
    case OfficialCaseSupport::kExpectedFail:
      return "xfail";
  }
  return "unsupported";
}

auto
OutcomeText(OfficialCaseOutcome outcome) -> std::string_view
{
  switch (outcome) {
    case OfficialCaseOutcome::kPassed:
      return "pass";
    case OfficialCaseOutcome::kFailed:
      return "fail";
    case OfficialCaseOutcome::kUnsupported:
      return "unsupported";
    case OfficialCaseOutcome::kExpectedFail:
      return "xfail";
    case OfficialCaseOutcome::kUnexpectedPass:
      return "unexpected-pass";
    case OfficialCaseOutcome::kPartial:
      return "partial";
    case OfficialCaseOutcome::kOfficiallyVerified:
      return "official-verified";
  }
  return "unsupported";
}

auto
VerificationStatusText(OfficialCaseVerificationStatus status) -> std::string_view
{
  switch (status) {
    case OfficialCaseVerificationStatus::kUnspecified:
      return "unspecified";
    case OfficialCaseVerificationStatus::kPartial:
      return "partial";
    case OfficialCaseVerificationStatus::kVerified:
      return "verified";
  }
  return "unspecified";
}

struct VerificationPredicate
{
  std::string token;
  std::string key;
  std::string value;
};

struct VerificationEvaluation
{
  bool verified{ false };
  std::vector<std::string> missing_predicates;
};

auto
ParseUnsignedValue(std::string_view value) -> std::optional<std::uint64_t>
{
  value = Trim(value);
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0U;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

auto
ParseBinaryValue(std::string_view value) -> std::optional<bool>
{
  const auto parsed = ParseUnsignedValue(value);
  if (!parsed.has_value() || parsed.value() > 1U) {
    return std::nullopt;
  }
  return parsed.value() != 0U;
}

auto
ParseScenarioBoolean(std::string_view value) -> std::optional<bool>
{
  value = Trim(value);
  if (value == "Y" || value == "y" || value == "1" || value == "true" || value == "yes") {
    return true;
  }
  if (value == "N" || value == "n" || value == "0" || value == "false" || value == "no") {
    return false;
  }
  return std::nullopt;
}

auto
ParseVerifiedPredicate(std::string_view token) -> base::Result<VerificationPredicate>
{
  const auto trimmed = Trim(token);
  if (trimmed.empty()) {
    return base::Status::InvalidArgument("official verified assertion token is empty");
  }

  VerificationPredicate predicate;
  predicate.token = std::string(trimmed);

  const auto equals = trimmed.find('=');
  if (equals == std::string_view::npos) {
    predicate.key = std::string(trimmed);
  } else {
    predicate.key = std::string(Trim(trimmed.substr(0, equals)));
    predicate.value = std::string(Trim(trimmed.substr(equals + 1U)));
    if (predicate.key.empty() || predicate.value.empty()) {
      return base::Status::InvalidArgument("official verified assertion token requires non-empty key and value");
    }
  }

  const auto requires_number =
    predicate.key == "outbound" || predicate.key == "app" || predicate.key == "queued-app" ||
    predicate.key == "processed-app" || predicate.key == "ignored-app" || predicate.key == "poss-resend-app" ||
    predicate.key == "next-in-after" || predicate.key == "ref-tag" || predicate.key == "reject-reason" ||
    predicate.key == "business-reject-reason" || predicate.key == "msg-seq-num" || predicate.key == "begin-seq" ||
    predicate.key == "end-seq" || predicate.key == "new-seq" || predicate.key == "inbound-new-seq" ||
    predicate.key == "inbound-seq";
  if (requires_number) {
    if (predicate.value.empty() || !ParseUnsignedValue(predicate.value).has_value()) {
      return base::Status::InvalidArgument("official verified assertion token requires an unsigned integer value");
    }
    return predicate;
  }

  const auto requires_binary = predicate.key == "gap-fill" || predicate.key == "session-reject" ||
                               predicate.key == "inbound-gap-fill" || predicate.key == "inbound-possdup";
  if (requires_binary) {
    if (predicate.value.empty() || !ParseBinaryValue(predicate.value).has_value()) {
      return base::Status::InvalidArgument("official verified assertion token requires a 0 or 1 value");
    }
    return predicate;
  }

  const auto requires_text =
    predicate.key == "msg-type" || predicate.key == "ref-msg-type" || predicate.key == "inbound-msg-type";
  if (requires_text) {
    if (predicate.value.empty()) {
      return base::Status::InvalidArgument("official verified assertion token requires a text value");
    }
    return predicate;
  }

  if (!predicate.value.empty()) {
    return base::Status::InvalidArgument("unknown official verified assertion token");
  }

  if (predicate.key == "disconnect" || predicate.key == "error" || predicate.key == "warning" ||
      predicate.key == "session-active" || predicate.key == "session-disconnected" || predicate.key == "reject" ||
      predicate.key == "logout" || predicate.key == "heartbeat" || predicate.key == "test-req-id" ||
      predicate.key == "sending-time-present" || predicate.key == "next-in-unchanged" ||
      predicate.key == "subsequent-valid-message-accepted" || predicate.key == "logout-sent" ||
      predicate.key == "logout-ack" || predicate.key == "heartbeat-response" ||
      predicate.key == "testrequest-at-heartbtint-plus-20pct" || predicate.key == "flush-after-logon" ||
      predicate.key == "transport-connected" || predicate.key == "no-session-rejects" ||
      predicate.key == "fix42-active" || predicate.key == "fix44-active" || predicate.key == "fixt11-active") {
    return predicate;
  }

  return base::Status::InvalidArgument("unknown official verified assertion token");
}

auto
ValidateVerifiedPredicateSyntax(const std::vector<std::string>& predicates) -> base::Status
{
  for (const auto& token : predicates) {
    auto parsed = ParseVerifiedPredicate(token);
    if (!parsed.ok()) {
      return base::Status::InvalidArgument(parsed.status().message().empty()
                                             ? "invalid official verified assertion token: " + token
                                             : std::string(parsed.status().message()) + ": " + token);
    }
  }
  return base::Status::Ok();
}

auto
FindActionReport(const InteropReport& report, std::size_t action_index) -> const InteropActionReport*
{
  if (action_index == 0U || action_index > report.action_reports.size()) {
    return nullptr;
  }
  return &report.action_reports[action_index - 1U];
}

auto
FindAction(const InteropScenario& scenario, std::size_t action_index) -> const InteropAction*
{
  if (action_index == 0U || action_index > scenario.actions.size()) {
    return nullptr;
  }
  return &scenario.actions[action_index - 1U];
}

auto
FindSessionSnapshot(const InteropReport& report, std::uint64_t session_id) -> const session::SessionSnapshot*
{
  const auto it = std::find_if(report.sessions.begin(), report.sessions.end(), [&](const auto& snapshot) {
    return snapshot.session_id == session_id;
  });
  return it == report.sessions.end() ? nullptr : &*it;
}

auto
HasSessionStateProof(const InteropScenario& scenario,
                     const InteropReport& report,
                     std::uint64_t session_id,
                     session::SessionState state) -> bool
{
  const auto expected = std::find_if(
    scenario.session_expectations.begin(), scenario.session_expectations.end(), [&](const auto& expectation) {
      return expectation.session_id == session_id && expectation.state == state;
    });
  if (expected == scenario.session_expectations.end()) {
    return false;
  }
  const auto* snapshot = FindSessionSnapshot(report, session_id);
  return snapshot != nullptr && snapshot->state == state;
}

auto
HasAnySessionStateProof(const InteropScenario& scenario, const InteropReport& report, session::SessionState state)
  -> bool
{
  return std::any_of(scenario.session_expectations.begin(), scenario.session_expectations.end(), [&](const auto& exp) {
    return exp.state == state && HasSessionStateProof(scenario, report, exp.session_id, state);
  });
}

auto
HasActionPredicateProof(const InteropScenario& scenario,
                        const InteropReport& report,
                        const VerificationPredicate& predicate) -> bool
{
  const auto numeric_value = ParseUnsignedValue(predicate.value);
  const auto binary_value = ParseBinaryValue(predicate.value);

  for (const auto& expectation : scenario.action_expectations) {
    const auto* action = FindActionReport(report, expectation.action_index);
    if (action == nullptr) {
      continue;
    }

    if (predicate.key == "outbound" && numeric_value.has_value() && expectation.outbound_frames.has_value() &&
        expectation.outbound_frames.value() == numeric_value.value() &&
        action->outbound_frames == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "app" && numeric_value.has_value() && expectation.application_messages.has_value() &&
        expectation.application_messages.value() == numeric_value.value() &&
        action->application_messages == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "queued-app" && numeric_value.has_value() &&
        expectation.queued_application_messages.has_value() &&
        expectation.queued_application_messages.value() == numeric_value.value() &&
        action->queued_application_messages == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "processed-app" && numeric_value.has_value() &&
        expectation.processed_application_messages.has_value() &&
        expectation.processed_application_messages.value() == numeric_value.value() &&
        action->processed_application_messages == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "ignored-app" && numeric_value.has_value() &&
        expectation.ignored_application_messages.has_value() &&
        expectation.ignored_application_messages.value() == numeric_value.value() &&
        action->ignored_application_messages == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "poss-resend-app" && numeric_value.has_value() &&
        expectation.poss_resend_application_messages.has_value() &&
        expectation.poss_resend_application_messages.value() == numeric_value.value() &&
        action->poss_resend_application_messages == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "disconnect" && expectation.disconnect.value_or(false) && action->disconnect) {
      return true;
    }
    if (predicate.key == "session-reject" && binary_value.has_value() && expectation.session_reject.has_value() &&
        expectation.session_reject.value() == binary_value.value() && action->session_reject == binary_value.value()) {
      return true;
    }
    if (predicate.key == "error" && expectation.error_generated.value_or(false) && !action->errors.empty()) {
      return true;
    }
    if (predicate.key == "warning" && expectation.warning_generated.value_or(false) && !action->warnings.empty()) {
      return true;
    }
    if (predicate.key == "session-active" && expectation.session_active.value_or(false) && action->session_active) {
      return true;
    }
    if (predicate.key == "session-disconnected" && expectation.session_active.has_value() &&
        !expectation.session_active.value() && !action->session_active) {
      return true;
    }
    if (predicate.key == "next-in-after" && numeric_value.has_value() &&
        expectation.next_in_seq_after_action.has_value() &&
        expectation.next_in_seq_after_action.value() == numeric_value.value() &&
        action->next_in_seq_after_action == numeric_value.value()) {
      return true;
    }
  }

  if (predicate.key == "session-active") {
    return HasAnySessionStateProof(scenario, report, session::SessionState::kActive);
  }
  if (predicate.key == "session-disconnected") {
    return HasAnySessionStateProof(scenario, report, session::SessionState::kDisconnected);
  }

  return false;
}

auto
HasFramePredicateProof(const InteropScenario& scenario,
                       const InteropReport& report,
                       const VerificationPredicate& predicate) -> bool
{
  const auto numeric_value = ParseUnsignedValue(predicate.value);
  const auto binary_value = ParseBinaryValue(predicate.value);

  for (const auto& expectation : scenario.outbound_expectations) {
    const auto* action = FindActionReport(report, expectation.action_index);
    if (action == nullptr || expectation.frame_index == 0U ||
        expectation.frame_index > action->outbound_frame_summaries.size()) {
      continue;
    }
    const auto& frame = action->outbound_frame_summaries[expectation.frame_index - 1U];

    if (predicate.key == "reject" && expectation.msg_type == "3" && frame.msg_type == "3") {
      return true;
    }
    if (predicate.key == "logout" && expectation.msg_type == "5" && frame.msg_type == "5") {
      return true;
    }
    if (predicate.key == "heartbeat" && expectation.msg_type == "0" && frame.msg_type == "0") {
      return true;
    }
    if (predicate.key == "msg-type" && expectation.msg_type == predicate.value && frame.msg_type == predicate.value) {
      return true;
    }
    if (predicate.key == "msg-seq-num" && numeric_value.has_value() && expectation.msg_seq_num.has_value() &&
        expectation.msg_seq_num.value() == numeric_value.value() && frame.msg_seq_num == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "begin-seq" && numeric_value.has_value() && expectation.begin_seq_no.has_value() &&
        expectation.begin_seq_no.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.begin_seq_no.has_value() &&
        frame.begin_seq_no.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "end-seq" && numeric_value.has_value() && expectation.end_seq_no.has_value() &&
        expectation.end_seq_no.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.end_seq_no.has_value() && frame.end_seq_no.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "new-seq" && numeric_value.has_value() && expectation.new_seq_no.has_value() &&
        expectation.new_seq_no.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.new_seq_no.has_value() && frame.new_seq_no.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "gap-fill" && binary_value.has_value() && expectation.gap_fill_flag.has_value() &&
        expectation.gap_fill_flag.value() == binary_value.value() && frame.gap_fill_flag.has_value() &&
        frame.gap_fill_flag.value() == binary_value.value()) {
      return true;
    }
    if (predicate.key == "ref-tag" && numeric_value.has_value() && expectation.ref_tag_id.has_value() &&
        expectation.ref_tag_id.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.ref_tag_id.has_value() && frame.ref_tag_id.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "reject-reason" && numeric_value.has_value() && expectation.reject_reason.has_value() &&
        expectation.reject_reason.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.reject_reason.has_value() &&
        frame.reject_reason.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "business-reject-reason" && numeric_value.has_value() &&
        expectation.business_reject_reason.has_value() &&
        expectation.business_reject_reason.value() == static_cast<std::int64_t>(numeric_value.value()) &&
        frame.business_reject_reason.has_value() &&
        frame.business_reject_reason.value() == static_cast<std::int64_t>(numeric_value.value())) {
      return true;
    }
    if (predicate.key == "ref-msg-type" && !expectation.ref_msg_type.empty() &&
        expectation.ref_msg_type == predicate.value && frame.ref_msg_type == predicate.value) {
      return true;
    }
    if (predicate.key == "test-req-id" && !expectation.test_req_id.empty() && !frame.test_req_id.empty()) {
      return true;
    }
    if (predicate.key == "sending-time-present" && expectation.sending_time_present.value_or(false) &&
        !frame.sending_time.empty()) {
      return true;
    }
  }

  return false;
}

auto
HasTransportConnectedProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  for (const auto& expectation : scenario.action_expectations) {
    const auto* scenario_action = FindAction(scenario, expectation.action_index);
    const auto* action = FindActionReport(report, expectation.action_index);
    if (scenario_action == nullptr || action == nullptr) {
      continue;
    }
    if (scenario_action->kind == InteropActionKind::kProtocolConnect &&
        expectation.disconnect.value_or(true) == false && !action->disconnect) {
      return true;
    }
  }
  return false;
}

auto
HasNoSessionRejectsProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  bool saw_no_reject_expectation = false;
  for (const auto& expectation : scenario.action_expectations) {
    if (!expectation.session_reject.has_value() || expectation.session_reject.value()) {
      continue;
    }
    saw_no_reject_expectation = true;
    const auto* action = FindActionReport(report, expectation.action_index);
    if (action == nullptr || action->session_reject) {
      return false;
    }
  }
  return saw_no_reject_expectation;
}

auto
FindScenarioBodyField(std::string_view body, std::string_view tag) -> std::optional<std::string_view>
{
  std::size_t begin = 0U;
  while (begin <= body.size()) {
    const auto end = body.find_first_of("^\x01", begin);
    const auto field = body.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && field.substr(0U, equals) == tag) {
      return Trim(field.substr(equals + 1U));
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return std::nullopt;
}

auto
FindSuccessfulInboundActionReport(const InteropReport& report, std::size_t action_index) -> const InteropActionReport*
{
  return FindActionReport(report, action_index);
}

auto
HasInboundPredicateProof(const InteropScenario& scenario,
                         const InteropReport& report,
                         const VerificationPredicate& predicate) -> bool
{
  const auto numeric_value = ParseUnsignedValue(predicate.value);
  const auto binary_value = ParseBinaryValue(predicate.value);

  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& action = scenario.actions[index];
    if (action.kind != InteropActionKind::kProtocolInbound) {
      continue;
    }
    if (FindSuccessfulInboundActionReport(report, index + 1U) == nullptr) {
      continue;
    }

    if (predicate.key == "inbound-seq" && numeric_value.has_value() && action.seq_num == numeric_value.value()) {
      return true;
    }
    if (predicate.key == "inbound-possdup" && binary_value.has_value() && action.poss_dup == binary_value.value()) {
      return true;
    }

    const auto msg_type = FindScenarioBodyField(action.text, "35");
    if (predicate.key == "inbound-msg-type" && msg_type.has_value() && msg_type.value() == predicate.value) {
      return true;
    }

    const auto new_seq = FindScenarioBodyField(action.text, "36");
    if (predicate.key == "inbound-new-seq" && numeric_value.has_value() && new_seq.has_value()) {
      const auto parsed_new_seq = ParseUnsignedValue(new_seq.value());
      if (parsed_new_seq.has_value() && parsed_new_seq.value() == numeric_value.value()) {
        return true;
      }
    }

    const auto gap_fill = FindScenarioBodyField(action.text, "123");
    if (predicate.key == "inbound-gap-fill" && binary_value.has_value() && gap_fill.has_value()) {
      const auto parsed_gap_fill = ParseScenarioBoolean(gap_fill.value());
      if (parsed_gap_fill.has_value() && parsed_gap_fill.value() == binary_value.value()) {
        return true;
      }
    }
  }

  return false;
}

auto
HasNextInUnchangedProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  for (const auto& expectation : scenario.action_expectations) {
    if (!expectation.next_in_seq_after_action.has_value()) {
      continue;
    }
    const auto* scenario_action = FindAction(scenario, expectation.action_index);
    const auto* action = FindActionReport(report, expectation.action_index);
    if (scenario_action == nullptr || action == nullptr) {
      continue;
    }
    const auto is_inbound = scenario_action->kind == InteropActionKind::kProtocolInbound ||
                            scenario_action->kind == InteropActionKind::kProtocolInboundRaw;
    if (is_inbound && scenario_action->seq_num != 0U &&
        expectation.next_in_seq_after_action.value() == scenario_action->seq_num &&
        action->next_in_seq_after_action == scenario_action->seq_num) {
      return true;
    }
    if (scenario_action->kind == InteropActionKind::kProtocolInboundRaw &&
        expectation.warning_generated.value_or(false) && expectation.outbound_frames.value_or(1U) == 0U &&
        action->next_in_seq_after_action == expectation.next_in_seq_after_action.value()) {
      for (std::size_t next_index = expectation.action_index; next_index < scenario.actions.size(); ++next_index) {
        const auto& later_action = scenario.actions[next_index];
        if (later_action.session_id == scenario_action->session_id &&
            later_action.kind == InteropActionKind::kProtocolInbound &&
            later_action.seq_num == expectation.next_in_seq_after_action.value()) {
          return true;
        }
      }
    }
  }
  return false;
}

auto
HasSubsequentValidMessageAcceptedProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& ignored_action = scenario.actions[index];
    if (ignored_action.kind != InteropActionKind::kProtocolInboundRaw || ignored_action.session_id == 0U) {
      continue;
    }
    const auto* ignored_report = FindActionReport(report, index + 1U);
    if (ignored_report == nullptr || ignored_report->warnings.empty()) {
      continue;
    }

    for (std::size_t next = index + 1U; next < scenario.actions.size(); ++next) {
      const auto& accepted_action = scenario.actions[next];
      if (accepted_action.session_id != ignored_action.session_id ||
          accepted_action.kind != InteropActionKind::kProtocolInbound || accepted_action.seq_num == 0U) {
        continue;
      }
      const auto* accepted_report = FindActionReport(report, next + 1U);
      if (accepted_report == nullptr || accepted_report->disconnect ||
          accepted_report->next_in_seq_after_action != accepted_action.seq_num + 1U) {
        continue;
      }
      const auto final_proof = std::any_of(
        scenario.session_expectations.begin(), scenario.session_expectations.end(), [&](const auto& expectation) {
          return expectation.session_id == accepted_action.session_id &&
                 expectation.next_in_seq >= accepted_action.seq_num + 1U;
        });
      if (final_proof) {
        return true;
      }
    }
  }
  return false;
}

auto
HasLogoutSentProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    if (scenario.actions[index].kind != InteropActionKind::kProtocolBeginLogout) {
      continue;
    }
    const VerificationPredicate logout_predicate{ .token = "logout", .key = "logout" };
    const auto has_logout_expectation = std::any_of(
      scenario.outbound_expectations.begin(), scenario.outbound_expectations.end(), [&](const auto& expectation) {
        return expectation.action_index == index + 1U && expectation.msg_type == "5";
      });
    const auto* action = FindActionReport(report, index + 1U);
    if (has_logout_expectation && action != nullptr &&
        std::any_of(action->outbound_frame_summaries.begin(),
                    action->outbound_frame_summaries.end(),
                    [](const auto& frame) { return frame.msg_type == "5"; }) &&
        HasFramePredicateProof(scenario, report, logout_predicate)) {
      return true;
    }
  }
  return false;
}

auto
HasLogoutAckProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  bool saw_local_logout = false;
  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& action = scenario.actions[index];
    if (action.kind == InteropActionKind::kProtocolBeginLogout) {
      saw_local_logout = true;
      continue;
    }
    if (!saw_local_logout || action.kind != InteropActionKind::kProtocolInbound ||
        action.text.find("35=5") == std::string::npos) {
      continue;
    }
    const auto* report_action = FindActionReport(report, index + 1U);
    const auto has_disconnect_expectation = std::any_of(
      scenario.action_expectations.begin(), scenario.action_expectations.end(), [&](const auto& expectation) {
        return expectation.action_index == index + 1U && expectation.disconnect.value_or(false);
      });
    if (report_action != nullptr && report_action->disconnect && has_disconnect_expectation) {
      return true;
    }
  }
  return false;
}

auto
FindCounterpartyConfig(const InteropScenario& scenario, std::uint64_t session_id) -> const CounterpartyConfig*
{
  const auto it = std::find_if(scenario.engine_config.counterparties.begin(),
                               scenario.engine_config.counterparties.end(),
                               [&](const auto& counterparty) { return counterparty.session.session_id == session_id; });
  return it == scenario.engine_config.counterparties.end() ? nullptr : &*it;
}

auto
HasTestRequestAtHeartBtIntPlus20PctProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
  constexpr std::uint64_t kGraceDivisor = 5U;

  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& action = scenario.actions[index];
    if (action.kind != InteropActionKind::kProtocolTimer) {
      continue;
    }
    const auto* counterparty = FindCounterpartyConfig(scenario, action.session_id);
    if (counterparty == nullptr) {
      continue;
    }
    std::optional<std::uint64_t> last_inbound_ns;
    for (std::size_t previous = 0U; previous < index; ++previous) {
      const auto& previous_action = scenario.actions[previous];
      if (previous_action.session_id == action.session_id &&
          previous_action.kind == InteropActionKind::kProtocolInbound) {
        last_inbound_ns = previous_action.timestamp_ns;
      }
    }
    if (!last_inbound_ns.has_value()) {
      continue;
    }
    const auto interval_ns =
      static_cast<std::uint64_t>(counterparty->session.heartbeat_interval_seconds) * kNanosPerSecond;
    const auto expected_fire_ns = last_inbound_ns.value() + interval_ns + (interval_ns / kGraceDivisor) + 1U;
    const auto* action_report = FindActionReport(report, index + 1U);
    if (action.timestamp_ns == expected_fire_ns && action_report != nullptr &&
        std::any_of(action_report->outbound_frame_summaries.begin(),
                    action_report->outbound_frame_summaries.end(),
                    [](const auto& frame) { return frame.msg_type == "1"; })) {
      return true;
    }
  }
  return false;
}

auto
HasHeartbeatResponseProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  bool saw_test_request = false;
  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& action = scenario.actions[index];
    const auto* action_report = FindActionReport(report, index + 1U);
    if (action_report != nullptr &&
        std::any_of(action_report->outbound_frame_summaries.begin(),
                    action_report->outbound_frame_summaries.end(),
                    [](const auto& frame) { return frame.msg_type == "1" && !frame.test_req_id.empty(); })) {
      saw_test_request = true;
      continue;
    }
    if (saw_test_request && action.kind == InteropActionKind::kProtocolInbound &&
        action.text.find("35=0") != std::string::npos && action.text.find("112=") != std::string::npos &&
        action_report != nullptr && !action_report->disconnect) {
      return true;
    }
  }
  return false;
}

auto
HasFlushAfterLogonProof(const InteropScenario& scenario, const InteropReport& report) -> bool
{
  bool saw_queued_application = false;
  for (std::size_t index = 0U; index < scenario.actions.size(); ++index) {
    const auto& action = scenario.actions[index];
    const auto* action_report = FindActionReport(report, index + 1U);
    if (action.kind == InteropActionKind::kProtocolQueueApplication && action_report != nullptr &&
        action_report->queued_application_messages == 1U) {
      saw_queued_application = true;
      continue;
    }
    if (!saw_queued_application || action.kind != InteropActionKind::kProtocolInbound ||
        action.text.find("35=A") == std::string::npos || action_report == nullptr) {
      continue;
    }
    const auto has_logon_then_app = action_report->outbound_frame_summaries.size() >= 2U &&
                                    action_report->outbound_frame_summaries[0].msg_type == "A" &&
                                    action_report->outbound_frame_summaries[1].msg_type != "A";
    const auto has_frame_expectation = std::any_of(
      scenario.outbound_expectations.begin(), scenario.outbound_expectations.end(), [&](const auto& expectation) {
        return expectation.action_index == index + 1U && expectation.frame_index > 1U;
      });
    if (has_logon_then_app && has_frame_expectation) {
      return true;
    }
  }
  return false;
}

auto
HasPredicateProof(const VerificationPredicate& predicate, const InteropScenario& scenario, const InteropReport& report)
  -> bool
{
  if (HasActionPredicateProof(scenario, report, predicate) || HasFramePredicateProof(scenario, report, predicate) ||
      HasInboundPredicateProof(scenario, report, predicate)) {
    return true;
  }
  if (predicate.key == "next-in-unchanged") {
    return HasNextInUnchangedProof(scenario, report);
  }
  if (predicate.key == "subsequent-valid-message-accepted") {
    return HasSubsequentValidMessageAcceptedProof(scenario, report);
  }
  if (predicate.key == "logout-sent") {
    return HasLogoutSentProof(scenario, report);
  }
  if (predicate.key == "logout-ack") {
    return HasLogoutAckProof(scenario, report);
  }
  if (predicate.key == "heartbeat-response") {
    return HasHeartbeatResponseProof(scenario, report);
  }
  if (predicate.key == "testrequest-at-heartbtint-plus-20pct") {
    return HasTestRequestAtHeartBtIntPlus20PctProof(scenario, report);
  }
  if (predicate.key == "flush-after-logon") {
    return HasFlushAfterLogonProof(scenario, report);
  }
  if (predicate.key == "transport-connected") {
    return HasTransportConnectedProof(scenario, report);
  }
  if (predicate.key == "no-session-rejects") {
    return HasNoSessionRejectsProof(scenario, report);
  }
  if (predicate.key == "fix42-active") {
    return HasSessionStateProof(scenario, report, kOfficialFix42SessionId, session::SessionState::kActive);
  }
  if (predicate.key == "fix44-active") {
    return HasSessionStateProof(scenario, report, kOfficialFix44SessionId, session::SessionState::kActive);
  }
  if (predicate.key == "fixt11-active") {
    return HasSessionStateProof(scenario, report, kOfficialFixT11SessionId, session::SessionState::kActive);
  }
  return false;
}

auto
EvaluateOfficialVerification(const OfficialCaseManifestEntry& entry,
                             const InteropScenario& scenario,
                             const InteropReport& report) -> VerificationEvaluation
{
  VerificationEvaluation evaluation;
  if (entry.verification_status != OfficialCaseVerificationStatus::kVerified) {
    evaluation.missing_predicates = entry.missing_assertions;
    return evaluation;
  }
  if (entry.verified_assertions.empty()) {
    evaluation.missing_predicates.push_back("verified assertions are empty");
    return evaluation;
  }
  if (!entry.missing_assertions.empty()) {
    evaluation.missing_predicates = entry.missing_assertions;
    return evaluation;
  }

  for (const auto& token : entry.verified_assertions) {
    auto predicate = ParseVerifiedPredicate(token);
    if (!predicate.ok() || !HasPredicateProof(predicate.value(), scenario, report)) {
      evaluation.missing_predicates.push_back(token);
    }
  }
  evaluation.verified = evaluation.missing_predicates.empty();
  return evaluation;
}

auto
SplitVersions(std::string_view text) -> std::vector<std::string>
{
  if (Trim(text).empty()) {
    return {};
  }
  return Split(text, ',');
}

auto
JoinVersions(const std::vector<std::string>& versions) -> std::string
{
  std::string joined;
  for (std::size_t index = 0; index < versions.size(); ++index) {
    if (index != 0U) {
      joined.push_back(',');
    }
    joined.append(versions[index]);
  }
  return joined;
}

auto
JoinList(const std::vector<std::string>& values) -> std::string
{
  std::string joined;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      joined.push_back(',');
    }
    joined.append(values[index]);
  }
  return joined;
}

auto
ParseMetadataField(std::string_view field, std::string* key, std::string* value) -> base::Status
{
  const auto equals = field.find('=');
  if (equals == std::string_view::npos || equals == 0U) {
    return base::Status::InvalidArgument("official case metadata field must be key=value");
  }
  *key = std::string(Trim(field.substr(0, equals)));
  *value = std::string(Trim(field.substr(equals + 1U)));
  if (key->empty()) {
    return base::Status::InvalidArgument("official case metadata key is empty");
  }
  return base::Status::Ok();
}

auto
ApplyManifestMetadata(const std::vector<std::string>& parts, OfficialCaseManifestEntry* entry) -> base::Status
{
  if (entry == nullptr) {
    return base::Status::InvalidArgument("official case manifest entry is null");
  }

  for (std::size_t index = 8U; index < parts.size(); ++index) {
    std::string key;
    std::string value;
    auto status = ParseMetadataField(parts[index], &key, &value);
    if (!status.ok()) {
      return status;
    }

    if (key == "verify") {
      auto verification = ParseVerificationStatus(value);
      if (!verification.ok()) {
        return verification.status();
      }
      entry->verification_status = verification.value();
    } else if (key == "condition") {
      entry->official_condition = std::move(value);
    } else if (key == "expected") {
      entry->official_expected = std::move(value);
    } else if (key == "scope") {
      entry->verification_scope = std::move(value);
    } else if (key == "verified") {
      entry->verified_assertions = SplitVersions(value);
    } else if (key == "missing") {
      entry->missing_assertions = SplitVersions(value);
    } else {
      return base::Status::InvalidArgument("unknown official case manifest metadata key");
    }
  }

  auto syntax_status = ValidateVerifiedPredicateSyntax(entry->verified_assertions);
  if (!syntax_status.ok()) {
    return syntax_status;
  }
  if (entry->verification_status == OfficialCaseVerificationStatus::kVerified) {
    if (entry->official_condition.empty()) {
      return base::Status::InvalidArgument("verify=verified requires condition metadata");
    }
    if (entry->official_expected.empty()) {
      return base::Status::InvalidArgument("verify=verified requires expected metadata");
    }
    if (entry->verified_assertions.empty()) {
      return base::Status::InvalidArgument("verify=verified requires at least one verified predicate");
    }
    if (!entry->missing_assertions.empty()) {
      return base::Status::InvalidArgument("verify=verified cannot include missing predicates");
    }
  }

  return base::Status::Ok();
}

auto
ReadFileText(const std::filesystem::path& path) -> base::Result<std::string>
{
  std::ifstream in(path);
  if (!in.is_open()) {
    return base::Status::IoError("unable to open file: '" + path.string() + "'");
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

auto
DecodeHtmlEntity(std::string_view entity) -> std::string_view
{
  if (entity == "nbsp") {
    return " ";
  }
  if (entity == "amp") {
    return "&";
  }
  if (entity == "lt") {
    return "<";
  }
  if (entity == "gt") {
    return ">";
  }
  if (entity == "quot") {
    return "\"";
  }
  if (entity == "apos") {
    return "'";
  }
  return {};
}

auto
StripHtml(std::string_view text) -> std::string
{
  std::string output;
  output.reserve(text.size());
  bool in_tag = false;
  bool in_entity = false;
  std::string entity;

  for (const auto ch : text) {
    if (in_tag) {
      if (ch == '>') {
        in_tag = false;
        output.push_back('\n');
      }
      continue;
    }

    if (in_entity) {
      if (ch == ';') {
        const auto decoded = DecodeHtmlEntity(entity);
        if (!decoded.empty()) {
          output.append(decoded);
        }
        entity.clear();
        in_entity = false;
        continue;
      }
      entity.push_back(ch);
      continue;
    }

    if (ch == '<') {
      in_tag = true;
      continue;
    }
    if (ch == '&') {
      in_entity = true;
      entity.clear();
      continue;
    }
    output.push_back(ch);
  }

  return output;
}

auto
NormalizeScenarioId(std::string_view scenario_id) -> std::string
{
  auto value = std::string(Trim(scenario_id));
  while (!value.empty() && value.back() == '.') {
    value.pop_back();
  }
  return value;
}

auto
ExtractScenarioHeader(std::string_view line, std::string* scenario_id) -> bool
{
  if (!line.starts_with("Scenario ")) {
    return false;
  }
  const auto remainder = Trim(line.substr(std::string_view("Scenario ").size()));
  const auto first_space = remainder.find(' ');
  if (first_space == std::string_view::npos) {
    *scenario_id = NormalizeScenarioId(remainder);
    return true;
  }
  *scenario_id = NormalizeScenarioId(remainder.substr(0, first_space));
  return !scenario_id->empty();
}

auto
ExtractSubcaseLetter(std::string_view line) -> char
{
  if (line.size() >= 2U && std::islower(static_cast<unsigned char>(line[0])) != 0 && line[1] == '.') {
    return line[0];
  }
  return '\0';
}

auto
LooksLikeContentLine(std::string_view line) -> bool
{
  if (line.empty()) {
    return false;
  }
  if (line == "Mandatory" || line == "Optional") {
    return false;
  }
  if (line.starts_with("Scenario ")) {
    return false;
  }
  if (line.starts_with("#") || line.starts_with("FIX session layer test cases") || line == "Test Cases") {
    return false;
  }
  return true;
}

} // namespace

auto
LoadOfficialCaseManifestText(std::string_view text, const std::filesystem::path& base_dir)
  -> base::Result<OfficialCaseManifest>
{
  OfficialCaseManifest manifest;

  for (const auto& raw_line : SplitLines(text)) {
    const auto trimmed = Trim(raw_line);
    if (trimmed.empty() || trimmed.starts_with(kCommentPrefix)) {
      continue;
    }

    const auto parts = Split(trimmed, kFieldSeparator);
    if (parts.empty()) {
      continue;
    }

    if (parts[0] == "source") {
      if (parts.size() != 4U) {
        return base::Status::InvalidArgument("source record requires 4 fields");
      }
      manifest.source_name = parts[1];
      manifest.source_version = parts[2];
      manifest.source_url = parts[3];
      continue;
    }

    if (parts[0] == "case") {
      if (parts.size() < 8U) {
        return base::Status::InvalidArgument("case record requires at least 8 fields");
      }
      auto role = ParseRole(parts[2]);
      auto requirement = ParseRequirement(parts[3]);
      auto support = ParseSupport(parts[5]);
      if (!role.ok()) {
        return role.status();
      }
      if (!requirement.ok()) {
        return requirement.status();
      }
      if (!support.ok()) {
        return support.status();
      }

      OfficialCaseManifestEntry entry;
      entry.official_case_id = parts[1];
      entry.role = role.value();
      entry.requirement = requirement.value();
      entry.protocol_versions = SplitVersions(parts[4]);
      entry.support = support.value();
      if (!parts[6].empty()) {
        entry.scenario_path = base_dir / parts[6];
      }
      entry.note = parts[7];
      auto metadata_status = ApplyManifestMetadata(parts, &entry);
      if (!metadata_status.ok()) {
        return metadata_status;
      }
      manifest.entries.push_back(std::move(entry));
      continue;
    }

    return base::Status::InvalidArgument("unknown official case manifest record kind");
  }

  if (manifest.source_name.empty()) {
    return base::Status::InvalidArgument("official case manifest is missing source metadata");
  }
  return manifest;
}

auto
LoadOfficialCaseManifestFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>
{
  auto text = ReadFileText(path);
  if (!text.ok()) {
    return text.status();
  }
  return LoadOfficialCaseManifestText(text.value(), path.parent_path());
}

auto
SerializeOfficialCaseManifest(const OfficialCaseManifest& manifest, const std::filesystem::path& base_dir)
  -> std::string
{
  std::string output;
  output.append("source|");
  output.append(manifest.source_name);
  output.push_back('|');
  output.append(manifest.source_version);
  output.push_back('|');
  output.append(manifest.source_url);
  output.push_back('\n');

  for (const auto& entry : manifest.entries) {
    output.append("case|");
    output.append(entry.official_case_id);
    output.push_back('|');
    output.append(RoleText(entry.role));
    output.push_back('|');
    output.append(RequirementText(entry.requirement));
    output.push_back('|');
    output.append(JoinVersions(entry.protocol_versions));
    output.push_back('|');
    output.append(SupportText(entry.support));
    output.push_back('|');
    if (!entry.scenario_path.empty()) {
      if (!base_dir.empty()) {
        output.append(entry.scenario_path.lexically_relative(base_dir).string());
      } else {
        output.append(entry.scenario_path.string());
      }
    }
    output.push_back('|');
    output.append(entry.note);
    if (entry.verification_status != OfficialCaseVerificationStatus::kUnspecified) {
      output.append("|verify=");
      output.append(VerificationStatusText(entry.verification_status));
    }
    if (!entry.official_condition.empty()) {
      output.append("|condition=");
      output.append(entry.official_condition);
    }
    if (!entry.official_expected.empty()) {
      output.append("|expected=");
      output.append(entry.official_expected);
    }
    if (!entry.verification_scope.empty()) {
      output.append("|scope=");
      output.append(entry.verification_scope);
    }
    if (!entry.verified_assertions.empty()) {
      output.append("|verified=");
      output.append(JoinList(entry.verified_assertions));
    }
    if (!entry.missing_assertions.empty()) {
      output.append("|missing=");
      output.append(JoinList(entry.missing_assertions));
    }
    output.push_back('\n');
  }

  return output;
}

auto
ImportOfficialCaseHtmlText(std::string_view text) -> base::Result<OfficialCaseManifest>
{
  OfficialCaseManifest manifest;
  manifest.source_name = "FIX Session Layer Test Cases";
  manifest.source_version = "1.0";
  manifest.source_url = "https://dev.fixtrading.org/standards/fix-session-testcases-online/";

  const auto stripped = StripHtml(text);
  auto lines = SplitLines(stripped);

  OfficialCaseRole current_role = OfficialCaseRole::kAll;
  OfficialCaseRequirement current_requirement = OfficialCaseRequirement::kMandatory;
  std::string current_scenario_id;
  bool current_requirement_known = false;
  bool current_scenario_has_subcases = false;
  bool current_scenario_emitted_base = false;

  for (const auto& raw_line : lines) {
    const auto line = Trim(raw_line);
    if (line.empty()) {
      continue;
    }

    if (line.find("Buyside-oriented") != std::string_view::npos) {
      current_role = OfficialCaseRole::kInitiator;
      continue;
    }
    if (line.find("Sellside-oriented") != std::string_view::npos) {
      current_role = OfficialCaseRole::kAcceptor;
      continue;
    }
    if (line.find("Test cases applicable to all FIX systems") != std::string_view::npos) {
      current_role = OfficialCaseRole::kAll;
      continue;
    }

    std::string scenario_id;
    if (ExtractScenarioHeader(line, &scenario_id)) {
      current_scenario_id = std::move(scenario_id);
      current_requirement_known = false;
      current_scenario_has_subcases = false;
      current_scenario_emitted_base = false;
      continue;
    }

    if (line == "Mandatory") {
      current_requirement = OfficialCaseRequirement::kMandatory;
      current_requirement_known = true;
      continue;
    }
    if (line == "Optional") {
      current_requirement = OfficialCaseRequirement::kOptional;
      current_requirement_known = true;
      continue;
    }

    if (current_scenario_id.empty() || !current_requirement_known) {
      continue;
    }

    const auto subcase_letter = ExtractSubcaseLetter(line);
    if (subcase_letter != '\0') {
      current_scenario_has_subcases = true;
      current_scenario_emitted_base = true;
      manifest.entries.push_back(OfficialCaseManifestEntry{
        .official_case_id = current_scenario_id + "." + std::string(1U, subcase_letter),
        .role = current_role,
        .requirement = current_requirement,
        .protocol_versions = { "FIX.4.2", "FIX.4.4", "FIXT.1.1" },
        .support = OfficialCaseSupport::kUnsupported,
        .note = "imported skeleton",
      });
      continue;
    }

    if (!current_scenario_has_subcases && !current_scenario_emitted_base && LooksLikeContentLine(line)) {
      current_scenario_emitted_base = true;
      manifest.entries.push_back(OfficialCaseManifestEntry{
        .official_case_id = current_scenario_id,
        .role = current_role,
        .requirement = current_requirement,
        .protocol_versions = { "FIX.4.2", "FIX.4.4", "FIXT.1.1" },
        .support = OfficialCaseSupport::kUnsupported,
        .note = "imported skeleton",
      });
    }
  }

  if (manifest.entries.empty()) {
    return base::Status::InvalidArgument("no official case ids were discovered in the supplied HTML");
  }

  std::sort(manifest.entries.begin(), manifest.entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.official_case_id < rhs.official_case_id;
  });
  manifest.entries.erase(
    std::unique(manifest.entries.begin(),
                manifest.entries.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.official_case_id == rhs.official_case_id; }),
    manifest.entries.end());
  return manifest;
}

auto
ImportOfficialCaseHtmlFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>
{
  auto text = ReadFileText(path);
  if (!text.ok()) {
    return text.status();
  }
  return ImportOfficialCaseHtmlText(text.value());
}

auto
RunOfficialCaseManifest(const OfficialCaseManifest& manifest) -> base::Result<OfficialCaseRunSummary>
{
  OfficialCaseRunSummary summary;
  summary.source_name = manifest.source_name;
  summary.source_version = manifest.source_version;
  summary.source_url = manifest.source_url;
  summary.total_cases = manifest.entries.size();
  summary.results.reserve(manifest.entries.size());

  for (const auto& entry : manifest.entries) {
    OfficialCaseResult result;
    result.official_case_id = entry.official_case_id;
    result.role = entry.role;
    result.requirement = entry.requirement;
    result.protocol_versions = entry.protocol_versions;
    result.support = entry.support;
    result.scenario_path = entry.scenario_path;
    result.note = entry.note;
    result.verification_status = entry.verification_status;
    result.official_condition = entry.official_condition;
    result.official_expected = entry.official_expected;
    result.verification_scope = entry.verification_scope;
    result.verified_assertions = entry.verified_assertions;
    result.missing_assertions = entry.missing_assertions;

    switch (entry.support) {
      case OfficialCaseSupport::kUnsupported:
        result.outcome = OfficialCaseOutcome::kUnsupported;
        result.message = entry.note.empty() ? "unsupported" : entry.note;
        ++summary.unsupported_cases;
        break;
      case OfficialCaseSupport::kMapped:
      case OfficialCaseSupport::kExpectedFail: {
        if (entry.scenario_path.empty()) {
          return base::Status::InvalidArgument("mapped official case is missing a scenario path");
        }
        ++summary.mapped_cases;

        auto scenario = LoadInteropScenarioFile(entry.scenario_path);
        if (!scenario.ok()) {
          result.outcome = entry.support == OfficialCaseSupport::kExpectedFail ? OfficialCaseOutcome::kExpectedFail
                                                                               : OfficialCaseOutcome::kFailed;
          result.message = scenario.status().message();
        } else {
          auto run = RunInteropScenario(scenario.value());
          if (run.ok()) {
            if (entry.support == OfficialCaseSupport::kExpectedFail) {
              result.outcome = OfficialCaseOutcome::kUnexpectedPass;
              result.message = "scenario passed unexpectedly";
              ++summary.unexpected_pass_cases;
            } else {
              ++summary.passed_cases;
              const auto verification = EvaluateOfficialVerification(entry, scenario.value(), run.value());
              if (verification.verified) {
                result.outcome = OfficialCaseOutcome::kOfficiallyVerified;
                result.message = "scenario passed; official predicates machine-verified";
                ++summary.officially_verified_cases;
              } else {
                result.outcome = OfficialCaseOutcome::kPartial;
                if (verification.missing_predicates.empty()) {
                  result.message = "scenario passed; official semantic verification is incomplete";
                } else if (entry.verification_status == OfficialCaseVerificationStatus::kVerified) {
                  result.message =
                    "scenario passed; verified predicates not proven: " + JoinList(verification.missing_predicates);
                } else {
                  result.message =
                    "scenario passed; missing official assertions: " + JoinList(verification.missing_predicates);
                }
                ++summary.partial_cases;
              }
            }
          } else {
            if (entry.support == OfficialCaseSupport::kExpectedFail) {
              result.outcome = OfficialCaseOutcome::kExpectedFail;
              result.message = run.status().message();
              ++summary.expected_fail_cases;
            } else {
              result.outcome = OfficialCaseOutcome::kFailed;
              result.message = run.status().message();
            }
          }
        }

        if (result.outcome == OfficialCaseOutcome::kFailed) {
          ++summary.failed_cases;
        }
        break;
      }
    }

    summary.results.push_back(std::move(result));
  }

  return summary;
}

auto
RenderOfficialCaseCoverageReport(const OfficialCaseRunSummary& summary) -> std::string
{
  std::string output;
  output.append("# FIX Session Layer Coverage\n\n");
  output.append("- Source: ");
  output.append(summary.source_name);
  output.append(" v");
  output.append(summary.source_version);
  output.push_back('\n');
  output.append("- URL: ");
  output.append(summary.source_url);
  output.push_back('\n');
  output.append("- Total cases: ");
  output.append(std::to_string(summary.total_cases));
  output.push_back('\n');
  output.append("- Mapped: ");
  output.append(std::to_string(summary.mapped_cases));
  output.push_back('\n');
  output.append("- Passed: ");
  output.append(std::to_string(summary.passed_cases));
  output.push_back('\n');
  output.append("- Officially verified: ");
  output.append(std::to_string(summary.officially_verified_cases));
  output.push_back('\n');
  output.append("- Partial: ");
  output.append(std::to_string(summary.partial_cases));
  output.push_back('\n');
  output.append("- Failed: ");
  output.append(std::to_string(summary.failed_cases));
  output.push_back('\n');
  output.append("- Unsupported: ");
  output.append(std::to_string(summary.unsupported_cases));
  output.push_back('\n');
  output.append("- Expected fail: ");
  output.append(std::to_string(summary.expected_fail_cases));
  output.push_back('\n');
  output.append("- Unexpected pass: ");
  output.append(std::to_string(summary.unexpected_pass_cases));
  output.append("\n\n");

  output.append("## Results\n\n");
  for (const auto& result : summary.results) {
    output.append("- ");
    output.append(result.official_case_id);
    output.append(": ");
    output.append(OutcomeText(result.outcome));
    if (!result.message.empty()) {
      output.append(" - ");
      output.append(result.message);
    }
    output.push_back('\n');
  }

  return output;
}

} // namespace nimble::runtime