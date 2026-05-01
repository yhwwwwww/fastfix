#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/management.h"

namespace {

constexpr std::uint64_t kFix44ProfileId = 4400U;

auto
Fix44ArtifactPath() -> std::filesystem::path
{
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
}

auto
MakeCounterparty(std::uint64_t session_id, std::string name) -> nimble::runtime::CounterpartyConfig
{
  const auto suffix = std::to_string(session_id);
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           std::move(name),
           session_id,
           nimble::session::SessionKey::ForInitiator("BUY" + suffix, "SELL" + suffix),
           kFix44ProfileId)
    .reconnect()
    .build();
}

auto
MakeEngineConfig() -> nimble::runtime::EngineConfig
{
  if (!std::filesystem::exists(Fix44ArtifactPath())) {
    SKIP("FIX44 artifact not available at: " << Fix44ArtifactPath().string());
  }
  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.profile_artifacts.push_back(Fix44ArtifactPath());
  config.listeners.push_back(nimble::runtime::ListenerConfig{ .name = "main", .host = "127.0.0.1", .port = 0U });
  config.counterparties.push_back(MakeCounterparty(1U, "venue-a"));
  return config;
}

} // namespace

TEST_CASE("management plane query engine status", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  auto status = management.QueryEngineStatus();
  REQUIRE(status.ok());
  CHECK(status.value().booted);
  CHECK(status.value().worker_count == 1U);
  CHECK(status.value().total_sessions == 1U);
  CHECK(status.value().loaded_profiles == 1U);
  CHECK(status.value().listener_count == 1U);
}

TEST_CASE("management plane query session status", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  auto status = management.QuerySessionStatus(1U);
  REQUIRE(status.ok());
  CHECK(status.value().session_id == 1U);
  CHECK(status.value().name == "venue-a");
  CHECK(status.value().is_initiator);
  CHECK(status.value().profile_id == kFix44ProfileId);
  CHECK(status.value().sender_comp_id == "BUY1");
  CHECK(status.value().target_comp_id == "SELL1");
}

TEST_CASE("management plane query all sessions", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  auto sessions = management.QueryAllSessions();
  REQUIRE(sessions.ok());
  CHECK(sessions.value().size() == 1U);
  CHECK(sessions.value()[0].session_id == 1U);
}

TEST_CASE("management plane health check booted engine", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  CHECK(management.IsHealthy());
  CHECK(management.boot_timestamp_ns() != 0U);
}

TEST_CASE("management plane health summary", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  const auto summary = management.HealthSummary();
  CHECK(summary.find("engine healthy") != std::string::npos);
  CHECK(summary.find("sessions=1") != std::string::npos);
}

TEST_CASE("management plane query unknown session returns not found", "[management]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::ManagementPlane management(&engine);

  auto status = management.QuerySessionStatus(999U);
  CHECK_FALSE(status.ok());
  CHECK(status.status().code() == nimble::base::ErrorCode::kNotFound);
}

TEST_CASE("management plane requires booted engine", "[management]")
{
  nimble::runtime::Engine engine;
  nimble::runtime::ManagementPlane management(&engine);

  auto status = management.QueryEngineStatus();
  CHECK_FALSE(status.ok());
  CHECK_FALSE(management.IsHealthy());
}
