#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/codec/raw_passthrough.h"
#include "nimblefix/runtime/router.h"

namespace {

struct Options
{
  std::string config_path;
};

auto
PrintUsage() -> void
{
  std::cout << "Usage: nimblefix-router --config <rules.txt> < messages.fix\n"
               "\n"
               "Config format (pipe-delimited):\n"
               "  rule|<name>|<expr>|<target-session>\n"
               "  drop|<name>|<expr>\n"
               "  rr|<name>|<expr>|<session>,<session>[,...]\n"
               "  sticky|<name>|<expr>|<key-tag>|<session>,<session>[,...]\n"
               "\n"
               "Input lines may be full FIX frames or body fields using | delimiters.\n";
}

[[nodiscard]] auto
Trim(std::string_view text) -> std::string_view
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1U);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1U);
  }
  return text;
}

[[nodiscard]] auto
Split(std::string_view text, char delimiter) -> std::vector<std::string_view>
{
  std::vector<std::string_view> parts;
  std::size_t begin = 0U;
  while (begin <= text.size()) {
    const auto end = text.find(delimiter, begin);
    if (end == std::string_view::npos) {
      parts.push_back(Trim(text.substr(begin)));
      break;
    }
    parts.push_back(Trim(text.substr(begin, end - begin)));
    begin = end + 1U;
  }
  return parts;
}

[[nodiscard]] auto
ParseU64(std::string_view text) -> std::optional<std::uint64_t>
{
  try {
    std::size_t consumed = 0U;
    auto value = std::stoull(std::string(text), &consumed);
    if (consumed != text.size() || value == 0U) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto
ParseU32(std::string_view text) -> std::optional<std::uint32_t>
{
  const auto parsed = ParseU64(text);
  if (!parsed.has_value() || *parsed > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*parsed);
}

[[nodiscard]] auto
ParseSessionPool(std::string_view text) -> std::optional<std::vector<std::uint64_t>>
{
  std::vector<std::uint64_t> sessions;
  for (const auto token : Split(text, ',')) {
    const auto parsed = ParseU64(token);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    sessions.push_back(*parsed);
  }
  if (sessions.empty()) {
    return std::nullopt;
  }
  return sessions;
}

[[nodiscard]] auto
ParseOptions(int argc, char** argv) -> std::optional<Options>
{
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--config" && index + 1 < argc) {
      options.config_path = argv[++index];
      continue;
    }
    return std::nullopt;
  }
  if (options.config_path.empty()) {
    return std::nullopt;
  }
  return options;
}

[[nodiscard]] auto
LoadRoutingTable(const std::string& config_path) -> nimble::base::Result<nimble::runtime::RoutingTable>
{
  std::ifstream input(config_path);
  if (!input) {
    return nimble::base::Status::IoError("unable to open router config");
  }

  nimble::runtime::RoutingTable table;
  std::string line;
  std::uint32_t priority = 100U;
  while (std::getline(input, line)) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }

    const auto parts = Split(trimmed, '|');
    if (parts.size() < 3U) {
      return nimble::base::Status::FormatError("router config line requires at least three fields");
    }

    auto parsed_expression = nimble::runtime::ParseRoutingExpression(parts[2]);
    if (!parsed_expression.ok()) {
      return parsed_expression.status();
    }

    nimble::runtime::RoutingRule rule{
      .name = std::string(parts[1]),
      .criterion = nimble::runtime::RoutingCriterion::kExpression,
      .expression = std::move(parsed_expression).value(),
      .action = nimble::runtime::RoutingAction::kForward,
      .priority = priority++,
    };

    if (parts[0] == "rule") {
      if (parts.size() != 4U) {
        return nimble::base::Status::FormatError("rule config requires target session");
      }
      const auto target = ParseU64(parts[3]);
      if (!target.has_value()) {
        return nimble::base::Status::FormatError("rule target session must be a positive integer");
      }
      rule.target_session_id = *target;
    } else if (parts[0] == "drop") {
      if (parts.size() != 3U) {
        return nimble::base::Status::FormatError("drop config requires name and expression only");
      }
      rule.action = nimble::runtime::RoutingAction::kDrop;
    } else if (parts[0] == "rr") {
      if (parts.size() != 4U) {
        return nimble::base::Status::FormatError("rr config requires a session pool");
      }
      auto sessions = ParseSessionPool(parts[3]);
      if (!sessions.has_value()) {
        return nimble::base::Status::FormatError("rr session pool is invalid");
      }
      rule.load_balancer = nimble::runtime::LoadBalancerConfig{
        .mode = nimble::runtime::LoadBalancingMode::kRoundRobin,
        .target_sessions = std::move(*sessions),
      };
    } else if (parts[0] == "sticky") {
      if (parts.size() != 5U) {
        return nimble::base::Status::FormatError("sticky config requires key tag and session pool");
      }
      auto key_tag = ParseU32(parts[3]);
      auto sessions = ParseSessionPool(parts[4]);
      if (!key_tag.has_value() || !sessions.has_value()) {
        return nimble::base::Status::FormatError("sticky config key tag or session pool is invalid");
      }
      rule.load_balancer = nimble::runtime::LoadBalancerConfig{
        .mode = nimble::runtime::LoadBalancingMode::kSticky,
        .target_sessions = std::move(*sessions),
        .sticky_key_tag = *key_tag,
      };
    } else {
      return nimble::base::Status::FormatError("unknown router config record type");
    }

    table.AddRule(std::move(rule));
  }

  return table;
}

