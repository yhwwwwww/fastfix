#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

auto
AddExpressionRule(nimble::runtime::RoutingTable* table,
                  std::string_view name,
                  std::string_view expression_text,
                  std::uint64_t target_session_id) -> void
{
  auto expression = nimble::runtime::ParseRoutingExpression(expression_text);
  REQUIRE(expression.ok());
  table->AddRule(nimble::runtime::RoutingRule{
    .name = std::string(name),
    .criterion = nimble::runtime::RoutingCriterion::kExpression,
    .expression = std::move(expression).value(),
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = target_session_id,
  });
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

TEST_CASE("expression rule equals", "[router]")
{
  nimble::runtime::RoutingTable table;
  AddExpressionRule(&table, "aapl", "55==AAPL", 91U);

  const auto message = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|55=AAPL|");
  const auto route = table.Route(message.view);

  CHECK(route.action == nimble::runtime::RoutingAction::kForward);
  CHECK(route.target_session_id == 91U);
  CHECK(route.matched_rule_name == "aapl");
}

TEST_CASE("expression rule not equals", "[router]")
{
  nimble::runtime::RoutingTable table;
  AddExpressionRule(&table, "not-buy", "49!=BUY", 92U);

  const auto message = DecodeRaw("35=D|49=CLIENT|56=SELL|34=1|52=20260430-00:00:00.000|55=AAPL|");
  const auto route = table.Route(message.view);

  CHECK(route.target_session_id == 92U);
  CHECK(route.matched_rule_name == "not-buy");
}

TEST_CASE("expression rule has tag", "[router]")
{
  nimble::runtime::RoutingTable table;
  AddExpressionRule(&table, "has-applver", "HAS(1128)", 93U);

  const auto message = DecodeRaw("35=D|49=BUY|56=SELL|34=1|1128=9|52=20260430-00:00:00.000|55=AAPL|");
  const auto route = table.Route(message.view);

  CHECK(route.target_session_id == 93U);
  CHECK(route.matched_rule_name == "has-applver");
}

TEST_CASE("expression rule and connector", "[router]")
{
  auto expression = nimble::runtime::ParseRoutingExpression("55==AAPL AND 54==1");
  REQUIRE(expression.ok());

  const auto matching = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|55=AAPL|54=1|");
  const auto wrong_side = DecodeRaw("35=D|49=BUY|56=SELL|34=2|52=20260430-00:00:00.000|55=AAPL|54=2|");

  CHECK(nimble::runtime::EvaluateExpression(expression.value(), matching.view));
  CHECK_FALSE(nimble::runtime::EvaluateExpression(expression.value(), wrong_side.view));
}

TEST_CASE("expression rule or connector", "[router]")
{
  auto expression = nimble::runtime::ParseRoutingExpression("35==D OR 35==G");
  REQUIRE(expression.ok());

  const auto order = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto cancel_replace = DecodeRaw("35=G|49=BUY|56=SELL|34=2|52=20260430-00:00:00.000|11=ORD-2|");
  const auto execution = DecodeRaw("35=8|49=SELL|56=BUY|34=3|52=20260430-00:00:00.000|17=EXEC-1|");

  CHECK(nimble::runtime::EvaluateExpression(expression.value(), order.view));
  CHECK(nimble::runtime::EvaluateExpression(expression.value(), cancel_replace.view));
  CHECK_FALSE(nimble::runtime::EvaluateExpression(expression.value(), execution.view));
}

TEST_CASE("expression parse error", "[router]")
{
  const auto expression = nimble::runtime::ParseRoutingExpression("55==AAPL XOR 49==BUY");
  CHECK_FALSE(expression.ok());
}

TEST_CASE("round robin load balancing", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "rr",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .load_balancer =
      nimble::runtime::LoadBalancerConfig{
        .mode = nimble::runtime::LoadBalancingMode::kRoundRobin,
        .target_sessions = { 101U, 102U, 103U },
      },
  });

  const auto first = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto second = DecodeRaw("35=D|49=BUY|56=SELL|34=2|52=20260430-00:00:00.000|11=ORD-2|");
  const auto third = DecodeRaw("35=D|49=BUY|56=SELL|34=3|52=20260430-00:00:00.000|11=ORD-3|");

  CHECK(table.Route(first.view).target_session_id == 101U);
  CHECK(table.Route(second.view).target_session_id == 102U);
  CHECK(table.Route(third.view).target_session_id == 103U);
}

