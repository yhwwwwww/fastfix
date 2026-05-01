#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/runtime/message_log.h"
#include "nimblefix/store/memory_store.h"

namespace {

auto
Payload(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

auto
BinaryPayload(std::initializer_list<unsigned int> values) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(values.size());
  for (const auto value : values) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
  }
  return bytes;
}

auto
BytesToString(std::span<const std::byte> bytes) -> std::string
{
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto
SampleLog() -> nimble::runtime::MessageLog
{
  return nimble::runtime::MessageLog{
    .session_id = 42U,
    .entries = {
      nimble::runtime::MessageLogEntry{
        .session_id = 42U,
        .seq_num = 1U,
        .timestamp_ns = 100U,
        .direction = nimble::runtime::MessageDirection::kInbound,
        .flags = 1U,
        .payload = Payload("8=FIX.4.4\x01"),
      },
      nimble::runtime::MessageLogEntry{
        .session_id = 42U,
        .seq_num = 1U,
        .timestamp_ns = 200U,
        .direction = nimble::runtime::MessageDirection::kOutbound,
        .flags = 0U,
        .payload = BinaryPayload({ 0x38U, 0x01U, 0x8FU, 0xFFU }),
      },
      nimble::runtime::MessageLogEntry{
        .session_id = 42U,
        .seq_num = 2U,
        .timestamp_ns = 300U,
        .direction = nimble::runtime::MessageDirection::kInbound,
        .flags = 2U,
        .payload = Payload("35=D|11=ABC|"),
      },
    },
  };
}

auto
RequireEntryEqual(const nimble::runtime::MessageLogEntry& lhs, const nimble::runtime::MessageLogEntry& rhs) -> void
{
  REQUIRE(lhs.session_id == rhs.session_id);
  REQUIRE(lhs.seq_num == rhs.seq_num);
  REQUIRE(lhs.timestamp_ns == rhs.timestamp_ns);
  REQUIRE(lhs.direction == rhs.direction);
  REQUIRE(lhs.flags == rhs.flags);
  REQUIRE(lhs.payload == rhs.payload);
}

auto
RequireLogEqual(const nimble::runtime::MessageLog& lhs, const nimble::runtime::MessageLog& rhs) -> void
{
  REQUIRE(lhs.session_id == rhs.session_id);
  REQUIRE(lhs.entries.size() == rhs.entries.size());
  for (std::size_t index = 0U; index < lhs.entries.size(); ++index) {
    RequireEntryEqual(lhs.entries[index], rhs.entries[index]);
  }
}

} // namespace

TEST_CASE("message log export from memory store", "[message-log]")
{
  nimble::store::MemorySessionStore store;
  REQUIRE(store
            .SaveOutbound(nimble::store::MessageRecord{
              .session_id = 42U,
              .seq_num = 1U,
              .timestamp_ns = 100U,
              .flags = 0U,
              .payload = Payload("OUT1"),
            })
            .ok());
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 42U,
              .seq_num = 1U,
              .timestamp_ns = 150U,
              .flags = 1U,
              .payload = Payload("IN1"),
            })
            .ok());
  REQUIRE(store
            .SaveOutbound(nimble::store::MessageRecord{
              .session_id = 42U,
              .seq_num = 2U,
              .timestamp_ns = 200U,
              .flags = 0U,
              .payload = Payload("OUT2"),
            })
            .ok());
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 42U,
              .seq_num = 2U,
              .timestamp_ns = 250U,
              .flags = 2U,
              .payload = Payload("IN2"),
            })
            .ok());
  REQUIRE(store
            .SaveRecoveryState(nimble::store::SessionRecoveryState{
              .session_id = 42U,
              .next_in_seq = 3U,
              .next_out_seq = 3U,
              .last_inbound_ns = 250U,
              .last_outbound_ns = 200U,
              .active = true,
            })
            .ok());

  auto exported = nimble::runtime::ExportMessageLog(store, 42U);
  REQUIRE(exported.ok());
  REQUIRE(exported.value().session_id == 42U);
  REQUIRE(exported.value().entries.size() == 4U);
  REQUIRE(exported.value().entries[0].direction == nimble::runtime::MessageDirection::kOutbound);
  REQUIRE(exported.value().entries[1].direction == nimble::runtime::MessageDirection::kInbound);
  REQUIRE(exported.value().entries[2].direction == nimble::runtime::MessageDirection::kOutbound);
  REQUIRE(exported.value().entries[3].direction == nimble::runtime::MessageDirection::kInbound);
  REQUIRE(BytesToString(exported.value().entries[0].payload) == "OUT1");
  REQUIRE(BytesToString(exported.value().entries[1].payload) == "IN1");
  REQUIRE(BytesToString(exported.value().entries[2].payload) == "OUT2");
  REQUIRE(BytesToString(exported.value().entries[3].payload) == "IN2");
}