[[nodiscard]] auto
ToBytes(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

[[nodiscard]] auto
WrapBodyFields(std::string body) -> std::vector<std::byte>
{
  for (auto& ch : body) {
    if (ch == '|') {
      ch = nimble::codec::kFixSoh;
    }
  }
  if (!body.empty() && body.back() != nimble::codec::kFixSoh) {
    body.push_back(nimble::codec::kFixSoh);
  }

  std::string full;
  full.append(nimble::codec::tags::kBeginStringPrefix);
  full.append("FIX.4.4");
  full.push_back(nimble::codec::kFixSoh);
  full.append(nimble::codec::tags::kBodyLengthPrefix);
  full.append(std::to_string(body.size()));
  full.push_back(nimble::codec::kFixSoh);
  full.append(body);

  std::uint32_t checksum = 0U;
  for (const auto ch : full) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;
  std::array<char, 3> digits{
    static_cast<char>('0' + ((checksum / 100U) % 10U)),
    static_cast<char>('0' + ((checksum / 10U) % 10U)),
    static_cast<char>('0' + (checksum % 10U)),
  };
  full.append(nimble::codec::tags::kCheckSumPrefix);
  full.append(digits.data(), digits.size());
  full.push_back(nimble::codec::kFixSoh);
  return ToBytes(full);
}

[[nodiscard]] auto
NormalizeFrame(std::string line) -> std::vector<std::byte>
{
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  if (line.rfind(nimble::codec::tags::kBeginStringPrefix, 0U) == 0U) {
    for (auto& ch : line) {
      if (ch == '|') {
        ch = nimble::codec::kFixSoh;
      }
    }
    return ToBytes(line);
  }
  return WrapBodyFields(std::move(line));
}

} // namespace

int
main(int argc, char** argv)
{
  const auto options = ParseOptions(argc, argv);
  if (!options.has_value()) {
    PrintUsage();
    return 1;
  }

  auto table_result = LoadRoutingTable(options->config_path);
  if (!table_result.ok()) {
    std::cerr << table_result.status().message() << '\n';
    return 1;
  }
  auto table = std::move(table_result).value();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (Trim(line).empty()) {
      continue;
    }

    const auto bytes = NormalizeFrame(std::move(line));
    auto decoded = nimble::codec::DecodeRawPassThrough(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!decoded.ok()) {
      std::cout << "ERROR " << decoded.status().message() << '\n';
      continue;
    }

    const auto route = table.Route(decoded.value());
    std::cout << "rule=" << (route.matched_rule_name.empty() ? "<default>" : route.matched_rule_name) << " action=";
    switch (route.action) {
      case nimble::runtime::RoutingAction::kForward:
        std::cout << "forward target=" << route.target_session_id;
        break;
      case nimble::runtime::RoutingAction::kDrop:
        std::cout << "drop";
        break;
      case nimble::runtime::RoutingAction::kReject:
        std::cout << "reject";
        break;
    }
    std::cout << '\n';
  }

  return 0;
}
