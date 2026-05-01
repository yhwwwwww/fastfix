#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/codec/raw_passthrough.h"
#include "nimblefix/runtime/router.h"
#include "test_support.h"

namespace {

struct RawFixture
{
  std::vector<std::byte> frame;
  nimble::codec::RawPassThroughView view;
};

auto
DecodeRaw(std::string_view body) -> RawFixture
{
  auto frame = nimble::tests::EncodeFixFrame(body);
  auto decoded = nimble::codec::DecodeRawPassThrough(std::span<const std::byte>(frame.data(), frame.size()));
  REQUIRE(decoded.ok());
  return RawFixture{ .frame = std::move(frame), .view = decoded.value() };
}

auto
MakeForwardingOptions() -> nimble::codec::ForwardingOptions
{
  return nimble::codec::ForwardingOptions{
    .sender_comp_id = "GW",
    .sender_sub_id = {},
    .target_comp_id = "VENUE",
    .target_sub_id = {},
    .msg_seq_num = 9U,
    .sending_time = "20260430-00:00:00.000",
    .begin_string = {},
    .on_behalf_of_comp_id = {},
    .deliver_to_comp_id = {},
    .poss_dup = false,
    .poss_resend = false,
    .orig_sending_time = {},
    .delimiter = nimble::codec::kFixSoh,
  };
}

} // namespace

TEST_CASE("routing table all messages to default target", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.SetDefaultAction(nimble::runtime::RoutingAction::kForward);
  table.SetDefaultTarget(42U);

  const auto message = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto route = table.Route(message.view);

  CHECK(route.action == nimble::runtime::RoutingAction::kForward);
  CHECK(route.target_session_id == 42U);
  CHECK(route.matched_rule_name.empty());
}

TEST_CASE("routing table msg type rule matches", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "orders",
    .criterion = nimble::runtime::RoutingCriterion::kMsgType,
    .msg_type = "D",
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 10U,
    .priority = 100U,
  });

  const auto message = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto route = table.Route(message.view);
  CHECK(route.action == nimble::runtime::RoutingAction::kForward);
  CHECK(route.target_session_id == 10U);
  CHECK(route.matched_rule_name == "orders");
}

TEST_CASE("routing table priority ordering", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "low-priority",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 1U,
    .priority = 200U,
  });
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "high-priority",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 2U,
    .priority = 10U,
  });

  const auto message = DecodeRaw("35=0|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|");
  const auto route = table.Route(message.view);
  CHECK(route.target_session_id == 2U);
  CHECK(route.matched_rule_name == "high-priority");
}

TEST_CASE("routing table custom predicate", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "predicate",
    .criterion = nimble::runtime::RoutingCriterion::kCustom,
    .predicate = [](const nimble::codec::RawPassThroughView& message) -> std::uint64_t {
      return message.sender_comp_id == "BUY" ? 77U : 0U;
    },
    .action = nimble::runtime::RoutingAction::kForward,
  });

  const auto message = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto route = table.Route(message.view);
  CHECK(route.action == nimble::runtime::RoutingAction::kForward);
  CHECK(route.target_session_id == 77U);
}

TEST_CASE("routing table drop action", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "drop-heartbeats",
    .criterion = nimble::runtime::RoutingCriterion::kMsgType,
    .msg_type = "0",
    .action = nimble::runtime::RoutingAction::kDrop,
    .target_session_id = 99U,
  });

  const auto message = DecodeRaw("35=0|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|");
  const auto route = table.Route(message.view);
  CHECK(route.action == nimble::runtime::RoutingAction::kDrop);
  CHECK(route.target_session_id == 0U);
}

TEST_CASE("routing table no match uses default", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "orders",
    .criterion = nimble::runtime::RoutingCriterion::kMsgType,
    .msg_type = "D",
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 10U,
  });
  table.SetDefaultAction(nimble::runtime::RoutingAction::kReject);

  const auto message = DecodeRaw("35=8|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|17=E1|");
  const auto route = table.Route(message.view);
  CHECK(route.action == nimble::runtime::RoutingAction::kReject);
  CHECK(route.target_session_id == 0U);
}

TEST_CASE("routing table remove rule by name", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{ .name = "remove-me", .target_session_id = 1U });
  table.AddRule(nimble::runtime::RoutingRule{ .name = "keep-me", .target_session_id = 2U });
  table.RemoveRule("remove-me");

  CHECK(table.rule_count() == 1U);
  const auto message = DecodeRaw("35=0|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|");
  const auto route = table.Route(message.view);
  CHECK(route.target_session_id == 2U);
  CHECK(route.matched_rule_name == "keep-me");
}

TEST_CASE("forward message encodes correctly", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "forward-all",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 200U,
  });

  const auto inbound = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|55=NIMBLE|");
  nimble::codec::EncodeBuffer buffer;
  auto forwarded = nimble::runtime::ForwardMessage(inbound.view, table, MakeForwardingOptions(), &buffer);
  REQUIRE(forwarded.ok());
  CHECK(forwarded.value().forwarded == 1U);
  CHECK(forwarded.value().dropped == 0U);

  auto decoded = nimble::codec::DecodeRawPassThrough(buffer.bytes());
  REQUIRE(decoded.ok());
  CHECK(decoded.value().sender_comp_id == "GW");
  CHECK(decoded.value().target_comp_id == "VENUE");
  CHECK(decoded.value().msg_seq_num == 9U);
  CHECK(buffer.text().find("11=ORD-1") != std::string_view::npos);
}
