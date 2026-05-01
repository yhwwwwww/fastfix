#include <catch2/catch_test_macros.hpp>

#include "nimblefix/session/session_key.h"

TEST_CASE("session key initiator factory", "[session-key]")
{
  const auto key = nimble::session::SessionKey::ForInitiator("BUY1", "SELL1");

  REQUIRE(key.begin_string == "FIX.4.4");
  REQUIRE(key.sender_comp_id == "BUY1");
  REQUIRE(key.target_comp_id == "SELL1");
}

TEST_CASE("session key acceptor factory", "[session-key]")
{
  const auto key = nimble::session::SessionKey::ForAcceptor("SELL1", "BUY1", "FIXT.1.1");

  REQUIRE(key.begin_string == "FIXT.1.1");
  REQUIRE(key.sender_comp_id == "SELL1");
  REQUIRE(key.target_comp_id == "BUY1");
}