TEST_CASE("sticky load balancing", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "sticky",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .load_balancer =
      nimble::runtime::LoadBalancerConfig{
        .mode = nimble::runtime::LoadBalancingMode::kSticky,
        .target_sessions = { 201U, 202U, 203U },
        .sticky_key_tag = 49U,
      },
  });

  const auto buy_first = DecodeRaw("35=D|49=BUY-A|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|");
  const auto buy_second = DecodeRaw("35=D|49=BUY-A|56=SELL|34=2|52=20260430-00:00:00.000|11=ORD-2|");
  const auto buy_other = DecodeRaw("35=D|49=BUY-B|56=SELL|34=3|52=20260430-00:00:00.000|11=ORD-3|");

  const auto first_route = table.Route(buy_first.view);
  const auto second_route = table.Route(buy_second.view);
  const auto other_route = table.Route(buy_other.view);

  CHECK(first_route.target_session_id == 201U);
  CHECK(second_route.target_session_id == first_route.target_session_id);
  CHECK(other_route.target_session_id == 202U);
}

TEST_CASE("field transform set", "[router]")
{
  nimble::runtime::RoutingTable table;
  table.AddRule(nimble::runtime::RoutingRule{
    .name = "set-symbol",
    .criterion = nimble::runtime::RoutingCriterion::kAll,
    .action = nimble::runtime::RoutingAction::kForward,
    .target_session_id = 301U,
    .transform =
      nimble::runtime::MessageTransform{
        .field_transforms = { nimble::runtime::FieldTransform{
          .action = nimble::runtime::FieldTransform::Action::kSet,
          .tag = 55U,
          .value = "MSFT",
        } },
      },
  });

  const auto inbound = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|55=AAPL|");
  nimble::codec::EncodeBuffer buffer;
  auto forwarded = nimble::runtime::ForwardMessage(inbound.view, table, MakeForwardingOptions(), &buffer);
  REQUIRE(forwarded.ok());

  CHECK(buffer.text().find("55=MSFT") != std::string_view::npos);
  CHECK(buffer.text().find("55=AAPL") == std::string_view::npos);
}

TEST_CASE("field transform remove", "[router]")
{
  const auto inbound = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|55=AAPL|58=DROP-ME|");
  std::vector<std::byte> output_body;
  const auto status = nimble::runtime::ApplyTransform(inbound.view,
                                                      nimble::runtime::MessageTransform{
                                                        .field_transforms = { nimble::runtime::FieldTransform{
                                                          .action = nimble::runtime::FieldTransform::Action::kRemove,
                                                          .tag = 58U,
                                                        } },
                                                      },
                                                      &output_body);
  REQUIRE(status.ok());

  const auto text = std::string_view(reinterpret_cast<const char*>(output_body.data()), output_body.size());
  CHECK(text.find("58=DROP-ME") == std::string_view::npos);
  CHECK(text.find("55=AAPL") != std::string_view::npos);
}

TEST_CASE("field transform add", "[router]")
{
  const auto inbound = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|55=AAPL|");
  std::vector<std::byte> output_body;
  const auto status = nimble::runtime::ApplyTransform(inbound.view,
                                                      nimble::runtime::MessageTransform{
                                                        .field_transforms = { nimble::runtime::FieldTransform{
                                                          .action = nimble::runtime::FieldTransform::Action::kAdd,
                                                          .tag = 58U,
                                                          .value = "ADDED",
                                                        } },
                                                      },
                                                      &output_body);
  REQUIRE(status.ok());

  const auto text = std::string_view(reinterpret_cast<const char*>(output_body.data()), output_body.size());
  CHECK(text.find("58=ADDED") != std::string_view::npos);
}

TEST_CASE("field transform copy", "[router]")
{
  const auto inbound = DecodeRaw("35=D|49=BUY|56=SELL|34=1|52=20260430-00:00:00.000|11=ORD-1|55=AAPL|");
  std::vector<std::byte> output_body;
  const auto status = nimble::runtime::ApplyTransform(inbound.view,
                                                      nimble::runtime::MessageTransform{
                                                        .field_transforms = { nimble::runtime::FieldTransform{
                                                          .action = nimble::runtime::FieldTransform::Action::kCopy,
                                                          .tag = 10055U,
                                                          .source_tag = 55U,
                                                        } },
                                                      },
                                                      &output_body);
  REQUIRE(status.ok());

  const auto text = std::string_view(reinterpret_cast<const char*>(output_body.data()), output_body.size());
  CHECK(text.find("10055=AAPL") != std::string_view::npos);
}
