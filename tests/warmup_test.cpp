#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/warmup.h"

namespace {

constexpr std::uint64_t kFix44ProfileId = 4400U;

auto
Fix44ArtifactPath() -> std::filesystem::path
{
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
}

auto
MakeCounterparty(std::uint64_t session_id) -> nimble::runtime::CounterpartyConfig
{
  const auto suffix = std::to_string(session_id);
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           "warmup-session-" + suffix,
           session_id,
           nimble::session::SessionKey::ForInitiator("BUY" + suffix, "SELL" + suffix),
           kFix44ProfileId)
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
  config.counterparties.push_back(MakeCounterparty(1U));
  return config;
}

} // namespace

TEST_CASE("default warmup config generates steps for loaded profiles", "[warmup]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  auto config = nimble::runtime::DefaultWarmupConfig(engine, 3U);

  CHECK(config.steps.size() == 8U);
  CHECK(std::all_of(config.steps.begin(), config.steps.end(), [](const auto& step) {
    return step.profile_id == kFix44ProfileId && step.iterations == 3U;
  }));
}

TEST_CASE("run warmup completes within budget", "[warmup]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::WarmupConfig config;
  config.time_budget = std::chrono::milliseconds{ 5000 };
  config.steps.push_back(nimble::runtime::WarmupStep{
    .action = nimble::runtime::WarmupAction::kEncode,
    .profile_id = kFix44ProfileId,
    .msg_type = "D",
    .iterations = 2U,
  });
  config.steps.push_back(nimble::runtime::WarmupStep{
    .action = nimble::runtime::WarmupAction::kRoundTrip,
    .profile_id = kFix44ProfileId,
    .msg_type = "0",
    .iterations = 2U,
  });

  auto result = nimble::runtime::RunWarmup(engine, config);
  REQUIRE(result.ok());
  CHECK(result.value().steps_completed == 2U);
  CHECK(result.value().steps_skipped == 0U);
  CHECK(result.value().within_budget);
}

TEST_CASE("run warmup skips steps when budget exceeded", "[warmup]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::WarmupConfig config;
  config.time_budget = std::chrono::milliseconds{ 0 };
  config.steps.push_back(nimble::runtime::WarmupStep{
    .action = nimble::runtime::WarmupAction::kEncode,
    .profile_id = kFix44ProfileId,
    .msg_type = "D",
    .iterations = 1U,
  });

  auto result = nimble::runtime::RunWarmup(engine, config);
  REQUIRE(result.ok());
  CHECK(result.value().steps_completed == 0U);
  CHECK(result.value().steps_skipped == 1U);
  CHECK_FALSE(result.value().within_budget);
}

TEST_CASE("warmup result summary formatting", "[warmup]")
{
  nimble::runtime::WarmupResult result;
  result.steps_completed = 2U;
  result.steps_skipped = 1U;
  result.total_elapsed = std::chrono::microseconds{ 15 };
  result.within_budget = false;

  const auto summary = result.summary();
  CHECK(summary.find("completed 2") != std::string::npos);
  CHECK(summary.find("skipped 1") != std::string::npos);
  CHECK(summary.find("budget exceeded") != std::string::npos);
}

TEST_CASE("warmup encode step exercises codec", "[warmup]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig()).ok());
  nimble::runtime::WarmupConfig config;
  config.steps.push_back(nimble::runtime::WarmupStep{
    .action = nimble::runtime::WarmupAction::kDrySend,
    .profile_id = kFix44ProfileId,
    .msg_type = "A",
    .iterations = 1U,
  });

  auto result = nimble::runtime::RunWarmup(engine, config);
  REQUIRE(result.ok());
  REQUIRE(result.value().steps.size() == 1U);
  CHECK(result.value().steps[0].iterations_completed == 1U);
  CHECK_FALSE(result.value().steps[0].skipped);
}

TEST_CASE("warmup requires booted engine", "[warmup]")
{
  nimble::runtime::Engine engine;
  nimble::runtime::WarmupConfig config;
  config.steps.push_back(nimble::runtime::WarmupStep{ .profile_id = kFix44ProfileId, .msg_type = "0" });

  auto result = nimble::runtime::RunWarmup(engine, config);
  CHECK_FALSE(result.ok());
}
