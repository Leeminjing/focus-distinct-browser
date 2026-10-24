#pragma once
#include <cstdint>
#include <string>

namespace distinct {
// UI-thread-owned state. Time is supplied explicitly so expiry is checked at
// dispatch, even when the message loop has not delivered a timer yet.
enum class Mode { available, automatic, taking_over, manual, paused, faulted, closed };
inline const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::available: return "available";
    case Mode::automatic: return "automatic";
    case Mode::taking_over: return "taking_over";
    case Mode::manual: return "manual";
    case Mode::paused: return "paused";
    case Mode::faulted: return "faulted";
    case Mode::closed: return "closed";
    }
    return "faulted";
}
class Control {
public:
    Mode mode = Mode::available;
    std::string owner;
    std::string token;
    bool busy = false;
    bool observed = false;
    uint64_t expires = 0;

    void revoke(Mode next) {
        if (mode == Mode::closed || (mode == Mode::faulted && next != Mode::closed)) return;
        owner.clear(); token.clear(); observed = false; expires = 0;
        mode = next == Mode::manual && busy ? Mode::taking_over : next;
    }
    bool expire(uint64_t now) {
        if (mode == Mode::automatic && now >= expires) {
            revoke(Mode::available);
            return true;
        }
        return false;
    }
    std::string acquire(const std::string& client, const std::string& fresh,
                        uint64_t now, uint64_t ttl) {
        expire(now);
        if (busy || mode == Mode::automatic) return "busy";
        if (mode != Mode::available) return "control_unavailable";
        mode = Mode::automatic; owner = client; token = fresh;
        expires = now + ttl; observed = false;
        return {};
    }
    std::string check(const std::string& candidate, uint64_t now, bool mutation = false) {
        expire(now);
        if (candidate.empty() || mode != Mode::automatic || candidate != token)
            return "invalid_token";
        if (mutation && !observed) return "observation_required";
        return {};
    }
    std::string begin(const std::string& candidate, uint64_t now, bool mutation) {
        auto error = check(candidate, now, mutation);
        if (!error.empty()) return error;
        if (busy) return "busy";
        busy = true;
        return {};
    }
    void end() {
        busy = false;
        if (mode == Mode::taking_over) mode = Mode::manual;
    }
    std::string hand_back() {
        if (busy) return "busy";
        if (mode != Mode::manual && mode != Mode::paused) return "control_unavailable";
        revoke(Mode::available);
        return {};
    }
};
}
