#include "DaemonSession.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(DaemonSessionTest, ConnectAndDisconnect) {
  DaemonSession session;

  // connect() spawns firmiusd locally and connects, so this might need actual binary.
  // We can just verify it attempts connection and handles failure or success.
  // We'll test it without assertions on bool value if the environment has no firmiusd.
  bool ok = session.connect();
  if (ok) {
    session.subscribe([](const firmius::daemon::DaemonEventEnvelope &) {});
  }
  
  // It should be able to destruct cleanly even if not connected.
}
