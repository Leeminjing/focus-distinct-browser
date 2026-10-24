#include "../src/control.h"
#include <cstdlib>
#include <iostream>
using namespace distinct;
static void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    Control a, b;
    require(a.acquire("alice", "first", 100, 100).empty(), "initial acquire");
    require(a.begin("first", 101, true) == "observation_required", "must observe after acquire");
    require(a.acquire("bob", "second", 101, 100) == "busy", "exclusive lease");
    require(b.acquire("bob", "other", 101, 100).empty(), "independent browsers");
    require(a.begin("first", 101, false).empty(), "begin observation");
    require(a.begin("first", 102, false) == "busy", "same browser operation serialization");
    a.observed = true; a.end();
    require(a.begin("first", 103, true).empty(), "observed action");
    a.revoke(Mode::manual);
    require(a.mode == Mode::taking_over, "handover waits for dispatched action");
    require(a.check("first", 104, true) == "invalid_token", "reject remaining substeps");
    require(a.hand_back() == "busy", "cannot hand back before settlement");
    a.end();
    require(a.mode == Mode::manual, "manual after settlement");
    require(a.hand_back().empty(), "hand back");
    require(a.acquire("bob", "second", 110, 100).empty(), "new controller");
    require(a.check("first", 111) == "invalid_token", "old generation rejected");
    require(a.begin("second", 111, true) == "observation_required", "new controller reobserves");
    a.observed = true;
    require(a.begin("second", 210, true) == "invalid_token", "expiry at exact deadline without timer");
    require(a.acquire("bob", "third", 211, 100).empty(), "expired lease is replaceable");
    a.revoke(Mode::paused);
    require(a.acquire("bob", "fourth", 212, 100) == "control_unavailable", "pause persists after restore");
    require(a.hand_back().empty(), "explicit hand back after pause");
    require(a.acquire("alice", "fourth", 220, 100).empty(), "acquire after pause");
    a.observed = true; a.begin("fourth", 221, true);
    a.revoke(Mode::available);
    require(a.acquire("bob", "fifth", 222, 100) == "busy", "release cannot overlap in-flight work");
    a.end();
    require(a.acquire("bob", "fifth", 223, 100).empty(), "acquire after old work settled");
    a.revoke(Mode::faulted);
    require(a.hand_back() == "control_unavailable", "fault cannot be cleared by hand back");
    a.revoke(Mode::paused);
    require(a.mode == Mode::faulted, "minimizing cannot clear a fault");
    a.revoke(Mode::closed);
    require(a.acquire("bob", "sixth", 224, 100) == "control_unavailable", "closed is terminal");
    std::cout << "control lifecycle tests passed\n";
}