TEST_CASE("message log JSON round-trip", "[message-log]")
{
  const auto log = SampleLog();
  const auto json = nimble::runtime::MessageLogToJson(log);
  REQUIRE(json.find("\"payload\": \"38018fff\"") != std::string::npos);
  REQUIRE(json.find("\"direction\": \"inbound\"") != std::string::npos);
  REQUIRE(json.find("\"direction\": \"outbound\"") != std::string::npos);

  auto decoded = nimble::runtime::MessageLogFromJson(json);
  REQUIRE(decoded.ok());
  RequireLogEqual(log, decoded.value());
}

TEST_CASE("message log file round-trip", "[message-log]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-message-log-test";
  std::filesystem::remove_all(temp_root);
  const auto path = temp_root / "session-42.json";

  const auto log = SampleLog();
  REQUIRE(nimble::runtime::WriteMessageLog(log, path).ok());
  auto decoded = nimble::runtime::ReadMessageLog(path);
  REQUIRE(decoded.ok());
  RequireLogEqual(log, decoded.value());

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("replay controller max speed", "[message-log]")
{
  const auto log = SampleLog();
  std::vector<std::uint32_t> replayed_seq_nums;

  nimble::runtime::ReplayController controller;
  controller.LoadLog(log);
  controller.SetSpeed(nimble::runtime::ReplaySpeed::kMaxSpeed);
  controller.SetCallback([&replayed_seq_nums](const nimble::runtime::MessageLogEntry& entry) {
    replayed_seq_nums.push_back(entry.seq_num);
    return nimble::base::Status::Ok();
  });

  REQUIRE(controller.Run().ok());
  REQUIRE(replayed_seq_nums == std::vector<std::uint32_t>{ 1U, 1U, 2U });
  REQUIRE(controller.finished());
  REQUIRE(controller.position() == controller.total_entries());
}

TEST_CASE("replay controller step mode", "[message-log]")
{
  const auto log = SampleLog();
  nimble::runtime::ReplayController controller;
  controller.LoadLog(log);
  controller.SetSpeed(nimble::runtime::ReplaySpeed::kStep);

  REQUIRE(controller.position() == 0U);
  auto first = controller.Step();
  REQUIRE(first.ok());
  REQUIRE(first.value() != nullptr);
  REQUIRE(first.value()->seq_num == 1U);
  REQUIRE(controller.position() == 1U);

  auto second = controller.Step();
  REQUIRE(second.ok());
  REQUIRE(second.value() != nullptr);
  REQUIRE(second.value()->direction == nimble::runtime::MessageDirection::kOutbound);
  REQUIRE(controller.position() == 2U);

  auto third = controller.Step();
  REQUIRE(third.ok());
  REQUIRE(third.value() != nullptr);
  REQUIRE(controller.finished());

  auto done = controller.Step();
  REQUIRE(done.ok());
  REQUIRE(done.value() == nullptr);
  REQUIRE(controller.Run().code() == nimble::base::ErrorCode::kInvalidArgument);
}

TEST_CASE("replay controller callback error stops replay", "[message-log]")
{
  nimble::runtime::ReplayController controller;
  controller.LoadLog(SampleLog());

  std::uint32_t callbacks = 0U;
  controller.SetCallback([&callbacks](const nimble::runtime::MessageLogEntry&) {
    ++callbacks;
    if (callbacks == 3U) {
      return nimble::base::Status::InvalidArgument("stop at third entry");
    }
    return nimble::base::Status::Ok();
  });

  const auto status = controller.Run();
  REQUIRE(status.code() == nimble::base::ErrorCode::kInvalidArgument);
  REQUIRE(callbacks == 3U);
  REQUIRE(controller.position() == 2U);
  REQUIRE(!controller.finished());
}

TEST_CASE("message log sort and filter", "[message-log]")
{
  nimble::runtime::MessageLog log{
    .session_id = 88U,
    .entries = {
      nimble::runtime::MessageLogEntry{
        .session_id = 88U,
        .seq_num = 3U,
        .timestamp_ns = 300U,
        .direction = nimble::runtime::MessageDirection::kOutbound,
        .payload = Payload("OUT3"),
      },
      nimble::runtime::MessageLogEntry{
        .session_id = 88U,
        .seq_num = 1U,
        .timestamp_ns = 100U,
        .direction = nimble::runtime::MessageDirection::kInbound,
        .payload = Payload("IN1"),
      },
      nimble::runtime::MessageLogEntry{
        .session_id = 88U,
        .seq_num = 2U,
        .timestamp_ns = 200U,
        .direction = nimble::runtime::MessageDirection::kOutbound,
        .payload = Payload("OUT2"),
      },
      nimble::runtime::MessageLogEntry{
        .session_id = 88U,
        .seq_num = 2U,
        .timestamp_ns = 250U,
        .direction = nimble::runtime::MessageDirection::kInbound,
        .payload = Payload("IN2"),
      },
    },
  };

  log.SortByTimestamp();
  REQUIRE(log.entries[0].seq_num == 1U);
  REQUIRE(log.entries[1].seq_num == 2U);
  REQUIRE(log.entries[1].direction == nimble::runtime::MessageDirection::kOutbound);
  REQUIRE(log.entries[2].seq_num == 2U);
  REQUIRE(log.entries[2].direction == nimble::runtime::MessageDirection::kInbound);
  REQUIRE(log.entries[3].seq_num == 3U);

  const auto inbound = log.InboundEntries();
  const auto outbound = log.OutboundEntries();
  REQUIRE(inbound.size() == 2U);
  REQUIRE(outbound.size() == 2U);
  REQUIRE(inbound[0]->seq_num == 1U);
  REQUIRE(inbound[1]->seq_num == 2U);
  REQUIRE(outbound[0]->seq_num == 2U);
  REQUIRE(outbound[1]->seq_num == 3U);
}

TEST_CASE("memory store LoadInboundRange", "[message-log]")
{
  nimble::store::MemorySessionStore store;
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 77U,
              .seq_num = 3U,
              .timestamp_ns = 300U,
              .flags = 0U,
              .payload = Payload("IN3"),
            })
            .ok());
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 77U,
              .seq_num = 1U,
              .timestamp_ns = 100U,
              .flags = 0U,
              .payload = Payload("IN1"),
            })
            .ok());
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 77U,
              .seq_num = 2U,
              .timestamp_ns = 200U,
              .flags = 4U,
              .payload = Payload("IN2"),
            })
            .ok());
  REQUIRE(store
            .SaveInbound(nimble::store::MessageRecord{
              .session_id = 78U,
              .seq_num = 2U,
              .timestamp_ns = 999U,
              .flags = 0U,
              .payload = Payload("OTHER"),
            })
            .ok());

  auto loaded = store.LoadInboundRange(77U, 2U, 3U);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().size() == 2U);
  REQUIRE(loaded.value()[0].seq_num == 2U);
  REQUIRE(loaded.value()[0].flags == 4U);
  REQUIRE(BytesToString(loaded.value()[0].payload) == "IN2");
  REQUIRE(loaded.value()[1].seq_num == 3U);
  REQUIRE(BytesToString(loaded.value()[1].payload) == "IN3");
}
