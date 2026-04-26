#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include "nimblefix/runtime/fix_session_testcases.h"

TEST_CASE("official FIX session manifest runner", "[fix-session-testcases]")
{
  const auto manifest_path =
    std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session" / "official-session-cases.nfcases";

  auto manifest = nimble::runtime::LoadOfficialCaseManifestFile(manifest_path);
  REQUIRE(manifest.ok());
  const auto& manifest_value = manifest.value();
  const auto entry_count = manifest_value.entries.size();
  REQUIRE(entry_count == 85U);

  auto summary = nimble::runtime::RunOfficialCaseManifest(manifest_value);
  REQUIRE(summary.ok());
  const auto& summary_value = summary.value();
  REQUIRE(summary_value.total_cases == 85U);
  REQUIRE(summary_value.mapped_cases == 72U);
  REQUIRE(summary_value.passed_cases == 72U);
  REQUIRE(summary_value.officially_verified_cases == 72U);
  REQUIRE(summary_value.partial_cases == 0U);
  REQUIRE(summary_value.failed_cases == 0U);
  REQUIRE(summary_value.unsupported_cases == 13U);
  REQUIRE(summary_value.expected_fail_cases == 0U);
  REQUIRE(summary_value.unexpected_pass_cases == 0U);

  const auto find_case = [&](std::string_view id) {
    return std::find_if(summary_value.results.begin(), summary_value.results.end(), [&](const auto& result) {
      return result.official_case_id == id;
    });
  };
  REQUIRE(find_case("2S")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("2.k")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("6")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("12")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("14.a")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("14.n")->outcome == nimble::runtime::OfficialCaseOutcome::kUnsupported);
  REQUIRE(find_case("16.b")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("19.a")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);
  REQUIRE(find_case("20")->outcome == nimble::runtime::OfficialCaseOutcome::kOfficiallyVerified);

  const auto report = nimble::runtime::RenderOfficialCaseCoverageReport(summary_value);
  REQUIRE(report.find("FIX Session Layer Coverage") != std::string::npos);
  REQUIRE(report.find("Officially verified: 72") != std::string::npos);
  REQUIRE(report.find("Partial: 0") != std::string::npos);
  REQUIRE(report.find("2S: official-verified") != std::string::npos);
  REQUIRE(report.find("2.k: official-verified") != std::string::npos);
  REQUIRE(report.find("14.a: official-verified") != std::string::npos);
  REQUIRE(report.find("14.n: unsupported") != std::string::npos);
  REQUIRE(report.find("16.a: official-verified") != std::string::npos);
}

TEST_CASE("official FIX session HTML importer", "[fix-session-testcases]")
{
  const auto html_path =
    std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session" / "sample-session-testcases.html";

  auto manifest = nimble::runtime::ImportOfficialCaseHtmlFile(html_path);
  REQUIRE(manifest.ok());
  REQUIRE(manifest.value().entries.size() == 5U);

  std::vector<std::string> ids;
  ids.reserve(manifest.value().entries.size());
  for (const auto& entry : manifest.value().entries) {
    ids.push_back(entry.official_case_id);
  }
  std::sort(ids.begin(), ids.end());

  REQUIRE(ids[0] == "1B.a");
  REQUIRE(ids[1] == "1B.b");
  REQUIRE(ids[2] == "2S");
  REQUIRE(ids[3] == "4.a");
  REQUIRE(ids[4] == "4.b");
}

TEST_CASE("official FIX session verified metadata lint", "[fix-session-testcases]")
{
  const auto official_root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session";

  const std::string unknown_token_manifest =
    "source|FIX Session Layer Test Cases|1.0|test\n"
    "case|bad|acceptor|mandatory|FIX.4.4|mapped|cases/2s-first-message-not-logon.nfscenario|bad metadata|"
    "verify=verified|condition=condition|expected=expected|verified=not-a-real-predicate\n";
  auto unknown_token = nimble::runtime::LoadOfficialCaseManifestText(unknown_token_manifest, official_root);
  REQUIRE(!unknown_token.ok());

  const std::string missing_condition_manifest =
    "source|FIX Session Layer Test Cases|1.0|test\n"
    "case|bad|acceptor|mandatory|FIX.4.4|mapped|cases/2s-first-message-not-logon.nfscenario|bad metadata|"
    "verify=verified|expected=expected|verified=outbound=0\n";
  auto missing_condition = nimble::runtime::LoadOfficialCaseManifestText(missing_condition_manifest, official_root);
  REQUIRE(!missing_condition.ok());
}

TEST_CASE("official FIX session verified predicates are machine enforced", "[fix-session-testcases]")
{
  const auto official_root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session";

  const std::string forged_verified_manifest =
    "source|FIX Session Layer Test Cases|1.0|test\n"
    "case|2S|acceptor|mandatory|FIX.4.4|mapped|cases/2s-first-message-not-logon.nfscenario|forged predicate|"
    "verify=verified|condition=Acceptor receives a first inbound message other than Logon|"
    "expected=Disconnect without sending a FIX message|verified=reject-reason=9\n";

  auto manifest = nimble::runtime::LoadOfficialCaseManifestText(forged_verified_manifest, official_root);
  REQUIRE(manifest.ok());

  auto summary = nimble::runtime::RunOfficialCaseManifest(manifest.value());
  REQUIRE(summary.ok());
  REQUIRE(summary.value().total_cases == 1U);
  REQUIRE(summary.value().mapped_cases == 1U);
  REQUIRE(summary.value().passed_cases == 1U);
  REQUIRE(summary.value().officially_verified_cases == 0U);
  REQUIRE(summary.value().partial_cases == 1U);
  REQUIRE(summary.value().failed_cases == 0U);
  REQUIRE(summary.value().results.front().outcome == nimble::runtime::OfficialCaseOutcome::kPartial);
  REQUIRE(summary.value().results.front().message.find("reject-reason=9") != std::string::npos);
}